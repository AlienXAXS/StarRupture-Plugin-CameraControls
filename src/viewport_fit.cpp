#include "viewport_fit.h"
#include "plugin_helpers.h"

#include "Engine_classes.hpp"

#include <cmath>
#include <cstdint>

namespace CameraControls::ViewportFit
{
	namespace
	{
		// ULocalPlayer::Origin and ::Size -- two FVector2Ds, which are pairs of
		// doubles in UE5. Neither is a UPROPERTY, so they land inside the
		// generated class's padding: the dump has ViewportClient at 0x78,
		// Pad_80[0x38], then AspectRatioAxisConstraint at 0xB8. The engine's
		// member order across that gap is Origin, Size, LastViewLocation, and
		// 0x10 + 0x10 + 0x18 is exactly 0x38 -- the arithmetic only closes here.
		constexpr int kOriginOffset = 0x80;
		constexpr int kSizeOffset   = 0x90;

		struct Rect
		{
			double x = 0.0, y = 0.0, w = 1.0, h = 1.0;
		};

		bool g_supported = true;
		bool g_active    = false;
		bool g_haveSaved = false;

		Rect g_saved;   // what the engine had before the first write

		SDK::ULocalPlayer* GetLocalPlayer()
		{
			try
			{
				SDK::UWorld* world = SDK::UWorld::GetWorld();
				if (!world)
					return nullptr;

				// The player controller's Player *is* the local player on a
				// client. Going through the game instance's array as well
				// covers the frame or two around a controller swap.
				if (SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0))
				{
					if (pc->Player && pc->Player->IsA(SDK::ULocalPlayer::StaticClass()))
						return static_cast<SDK::ULocalPlayer*>(pc->Player);
				}

				if (world->OwningGameInstance && world->OwningGameInstance->LocalPlayers.Num() > 0)
					return world->OwningGameInstance->LocalPlayers[0];
			}
			catch (...)
			{
			}

			return nullptr;
		}

		double* Field(SDK::ULocalPlayer* player, int offset)
		{
			return reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(player) + offset);
		}

		Rect Read(SDK::ULocalPlayer* player)
		{
			const double* origin = Field(player, kOriginOffset);
			const double* size   = Field(player, kSizeOffset);

			Rect rect;
			rect.x = origin[0];
			rect.y = origin[1];
			rect.w = size[0];
			rect.h = size[1];
			return rect;
		}

		void Write(SDK::ULocalPlayer* player, const Rect& rect)
		{
			double* origin = Field(player, kOriginOffset);
			double* size   = Field(player, kSizeOffset);

			origin[0] = rect.x;
			origin[1] = rect.y;
			size[0]   = rect.w;
			size[1]   = rect.h;
		}

		// The one guard against the offsets being wrong on some future build.
		// A normalised viewport rect is a very specific shape; anything else
		// there is either a pointer reinterpreted as a double or a different
		// member entirely, and writing over it would be a memory stomp.
		bool Plausible(const Rect& rect)
		{
			auto inUnit = [](double v, double slack) { return v >= -slack && v <= 1.0 + slack; };

			return std::isfinite(rect.x) && std::isfinite(rect.y) &&
			       std::isfinite(rect.w) && std::isfinite(rect.h) &&
			       inUnit(rect.x, 0.001) && inUnit(rect.y, 0.001) &&
			       rect.w > 0.001 && rect.h > 0.001 &&
			       inUnit(rect.w, 0.001) && inUnit(rect.h, 0.001) &&
			       rect.x + rect.w <= 1.002 && rect.y + rect.h <= 1.002;
		}

		bool Same(const Rect& a, const Rect& b)
		{
			constexpr double kEpsilon = 0.0005;   // well under a pixel at 4K
			return std::fabs(a.x - b.x) < kEpsilon && std::fabs(a.y - b.y) < kEpsilon &&
			       std::fabs(a.w - b.w) < kEpsilon && std::fabs(a.h - b.h) < kEpsilon;
		}

		// Reads the current rect and remembers it as the resting state the
		// first time through. Returns false if it does not look like one.
		bool CaptureBaseline(SDK::ULocalPlayer* player)
		{
			if (g_haveSaved)
				return true;

			const Rect current = Read(player);
			if (!Plausible(current))
			{
				g_supported = false;
				LOG_ERROR("ViewportFit: ULocalPlayer+0x%X does not look like a viewport rect "
				          "(%.3f,%.3f %.3fx%.3f) -- viewport fitting disabled for this session",
				          kOriginOffset, current.x, current.y, current.w, current.h);
				return false;
			}

			g_saved = current;
			g_haveSaved = true;
			LOG_DEBUG("ViewportFit: baseline rect %.3f,%.3f %.3fx%.3f",
			          g_saved.x, g_saved.y, g_saved.w, g_saved.h);
			return true;
		}
	}

	bool IsActive()    { return g_active; }
	bool IsSupported() { return g_supported; }

	void ForgetWorldState()
	{
		g_active    = false;
		g_haveSaved = false;
		g_saved     = Rect{};
		// g_supported deliberately survives: a failed probe is a property of
		// the build, not of the world that happened to be loaded at the time.
	}

	bool Apply(float x, float y, float width, float height)
	{
		if (!g_supported)
			return false;

		SDK::ULocalPlayer* player = GetLocalPlayer();
		if (!player)
			return false;

		try
		{
			if (!CaptureBaseline(player))
				return false;

			Rect wanted;
			wanted.x = x;
			wanted.y = y;
			wanted.w = width;
			wanted.h = height;

			if (!Plausible(wanted))
				return false;

			// Compared against what is actually in memory rather than against
			// what we last wrote: the engine rewrites these itself whenever it
			// re-lays-out the players (a window resize does it), and reading
			// live means that silently repairs itself on the next tick.
			const Rect current = Read(player);
			if (Same(current, wanted))
			{
				g_active = !Same(wanted, g_saved);
				return true;
			}

			Write(player, wanted);

			const bool wasActive = g_active;
			g_active = !Same(wanted, g_saved);

			if (g_active != wasActive)
			{
				LOG_DEBUG("ViewportFit: game view %s (%.3f,%.3f %.3fx%.3f)",
				          g_active ? "squeezed" : "full", wanted.x, wanted.y, wanted.w, wanted.h);
			}

			return true;
		}
		catch (...)
		{
			LOG_WARN("ViewportFit: Apply threw -- leaving the viewport alone");
			return false;
		}
	}

	void Restore()
	{
		if (!g_haveSaved)
		{
			g_active = false;
			return;
		}

		SDK::ULocalPlayer* player = GetLocalPlayer();
		if (!player)
		{
			// Nothing to write through. The world is either gone or between
			// controllers; either way the engine will lay the players out
			// again from scratch.
			g_active = false;
			return;
		}

		try
		{
			if (!Same(Read(player), g_saved))
			{
				Write(player, g_saved);
				LOG_DEBUG("ViewportFit: game view restored to %.3f,%.3f %.3fx%.3f",
				          g_saved.x, g_saved.y, g_saved.w, g_saved.h);
			}
		}
		catch (...)
		{
			LOG_WARN("ViewportFit: Restore threw -- the game view may stay letterboxed "
			         "until the window is resized");
		}

		g_active = false;
	}
}
