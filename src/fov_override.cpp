#include "fov_override.h"
#include "cc_math.h"
#include "plugin_helpers.h"

#include "AuCamera_classes.hpp"   // AAuPlayerCameraManager::StaticClass
#include "Engine_classes.hpp"

#include <cmath>
#include <cstdint>

namespace CameraControls::FovOverride
{
	namespace
	{
		// `AAuPlayerCameraManager::FOV` and `::AdditionalFOVOffset`. Neither is a
		// UPROPERTY, so the generated class knows the whole run of them only as
		// `Pad_28F8[0x118]` between `DefaultCameraSettings` (0x28A8) and
		// `CurrentConfig` (0x2A10).
		//
		// The offsets are measured from `UpdateViewTarget`, not guessed, and they
		// corroborate each other: that one function touches the manager's live
		// mirror of `FAuCameraConfig` in the struct's own field order --
		//
		//     0x295C YOffset        0x2968 SocketOffset (FVector)
		//     0x2960 ZOffset        0x2980 TargetOffset (FVector)
		//     0x2964 TargetArmLenght
		//     0x2998 FOV            0x299C AdditionalFOVOffset
		//
		// -- and the same function reads `CurrentConfig.InterpolationSpeed` at
		// 0x2A54, which is 0x2A10 + 0x44, matching the offset the generated header
		// gives `CurrentConfig` and the layout it gives `FAuCameraConfig`. The
		// arithmetic only closes if the run starts where we think it does.
		constexpr int kFovOffset           = 0x2998;
		constexpr int kAdditionalFovOffset = 0x299C;

		// A camera angle, loosely. Only wide enough to catch "this is not a float
		// that could be an FOV at all" -- a pointer, an FName, a count.
		constexpr float kMinPlausibleFov = 1.0f;
		constexpr float kMaxPlausibleFov = 179.0f;

		bool  g_supported = true;
		bool  g_engaged   = false;
		bool  g_haveSaved = false;
		float g_saved     = 0.0f;

		// Edge-triggered divergence report. Apply() runs every frame, so this may
		// only log on a change.
		bool  g_diverged   = false;
		float g_lastWanted = 0.0f;

		// The local player's camera manager, but only if it is the class these
		// offsets belong to.
		//
		// The type check is not a nicety. On any other manager class 0x2998 is a
		// different member -- or past the end of the object -- and writing it would
		// be a memory stomp with no symptom until something unrelated breaks.
		SDK::AAuPlayerCameraManager* GetManager()
		{
			try
			{
				SDK::UWorld* world = SDK::UWorld::GetWorld();
				if (!world)
					return nullptr;

				SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
				if (!pc || !pc->PlayerCameraManager)
					return nullptr;

				SDK::UClass* expected = SDK::AAuPlayerCameraManager::StaticClass();
				if (!expected || !pc->PlayerCameraManager->IsA(expected))
					return nullptr;

				return static_cast<SDK::AAuPlayerCameraManager*>(pc->PlayerCameraManager);
			}
			catch (...)
			{
				return nullptr;
			}
		}

		float Read(SDK::AAuPlayerCameraManager* manager, int offset)
		{
			return *reinterpret_cast<const float*>(
				reinterpret_cast<const uint8_t*>(manager) + offset);
		}

		void Write(SDK::AAuPlayerCameraManager* manager, int offset, float value)
		{
			*reinterpret_cast<float*>(
				reinterpret_cast<uint8_t*>(manager) + offset) = value;
		}

		bool PlausibleAngle(float fov)
		{
			return std::isfinite(fov) && fov >= kMinPlausibleFov && fov <= kMaxPlausibleFov;
		}

		// The one guard against the offsets being wrong on some future build, and a
		// strong one: `GetFOVAngle()` returns the manager's *cached* POV FOV, which
		// is the value `UpdateViewTarget` wrote out of these two fields. So
		// `FOV + AdditionalFOVOffset == GetFOVAngle()` is the engine confirming that
		// both offsets point where we think they do, using its own arithmetic.
		//
		// A mismatch has one other cause worth knowing before assuming a layout
		// change: `GetFOVAngle` returns `LockedFOV` instead whenever that is
		// positive, i.e. if anything has called `SetFOV`. Nothing in this build
		// reads `LockedFOV` for rendering -- it is at +0x2C4 and `GetFOVAngle` is
		// its only reader -- so `SetFOV` would change the number the engine
		// *reports* without moving the picture. That makes it useless as a fix and
		// a false negative here, in the safe direction.
		bool Validate(float fov, float offset, float reported)
		{
			if (!PlausibleAngle(fov) || !PlausibleAngle(reported))
				return false;

			if (!std::isfinite(offset) || std::fabs(offset) > 180.0f)
				return false;

			return std::fabs((fov + offset) - reported) < 0.05f;
		}
	}

	bool IsSupported() { return g_supported; }
	bool IsEngaged()   { return g_engaged; }

	void ForgetWorldState()
	{
		g_engaged   = false;
		g_haveSaved = false;
		g_saved     = 0.0f;
		g_diverged  = false;
	}

	bool Engage()
	{
		if (g_engaged)
			return true;

		if (!g_supported)
			return false;

		SDK::AAuPlayerCameraManager* manager = GetManager();
		if (!manager)
		{
			LOG_WARN("FovOverride: the local player's camera manager is not an "
			         "AAuPlayerCameraManager -- FOV control is unavailable this session");
			return false;
		}

		try
		{
			const float fov      = Read(manager, kFovOffset);
			const float offset   = Read(manager, kAdditionalFovOffset);
			const float reported = manager->GetFOVAngle();

			if (!Validate(fov, offset, reported))
			{
				// Session, not world: if the layout moved, it moved for this build.
				g_supported = false;
				LOG_ERROR("FovOverride: AAuPlayerCameraManager+0x%X does not add up -- FOV "
				          "%.3f + offset %.3f is %.3f, but the engine reports %.3f. FOV "
				          "control is disabled for this session rather than writing over "
				          "whatever is really there.",
				          kFovOffset, fov, offset, fov + offset, reported);
				return false;
			}

			g_saved      = offset;
			g_haveSaved  = true;
			g_engaged    = true;
			g_diverged   = false;
			g_lastWanted = reported;

			LOG_DEBUG("FovOverride: armed -- manager FOV %.2f, AdditionalFOVOffset %.2f, "
			          "engine reports %.2f", fov, offset, reported);
			return true;
		}
		catch (...)
		{
			LOG_WARN("FovOverride: Engage threw -- leaving the game's FOV alone");
			return false;
		}
	}

	void Apply(float fov)
	{
		if (!g_engaged)
			return;

		SDK::AAuPlayerCameraManager* manager = GetManager();
		if (!manager)
			return;

		try
		{
			const float wanted = Clamp(fov, 5.0f, 170.0f);
			const float live   = Read(manager, kFovOffset);

			// Re-checked every frame rather than trusted from Engage: the manager
			// interpolates this field, and reading it live is what makes the offset
			// exact instead of a one-off correction that drifts.
			if (!PlausibleAngle(live))
				return;

			Write(manager, kAdditionalFovOffset, wanted - live);

			// Did it take? `GetFOVAngle` is last composite's answer, so it lags a
			// frame -- which is why this only judges a *settled* FOV. A value the
			// user stopped dragging that the engine still is not showing means the
			// override has stopped working, and that is worth one line in the log
			// rather than a silently dead slider all over again.
			const bool settled = std::fabs(wanted - g_lastWanted) < 0.01f;
			g_lastWanted = wanted;

			if (!settled)
				return;

			const bool diverged = std::fabs(manager->GetFOVAngle() - wanted) > 1.0f;
			if (diverged == g_diverged)
				return;

			g_diverged = diverged;
			if (diverged)
			{
				LOG_WARN("FovOverride: asked for %.1f degrees and the engine is rendering "
				         "%.1f -- something else is writing POV.FOV after us",
				         wanted, manager->GetFOVAngle());
			}
			else
			{
				LOG_DEBUG("FovOverride: the engine is honouring our FOV again (%.1f)", wanted);
			}
		}
		catch (...)
		{
			// Per-frame path. Say nothing, try again next tick.
		}
	}

	void Release()
	{
		if (!g_haveSaved)
		{
			g_engaged = false;
			return;
		}

		SDK::AAuPlayerCameraManager* manager = GetManager();
		if (manager)
		{
			try
			{
				Write(manager, kAdditionalFovOffset, g_saved);
				LOG_DEBUG("FovOverride: AdditionalFOVOffset restored to %.2f", g_saved);
			}
			catch (...)
			{
				LOG_WARN("FovOverride: restoring AdditionalFOVOffset threw -- the game's FOV "
				         "may stay where the editor left it until the camera config changes");
			}
		}

		g_engaged   = false;
		g_haveSaved = false;
		g_saved     = 0.0f;
		g_diverged  = false;
	}
}
