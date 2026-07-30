#include "camera_rig.h"
#include "control_probe.h"
#include "plugin_config.h"
#include "plugin_helpers.h"

#include "Chimera_classes.hpp"   // UCrHeroComponent input-config binding
#include "Engine_classes.hpp"
#include "UMG_classes.hpp"       // UWidgetBlueprintLibrary::SetInputMode_GameOnly

#include <iterator>
#include <string>

namespace CameraControls::Rig
{
	namespace
	{
		SDK::ACameraActor* g_camera = nullptr;
		bool               g_active = false;

		// Survive ForgetWorldState so Deactivate can still restore the view
		// after it has dropped the camera pointer.
		SDK::AActor*  g_previousTargetAtActivate = nullptr;

		// Where the player was looking the instant the editor was opened.
		// Handing the view target back is not enough on its own: the pawn's
		// control rotation is still whatever it was left at, so without this
		// the player comes back facing a random direction.
		SDK::FRotator g_controlRotationAtActivate{};
		bool          g_haveControlRotation = false;

		// Whether the player controller was already acting as a World Partition
		// streaming source before we forced it on. See Activate().
		bool g_streamingSourceWasEnabled = false;
		bool g_touchedStreamingSource    = false;

		// --- Input restore ---------------------------------------------------
		//
		// The game's contextual input configs that an alive, on-foot player has
		// applied, in the order they are applied. Restoring these is what gives the
		// player their keys back after the editor has closed.
		//
		// Both are needed and neither is optional:
		//   OnlyMovement    -- move, look, jump, crouch, sprint
		//   BasePlayerAlive -- weapons, tools, menus, interaction
		//
		// The list is exact, and getting there took three wrong answers worth
		// recording, because each is a plausible thing to try again:
		//
		//   * Binding *every* config the component knows about is worse than the bug.
		//     Building, Zipline and Drone come back at higher priority than the base
		//     set and override unrelated keys -- `BuildConfirm/LeftMouseButton`
		//     replaced `WeaponPrimaryFire/LeftMouseButton`, so left click placed a
		//     building instead of firing; `Reload/R`, `WeaponADS/RightMouseButton`,
		//     `Dash/LeftAlt`, `OpenMapMenu/M` and `Flashlight/T` vanished; 73 mappings
		//     came back as 91; and a higher-priority context supplying its own
		//     `LookMouse/Mouse2D` changed the mouse sensitivity. A restore that puts
		//     back more than it took is not a restore.
		//
		//   * Binding only `BasePlayerAlive` restored 60 of the 73. The missing 13 were
		//     exactly Move, LookMouse, Jump, Crouch and Sprint -- locomotion, which
		//     lives in `OnlyMovement`. Found by diffing the restored live set against a
		//     known-good one; the count alone only said "still short".
		//
		//   * Re-applying the contexts recorded from
		//     `UEnhancedPlayerInput::AppliedInputContextData` would be the exact
		//     repair, and it is unreachable: handing a context back needs
		//     `IEnhancedInputSubsystemInterface::AddMappingContext`, and calling a
		//     UFunction the SDK declares on an interface class crashes (CLAUDE.md §6).
		constexpr const char* kPlayerInputConfigTags[] = {
			"InputConfig.OnlyMovement",
			"InputConfig.BasePlayerAlive",
		};

		// How many Enhanced Input action mappings the player had before the editor
		// touched anything. The target the restore has to get back to, measured
		// rather than assumed -- it differs between builds and save states, and the
		// only number that means "healthy" is the one this session started with.
		int g_mappingsAtActivate = -1;

		bool BindOneInputConfig(SDK::UCrHeroComponent* hero, const char* wanted, bool logIt);

		// Re-binds every config in kPlayerInputConfigTags. Returns false only if none
		// of them is on this pawn, which is the failure worth reporting.
		bool BindPlayerInputConfigs(SDK::UCrHeroComponent* hero, bool logIt)
		{
			int bound = 0;

			for (const char* tag : kPlayerInputConfigTags)
			{
				if (BindOneInputConfig(hero, tag, logIt))
					++bound;
			}

			if (logIt && bound != static_cast<int>(std::size(kPlayerInputConfigTags)))
			{
				LOG_ERROR("Rig: only %d of %zu player input configs could be re-bound -- some "
				          "keys will stay dead. The tags this pawn does have are logged above; "
				          "if the game has renamed them, kPlayerInputConfigTags in "
				          "camera_rig.cpp is the list to update.",
				          bound, std::size(kPlayerInputConfigTags));
			}

			return bound > 0;
		}

		// Unbinds then re-binds a single tag by name. Returns false if the pawn has no
		// contextual config with that tag.
		bool BindOneInputConfig(SDK::UCrHeroComponent* hero, const char* wanted, bool logIt)
		{
			for (int i = 0; i < hero->ContextualInputBindings.Num(); ++i)
			{
				if (!hero->ContextualInputBindings.IsValidIndex(i))
					continue;

				const SDK::FGameplayTag tag = hero->ContextualInputBindings[i].Key();
				if (tag.TagName.ToString() != wanted)
					continue;

				if (logIt)
					LOG_INFO("Rig: re-binding player input config '%s' (unbind, then bind)",
					         wanted);

				// UNBIND FIRST. This is not belt-and-braces, it is the difference
				// between working and doing nothing at all.
				//
				// `UCrHeroComponent::BindContextualMapping` (0x1476E2F50) ends with
				//
				//     if (!ContextualConfig || v41 != -1) return;
				//
				// where v41 is the tag's index in the component's own
				// `ContextualBindingHandles` map. So binding a tag that is already
				// recorded there returns immediately, having done nothing.
				//
				// That is exactly our situation: whatever strips the player's mappings
				// when the view target moves leaves the *handle* behind, so the config
				// still counts as bound while none of its keys are mapped. Binding it
				// again was a guaranteed no-op -- and it is why binding all eight tags
				// looked like it worked: the other seven were genuinely unbound and did
				// get added, which is where the extra mappings and the wrong sensitivity
				// came from, while the one that mattered was skipped in silence.
				//
				// Unbinding clears the handle so the bind can actually run. Removing
				// mapping contexts that are no longer applied is a no-op in Enhanced
				// Input, so this is safe even when the config really was intact.
				hero->UnbindContextualMapping(tag);
				hero->BindContextualMapping(tag);
				return true;
			}

			if (logIt)
			{
				LOG_WARN("Rig: input config tag '%s' is not one of this pawn's %d contextual "
				         "configs", wanted, hero->ContextualInputBindings.Num());
			}

			return false;
		}

		// About two seconds at 60fps. Generous on purpose: the removal we are racing
		// is deferred by the engine and the game may take several frames to settle.
		constexpr int kInputVerifyTicks = 120;

		// ...but only re-attempt every so often within that window. Each attempt is an
		// unbind followed by a bind, which tears the whole config down and rebuilds it;
		// doing that 60 times a second would be a lot of churn in the game's input
		// state for no extra chance of success. Every 15 ticks gives eight tries across
		// the window, and the race being caught is a matter of a frame or two.
		constexpr int kInputRetryInterval = 15;

		int  g_inputVerifyTicks = 0;
		bool g_inputVerifyLogged = false;

		SDK::UWorld* GetWorld()
		{
			try { return SDK::UWorld::GetWorld(); }
			catch (...) { return nullptr; }
		}

		SDK::APlayerController* GetLocalController(SDK::UWorld* world)
		{
			if (!world) return nullptr;
			try { return SDK::UGameplayStatics::GetPlayerController(world, 0); }
			catch (...) { return nullptr; }
		}

		SDK::FVector ToSDK(const Vec3& v)  { return SDK::FVector(v.x, v.y, v.z); }

		SDK::FRotator ToSDK(const Rot& r)
		{
			SDK::FRotator out{};
			out.Pitch = r.pitch;
			out.Yaw   = r.yaw;
			out.Roll  = r.roll;
			return out;
		}

		SDK::FTransform IdentityAt(const Vec3& location)
		{
			SDK::FTransform t{};
			t.Rotation    = SDK::FQuat{ 0.0, 0.0, 0.0, 1.0 };
			t.Translation = ToSDK(location);
			t.Scale3D     = SDK::FVector(1.0, 1.0, 1.0);
			return t;
		}

		// Drops the camera actor without touching the input-restore state.
		//
		// The distinction matters: `Deactivate` runs *before* the input restore in the
		// teardown, so if it cleared the recorded mapping count and context list there
		// would be nothing left to restore to and the watchdog would never fire.
		// `ForgetWorldState` clears both because it is for world teardown, where the
		// contexts and the pawn are going away together.
		void DropCameraPointers()
		{
			g_camera = nullptr;
			g_active = false;
		}
	}

	bool IsActive() { return g_active && g_camera != nullptr; }

	void ForgetWorldState()
	{
		DropCameraPointers();

		// Disarm the input watchdog too. On a world teardown the pawn, its hero
		// component and the mapping contexts are all going away, and re-binding
		// through them would fault.
		g_inputVerifyTicks   = 0;
		g_mappingsAtActivate = -1;
	}

	bool GetPlayerViewpoint(CameraPose& outPose)
	{
		try
		{
			SDK::UWorld* world = GetWorld();
			SDK::APlayerController* pc = GetLocalController(world);
			if (!pc)
				return false;

			SDK::FVector  location{};
			SDK::FRotator rotation{};
			pc->GetPlayerViewPoint(&location, &rotation);

			outPose.location = Vec3{ location.X, location.Y, location.Z };
			outPose.rotation = Rot{ rotation.Pitch, rotation.Yaw, rotation.Roll };
			outPose.fov      = pc->PlayerCameraManager ? pc->PlayerCameraManager->GetFOVAngle() : 90.0f;

			// A zero/absurd FOV means the camera manager was not ready; fall back
			// to something sane rather than spawning a degenerate view.
			if (outPose.fov < 5.0f || outPose.fov > 175.0f)
				outPose.fov = 90.0f;

			return true;
		}
		catch (...)
		{
			LOG_WARN("Rig: GetPlayerViewpoint threw");
			return false;
		}
	}

	bool ProjectToScreen(const Vec3& world, float scaleX, float scaleY,
	                     float& outX, float& outY)
	{
		try
		{
			SDK::APlayerController* pc = GetLocalController(GetWorld());
			if (!pc)
				return false;

			SDK::FVector2D screen{};

			// bPlayerViewportRelative = TRUE, then scaled -- and the reason is the
			// whole bug this function exists to work around.
			//
			// Writing ULocalPlayer::Origin/Size moves the rectangle the engine
			// *projects* into, and it does not move the rectangle the engine
			// *renders* into: the scene still fills the whole window, and
			// `viewport_fit`'s "squeeze" is really our own matte cropping a
			// full-screen picture. Measured, not guessed -- the engine reported a
			// 1459x820 view rect inside a 1920x1080 window while every gizmo was
			// still landing at its full-window position.
			//
			// So the absolute answer is wrong by construction: it is offset by the
			// sub-rect's origin and scaled by its size. The viewport-relative
			// answer is a clean fraction of the engine's rect, which the caller
			// rescales to the window the picture is actually in. With no squeeze
			// the scale is 1 and this is an identity.
			const SDK::FVector location(world.x, world.y, world.z);
			if (!pc->ProjectWorldLocationToScreen(location, &screen, true))
				return false;   // behind the camera, or no projection data yet

			outX = static_cast<float>(screen.X) * scaleX;
			outY = static_cast<float>(screen.Y) * scaleY;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	bool MeasureViewRect(const CameraPose& view, float& outX, float& outY,
	                     float& outW, float& outH)
	{
		try
		{
			SDK::APlayerController* pc = GetLocalController(GetWorld());
			if (!pc)
				return false;

			// A point straight down the camera's forward axis lands on the exact
			// centre of the view rectangle, so its two projections carry the whole
			// rectangle between them:
			//
			//   ProjectWorldToScreen returns  Min + normalised * Size
			//   ...and the relative form is   normalised * Size
			//
			// At the centre, normalised is (0.5, 0.5). So absolute - relative is
			// Min, and relative * 2 is Size. The engine's own rectangle, measured
			// rather than assumed -- which is the only way to tell a projection
			// that disagrees with us from a composite that ignores us both.
			const Vec3 ahead = view.location + ForwardVector(view.rotation) * 1000.0;
			const SDK::FVector location(ahead.x, ahead.y, ahead.z);

			SDK::FVector2D absolute{};
			SDK::FVector2D relative{};

			if (!pc->ProjectWorldLocationToScreen(location, &absolute, false) ||
			    !pc->ProjectWorldLocationToScreen(location, &relative, true))
				return false;

			outX = static_cast<float>(absolute.X - relative.X);
			outY = static_cast<float>(absolute.Y - relative.Y);
			outW = static_cast<float>(relative.X * 2.0);
			outH = static_cast<float>(relative.Y * 2.0);
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	bool Activate(const CameraPose& startPose)
	{
		if (IsActive())
			return true;

		try
		{
			SDK::UWorld* world = GetWorld();
			SDK::APlayerController* pc = GetLocalController(world);
			if (!world || !pc)
			{
				LOG_WARN("Rig: cannot activate -- no world or player controller");
				return false;
			}

			SDK::UClass* cameraClass = SDK::ACameraActor::StaticClass();
			if (!cameraClass)
			{
				LOG_ERROR("Rig: ACameraActor::StaticClass() returned null");
				return false;
			}

			const SDK::FTransform spawnTransform = IdentityAt(startPose.location);

			// AlwaysSpawn: the shot often starts inside geometry (a wall, a
			// machine), and a camera has no collision worth respecting anyway.
			SDK::AActor* spawned = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
				world,
				cameraClass,
				spawnTransform,
				SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
				nullptr,
				SDK::ESpawnActorScaleMethod::MultiplyWithRoot);

			if (!spawned)
			{
				LOG_ERROR("Rig: BeginDeferredActorSpawnFromClass returned null");
				return false;
			}

			SDK::UGameplayStatics::FinishSpawningActor(
				spawned, spawnTransform, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);

			g_camera = static_cast<SDK::ACameraActor*>(spawned);
			LOG_DEBUG("Rig: camera actor spawned at %p", static_cast<void*>(spawned));

			// ACameraActor ships with aspect-ratio constraint on and a 16:9
			// AspectRatio, which makes the engine letterbox the scene inside
			// the viewport and derive the vertical FOV from 1.7778 rather than
			// from the actual view rectangle. On anything that is not exactly
			// 16:9 that both crops the shot and breaks every world-to-screen
			// calculation the editor does. We want the viewport's own shape.
			if (g_camera->CameraComponent)
			{
				LOG_DEBUG("Rig: camera component fov %.1f aspect %.3f constrain %d",
				          g_camera->CameraComponent->FieldOfView,
				          g_camera->CameraComponent->AspectRatio,
				          g_camera->CameraComponent->bConstrainAspectRatio ? 1 : 0);

				g_camera->CameraComponent->bConstrainAspectRatio = 0;
			}

			// Remember what the player was looking through so ExitEditor can put
			// it back -- normally the pawn, but not necessarily (drone, vehicle).
			g_previousTargetAtActivate  = pc->GetViewTarget();
			g_controlRotationAtActivate = pc->GetControlRotation();
			g_haveControlRotation       = true;

			LOG_DEBUG("Rig: previous view target %p, control rotation pitch %.1f yaw %.1f roll %.1f",
			          static_cast<void*>(g_previousTargetAtActivate),
			          g_controlRotationAtActivate.Pitch,
			          g_controlRotationAtActivate.Yaw,
			          g_controlRotationAtActivate.Roll);

			// A World Partition world streams around a *source*, and a UE player
			// controller is one -- it reports its position from
			// GetStreamingSourceLocationAndRotation, which is GetPlayerViewPoint,
			// which follows the view target. Handing the view target to our camera
			// actor therefore makes the shot itself pull the world in, and the
			// player's body does not have to be towed along to do the streaming.
			//
			// Force it on rather than trusting the engine default, and log what it
			// was: if this reads "off", the game streams from something else
			// (the pawn's own component, most likely) and leaving the body parked
			// at home will show holes in the distance. That one log line is what
			// tells us which of the two safeguard modes this build needs.
			// Recorded before the view target moves, because moving it is what makes
			// the game strip the player's mappings. This is the number the restore has
			// to reach again.
			g_mappingsAtActivate = Probe::ActionMappingCount();
			LOG_DEBUG("Rig: %d input mapping(s) before taking the view target",
			          g_mappingsAtActivate);

			g_streamingSourceWasEnabled = pc->bEnableStreamingSource != 0;
			g_touchedStreamingSource    = true;
			pc->bEnableStreamingSource  = 1;

			LOG_DEBUG("Rig: player controller streaming source was %s -- forcing it on",
			          g_streamingSourceWasEnabled ? "on" : "off");

			ApplyPose(startPose);

			// Cut, not blend: the editor camera is seeded from the player's own
			// viewpoint, so a blend would just be a visible nudge to nowhere.
			pc->SetViewTargetWithBlend(
				g_camera, 0.0f,
				SDK::EViewTargetBlendFunction::VTBlend_Linear, 0.0f, false);

			g_active = true;
			LOG_INFO("Rig: camera actor spawned and view target acquired");
			return true;
		}
		catch (const std::exception& e)
		{
			LOG_ERROR("Rig: Activate threw: %s", e.what());
			ForgetWorldState();
			return false;
		}
		catch (...)
		{
			LOG_ERROR("Rig: Activate threw an unknown exception");
			ForgetWorldState();
			return false;
		}
	}

	void Deactivate()
	{
		if (!g_active && !g_camera)
			return;

		SDK::ACameraActor* camera = g_camera;

		// Drop our own pointers up front. Whatever happens below, the tick must
		// never come back to this actor -- and doing it at the end would be
		// skipped entirely if one of the calls faulted rather than threw (an
		// access violation is not a C++ exception).
		//
		// Camera pointers only: the input-restore state has to survive, because
		// RestorePlayerInputConfigs runs after this and needs it.
		DropCameraPointers();

		try
		{
			SDK::UWorld* world = GetWorld();
			SDK::APlayerController* pc = GetLocalController(world);

			if (pc)
			{
				if (g_touchedStreamingSource)
				{
					pc->bEnableStreamingSource = g_streamingSourceWasEnabled ? 1 : 0;
					LOG_DEBUG("Rig: streaming source restored to %s",
					          g_streamingSourceWasEnabled ? "on" : "off");
				}

				// Prefer the remembered target, but fall back to the current pawn
				// if it died or was replaced while the editor was open.
				SDK::AActor* restore = g_previousTargetAtActivate;
				if (!restore)
					restore = static_cast<SDK::AActor*>(pc->Pawn);

				LOG_DEBUG("Rig: restoring view target to %p (remembered %p, pawn %p)",
				          static_cast<void*>(restore),
				          static_cast<void*>(g_previousTargetAtActivate),
				          static_cast<void*>(pc->Pawn));

				if (restore)
				{
					pc->SetViewTargetWithBlend(
						restore, 0.0f,
						SDK::EViewTargetBlendFunction::VTBlend_Linear, 0.0f, false);

					// Put the aim back after the view target, not before -- the
					// camera manager re-reads the control rotation when the
					// target changes, so setting it first is simply discarded.
					if (g_haveControlRotation)
						pc->SetControlRotation(g_controlRotationAtActivate);
				}
				else
				{
					LOG_WARN("Rig: no view target to restore -- the player may be left "
					         "looking through nothing until they respawn");
				}
			}
		}
		catch (...)
		{
			LOG_WARN("Rig: restoring the view target threw");
		}

		// Destroy last, and only after the view target is no longer this actor.
		try
		{
			if (camera)
				camera->K2_DestroyActor();
		}
		catch (...)
		{
			LOG_WARN("Rig: destroying the camera actor threw -- leaking it");
		}

		g_previousTargetAtActivate = nullptr;
		g_haveControlRotation      = false;
		g_touchedStreamingSource   = false;
		LOG_INFO("Rig: camera actor released");
	}

	void RestoreGameInputMode()
	{
		try
		{
			SDK::APlayerController* pc = GetLocalController(GetWorld());
			if (!pc)
			{
				LOG_WARN("Rig: cannot restore the game input mode -- no player controller");
				return;
			}

			// bFlushInput = true. The flush is wanted, not incidental: see the
			// header on swallowed key releases.
			SDK::UWidgetBlueprintLibrary::SetInputMode_GameOnly(pc, /*bFlushInput=*/true);

			LOG_INFO("Rig: game input mode restored (viewport focus, mouse capture, "
			         "input flushed)");
		}
		catch (...)
		{
			LOG_WARN("Rig: restoring the game input mode threw");
		}
	}

	void RestorePlayerInputConfigs(bool repair)
	{
		try
		{
			SDK::APlayerController* pc = GetLocalController(GetWorld());
			if (!pc || !pc->Pawn)
			{
				LOG_WARN("Rig: cannot restore input configs -- no pawn");
				return;
			}

			SDK::UCrHeroComponent* hero =
				SDK::UCrHeroComponent::FindHeroComponent(static_cast<SDK::AActor*>(pc->Pawn));
			if (!hero)
			{
				LOG_WARN("Rig: no UCrHeroComponent on the pawn -- cannot restore input configs");
				return;
			}

			// Log the vocabulary first. Which tags this build even has is half the
			// diagnosis, and it is the only way to know what an unbind is unbinding.
			const int exclusiveCount  = hero->ExclusiveInputBindings.Num();
			const int contextualCount = hero->ContextualInputBindings.Num();

			LOG_TRACE("Rig: hero component %p -- %d exclusive, %d contextual input configs",
			          static_cast<void*>(hero), exclusiveCount, contextualCount);

			for (int i = 0; i < contextualCount; ++i)
			{
				if (!hero->ContextualInputBindings.IsValidIndex(i))
					continue;

				LOG_TRACE("Rig:   contextual config tag '%s'",
				          hero->ContextualInputBindings[i].Key().TagName.ToString().c_str());
			}

			for (int i = 0; i < exclusiveCount; ++i)
			{
				if (!hero->ExclusiveInputBindings.IsValidIndex(i))
					continue;

				LOG_TRACE("Rig:   exclusive config tag '%s'",
				          hero->ExclusiveInputBindings[i].Key().TagName.ToString().c_str());
			}

			// The list `InitializePlayerInput` adds, straight from the component.
			//
			// Reported by its stored asset *path*, never by resolving the soft
			// pointer. `TSoftObjectPtr::Get()` in this SDK bottoms out in
			// `GObjects->GetByIndex(ObjectIndex)` with no bounds check and no
			// serial-number validation, so calling it on an entry that was never
			// resolved indexes off the end of the object array and takes the game
			// down. It did exactly that. The asset path is plain FName data sitting
			// next to the weak pointer and is safe to read either way.
			const int defaults = hero->DefaultInputMappings.Num();
			LOG_TRACE("Rig: hero component has %d default input mapping(s)", defaults);

			for (int i = 0; i < defaults; ++i)
			{
				const auto& entry = hero->DefaultInputMappings[i];
				LOG_TRACE("Rig:   default mapping '%s' priority %d",
				          entry.InputMapping.ObjectID.AssetPath.AssetName.ToString().c_str(),
				          entry.Priority);
			}

			if (!repair)
			{
				LOG_TRACE("Rig: not re-adding player input -- RestoreInputConfigs is off");
				return;
			}

			// Re-bind the one player input config named in the ini.
			//
			// The tag is configurable rather than compiled in because it is the last
			// remaining guess in this repair: if the game renames its configs, every
			// available tag is logged right above this line and the fix is an ini edit
			// rather than a rebuild.
			if (!BindPlayerInputConfigs(hero, /*logIt=*/true))
				return;

			// Arm the watchdog. Whatever this call achieved, a removal already in
			// flight can undo it a frame from now, and only re-checking catches that.
			g_inputVerifyTicks  = kInputVerifyTicks;
			g_inputVerifyLogged = false;
		}
		catch (...)
		{
			LOG_WARN("Rig: restoring the player input configs threw");
		}
	}

	void VerifyInputRestore()
	{
		if (g_inputVerifyTicks <= 0)
			return;

		--g_inputVerifyTicks;

		// Nothing to compare against means nothing to verify. Better to do nothing
		// than to re-bind on a guess and risk duplicating mappings.
		if (g_mappingsAtActivate <= 0)
		{
			g_inputVerifyTicks = 0;
			return;
		}

		const int live = Probe::ActionMappingCount();
		if (live < 0)
			return;   // unreadable this tick; try again

		if (live >= g_mappingsAtActivate)
		{
			if (!g_inputVerifyLogged)
			{
				g_inputVerifyLogged = true;
				LOG_INFO("Rig: input mappings settled at %d (had %d before the editor) after "
				         "%d tick(s)", live, g_mappingsAtActivate,
				         kInputVerifyTicks - g_inputVerifyTicks);

				// More mappings than we started with means the re-bind added a
				// context that was still applied, so some actions are now bound
				// twice. A doubled look binding consumes the frame's mouse delta
				// twice, which is felt as the sensitivity being wrong rather than
				// seen as anything obviously broken -- so it is worth saying out
				// loud rather than leaving as a number nobody compares.
				if (live > g_mappingsAtActivate)
					LOG_WARN("Rig: input set is %d mapping(s) LARGER than before the editor "
					         "(%d vs %d) -- duplicated bindings are likely, and a doubled "
					         "look or move action applies its input twice.",
					         live - g_mappingsAtActivate, live, g_mappingsAtActivate);
			}
			g_inputVerifyTicks = 0;
			return;
		}

		// Still short. Re-bind and keep watching -- but not on every single tick.
		if (g_inputVerifyTicks % kInputRetryInterval != 0)
			return;

		try
		{
			SDK::APlayerController* pc = GetLocalController(GetWorld());
			if (!pc)
				return;

			if (!pc->Pawn)
				return;

			SDK::UCrHeroComponent* hero =
				SDK::UCrHeroComponent::FindHeroComponent(static_cast<SDK::AActor*>(pc->Pawn));
			if (!hero)
				return;

			// Silent: this runs at 60Hz and a stubborn case would bury the log. The
			// settled/gave-up lines around it are the ones worth reading.
			BindPlayerInputConfigs(hero, /*logIt=*/false);
		}
		catch (...)
		{
			// Per-tick path. Say nothing, try again.
		}

		if (g_inputVerifyTicks == 0)
			LOG_ERROR("Rig: gave up restoring the player's input mappings -- %d live, %d "
			          "expected. Movement keys are probably still dead; re-opening and "
			          "closing the editor will retry.",
			          live, g_mappingsAtActivate);
	}

	void ApplyPose(const CameraPose& pose)
	{
		if (!g_camera)
			return;

		try
		{
			SDK::FHitResult hit{};
			g_camera->K2_SetActorLocationAndRotation(
				ToSDK(pose.location), ToSDK(pose.rotation),
				/*bSweep=*/false, &hit, /*bTeleport=*/true);

			if (g_camera->CameraComponent)
				g_camera->CameraComponent->SetFieldOfView(Clamp(pose.fov, 5.0f, 170.0f));
		}
		catch (...)
		{
			// A pose apply failing usually means the actor was GC'd out from
			// under us; drop it so the next tick re-activates cleanly. Camera
			// pointers only -- the input snapshot is still the record of what the
			// player had, and losing it here would lose the restore with it.
			LOG_WARN("Rig: ApplyPose threw -- releasing camera actor");
			DropCameraPointers();
		}
	}
}
