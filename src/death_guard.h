#pragma once

// ---------------------------------------------------------------------------
// Making the player unkillable while the editor is open.
//
// `player_safeguard` keeps the body still and `player_vitals` keeps its meters
// healthy, but neither can win a race it only finds out about afterwards. A
// kill trigger -- a radiation field, an out-of-bounds volume, the world's own
// KillZ plane -- takes the body from alive to dead inside a single frame, and
// writing health back to full on the next tick does not un-die it. That is the
// one failure this plugin cannot recover from: the view snaps to the respawn
// screen and the take is gone.
//
// So this module does not repair damage, it removes the mechanisms.
//
//   1. World bounds. `AWorldSettings::bEnableWorldBoundsChecks` and `::KillZ`
//      are what UE itself uses to destroy anything that falls below the map --
//      the "went too low" death. Both are switched off while the editor is
//      open and restored verbatim on the way out.
//
//   2. Damage. `AActor::bCanBeDamaged` is cleared on the pawn, which makes
//      `TakeDamage` a no-op. That covers pain volumes and anything else routed
//      through the engine's damage path.
//
//   3. Optionally, the game's own immortality. StarRupture ships a cheat
//      manager with an `Immortal` exec (`UCrCheatManager::Immortal`); asked to,
//      we let the player controller create the manager and call it. This is
//      the only layer that also covers the gameplay-ability damage path,
//      because it is the game's own code rather than our guess at it -- but it
//      means the session runs with the cheat manager instantiated, which is
//      not a side effect to inflict on someone by default. Hence opt-in.
//
// Layers 1 and 2 are engine state with an exact restore. Layer 3 is the game's
// own state and we can only ask it to toggle back, which is why they are split.
//
// Everything is snapshotted before it is written and put back on Release(), and
// `Hold()` re-asserts the per-actor flag every tick rather than trusting it to
// stay -- the game rewrites `bCanBeDamaged` itself on respawn and on some
// ability effects.
//
// GAME THREAD ONLY.
// ---------------------------------------------------------------------------

namespace CameraControls::DeathGuard
{
	// Snapshots and suppresses. Idempotent -- a second call while engaged does
	// nothing, so the snapshot can never be overwritten with our own values.
	void Engage(bool useGameImmortality);

	// Re-asserts the per-actor flags for one frame. Call once per game-thread
	// tick while engaged. Cheap: two comparisons and, normally, no writes.
	void Hold();

	// Puts everything back. Safe to call when not engaged.
	void Release();

	// Drops cached state without touching the game, for when the world has
	// already gone away underneath us.
	void ForgetWorldState();

	// What actually took effect, so the inspector can say so rather than
	// promising protection it did not manage to apply.
	bool IsEngaged();
	bool BoundsSuppressed();
	bool DamageBlocked();
	bool GameImmortal();
}
