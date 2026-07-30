#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"

#include "camera_rig.h"
#include "control_probe.h"
#include "death_guard.h"
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
#include <atomic>
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

	// The engine's game-thread id, learned from the first tick. Zero until then.
	//
	// This exists for PluginShutdown, which the modloader calls from whichever
	// thread clicked Unload -- and that is its own ImGui window, i.e. the RENDER
	// thread. Every restore step in the teardown touches a UObject, and the engine
	// asserts IsInGameThread() the moment one does, so the teardown has to know
	// where it is running before it starts.
	std::atomic<unsigned long> g_gameThreadId{ 0 };

	bool OnGameThread()
	{
		const unsigned long id = g_gameThreadId.load(std::memory_order_relaxed);
		return id != 0 && id == ::GetCurrentThreadId();
	}

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
			case Request::DumpControlProbe:           return "DumpControlProbe";
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

		// Before the token exists, so the log records what the game could still
		// see. Anything down here is a key whose release the modloader is about to
		// start swallowing -- see LogPhysicalMovementKeys.
		Input::LogPhysicalMovementKeys("input acquire");

		g_tokenIsPassthrough = passthrough;
		g_inputToken = passthrough
			? hooks->UI->AcquireInputPassthrough()
			: hooks->UI->AcquireInputCapture();

		// Recorded for the probe so every snapshot carries our side of the input
		// story. The modloader's own token count is on its debug HUD; the two
		// disagreeing is a specific bug this is here to make visible.
		Probe::NoteInputToken(g_inputToken, passthrough);

		LOG_DEBUG("Input: acquired %s token %p",
		          passthrough ? "passthrough" : "exclusive", g_inputToken);
	}

	// Idempotent, and the single most important thing in this file to get right:
	// while the modloader holds an input token on our behalf it swallows the
	// game's input and draws its own cursor, so a leaked token leaves the player
	// unable to move with no clue why and no way to recover short of restarting
	// the modloader.
	void ReleaseInput()
	{
		auto* hooks = GetHooks();
		if (!hooks || !hooks->UI || !g_inputToken)
			return;

		void* token = g_inputToken;
		const bool passthrough = g_tokenIsPassthrough;

		// Cleared before the call, not after: if the release itself throws, the
		// worst outcome is a token the modloader still holds, which is bad --
		// but retrying it forever from the watchdog below would be worse.
		g_inputToken = nullptr;
		Probe::NoteInputToken(nullptr, false);

		if (passthrough)
			hooks->UI->ReleaseInputPassthrough(token);
		else
			hooks->UI->ReleaseInputCapture(token);

		// Logged after the call, so the line existing at all is the evidence that
		// the modloader accepted the release rather than faulting inside it.
		LOG_DEBUG("Input: released %s token %p",
		          passthrough ? "passthrough" : "exclusive", token);

		// The other half of the pair. A key that was listed at acquire and is not
		// listed here never had its release delivered to the game.
		Input::LogPhysicalMovementKeys("input release");
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

		// Before anything is touched. This is the reference every later probe is
		// compared against, and it is also the one snapshot that can prove the
		// problem is *not* ours -- if control is already suppressed here, no part
		// of the teardown was ever going to explain it.
		Probe::CaptureBaseline(state);

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

		// Before the safeguard, and before the camera: the kill mechanisms have to
		// be off by the time anything starts moving. Engaging it after the body
		// has been stashed leaves a window of a frame or two in which the world's
		// out-of-bounds check is still armed and the body is already in the air.
		if (state.options.preventDeath || state.options.gameImmortality)
		{
			LOG_DEBUG("EnterEditor: engaging the death guard (immortality=%d)",
			          state.options.gameImmortality ? 1 : 0);
			DeathGuard::Engage(state.options.gameImmortality);
		}
		else
		{
			LOG_DEBUG("EnterEditor: death prevention is off");
		}

		// The entry probes matter as much as the exit ones: what entering the
		// editor changes is the exact list of things leaving it has to change back,
		// derived from the engine rather than from our own account of it. A value
		// that appears here and not in the exit diff is a restore we never wrote.
		Probe::Step("engage death guard", state);

		if (state.options.protectPlayer)
		{
			LOG_DEBUG("EnterEditor: engaging the safeguard (habitat=%d altitude=%.0f)",
			          state.options.spawnHabitat ? 1 : 0,
			          state.options.stashAltitude);

			if (!Safeguard::Engage(state.options.stashAltitude, state.options.spawnHabitat))
				LOG_WARN("EnterEditor: safeguard refused -- the player stays where they are");
		}
		else
		{
			LOG_DEBUG("EnterEditor: player protection is off");
		}

		Probe::Step("engage safeguard", state);

		if (!Rig::Activate(start))
		{
			Safeguard::Release();
			DeathGuard::Release();
			SetStatus(state, now, "Could not take over the camera");
			return;
		}

		Probe::Step("activate rig", state);

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

		Probe::Step("acquire input", state);

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

	// How long PluginShutdown will wait for the game thread to run the teardown.
	// Generous on purpose: if the engine has not ticked in this long the game is
	// wedged anyway, and the cost of waiting is a slow unload rather than a
	// corrupted world.
	constexpr double kTeardownTimeoutSeconds = 3.0;

	// Drives the editor teardown to completion from a thread that is *not* the
	// game thread, by posting to the request queue the tick already drains and
	// waiting for the mode to clear.
	//
	// Only ever called from PluginShutdown. Returns true if the editor is Off by
	// the time it returns -- either because it already was, or because the game
	// thread did the work.
	bool RequestTeardownAndWait()
	{
		{
			auto lock = Lock();
			State& state = Get();
			if (state.mode == Mode::Off)
				return true;

			LOG_INFO("PluginShutdown: asking the game thread to leave editor mode");
			Post(state, Request::ExitEditor);
		}

		// ExitEditor clears the mode as its very first act, before any of the
		// steps that can fail, so polling the mode really does mean "the teardown
		// has been reached" rather than "the teardown succeeded". That is the
		// right test here: what we are waiting for is the UObject work to have
		// happened on the correct thread, not for it to have gone well.
		const double deadline = Now() + kTeardownTimeoutSeconds;

		while (Now() < deadline)
		{
			::Sleep(2);

			auto lock = Lock();
			if (Get().mode == Mode::Off)
			{
				LOG_DEBUG("PluginShutdown: game thread completed the teardown");
				return true;
			}
		}

		return false;
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

		// Every step is followed by a probe, so the log carries what each one
		// actually changed in the engine rather than only whether it threw. That
		// distinction is the whole reason control could be broken with a teardown
		// log full of successes: a step can complete perfectly and still not have
		// changed the thing it was supposed to, and a later step can quietly undo
		// an earlier one. Both are visible here and nowhere else.
		auto restoreStep = [&state](const char* what, auto&& step)
		{
			TryStep(what, step);
			Probe::Step(what, state);
		};

		// Input first, before anything that touches a UObject. Everything below
		// is wrapped, but "wrapped" only covers C++ exceptions -- and of all the
		// things that can go wrong on the way out, the only one that leaves the
		// game completely unusable is the input token still being held.
		restoreStep("release input",   []             { ReleaseInput(); });
		restoreStep("release keys",    [&state]       { Input::ReleaseAllKeys(state); });

		// Restore the player before the camera: if only one of the two can be
		// salvaged, having your body back where it started matters far more
		// than a tidy view transition.
		restoreStep("release player",  []             { Safeguard::Release(); });

		// After the body is home, not before. Re-arming the world's out-of-bounds
		// kill while the player is still parked wherever the camera left them is
		// the one ordering here that can actually kill somebody.
		restoreStep("release death guard", []         { DeathGuard::Release(); });

		// No attributes are restored -- you keep the top-up -- but the tick stops
		// pinning them here, and the flag has to stop saying otherwise.
		restoreStep("stop vitals",     []             { Vitals::Stop(); });

		restoreStep("restore viewport",[]             { ViewportFit::Restore(); });
		restoreStep("restore camera",  []             { Rig::Deactivate(); });
		// Immediately after the view target is back on the pawn, because that is
		// what the game reacts to and what dropped the mappings in the first place.
		restoreStep("restore input configs", []
		{
			Rig::RestorePlayerInputConfigs(
				CameraControlsConfig::Config::RestoreInputConfigs());
		});

		restoreStep("restore HUD",     []             { g_hudRequested = false; Hud::Show(); });
		restoreStep("hide editor UI",  []             { UI::Editor::SetVisible(false); });

		// After the editor windows are gone, and last of everything that touches
		// the world. This hands Slate focus and mouse capture back to the game
		// viewport, and doing it while our windows are still up would only invite
		// whatever ImGui does with the cursor to take it straight back.
		//
		// The restore this plugin was missing entirely -- see
		// Rig::RestoreGameInputMode for the evidence.
		restoreStep("restore game input mode", []     { Rig::RestoreGameInputMode(); });

		restoreStep("save options",    [&state]       { SaveOptionsToConfig(state); });

		// Full end state, plus the diff against the baseline: anything still
		// different is either something we changed and did not put back, or
		// something the game changed while we had the camera.
		Probe::FinishExit(state);

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

			case Request::DumpControlProbe:
			{
				// Physical keys first, then the engine, so a single keypress
				// produces both halves of the picture in the log next to each
				// other: what the keyboard has down, and what the engine did with
				// it.
				Input::LogPhysicalMovementKeys("probe dump");
				Probe::DumpNow(state);
				SetStatus(state, now, "Control probe written to the log");
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

	// Projects every keyframe onto the screen for the UI to hang handles off.
	//
	// This lives on the game thread because the engine's projection needs the
	// player controller, and the render thread may not touch one. It replaced a
	// hand-rolled world-to-screen in ui_viewport that had to guess at UE's FOV
	// convention, aspect-ratio axis constraint and sub-rect origin -- and put the
	// handles a couple of hundred pixels off the gizmos they belong to.
	//
	// Editor mode only: playback draws no handles, and this is one UFunction call
	// per keyframe per frame.
	void UpdateKeyScreenPositions(State& state)
	{
		state.keyScreen.clear();

		if (state.mode != Mode::Editor || state.timeline.Empty())
			return;

		// The engine projects into whatever rectangle ULocalPlayer::Origin/Size
		// describe, but it goes on *rendering* into the whole window -- so the
		// engine's own answer has to be rescaled from the one to the other or every
		// handle sits up and left of its gizmo by the difference. Measured once per
		// tick rather than assumed, and 1:1 when the viewport is not squeezed.
		float scaleX = 1.0f, scaleY = 1.0f;

		float ex = 0.0f, ey = 0.0f, ew = 0.0f, eh = 0.0f;
		if (state.displayW > 0.0f && state.displayH > 0.0f &&
		    Rig::MeasureViewRect(state.renderViewValid ? state.renderView : state.flyPose,
		                         ex, ey, ew, eh) &&
		    ew > 1.0f && eh > 1.0f)
		{
			scaleX = state.displayW / ew;
			scaleY = state.displayH / eh;
		}

		const auto& keys = state.timeline.Keys();
		state.keyScreen.reserve(keys.size());

		for (const Keyframe& key : keys)
		{
			float x = 0.0f, y = 0.0f;
			if (!Rig::ProjectToScreen(key.location, scaleX, scaleY, x, y))
				continue;   // behind the camera, or the projection is not ready

			KeyScreen entry;
			entry.id       = key.id;
			entry.x        = x;
			entry.y        = y;
			entry.distance = (key.location - state.flyPose.location).Length();
			state.keyScreen.push_back(entry);
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
		// Learn which OS thread the engine considers its game thread, so
		// PluginShutdown can tell whether it is allowed to touch a UObject.
		// One relaxed store a frame, and there is nowhere better to get it:
		// the engine asserts IsInGameThread() but does not expose it to us.
		g_gameThreadId.store(::GetCurrentThreadId(), std::memory_order_relaxed);

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

		// Watchdog. Holding an input token with no mode active means the game is
		// swallowing input for a UI that is not on screen -- the player cannot
		// move, and there is nothing on screen to tell them why.
		//
		// Every teardown path releases the token itself; this exists because
		// none of them are guaranteed to *run*. A request lost to an exception
		// mid-drain, a mode cleared by something that did not know about the
		// token, a step skipped by a fault rather than a throw: all of those end
		// here instead of ending the session. Cheap, and the only recovery the
		// player has that does not involve restarting the modloader.
		if (state.mode == Mode::Off && g_inputToken)
		{
			LOG_WARN("Tick: input token %p still held with no mode active -- releasing",
			         g_inputToken);
			ReleaseInput();
			Input::ReleaseAllKeys(state);
		}

		// Runs while idle, because that is exactly when it matters: Release() has
		// already happened and the editor is shut. No-op unless the restore did
		// not take.
		Safeguard::VerifyRestore();

		// Same idea for the input mappings. The game's removal is deferred, so a
		// quick open-then-close can land our re-bind *before* the removal it is
		// meant to undo, and only re-checking afterwards catches that.
		Rig::VerifyInputRestore();

		// Every mode, every tick, silent unless the count moves. Deliberately
		// before the mode gate: the drop this is hunting has to be pinned to a
		// frame, and it could just as easily land while the plugin is idle as
		// during a session.
		Probe::WatchInputMappings(state);

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

		DeathGuard::Hold();

		if (state.options.lockVitals)
			Vitals::Pin();

		if (state.options.protectPlayer)
			Safeguard::Hold();

		static double s_nextHeartbeat = 0.0;
		if (now >= s_nextHeartbeat)
		{
			s_nextHeartbeat = now + 5.0;
			LOG_TRACE("Tick: mode=%s playhead=%.2f/%.2f keys=%d rig=%d safeguard=%d vitals=%d "
			          "guard=%d%d%d",
			          ModeName(state.mode), state.playhead, state.timeline.TotalDuration(),
			          state.timeline.Count(), Rig::IsActive() ? 1 : 0,
			          Safeguard::IsEngaged() ? 1 : 0, Vitals::IsActive() ? 1 : 0,
			          DeathGuard::BoundsSuppressed() ? 1 : 0,
			          DeathGuard::DamageBlocked() ? 1 : 0,
			          DeathGuard::GameImmortal() ? 1 : 0);

			// Alignment diagnostic, on the same 5-second beat, all three rects in
			// the same units. The engine's rect is measured from its own
			// projections; ours is what we asked viewport_fit for; the window is
			// what ImGui is drawing into. Any handle/gizmo disagreement has to
			// show up as a mismatch between these.
			if (state.mode == Mode::Editor && state.displayW > 0.0f)
			{
				float ex = 0.0f, ey = 0.0f, ew = 0.0f, eh = 0.0f;
				if (Rig::MeasureViewRect(state.renderViewValid ? state.renderView : state.flyPose,
				                         ex, ey, ew, eh))
				{
					LOG_TRACE("Tick: engine rect %.0f,%.0f %.0fx%.0f | ours %.0f,%.0f %.0fx%.0f "
					          "| window %.0fx%.0f | squeezed %d",
					          ex, ey, ew, eh,
					          state.gameView.x * state.displayW, state.gameView.y * state.displayH,
					          state.gameView.w * state.displayW, state.gameView.h * state.displayH,
					          state.displayW, state.displayH,
					          state.gameViewApplied ? 1 : 0);
				}
			}
		}

		if (state.mode == Mode::Editor)
			TickEditor(state, deltaSeconds, now);
		else
			TickPlayback(state, deltaSeconds, now);

		// Last, after the tick has moved the camera: the handles have to be
		// projected through the pose the engine will render *this* frame, not the
		// one it rendered last.
		UpdateKeyScreenPositions(state);
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

		Probe::Forget();
		Rig::ForgetWorldState();
		Safeguard::ForgetWorldState();
		DeathGuard::ForgetWorldState();
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
		state.options.preventDeath          = Cfg::PreventDeath();
		state.options.gameImmortality       = Cfg::GameImmortality();
		state.options.countdownSeconds      = Cfg::CountdownSeconds();
		state.options.mouseSensitivity      = Cfg::MouseSensitivity();
		state.options.flySpeed              = Cfg::FlySpeed();
		state.options.splineSamples         = Clamp(Cfg::SplineSamples(), 2, 64);
		state.options.gizmoScale            = Clamp(Cfg::GizmoScale(), 0.25f, 40.0f);
		state.options.gizmoNearCull         = Clamp(Cfg::GizmoNearCull(), 0.0f, 5000.0f);
		state.options.stashAltitude         = Clamp(Cfg::StashAltitude(), -200000.0f, 200000.0f);
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
		config->WriteFloat(self, "Safety", "StashAltitude",         state.options.stashAltitude);
		config->WriteBool (self, "Safety", "ShowPlayerMarker",      state.options.showPlayerMarker);
		config->WriteBool (self, "Safety", "LockVitals",           state.options.lockVitals);
		config->WriteBool (self, "Safety", "ProtectPlayer",         state.options.protectPlayer);
		config->WriteBool (self, "Safety", "SpawnHabitat",          state.options.spawnHabitat);
		config->WriteBool (self, "Safety", "PreventDeath",          state.options.preventDeath);
		config->WriteBool (self, "Safety", "GameImmortality",       state.options.gameImmortality);
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
		// Put the world back BEFORE detaching anything, and on the right thread.
		//
		// The modloader calls PluginShutdown from whichever thread clicked
		// Unload, and that is its own ImGui window -- the RENDER thread. Every
		// step of the teardown touches a UObject (the pawn, the camera actor, the
		// HUD, the local player), and the engine asserts IsInGameThread() the
		// moment one does. Doing it here would take the game down with us, which
		// is exactly what unloading mid-session used to do.
		//
		// So the work is posted to the request queue the engine tick already
		// drains, and we wait for it. That is only safe because the tick is still
		// registered at this point -- which is why this now runs *before* the
		// unregistration below rather than after it. The DLL is not unmapped
		// until we return, so a callback firing during the wait is not merely
		// harmless, it is the entire mechanism.
		// ------------------------------------------------------------------
		if (OnGameThread())
		{
			// Already legal here -- do it inline and skip the wait entirely.
			LOG_DEBUG("PluginShutdown: on the game thread, tearing down inline");
			TryStep("exit editor", []
			{
				auto lock = Lock();
				State& state = Get();
				if (state.mode != Mode::Off)
					ExitEditor(state, Now());
			});
			TryStep("restore viewport", [] { ViewportFit::Restore(); });
		}
		else if (!RequestTeardownAndWait())
		{
			// Deliberately touching nothing. A crashed game is worse than a
			// session left with the editor camera attached, and the player can
			// still recover that by reloading a save.
			LOG_ERROR("PluginShutdown: the game thread did not run the teardown within "
			          "%.0fs -- leaving the world as it is rather than touching a UObject "
			          "from this thread", kTeardownTimeoutSeconds);
		}

		// ------------------------------------------------------------------
		// Detach every callback, each one isolated.
		//
		// The modloader is about to unload this DLL. Anything still registered
		// when it does is a function pointer into memory that is no longer
		// mapped -- and the render thread calling one mid-frame writes garbage
		// into the D3D12 command list, which surfaces as a device-removed
		// fatal error inside Present rather than as anything resembling a
		// plugin bug.
		//
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
		// Whatever is left that is safe from any thread. The input token is a
		// modloader handle, not a UObject, and releasing it is the one thing that
		// absolutely must happen -- a leaked token suppresses game input until
		// the modloader itself restarts.
		// ------------------------------------------------------------------
		TryStep("release input token", [] { ReleaseInput(); });
		TryStep("reset UI state",      [] { UI::Properties::Reset(); });

		g_inChimeraMain = false;
		g_self = nullptr;

		LOG_INFO("CameraControls shut down");
	}

} // extern "C"
