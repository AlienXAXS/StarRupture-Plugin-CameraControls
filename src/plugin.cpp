#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"

#include "camera_rig.h"
#include "editor_state.h"
#include "fly_controls.h"
#include "hud_visibility.h"
#include "input_binds.h"
#include "player_safeguard.h"
#include "player_vitals.h"
#include "project_io.h"
#include "ui_editor.h"
#include "ui_overlay.h"
#include "ui_properties.h"
#include "viewport_fit.h"
#include "world_draw.h"

#include "Engine_classes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace CameraControls;

// Global plugin self pointer — stable for the plugin's lifetime, retained from PluginInit
static IPluginSelf* g_self = nullptr;

IPluginSelf* GetSelf() { return g_self; }

// Plugin metadata
#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

// Build target — derived from the configuration's define (see Shared.props)
#if defined(MODLOADER_SERVER_BUILD)
#define PLUGIN_TARGET_THIS PLUGIN_TARGET_SERVER
#else
#define PLUGIN_TARGET_THIS PLUGIN_TARGET_CLIENT
#endif

static PluginInfo s_pluginInfo = {
	"CameraControls",
	MODLOADER_BUILD_TAG,
	"AlienX",
	"Keyframe camera timeline editor -- fly, keyframe, scrub and play back cinematic camera moves",
	PLUGIN_INTERFACE_VERSION,
	PLUGIN_TARGET_THIS
};

namespace
{
	constexpr const char* kWorldName = "ChimeraMain";

	bool  g_inChimeraMain = false;

	// The editor may only ever run in the main game world. Nothing it does makes
	// sense anywhere else -- there is no player to stash, no camera manager to
	// take over, and in the menu worlds the pointers it would cache are torn
	// down without warning.
	//
	// The cached flag is maintained by the world callbacks, but this also probes
	// the live world before committing: a begin/end-play pair that we somehow
	// missed would otherwise leave the flag stuck on, and the failure mode for
	// that is spawning a camera actor into a world that is going away.
	// Game thread only.
	bool VerifyInChimeraMain()
	{
		try
		{
			SDK::UWorld* world = SDK::UWorld::GetWorld();
			g_inChimeraMain = world && world->GetName() == kWorldName;
		}
		catch (...)
		{
			g_inChimeraMain = false;
		}

		return g_inChimeraMain;
	}
	void* g_inputToken    = nullptr;   // exclusive capture, or passthrough (see below)
	bool  g_tokenIsPassthrough = false;

	// Previous frame's playhead, so the tick can tell "the user scrubbed" from
	// "nothing happened" without the UI having to report a drag.
	double g_lastPlayhead = 0.0;

	// Last value of the hide-HUD option the tick acted on. Hiding walks the
	// object list, so it is driven off the edge rather than reconciled every
	// frame against whether it happened to succeed.
	bool g_hudRequested = false;

	void SaveOptionsToConfig(const State& state);

	// Logging only. Keeps the request drain readable at DEBUG without anyone
	// having to map integers back to enum values by hand.
	const char* RequestName(Request request)
	{
		switch (request)
		{
			case Request::EnterEditor:                return "EnterEditor";
			case Request::ExitEditor:                 return "ExitEditor";
			case Request::StartPlayback:              return "StartPlayback";
			case Request::StopPlayback:               return "StopPlayback";
			case Request::CaptureAppend:              return "CaptureAppend";
			case Request::CaptureInsertAfterSelected: return "CaptureInsertAfterSelected";
			case Request::UpdateSelectedFromCamera:   return "UpdateSelectedFromCamera";
			case Request::GotoSelected:               return "GotoSelected";
			case Request::SnapCameraToPlayhead:       return "SnapCameraToPlayhead";
			case Request::LookAtSelectionFromCamera:  return "LookAtSelectionFromCamera";
			default:                                  return "?";
		}
	}

	const char* ModeName(Mode mode)
	{
		switch (mode)
		{
			case Mode::Off:      return "Off";
			case Mode::Editor:   return "Editor";
			case Mode::Playback: return "Playback";
			default:             return "?";
		}
	}

	// -----------------------------------------------------------------------
	// Input ownership
	//
	// Editor mode takes *exclusive* capture by default: with the camera
	// detached, a stray click would otherwise fire the weapon the player is
	// still holding, and Escape would open the pause menu mid-shot. The fly
	// camera keeps working regardless because the modloader dispatches plugin
	// keybinds from its WndProc hook whether or not input is captured.
	//
	// Users who want the game to keep receiving input (co-op, or triggering
	// something in-world while filming) can flip PassthroughInput in the
	// config, which switches to the v51 cooperative token instead.
	// -----------------------------------------------------------------------
	void AcquireInput(bool passthrough)
	{
		auto* hooks = GetHooks();
		if (!hooks || !hooks->UI || g_inputToken)
			return;

		g_tokenIsPassthrough = passthrough;
		g_inputToken = passthrough
			? hooks->UI->AcquireInputPassthrough()
			: hooks->UI->AcquireInputCapture();
	}

	void ReleaseInput()
	{
		auto* hooks = GetHooks();
		if (!hooks || !hooks->UI || !g_inputToken)
			return;

		if (g_tokenIsPassthrough)
			hooks->UI->ReleaseInputPassthrough(g_inputToken);
		else
			hooks->UI->ReleaseInputCapture(g_inputToken);

		g_inputToken = nullptr;
	}

	// -----------------------------------------------------------------------
	// Mode transitions -- all game thread, all called with the state lock held.
	// -----------------------------------------------------------------------
	void EnterEditor(State& state, double now)
	{
		if (state.mode != Mode::Off)
			return;

		if (!VerifyInChimeraMain())
		{
			SetStatus(state, now, "Camera Controls only works in-game -- load a save first");
			LOG_WARN("EnterEditor refused: not in %s", kWorldName);
			return;
		}

		// Start exactly where the player is looking, so opening the editor is
		// visually a no-op and the first keyframe can be taken immediately.
		LOG_DEBUG("EnterEditor: reading the player viewpoint");

		CameraPose start;
		if (!Rig::GetPlayerViewpoint(start))
		{
			SetStatus(state, now, "Could not read the player camera");
			return;
		}

		LOG_DEBUG("EnterEditor: viewpoint %.0f,%.0f,%.0f  pitch %.1f yaw %.1f  fov %.1f",
		          start.location.x, start.location.y, start.location.z,
		          start.rotation.pitch, start.rotation.yaw, start.fov);

		if (state.options.protectPlayer)
		{
			const Vec3 stash = start.location + Vec3{ state.options.followOffsetX,
			                                          state.options.followOffsetY,
			                                          state.options.followOffsetZ };
			LOG_DEBUG("EnterEditor: engaging the safeguard (habitat=%d)",
			          state.options.spawnHabitat ? 1 : 0);

			if (!Safeguard::Engage(stash, state.options.spawnHabitat))
				LOG_WARN("EnterEditor: safeguard refused -- the player stays where they are");
		}
		else
		{
			LOG_DEBUG("EnterEditor: player protection is off");
		}

		if (!Rig::Activate(start))
		{
			Safeguard::Release();
			SetStatus(state, now, "Could not take over the camera");
			return;
		}

		state.flyPose  = start;
		state.rigActive = true;
		state.mode      = Mode::Editor;
		state.playing   = false;
		g_lastPlayhead  = state.playhead;

		Fly::ResetMomentum();

		// The game's interface is drawn for a player who is not looking through
		// this camera, so it goes away for the whole session rather than just
		// for the take.
		g_hudRequested = state.options.hideGameHud;
		if (g_hudRequested)
			Hud::Hide();

		const bool passthrough = CameraControlsConfig::Config::PassthroughInput();
		AcquireInput(passthrough);
		LOG_DEBUG("EnterEditor: input acquired (%s), token=%p",
		          passthrough ? "passthrough" : "exclusive", g_inputToken);

		UI::Editor::ResetView();
		UI::Editor::SetVisible(true);

		if (!WorldDraw::IsAvailable())
			SetStatus(state, now, "Editor ready -- in-world gizmos unavailable on this build");
		else
			SetStatus(state, now, "Editor ready");

		LOG_INFO("Entered editor mode");
	}

	// Runs one teardown step, swallowing anything it throws.
	//
	// Teardown has to be all-or-nothing in the *opposite* direction to normal
	// code: every step must be attempted even if an earlier one failed. A
	// single throw partway through used to leave the player buried underground
	// with the camera still detached, because the steps that would have put
	// them back were never reached.
	template <typename Step>
	void TryStep(const char* what, Step&& step)
	{
		try
		{
			step();
		}
		catch (const std::exception& e)
		{
			LOG_ERROR("Teardown step '%s' threw: %s -- continuing", what, e.what());
		}
		catch (...)
		{
			LOG_ERROR("Teardown step '%s' threw -- continuing", what);
		}
	}

	void ExitEditor(State& state, double now)
	{
		if (state.mode == Mode::Off)
			return;

		LOG_DEBUG("ExitEditor: tearing down from mode %s", ModeName(state.mode));

		// Clear the mode first. If everything below somehow fails, the user is
		// at least not stuck in an editor they cannot leave, and the tick stops
		// driving the camera.
		state.mode            = Mode::Off;
		state.playing         = false;
		state.rigActive       = false;
		state.countdown       = 0.0;
		state.uiHidden        = false;
		state.uiHovered       = false;
		state.textInputActive = false;
		state.gameViewValid   = false;
		state.gameViewApplied = false;

		// Restore the player before the camera: if only one of the two can be
		// salvaged, having your body back where it started matters far more
		// than a tidy view transition.
		TryStep("release player",  []             { Safeguard::Release(); });
		TryStep("restore viewport",[]             { ViewportFit::Restore(); });
		TryStep("restore camera",  []             { Rig::Deactivate(); });
		TryStep("restore HUD",     []             { g_hudRequested = false; Hud::Show(); });
		TryStep("hide editor UI",  []             { UI::Editor::SetVisible(false); });
		TryStep("release input",   []             { ReleaseInput(); });
		TryStep("release keys",    [&state]       { Input::ReleaseAllKeys(state); });
		TryStep("save options",    [&state]       { SaveOptionsToConfig(state); });

		SetStatus(state, now, "Editor closed");
		LOG_INFO("Left editor mode");
	}

	void StartPlayback(State& state, double now)
	{
		if (state.mode != Mode::Editor)
			return;

		if (state.timeline.TotalDuration() <= 0.0)
		{
			SetStatus(state, now, "Add at least two keyframes first");
			return;
		}

		state.mode      = Mode::Playback;
		state.playing   = false;                       // the countdown gates it
		state.playhead  = 0.0;
		state.countdown = state.options.countdownSeconds;
		g_lastPlayhead  = 0.0;

		UI::Editor::SetVisible(false);

		// Normally already hidden by EnterEditor; this only bites if the option
		// was turned on after the editor was opened.
		if (state.options.hideGameHud)
		{
			g_hudRequested = true;
			Hud::Hide();
		}

		// Playback always takes exclusive input, whatever the editor was using:
		// a stray keypress must not steer anything mid-take.
		ReleaseInput();
		AcquireInput(/*passthrough=*/false);

		LOG_INFO("Playback started (%.2fs timeline)", state.timeline.TotalDuration());
	}

	void StopPlayback(State& state, double now)
	{
		if (state.mode != Mode::Playback)
			return;

		// The HUD deliberately stays hidden -- we are going back to the editor,
		// not back to playing the game, and it would only be in the way again.

		state.mode      = Mode::Editor;
		state.playing   = false;
		state.countdown = 0.0;

		// Leave the camera where the take ended rather than snapping back --
		// that pose is usually the one worth carrying on from.
		CameraPose pose;
		if (state.timeline.Evaluate(state.playhead, pose))
			state.flyPose = pose;

		Fly::ResetMomentum();

		ReleaseInput();
		AcquireInput(CameraControlsConfig::Config::PassthroughInput());
		UI::Editor::SetVisible(true);

		SetStatus(state, now, "Playback stopped");
		LOG_INFO("Playback stopped");
	}

	// -----------------------------------------------------------------------
	// Requests posted by the UI / keybinds
	// -----------------------------------------------------------------------
	Keyframe MakeKeyframeFromPose(const CameraPose& pose)
	{
		Keyframe key;
		key.location = pose.location;
		key.rotation = pose.rotation;
		key.fov      = pose.fov;
		return key;
	}

	void HandleRequest(State& state, Request request, double now)
	{
		switch (request)
		{
			case Request::EnterEditor:   EnterEditor(state, now);   break;
			case Request::ExitEditor:    ExitEditor(state, now);    break;
			case Request::StartPlayback: StartPlayback(state, now); break;
			case Request::StopPlayback:  StopPlayback(state, now);  break;

			case Request::CaptureAppend:
			{
				const uint32_t id = state.timeline.Append(MakeKeyframeFromPose(state.flyPose));
				state.selectedId  = id;
				state.selection   = Selection::Keyframe;
				state.dirty       = true;

				char message[64];
				snprintf(message, sizeof(message), "Keyframe %d added", state.timeline.Count());
				SetStatus(state, now, message);
				break;
			}

			case Request::CaptureInsertAfterSelected:
			{
				const int index = state.timeline.IndexOf(state.selectedId);
				const uint32_t id = state.timeline.InsertAfter(index, MakeKeyframeFromPose(state.flyPose));
				state.selectedId  = id;
				state.selection   = Selection::Keyframe;
				state.dirty       = true;
				SetStatus(state, now, "Keyframe inserted");
				break;
			}

			case Request::UpdateSelectedFromCamera:
			{
				Keyframe* key = state.timeline.Find(state.selectedId);
				if (!key)
				{
					SetStatus(state, now, "Select a keyframe first");
					break;
				}

				key->location = state.flyPose.location;
				key->rotation = state.flyPose.rotation;
				key->fov      = state.flyPose.fov;
				state.dirty   = true;
				SetStatus(state, now, "Keyframe re-recorded");
				break;
			}

			case Request::GotoSelected:
			{
				const Keyframe* key = state.timeline.Find(state.selectedId);
				if (!key)
				{
					SetStatus(state, now, "Select a keyframe first");
					break;
				}

				state.flyPose.location = key->location;
				state.flyPose.rotation = key->lookAt
					? LookAtRotation(key->location, key->lookAtTarget)
					: key->rotation;
				state.flyPose.fov = key->fov;

				// Park the playhead on it too, so the world playhead gizmo and
				// the camera agree about where we are.
				const int index = state.timeline.IndexOf(state.selectedId);
				if (index >= 0)
					state.playhead = state.timeline.AbsoluteTime(index);

				state.playing = false;
				Fly::ResetMomentum();
				SetStatus(state, now, "Camera moved to keyframe");
				break;
			}

			case Request::SnapCameraToPlayhead:
			{
				CameraPose pose;
				if (state.timeline.Evaluate(state.playhead, pose))
				{
					state.flyPose = pose;
					Fly::ResetMomentum();
				}
				break;
			}

			case Request::LookAtSelectionFromCamera:
			{
				Keyframe* key = state.timeline.Find(state.selectedId);
				if (!key)
					break;

				// Ten metres down the camera's current aim -- close enough to
				// be a deliberate subject, far enough not to be inside a wall.
				key->lookAtTarget = state.flyPose.location +
					ForwardVector(state.flyPose.rotation) * 1000.0;
				key->lookAt = true;
				state.dirty = true;
				SetStatus(state, now, "Look-at target set from camera");
				break;
			}
		}
	}

	// -----------------------------------------------------------------------
	// Per-mode update
	// -----------------------------------------------------------------------
	bool HasFlyInput(const FlyInput& input)
	{
		return input.forward != 0 || input.right != 0 || input.up != 0 ||
		       input.rollAxis != 0 || input.fovAxis != 0 ||
		       input.yawDelta != 0.0 || input.pitchDelta != 0.0;
	}

	void TickEditor(State& state, double dt, double now)
	{
		// F9 has to actually unregister the windows from rendering, not just
		// make their callbacks return early -- an early return still leaves an
		// empty titled box sitting on screen.
		static bool s_windowsShown = true;
		const bool wantWindows = !state.uiHidden;
		if (wantWindows != s_windowsShown)
		{
			s_windowsShown = wantWindows;
			UI::Editor::SetVisible(wantWindows);
		}

		// Toggling the option in the inspector takes effect immediately, but
		// only on the edge: Hide() walks the object list, and retrying that
		// every frame because it failed once is exactly the sort of thing that
		// turns a 60 Hz tick into a stutter.
		if (state.options.hideGameHud != g_hudRequested)
		{
			g_hudRequested = state.options.hideGameHud;
			LOG_DEBUG("Tick: hide-HUD option changed to %d", g_hudRequested ? 1 : 0);

			if (g_hudRequested)
				Hud::Hide();
			else
				Hud::Show();
		}

		const double total = state.timeline.TotalDuration();

		// Preview playback advances the playhead; a scrub moves it from the UI.
		if (state.playing && total > 0.0)
		{
			state.playhead += dt * std::max(state.timeline.globalSpeed, 0.01f);
			if (state.playhead >= total)
			{
				if (state.timeline.loop)
					state.playhead = 0.0;
				else
				{
					state.playhead = total;
					state.playing  = false;
				}
			}
		}

		const bool scrubbed = std::abs(state.playhead - g_lastPlayhead) > 1e-6;
		const bool manual   = HasFlyInput(state.flyInput);

		// Touching the fly controls always wins: grabbing WASD mid-preview
		// should take the camera, not fight the playhead for it.
		if (manual)
			state.playing = false;

		const bool previewing = state.options.scrubPreview && !manual &&
		                        (state.playing || scrubbed);

		if (previewing)
		{
			CameraPose pose;
			if (state.timeline.Evaluate(state.playhead, pose))
			{
				// Mirror into flyPose so the moment the user takes over, the
				// camera carries on from what they were just looking at.
				state.flyPose = pose;
				Rig::ApplyPose(pose);
			}
		}
		else
		{
			Fly::Integrate(state.flyPose, state.flyInput, state.options, dt);
			Rig::ApplyPose(state.flyPose);
		}

		g_lastPlayhead = state.playhead;

		// Read the view back *after* applying the pose, so the on-screen
		// keyframe handles are drawn from what the camera manager actually
		// ended up with rather than from what we asked it for.
		state.renderViewValid = Rig::GetPlayerViewpoint(state.renderView);

		if (state.options.showGizmos)
		{
			WorldDraw::DrawParams params;
			params.splineSamples  = state.options.splineSamples;
			params.gizmoScale     = state.options.gizmoScale;
			params.selectedId     = state.selection == Selection::None ? 0 : state.selectedId;
			params.playhead       = state.playhead;
			params.now            = now;
			params.showPlayhead   = true;
			params.cameraLocation = state.flyPose.location;
			params.nearCull       = state.options.gizmoNearCull;
			params.showPlayerMarker = state.options.protectPlayer &&
			                          state.options.showPlayerMarker &&
			                          Safeguard::IsEngaged();
			params.playerLocation = Safeguard::StashLocation();
			WorldDraw::DrawTimeline(state.timeline, params);
		}
	}

	void TickPlayback(State& state, double dt, double now)
	{
		const double total = state.timeline.TotalDuration();

		if (state.countdown > 0.0)
		{
			state.countdown -= dt;

			// Hold on the opening pose through the countdown so the recording
			// starts on a settled frame rather than mid-move.
			CameraPose pose;
			if (state.timeline.Evaluate(0.0, pose))
				Rig::ApplyPose(pose);

			if (state.countdown <= 0.0)
			{
				state.countdown = 0.0;
				state.playing   = true;
				state.playhead  = 0.0;
			}
			return;
		}

		state.playhead += dt * std::max(state.timeline.globalSpeed, 0.01f);

		if (state.playhead >= total)
		{
			if (state.timeline.loop)
			{
				state.playhead = std::fmod(state.playhead, total);
			}
			else
			{
				state.playhead = total;
				CameraPose pose;
				if (state.timeline.Evaluate(state.playhead, pose))
					Rig::ApplyPose(pose);

				StopPlayback(state, now);
				return;
			}
		}

		CameraPose pose;
		if (state.timeline.Evaluate(state.playhead, pose))
		{
			state.flyPose = pose;
			Rig::ApplyPose(pose);
		}

		if (state.options.gizmosDuringPlayback && state.options.showGizmos)
		{
			WorldDraw::DrawParams params;
			params.splineSamples  = state.options.splineSamples;
			params.gizmoScale     = state.options.gizmoScale;
			params.selectedId     = 0;
			params.playhead       = state.playhead;
			params.now            = now;
			params.showPlayhead   = false;
			params.cameraLocation = state.flyPose.location;
			params.nearCull       = state.options.gizmoNearCull;
			WorldDraw::DrawTimeline(state.timeline, params);
		}
	}

	// -----------------------------------------------------------------------
	// Engine callbacks
	// -----------------------------------------------------------------------
	void OnEngineTick(float deltaSeconds)
	{
		// Takes the state lock briefly on its own; must not run inside the
		// lock held below.
		Input::PumpMouseLook();

		const double now = Now();
		auto lock = Lock();
		State& state = Get();

		// Drain first: a request may change the mode the rest of this tick runs in.
		if (!state.requests.empty())
		{
			std::vector<Request> pending;
			pending.swap(state.requests);
			for (Request request : pending)
			{
				LOG_DEBUG("Tick: handling request %s (mode %s)",
				          RequestName(request), ModeName(state.mode));
				HandleRequest(state, request, now);
			}
		}

		if (state.mode == Mode::Off)
			return;

		// A world teardown while the editor is open leaves every cached pointer
		// dangling; bail out rather than touching them.
		if (!g_inChimeraMain)
		{
			LOG_DEBUG("Tick: no longer in %s -- closing the editor", kWorldName);
			ExitEditor(state, now);
			return;
		}

		// The body rides with the camera so the Mass subsystem keeps streaming
		// the world in around wherever the shot actually is -- park it at base
		// and you fly out to empty terrain.
		// Survival attributes tick down whether or not the body is moving, so a
		// long session behind the camera will starve you without this.
		// The game's own slice of the window. The render thread works out where
		// the picture should sit from the panel layout; this is the only place
		// that pushes it into the engine, and the only place that puts it back.
		// Playback and a hidden UI both want the full screen, so neither goes
		// anywhere near the Apply branch.
		if (state.mode == Mode::Editor && state.options.fitViewport &&
		    state.gameViewValid && !state.uiHidden)
		{
			ViewportFit::Apply(state.gameView.x, state.gameView.y,
			                   state.gameView.w, state.gameView.h);
		}
		else
		{
			ViewportFit::Restore();
		}

		state.gameViewApplied   = ViewportFit::IsActive();
		state.gameViewSupported = ViewportFit::IsSupported();

		if (state.options.lockVitals)
			Vitals::Pin();

		if (state.options.protectPlayer)
		{
			Safeguard::Follow(state.flyPose.location,
			                  Vec3{ state.options.followOffsetX,
			                        state.options.followOffsetY,
			                        state.options.followOffsetZ },
			                  deltaSeconds);
		}

		static double s_nextHeartbeat = 0.0;
		if (now >= s_nextHeartbeat)
		{
			s_nextHeartbeat = now + 5.0;
			LOG_TRACE("Tick: mode=%s playhead=%.2f/%.2f keys=%d rig=%d safeguard=%d vitals=%d",
			          ModeName(state.mode), state.playhead, state.timeline.TotalDuration(),
			          state.timeline.Count(), Rig::IsActive() ? 1 : 0,
			          Safeguard::IsEngaged() ? 1 : 0, Vitals::IsActive() ? 1 : 0);
		}

		if (state.mode == Mode::Editor)
			TickEditor(state, deltaSeconds, now);
		else
			TickPlayback(state, deltaSeconds, now);
	}

	void OnWorldBeginPlay(SDK::UWorld* /*world*/, const char* worldName)
	{
		const bool isGameWorld = worldName && std::strcmp(worldName, kWorldName) == 0;

		if (isGameWorld)
		{
			g_inChimeraMain = true;
			LOG_INFO("%s active -- editor available", kWorldName);
			return;
		}

		// Some other world took over (a menu, a loading map). Clearing here as
		// well as on end-play means the flag cannot stay stuck on if a
		// transition skips the end-play callback for the game world.
		if (g_inChimeraMain)
		{
			LOG_INFO("World '%s' active -- editor unavailable", worldName ? worldName : "(null)");
			g_inChimeraMain = false;
		}
	}

	void OnWorldEndPlay(SDK::UWorld* /*world*/, const char* worldName)
	{
		if (!worldName || std::strcmp(worldName, kWorldName) != 0)
			return;

		g_inChimeraMain = false;

		// The world is going away underneath every actor we cached, so drop the
		// pointers without trying to tidy up through them.
		auto lock = Lock();
		State& state = Get();

		if (state.mode != Mode::Off)
		{
			state.mode      = Mode::Off;
			state.playing   = false;
			state.rigActive = false;
			state.countdown = 0.0;

			TryStep("hide editor UI", []       { UI::Editor::SetVisible(false); });
			TryStep("release input",  []       { ReleaseInput(); });
			TryStep("release keys",   [&state] { Input::ReleaseAllKeys(state); });
		}

		// Deliberately Forget rather than Release: the actors these modules
		// cached are being torn down with the world, so reaching back through
		// those pointers to tidy up would fault. There is nothing left to
		// restore the player to either -- the save reloads them.
		state.gameViewValid   = false;
		state.gameViewApplied = false;

		Rig::ForgetWorldState();
		Safeguard::ForgetWorldState();
		ViewportFit::ForgetWorldState();
		Vitals::ForgetWorldState();
		Hud::ForgetWorldState();
	}

	// Pulls the ini values into the live Options, which the UI then edits for
	// the rest of the session.
	void LoadOptionsFromConfig(State& state)
	{
		using Cfg = CameraControlsConfig::Config;

		state.options.fitViewport           = Cfg::FitViewport();
		state.options.showGizmos            = Cfg::ShowGizmos();
		state.options.gizmosDuringPlayback  = Cfg::GizmosDuringPlayback();
		state.options.hideGameHud           = Cfg::HideGameHud();
		state.options.protectPlayer         = Cfg::ProtectPlayer();
		state.options.spawnHabitat          = Cfg::SpawnHabitat();
		state.options.countdownSeconds      = Cfg::CountdownSeconds();
		state.options.mouseSensitivity      = Cfg::MouseSensitivity();
		state.options.flySpeed              = Cfg::FlySpeed();
		state.options.splineSamples         = Clamp(Cfg::SplineSamples(), 2, 64);
		state.options.gizmoScale            = Clamp(Cfg::GizmoScale(), 0.25f, 40.0f);
		state.options.gizmoNearCull         = Clamp(Cfg::GizmoNearCull(), 0.0f, 5000.0f);
		state.options.followOffsetZ         = Clamp(Cfg::FollowOffsetZ(), -20000.0f, 20000.0f);
		state.options.showPlayerMarker      = Cfg::ShowPlayerMarker();
		state.options.lockVitals            = Cfg::LockVitals();
	}

	// Writes the live Options back out, so settings tuned in the editor survive
	// the session. Called on leaving the editor rather than on every change --
	// dragging a slider must not hammer the .ini once per frame.
	void SaveOptionsToConfig(const State& state)
	{
		auto* self = GetSelf();
		if (!self || !self->config)
			return;

		auto* config = self->config;
		config->WriteBool (self, "Editor", "FitViewport",           state.options.fitViewport);
		config->WriteBool (self, "Editor", "ShowGizmos",            state.options.showGizmos);
		config->WriteBool (self, "Editor", "GizmosDuringPlayback",  state.options.gizmosDuringPlayback);
		config->WriteBool (self, "Editor", "HideGameHud",           state.options.hideGameHud);
		config->WriteInt  (self, "Editor", "SplineSamples",         state.options.splineSamples);
		config->WriteFloat(self, "Editor", "GizmoScale",            state.options.gizmoScale);
		config->WriteFloat(self, "Editor", "GizmoNearCull",         state.options.gizmoNearCull);
		config->WriteFloat(self, "Safety", "FollowOffsetZ",         state.options.followOffsetZ);
		config->WriteBool (self, "Safety", "ShowPlayerMarker",      state.options.showPlayerMarker);
		config->WriteBool (self, "Safety", "LockVitals",           state.options.lockVitals);
		config->WriteBool (self, "Safety", "ProtectPlayer",         state.options.protectPlayer);
		config->WriteBool (self, "Safety", "SpawnHabitat",          state.options.spawnHabitat);
		config->WriteFloat(self, "Camera", "CountdownSeconds",      state.options.countdownSeconds);
		config->WriteFloat(self, "Camera", "MouseSensitivity",      state.options.mouseSensitivity);
		config->WriteFloat(self, "Camera", "FlySpeed",              state.options.flySpeed);
	}
}

extern "C" {

	__declspec(dllexport) PluginInfo* GetPluginInfo()
	{
		return &s_pluginInfo;
	}

	__declspec(dllexport) bool PluginInit(IPluginSelf* self)
	{
		try
		{
			g_self = self;

			LOG_INFO("CameraControls initializing...");

			CameraControlsConfig::Config::Initialize(self);

			if (!CameraControlsConfig::Config::IsEnabled())
			{
				LOG_WARN("Plugin is disabled in the config file");
				return true; // Load, but stay inactive
			}

			ProjectIO::Initialize();

			{
				auto lock = Lock();
				LoadOptionsFromConfig(Get());
			}

			UI::Overlay::CacheKeybindNames();
			Input::Register(self);
			UI::Editor::Register(self);

			if (self->hooks && self->hooks->World)
			{
				self->hooks->World->RegisterOnAnyWorldBeginPlay(&OnWorldBeginPlay);
				self->hooks->World->RegisterOnBeforeWorldEndPlay(&OnWorldEndPlay);
			}
			else
			{
				LOG_ERROR("PluginInit: world hooks unavailable");
			}

			if (self->hooks && self->hooks->Engine)
				self->hooks->Engine->RegisterOnTick(&OnEngineTick);
			else
				LOG_ERROR("PluginInit: engine hooks unavailable -- the camera will not update");

			// Hot-reload: begin-play may already have fired, so probe the world
			// directly rather than waiting for a callback that will not come.
			try
			{
				SDK::UWorld* world = SDK::UWorld::GetWorld();
				if (world && world->GetName() == kWorldName)
				{
					g_inChimeraMain = true;
					LOG_INFO("PluginInit: %s already active", kWorldName);
				}
			}
			catch (...) {}

			LOG_INFO("CameraControls initialized");
			return true;
		}
		catch (const std::exception& e)
		{
			LOG_ERROR("PluginInit: caught exception: %s", e.what());
			return false;
		}
		catch (...)
		{
			LOG_ERROR("PluginInit: caught unknown exception");
			return false;
		}
	}

	__declspec(dllexport) void PluginShutdown()
	{
		LOG_INFO("CameraControls shutting down...");

		// ------------------------------------------------------------------
		// Detach every callback FIRST, each one isolated.
		//
		// The modloader is about to unload this DLL. Anything still registered
		// when it does is a function pointer into memory that is no longer
		// mapped -- and the render thread calling one mid-frame writes garbage
		// into the D3D12 command list, which surfaces as a device-removed
		// fatal error inside Present rather than as anything resembling a
		// plugin bug.
		//
		// This used to run *after* the world teardown, so a single throw down
		// there took the unregistration with it.
		// ------------------------------------------------------------------
		TryStep("unregister tick", []
		{
			if (g_self && g_self->hooks && g_self->hooks->Engine)
				g_self->hooks->Engine->UnregisterOnTick(&OnEngineTick);
		});

		TryStep("unregister world hooks", []
		{
			if (g_self && g_self->hooks && g_self->hooks->World)
			{
				g_self->hooks->World->UnregisterOnAnyWorldBeginPlay(&OnWorldBeginPlay);
				g_self->hooks->World->UnregisterOnBeforeWorldEndPlay(&OnWorldEndPlay);
			}
		});

		TryStep("unregister widgets", [] { UI::Editor::Unregister(g_self); });
		TryStep("unregister keybinds", [] { Input::Unregister(g_self); });

		// ------------------------------------------------------------------
		// Only now put the game back the way we found it. Nothing can call
		// into us any more, so a failure here is survivable.
		// ------------------------------------------------------------------
		TryStep("exit editor", []
		{
			auto lock = Lock();
			State& state = Get();
			if (state.mode != Mode::Off)
				ExitEditor(state, Now());
		});

		TryStep("restore viewport",    [] { ViewportFit::Restore(); });
		TryStep("release input token", [] { ReleaseInput(); });
		TryStep("reset UI state",      [] { UI::Properties::Reset(); });

		g_inChimeraMain = false;
		g_self = nullptr;

		LOG_INFO("CameraControls shut down");
	}

} // extern "C"
