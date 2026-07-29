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
		constexpr double kHabitatFloorOffset = 600.0;

		// Don't chase movement smaller than this. A quarter of a metre of slack
		// is invisible and saves a move call on almost every frame.
		constexpr double kFollowSlack = 25.0;

		// The body is a World Partition streaming source, so moving it in one
		// jump makes the engine block on loading the cells it landed in -- which
		// is the loading screen you get when the camera flies any distance.
		// Below the snap threshold it is moved continuously instead, capped at
		// this speed, and streaming keeps up without ever stalling.
		constexpr double kFollowSpeed    = 4000.0;   // uu/s
		constexpr double kSnapDistance   = 6000.0;   // beyond this, accept one hitch

		// Where the body is heading. Kept separate from its actual position so
		// the smoothing has something stable to aim at.
		SDK::FVector g_followTarget{};

		// Follow() runs every tick, so neither failures nor mode changes there
		// may spam the log -- both are edge-triggered.
		bool g_followWarned = false;
		bool g_wasSnapping  = false;

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
		g_engaged  = false;
		g_habitat  = nullptr;
		g_snapshot = Snapshot{};
	}

	bool Engage(const Vec3& initialStash, bool spawnHabitat)
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

			// Move to the pod position straight away rather than waiting for the
			// first Follow(). Spawning the habitat around the player's original
			// spot and only then sliding both away is what made the shelter
			// appear at the player rather than under the floor.
			g_stashLocation = SDK::FVector(initialStash.x, initialStash.y, initialStash.z);
			g_followTarget  = g_stashLocation;
			g_followWarned  = false;
			g_wasSnapping   = false;
			g_engaged       = true;

			SDK::FHitResult hit{};
			character->K2_SetActorLocation(g_stashLocation, false, &hit, true);

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
			         g_habitat ? " inside a habitat" : "");
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

	void Follow(const Vec3& cameraLocation, const Vec3& offset, double deltaSeconds)
	{
		if (!g_engaged)
			return;

		SDK::ACharacter* character = GetLocalCharacter();
		if (!character)
			return;

		try
		{
			const Vec3 want = cameraLocation + offset;
			g_followTarget = SDK::FVector(want.x, want.y, want.z);

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

			double dx = g_followTarget.X - current.X;
			double dy = g_followTarget.Y - current.Y;
			double dz = g_followTarget.Z - current.Z;
			const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);

			if (distance <= kFollowSlack)
				return;

			SDK::FVector next = g_followTarget;

			// State-change only. This runs every tick, so anything logged
			// unconditionally here would bury the rest of the log.
			const bool snapping = distance >= kSnapDistance;
			if (snapping != g_wasSnapping)
			{
				g_wasSnapping = snapping;
				LOG_DEBUG("Safeguard: follow switched to %s (distance %.0f)",
				          snapping ? "snap" : "smooth", distance);
			}

			if (distance < kSnapDistance)
			{
				// Step towards the target at a capped speed. Continuous motion
				// lets World Partition stream in ahead of the body instead of
				// blocking on a jump.
				const double step = kFollowSpeed * Clamp(deltaSeconds, 0.0, 0.1);
				if (step < distance)
				{
					const double scale = step / distance;
					next.X = current.X + dx * scale;
					next.Y = current.Y + dy * scale;
					next.Z = current.Z + dz * scale;
				}
			}
			// else: a deliberate jump (the camera was flown to a keyframe far
			// away). One hitch is better than the body trailing for ten seconds
			// with nothing streamed in around the shot.

			// SetActorLocation, not TeleportTo: TeleportTo runs encroachment
			// checks and a full teleport path, which is both slower and far more
			// likely to trip a blocking stream than a plain move.
			SDK::FHitResult hit{};
			character->K2_SetActorLocation(next, /*bSweep=*/false, &hit, /*bTeleport=*/true);

			// The marker tracks where the body actually is, not where it is
			// heading -- otherwise it runs ahead during a long catch-up.
			g_stashLocation = next;

			// The shelter rides along, otherwise it is a box the body left
			// behind on the first camera move.
			if (g_habitat)
			{
				SDK::FVector habitatLocation = next;
				habitatLocation.Z -= kHabitatFloorOffset;

				SDK::FHitResult habitatHit{};
				g_habitat->K2_SetActorLocation(habitatLocation, false, &habitatHit, true);
			}
		}
		catch (...)
		{
			// Emphatically NOT ForgetWorldState(): the snapshot is the only
			// record of where the player came from, and throwing it away over a
			// transient per-frame failure is what stranded the body at the
			// camera with no way to put it back. Skip this frame, try the next.
			if (!g_followWarned)
			{
				LOG_WARN("Safeguard: Follow threw -- skipping this frame (logged once)");
				g_followWarned = true;
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
				character->K2_TeleportTo(g_snapshot.location, g_snapshot.rotation);

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

				// Report what we actually ended up with. If control still does
				// not come back, this line says whether the teleport and the
				// movement mode took effect at all.
				try
				{
					const SDK::FVector back = character->K2_GetActorLocation();
					const int mode = character->CharacterMovement
						? static_cast<int>(character->CharacterMovement->MovementMode) : -1;
					LOG_INFO("Safeguard: restored to %.0f,%.0f,%.0f (wanted %.0f,%.0f,%.0f), movement mode %d",
					         back.X, back.Y, back.Z,
					         g_snapshot.location.X, g_snapshot.location.Y, g_snapshot.location.Z,
					         mode);
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
		LOG_INFO("Safeguard: player restored");
	}
}
