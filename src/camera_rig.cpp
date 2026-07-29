#include "camera_rig.h"
#include "plugin_helpers.h"

#include "Engine_classes.hpp"

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
	}

	bool IsActive() { return g_active && g_camera != nullptr; }

	void ForgetWorldState()
	{
		g_camera = nullptr;
		g_active = false;
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
		// never come back to this actor -- and ForgetWorldState at the end
		// would be skipped entirely if one of the calls faulted rather than
		// threw (an access violation is not a C++ exception).
		ForgetWorldState();

		try
		{
			SDK::UWorld* world = GetWorld();
			SDK::APlayerController* pc = GetLocalController(world);

			if (pc)
			{
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
		LOG_INFO("Rig: camera actor released");
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
			// under us; drop it so the next tick re-activates cleanly.
			LOG_WARN("Rig: ApplyPose threw -- releasing camera actor");
			ForgetWorldState();
		}
	}
}
