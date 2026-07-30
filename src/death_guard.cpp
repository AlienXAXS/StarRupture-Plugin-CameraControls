#include "death_guard.h"
#include "plugin_helpers.h"

#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"

namespace CameraControls::DeathGuard
{
	namespace
	{
		// Far below anything a level could plausibly contain, and still a finite
		// float -- UE compares `Location.Z < KillZ` and nothing good comes of
		// putting an infinity on the right-hand side of that.
		constexpr float kNoKillZ = -1.0e30f;

		struct WorldSnapshot
		{
			float killZ        = 0.0f;
			bool  boundsChecks = false;
			bool  valid        = false;
		};

		struct PawnSnapshot
		{
			bool canBeDamaged = true;
			bool valid        = false;
		};

		bool          g_engaged      = false;
		bool          g_bounds       = false;   // layer 1 took effect
		bool          g_damage       = false;   // layer 2 took effect
		bool          g_immortal     = false;   // layer 3 took effect
		WorldSnapshot g_world;
		PawnSnapshot  g_pawn;

		// Hold() runs every tick, so it may only log on a state change.
		bool g_reassertedDamage = false;
		bool g_reassertedBounds = false;

		SDK::UWorld* GetWorld()
		{
			try { return SDK::UWorld::GetWorld(); }
			catch (...) { return nullptr; }
		}

		SDK::AWorldSettings* GetWorldSettings()
		{
			try
			{
				SDK::UWorld* world = GetWorld();
				return world ? world->K2_GetWorldSettings() : nullptr;
			}
			catch (...)
			{
				return nullptr;
			}
		}

		SDK::APlayerController* GetController()
		{
			try
			{
				SDK::UWorld* world = GetWorld();
				return world ? SDK::UGameplayStatics::GetPlayerController(world, 0) : nullptr;
			}
			catch (...)
			{
				return nullptr;
			}
		}

		SDK::APawn* GetPawn()
		{
			try
			{
				SDK::APlayerController* pc = GetController();
				return pc ? pc->Pawn : nullptr;
			}
			catch (...)
			{
				return nullptr;
			}
		}

		// --- Layer 1: the world's own out-of-bounds kill --------------------
		void SuppressBounds()
		{
			SDK::AWorldSettings* settings = GetWorldSettings();
			if (!settings)
			{
				LOG_WARN("DeathGuard: no world settings -- the out-of-bounds kill stays armed");
				return;
			}

			try
			{
				g_world.killZ        = settings->KillZ;
				g_world.boundsChecks = settings->bEnableWorldBoundsChecks != 0;
				g_world.valid        = true;

				settings->bEnableWorldBoundsChecks = 0;
				settings->KillZ                    = kNoKillZ;

				g_bounds = true;
				LOG_DEBUG("DeathGuard: world bounds off (was checks=%d killZ=%.0f)",
				          g_world.boundsChecks ? 1 : 0, g_world.killZ);
			}
			catch (...)
			{
				LOG_WARN("DeathGuard: suppressing the world bounds threw");
				g_bounds = false;
			}
		}

		void RestoreBounds()
		{
			if (!g_world.valid)
				return;

			SDK::AWorldSettings* settings = GetWorldSettings();
			if (!settings)
			{
				// Nothing to put it back on. Not worth a warning on the way out
				// of a world that is being torn down anyway.
				g_world = WorldSnapshot{};
				return;
			}

			try
			{
				settings->bEnableWorldBoundsChecks = g_world.boundsChecks ? 1 : 0;
				settings->KillZ                    = g_world.killZ;
				LOG_DEBUG("DeathGuard: world bounds restored (checks=%d killZ=%.0f)",
				          g_world.boundsChecks ? 1 : 0, g_world.killZ);
			}
			catch (...)
			{
				LOG_WARN("DeathGuard: restoring the world bounds threw");
			}

			g_world = WorldSnapshot{};
		}

		// --- Layer 2: the engine damage path -------------------------------
		void BlockDamage()
		{
			SDK::APawn* pawn = GetPawn();
			if (!pawn)
			{
				LOG_WARN("DeathGuard: no pawn -- damage immunity not applied");
				return;
			}

			try
			{
				g_pawn.canBeDamaged = pawn->bCanBeDamaged != 0;
				g_pawn.valid        = true;

				pawn->bCanBeDamaged = 0;

				g_damage = true;
				LOG_DEBUG("DeathGuard: pawn damage disabled (was %d)",
				          g_pawn.canBeDamaged ? 1 : 0);
			}
			catch (...)
			{
				LOG_WARN("DeathGuard: disabling pawn damage threw");
				g_damage = false;
			}
		}

		void RestoreDamage()
		{
			if (!g_pawn.valid)
				return;

			SDK::APawn* pawn = GetPawn();
			if (pawn)
			{
				try
				{
					pawn->bCanBeDamaged = g_pawn.canBeDamaged ? 1 : 0;
					LOG_DEBUG("DeathGuard: pawn damage restored to %d",
					          g_pawn.canBeDamaged ? 1 : 0);
				}
				catch (...)
				{
					LOG_WARN("DeathGuard: restoring pawn damage threw");
				}
			}

			g_pawn = PawnSnapshot{};
		}

		// --- Layer 3: the game's own immortality cheat ----------------------
		//
		// `Immortal` is an exec on the game's cheat manager, which a shipping
		// player controller does not create for itself. EnableCheats() is the
		// engine's own "make one anyway" entry point (AddCheats(bForce=true)),
		// so this needs no offsets and no hooks -- but it does leave the cheat
		// manager instantiated for the rest of the session, which is why the
		// caller has to ask for it explicitly.
		void ApplyGameImmortality()
		{
			SDK::APlayerController* pc = GetController();
			if (!pc)
			{
				LOG_WARN("DeathGuard: no player controller -- game immortality not applied");
				return;
			}

			try
			{
				if (!pc->CheatManager)
				{
					LOG_DEBUG("DeathGuard: no cheat manager -- asking the controller for one");
					pc->EnableCheats();
				}

				SDK::UCheatManager* manager = pc->CheatManager;
				if (!manager)
				{
					LOG_WARN("DeathGuard: EnableCheats produced no cheat manager -- "
					         "game immortality unavailable on this build");
					return;
				}

				// Not every build has to use the game's own subclass, and calling
				// a Chimera exec on a plain UCheatManager would look up a function
				// that is not there.
				if (!manager->IsA(SDK::UCrCheatManager::StaticClass()))
				{
					LOG_WARN("DeathGuard: cheat manager %p is not a CrCheatManager -- "
					         "game immortality unavailable",
					         static_cast<void*>(manager));
					return;
				}

				static_cast<SDK::UCrCheatManager*>(manager)->Immortal();

				g_immortal = true;
				LOG_INFO("DeathGuard: the game's Immortal cheat is on");
			}
			catch (...)
			{
				LOG_WARN("DeathGuard: applying game immortality threw");
				g_immortal = false;
			}
		}

		void ClearGameImmortality()
		{
			if (!g_immortal)
				return;

			g_immortal = false;

			SDK::APlayerController* pc = GetController();
			if (!pc || !pc->CheatManager)
				return;

			try
			{
				// `Immortal` is a toggle, so the way to turn it off is to ask for
				// it again. If a future build makes it a one-way switch this is a
				// no-op rather than a second application -- either way, calling it
				// is strictly better than leaving the player immortal for the rest
				// of their session.
				if (pc->CheatManager->IsA(SDK::UCrCheatManager::StaticClass()))
				{
					static_cast<SDK::UCrCheatManager*>(pc->CheatManager)->Immortal();
					LOG_DEBUG("DeathGuard: Immortal toggled back off");
				}
			}
			catch (...)
			{
				LOG_WARN("DeathGuard: clearing game immortality threw");
			}
		}
	}

	bool IsEngaged()        { return g_engaged; }
	bool BoundsSuppressed() { return g_bounds; }
	bool DamageBlocked()    { return g_damage; }
	bool GameImmortal()     { return g_immortal; }

	void ForgetWorldState()
	{
		g_engaged  = false;
		g_bounds   = false;
		g_damage   = false;
		g_immortal = false;
		g_world    = WorldSnapshot{};
		g_pawn     = PawnSnapshot{};
		g_reassertedDamage = false;
		g_reassertedBounds = false;
	}

	void Engage(bool useGameImmortality)
	{
		if (g_engaged)
			return;

		// Set first. Every layer below can fail independently, and a half-applied
		// guard still needs Release() to run -- the alternative is a world left
		// with its bounds checks off for the rest of the session.
		g_engaged = true;
		g_reassertedDamage = false;
		g_reassertedBounds = false;

		SuppressBounds();
		BlockDamage();

		if (useGameImmortality)
			ApplyGameImmortality();

		LOG_INFO("DeathGuard: engaged (bounds=%d damage=%d immortal=%d)",
		         g_bounds ? 1 : 0, g_damage ? 1 : 0, g_immortal ? 1 : 0);
	}

	void Hold()
	{
		if (!g_engaged)
			return;

		try
		{
			// The pawn flag is the one that drifts: the game writes it itself on
			// respawn and some ability effects set it back. Re-assert rather than
			// trust, but only write when it has actually changed.
			if (g_pawn.valid)
			{
				if (SDK::APawn* pawn = GetPawn())
				{
					if (pawn->bCanBeDamaged != 0)
					{
						pawn->bCanBeDamaged = 0;

						if (!g_reassertedDamage)
						{
							g_reassertedDamage = true;
							LOG_DEBUG("DeathGuard: something re-enabled pawn damage -- "
							          "holding it off (logged once)");
						}
					}
				}
			}

			// World settings are rewritten far less often, but a level-streaming
			// transition can swap the settings actor out from under us.
			if (g_world.valid)
			{
				if (SDK::AWorldSettings* settings = GetWorldSettings())
				{
					if (settings->bEnableWorldBoundsChecks != 0)
					{
						settings->bEnableWorldBoundsChecks = 0;
						settings->KillZ                    = kNoKillZ;

						if (!g_reassertedBounds)
						{
							g_reassertedBounds = true;
							LOG_DEBUG("DeathGuard: world bounds checks came back on -- "
							          "holding them off (logged once)");
						}
					}
				}
			}
		}
		catch (...)
		{
			// Per-frame path: never log, never tear down. Skip and try again.
		}
	}

	void Release()
	{
		// Keyed on "is there anything to put back" rather than on g_engaged, for
		// the same reason the safeguard is: if anything cleared the flag while
		// the editor was open, keying on it would leave the world with its
		// out-of-bounds kill disabled for the rest of the session.
		if (!g_engaged && !g_world.valid && !g_pawn.valid && !g_immortal)
			return;

		ClearGameImmortality();
		RestoreDamage();
		RestoreBounds();

		ForgetWorldState();
		LOG_INFO("DeathGuard: released");
	}
}
