#include "player_safeguard.h"
#include "plugin_helpers.h"

#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"

#include <cmath>

namespace CameraControls::Safeguard
{
	namespace
	{
		// The class of the habitat spawned by the optional shelter mode. Looked
		// up by name through the object walker rather than by including the
		// generated BP header, so the whole BP_HabitatBig module does not have
		// to be compiled into the plugin for one optional feature.
		constexpr const char* kHabitatClassName = "BP_HabitatSmall_C";

		// How far below the body the habitat sits, so the body ends up standing
		// on its floor rather than buried in it.
		constexpr double kHabitatFloorOffset = 200.0;

		// Don't chase movement smaller than this. A quarter of a metre of slack
		// is invisible and saves a move call on almost every frame.
		constexpr double kStashSlack = 25.0;

		// Hold() runs every tick, so neither failures nor corrections there may
		// spam the log -- both are edge-triggered.
		bool g_holdWarned = false;
		bool g_drifted    = false;

		// How many ticks after Release() to keep checking that the body really did
		// come back. About two seconds at 60fps.
		constexpr int kRestoreRetries = 120;

		struct Snapshot
		{
			SDK::FVector  location{};
			SDK::FRotator rotation{};
			float         gravityScale = 1.0f;
			uint8_t       movementMode = 1;   // MOVE_Walking
			bool          valid        = false;
		};

		bool         g_engaged = false;
		Snapshot     g_snapshot;
		SDK::FVector g_stashLocation{};
		SDK::AActor* g_habitat = nullptr;

		// --- Restore verification --------------------------------------------
		//
		// The single worst outcome this module has is a body that stays down the
		// hole in MOVE_None: from the player's chair that is indistinguishable
		// from the plugin having broken their game, and there is no in-game way
		// back from it. So Release() does not trust its own work -- it checks,
		// and if the body is still stashed or still frozen it keeps re-applying
		// for a couple of seconds afterwards.
		//
		// Deliberately outside ForgetWorldState's remit in Release (it is armed
		// after that call), but cleared by it on world teardown, where retrying
		// against a dying world would be the wrong thing.
		Snapshot     g_pendingRestore;
		SDK::FVector g_pendingStash{};
		int          g_restoreRetries = 0;

		SDK::UWorld* GetWorld()
		{
			try { return SDK::UWorld::GetWorld(); }
			catch (...) { return nullptr; }
		}

		SDK::ACharacter* GetLocalCharacter()
		{
			try
			{
				SDK::UWorld* world = GetWorld();
				if (!world) return nullptr;

				SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
				if (!pc || !pc->Pawn) return nullptr;

				return static_cast<SDK::ACharacter*>(pc->Pawn);
			}
			catch (...)
			{
				return nullptr;
			}
		}

		// "Is the body still down the hole?" -- the test that matters, rather than
		// "is it near where it started". Where it ends up after the movement mode
		// comes back is not knowable (it falls, it settles, the floor nudges it),
		// but the stash point is a single known coordinate and being within a few
		// metres of it after a restore means the restore did not happen.
		bool NearStash(const SDK::FVector& location, const SDK::FVector& stash)
		{
			const double dx = location.X - stash.X;
			const double dy = location.Y - stash.Y;
			const double dz = location.Z - stash.Z;
			return (dx * dx + dy * dy + dz * dz) < (500.0 * 500.0);
		}

		bool StillAtStash(const SDK::FVector& location)
		{
			return NearStash(location, g_stashLocation);
		}

		// Same test, against the copy the retry pass kept -- by the time it runs,
		// ForgetWorldState has cleared the live one.
		bool StillAtPendingStash(const SDK::FVector& location)
		{
			return NearStash(location, g_pendingStash);
		}

		void EnterStasis(SDK::ACharacter* character)
		{
			if (!character || !character->CharacterMovement)
				return;

			character->CharacterMovement->GravityScale = 0.0f;
			character->CharacterMovement->SetMovementMode(SDK::EMovementMode::MOVE_None, 0);
			character->CharacterMovement->Velocity = SDK::FVector(0.0, 0.0, 0.0);
		}

		SDK::UClass* ResolveHabitatClass()
		{
			auto* hooks = GetHooks();
			if (!hooks || !hooks->ObjectWalker || !hooks->ObjectWalker->IsReady())
				return nullptr;

			// The UClass itself is a UObject in GObjects named after the
			// blueprint, so a plain name lookup finds it.
			void* found = hooks->ObjectWalker->FindFirstObjectByName(kHabitatClassName);
			LOG_DEBUG("Safeguard: habitat class '%s' resolved to %p", kHabitatClassName, found);
			return static_cast<SDK::UClass*>(found);
		}

		void SpawnHabitatAt(SDK::UWorld* world, const SDK::FVector& location)
		{
			SDK::UClass* habitatClass = ResolveHabitatClass();
			if (!habitatClass)
			{
				LOG_WARN("Safeguard: habitat class '%s' not found -- riding without a shelter",
				         kHabitatClassName);
				return;
			}

			try
			{
				SDK::FTransform transform{};
				transform.Rotation    = SDK::FQuat{ 0.0, 0.0, 0.0, 1.0 };
				transform.Translation = location;
				transform.Scale3D     = SDK::FVector(1.0, 1.0, 1.0);

				SDK::AActor* spawned = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
					world, habitatClass, transform,
					SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
					nullptr,
					SDK::ESpawnActorScaleMethod::MultiplyWithRoot);

				if (!spawned)
				{
					LOG_WARN("Safeguard: habitat spawn returned null -- riding without a shelter");
					return;
				}

				SDK::UGameplayStatics::FinishSpawningActor(
					spawned, transform, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);

				g_habitat = spawned;
				LOG_INFO("Safeguard: habitat shelter spawned");
			}
			catch (...)
			{
				LOG_WARN("Safeguard: habitat spawn threw -- riding without a shelter");
				g_habitat = nullptr;
			}
		}
	}

	bool IsEngaged() { return g_engaged; }

	Vec3 StashLocation()
	{
		return Vec3{ g_stashLocation.X, g_stashLocation.Y, g_stashLocation.Z };
	}

	void ForgetWorldState()
	{
		g_engaged        = false;
		g_habitat        = nullptr;
		g_snapshot       = Snapshot{};
		g_restoreRetries = 0;
	}

	bool Engage(double stashAltitude, bool spawnHabitat)
	{
		if (g_engaged)
			return true;

		SDK::ACharacter* character = GetLocalCharacter();
		if (!character)
		{
			LOG_WARN("Safeguard: no local character -- player will be left where they stand");
			return false;
		}

		try
		{
			g_snapshot.location = character->K2_GetActorLocation();
			g_snapshot.rotation = character->K2_GetActorRotation();

			if (character->CharacterMovement)
			{
				g_snapshot.gravityScale = character->CharacterMovement->GravityScale;
				g_snapshot.movementMode = static_cast<uint8_t>(character->CharacterMovement->MovementMode);
			}
			g_snapshot.valid = true;

			LOG_DEBUG("Safeguard: snapshot location %.0f,%.0f,%.0f  gravity %.2f  movementMode %d",
			          g_snapshot.location.X, g_snapshot.location.Y, g_snapshot.location.Z,
			          g_snapshot.gravityScale, static_cast<int>(g_snapshot.movementMode));

			// Stasis before the first move -- a walking character teleported
			// into open air immediately starts falling and spends a few frames
			// fighting the pin.
			EnterStasis(character);

			// Straight down, to a fixed altitude rather than a fixed drop. An
			// absolute Z is deliberate: a relative one would put the body a few
			// thousand units under wherever the player happened to be standing,
			// which on a cliff top is deep bedrock and at the bottom of a canyon
			// might not clear the floor at all. One number, one outcome, wherever
			// the editor is opened.
			//
			// X and Y are kept, so the body stays under the same streaming cell
			// and the same set of hazard volumes the player already chose to be in.
			g_stashLocation   = g_snapshot.location;
			g_stashLocation.Z = stashAltitude;
			g_holdWarned      = false;
			g_drifted         = false;
			g_engaged         = true;

			SDK::FHitResult hit{};
			character->K2_SetActorLocation(g_stashLocation, false, &hit, true);

			// After the move, not before: the shelter has to end up around where
			// the body arrived. Spawning it first and then teleporting away is
			// what used to leave the habitat standing at the player's old feet.
			if (spawnHabitat)
			{
				SDK::UWorld* world = GetWorld();
				if (world)
				{
					SDK::FVector habitatLocation = g_stashLocation;
					habitatLocation.Z -= kHabitatFloorOffset;
					SpawnHabitatAt(world, habitatLocation);
				}
			}

			LOG_INFO("Safeguard: stashed at %.0f,%.0f,%.0f (from %.0f,%.0f,%.0f)%s",
			         g_stashLocation.X, g_stashLocation.Y, g_stashLocation.Z,
			         g_snapshot.location.X, g_snapshot.location.Y, g_snapshot.location.Z,
			         g_habitat ? " inside a habitat" : " with no shelter");
			return true;
		}
		catch (const std::exception& e)
		{
			LOG_ERROR("Safeguard: Engage threw: %s", e.what());
			ForgetWorldState();
			return false;
		}
		catch (...)
		{
			LOG_ERROR("Safeguard: Engage threw an unknown exception");
			ForgetWorldState();
			return false;
		}
	}

	void Hold()
	{
		if (!g_engaged)
			return;

		SDK::ACharacter* character = GetLocalCharacter();
		if (!character)
			return;

		try
		{
			// Re-assert stasis every frame rather than trusting it to hold: a
			// knockback, a ragdoll or the movement component recovering on its
			// own would otherwise walk the body out of the pod.
			if (character->CharacterMovement)
			{
				character->CharacterMovement->Velocity = SDK::FVector(0.0, 0.0, 0.0);

				if (character->CharacterMovement->MovementMode != SDK::EMovementMode::MOVE_None)
					character->CharacterMovement->SetMovementMode(SDK::EMovementMode::MOVE_None, 0);
			}

			const SDK::FVector current = character->K2_GetActorLocation();

			const double dx = g_stashLocation.X - current.X;
			const double dy = g_stashLocation.Y - current.Y;
			const double dz = g_stashLocation.Z - current.Z;
			const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);

			// Nothing should be moving the body at all now that it is not being
			// towed, so drift means something else pushed it -- worth knowing
			// about once, and worth undoing every frame.
			if (distance <= kStashSlack)
				return;

			if (!g_drifted)
			{
				g_drifted = true;
				LOG_DEBUG("Safeguard: body drifted %.0f off the stash point -- "
				          "putting it back (logged once)", distance);
			}

			// SetActorLocation, not TeleportTo: TeleportTo runs encroachment
			// checks and a full teleport path, which is both slower and far more
			// likely to trip a blocking stream than a plain move.
			SDK::FHitResult hit{};
			character->K2_SetActorLocation(g_stashLocation, /*bSweep=*/false, &hit,
			                               /*bTeleport=*/true);
		}
		catch (...)
		{
			// Emphatically NOT ForgetWorldState(): the snapshot is the only
			// record of where the player came from, and throwing it away over a
			// transient per-frame failure is what stranded the body with no way
			// to put it back. Skip this frame, try the next.
			if (!g_holdWarned)
			{
				LOG_WARN("Safeguard: Hold threw -- skipping this frame (logged once)");
				g_holdWarned = true;
			}
		}
	}

	void Release()
	{
		// Keyed on "is there anything to put back", not on g_engaged. If
		// anything cleared the engaged flag while the editor was open, keying
		// on it would silently skip the restore and leave the player stranded
		// wherever the camera happened to be, frozen in stasis.
		if (!g_snapshot.valid && !g_habitat)
		{
			g_engaged = false;
			return;
		}

		SDK::ACharacter* character = GetLocalCharacter();

		// Captured before ForgetWorldState wipes them, so the retry pass below
		// still knows where the body was meant to end up.
		const Snapshot     snapshot = g_snapshot;
		const SDK::FVector stash    = g_stashLocation;
		bool               armRetry = false;

		try
		{
			if (character && g_snapshot.valid)
			{
				if (character->CharacterMovement)
				{
					character->CharacterMovement->GravityScale = g_snapshot.gravityScale;
					character->CharacterMovement->Velocity = SDK::FVector(0.0, 0.0, 0.0);
				}

				// Move back BEFORE handing movement over. Restoring MOVE_Walking
				// while the body is still parked in mid-air (or underground)
				// leaves the movement component with a floor cache for a place
				// the character is about to stop being, and it stays stuck --
				// which is why control did not come back.
				//
				// TeleportTo first, because it finds a free spot rather than
				// stuffing the capsule into whatever happens to be there. But it
				// also runs encroachment checks and is allowed to *fail*, and a
				// failure leaves the body several thousand units down in solid
				// rock. So if it did not move, force it: a body in slightly the
				// wrong place is recoverable, a body still in the hole is not.
				character->K2_TeleportTo(g_snapshot.location, g_snapshot.rotation);

				if (StillAtStash(character->K2_GetActorLocation()))
				{
					LOG_WARN("Safeguard: TeleportTo would not move the body out of the stash "
					         "-- forcing it to %.0f,%.0f,%.0f",
					         g_snapshot.location.X, g_snapshot.location.Y, g_snapshot.location.Z);

					SDK::FHitResult hit{};
					character->K2_SetActorLocation(g_snapshot.location, /*bSweep=*/false, &hit,
					                               /*bTeleport=*/true);
				}

				if (character->CharacterMovement)
				{
					// Falling rather than the snapshot's Walking: it makes the
					// engine re-run its floor check at the restored position and
					// settle into Walking itself. Restoring an exotic mode
					// (swimming, flying) verbatim is still correct.
					const auto snapshotMode = static_cast<SDK::EMovementMode>(g_snapshot.movementMode);
					const bool groundMode =
						snapshotMode == SDK::EMovementMode::MOVE_Walking ||
						snapshotMode == SDK::EMovementMode::MOVE_NavWalking ||
						snapshotMode == SDK::EMovementMode::MOVE_None;

					character->CharacterMovement->SetMovementMode(
						groundMode ? SDK::EMovementMode::MOVE_Falling : snapshotMode, 0);
				}

				// Belt and braces on the input side. Nothing here sets these,
				// but the game may while the view target is swapped out, and a
				// latched ignore-input flag is indistinguishable, from the
				// player's chair, from a stuck movement mode.
				try
				{
					SDK::UWorld* world = GetWorld();
					if (world)
					{
						if (SDK::APlayerController* pc =
						        SDK::UGameplayStatics::GetPlayerController(world, 0))
						{
							pc->ResetIgnoreInputFlags();
							character->EnableInput(pc);
						}
					}
				}
				catch (...)
				{
					LOG_WARN("Safeguard: clearing the input flags threw");
				}

				// Report what we actually ended up with, and decide whether to
				// keep working on it. If control still does not come back, this
				// line says whether the teleport and the movement mode took
				// effect at all.
				try
				{
					const SDK::FVector back = character->K2_GetActorLocation();
					const int mode = character->CharacterMovement
						? static_cast<int>(character->CharacterMovement->MovementMode) : -1;
					LOG_INFO("Safeguard: restored to %.0f,%.0f,%.0f (wanted %.0f,%.0f,%.0f), movement mode %d",
					         back.X, back.Y, back.Z,
					         g_snapshot.location.X, g_snapshot.location.Y, g_snapshot.location.Z,
					         mode);

					const bool stuckInHole = StillAtStash(back);
					const bool frozen      = mode == static_cast<int>(SDK::EMovementMode::MOVE_None);

					if (stuckInHole || frozen)
					{
						LOG_WARN("Safeguard: the body did not come back cleanly (inHole=%d frozen=%d)"
						         " -- retrying for the next %d ticks",
						         stuckInHole ? 1 : 0, frozen ? 1 : 0, kRestoreRetries);
						armRetry = true;
					}
				}
				catch (...) {}
			}
			else
			{
				LOG_WARN("Safeguard: character=%p snapshotValid=%d -- cannot restore the player",
				         static_cast<void*>(character), g_snapshot.valid ? 1 : 0);
			}
		}
		catch (...)
		{
			LOG_WARN("Safeguard: restoring the player threw");
		}

		try
		{
			if (g_habitat)
			{
				LOG_DEBUG("Safeguard: destroying the habitat at %p", static_cast<void*>(g_habitat));
				g_habitat->K2_DestroyActor();
			}
		}
		catch (...)
		{
			LOG_WARN("Safeguard: destroying the habitat threw -- leaking it");
		}

		ForgetWorldState();

		// After ForgetWorldState, not before -- it clears the retry counter, which
		// is exactly what we want on a world teardown and exactly what we do not
		// want here.
		if (armRetry)
		{
			g_pendingRestore = snapshot;
			g_pendingStash   = stash;
			g_restoreRetries = kRestoreRetries;
		}

		LOG_INFO("Safeguard: player restored");
	}

	void VerifyRestore()
	{
		if (g_restoreRetries <= 0)
			return;

		SDK::ACharacter* character = GetLocalCharacter();
		if (!character)
		{
			// No pawn to work on yet -- respawning, or mid-travel. Burn a retry
			// rather than giving up: the pawn usually comes back within a frame or
			// two, and that is precisely the case worth waiting for.
			--g_restoreRetries;
			return;
		}

		try
		{
			bool settled = true;

			if (StillAtPendingStash(character->K2_GetActorLocation()))
			{
				SDK::FHitResult hit{};
				character->K2_SetActorLocation(g_pendingRestore.location, /*bSweep=*/false, &hit,
				                               /*bTeleport=*/true);
				settled = false;
			}

			if (character->CharacterMovement)
			{
				character->CharacterMovement->GravityScale = g_pendingRestore.gravityScale;

				if (character->CharacterMovement->MovementMode == SDK::EMovementMode::MOVE_None)
				{
					character->CharacterMovement->SetMovementMode(
						SDK::EMovementMode::MOVE_Falling, 0);
					settled = false;
				}
			}

			if (settled)
			{
				LOG_INFO("Safeguard: restore settled after %d retries",
				         kRestoreRetries - g_restoreRetries);
				g_restoreRetries = 0;
				return;
			}
		}
		catch (...)
		{
			// Per-tick path. Say nothing, try again next tick.
		}

		if (--g_restoreRetries <= 0)
		{
			LOG_ERROR("Safeguard: gave up putting the body back at %.0f,%.0f,%.0f with a live "
			          "movement mode. The player is probably stuck -- reloading a save will "
			          "recover it.",
			          g_pendingRestore.location.X, g_pendingRestore.location.Y,
			          g_pendingRestore.location.Z);
		}
	}
}
