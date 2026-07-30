#pragma once

// ---------------------------------------------------------------------------
// Getting the player's body out of harm's way while nobody is driving it.
//
// The moment the view target moves to our camera actor the body is left standing
// in the open, still fully simulated, and a five-minute shot is a five-minute
// free hit for anything nearby.
//
// So on entering the editor the body is snapshotted, put into stasis (movement
// mode None, no gravity, zero velocity) and buried: dropped straight down to a
// fixed altitude, keeping its X and Y, with a habitat shelter spawned around it.
// Several thousand units below the surface there is nothing to shoot it, nothing
// to walk into, and no volume to fall out of. On exit it is teleported back and
// its movement state restored exactly.
//
// It does not move again after that. It used to be towed along under the camera
// -- on the theory that a world this size streams around the *player*, so the
// body had to follow the shot or the shot would be of empty terrain. That turned
// out to be both unnecessary and actively harmful:
//
//   * Unnecessary, because a UE player controller is itself a World Partition
//     streaming source and reports its position from
//     GetStreamingSourceLocationAndRotation -> GetPlayerViewPoint, which follows
//     the view target. The view target is our camera, so the shot already pulls
//     the world in around itself. `camera_rig::Activate` forces
//     `bEnableStreamingSource` on and logs the prior value so this stays
//     verifiable rather than assumed.
//
//   * Harmful, because towing meant the body visited every radiation field, kill
//     volume and lethal drop the camera flew through -- and a kill trigger ends
//     the take on the respawn screen, which is the one failure nothing here can
//     undo.
//
// See also `death_guard`, which switches off the kill mechanisms themselves
// rather than keeping the body away from them. This module and that one are
// belt and braces for the same failure.
//
// GAME THREAD ONLY.
// ---------------------------------------------------------------------------

#include "cc_math.h"

namespace CameraControls::Safeguard
{
	// Snapshots the player's transform and movement state, puts them into stasis
	// and drops them to world Z `stashAltitude`, keeping their X and Y.
	//
	// An absolute altitude rather than a drop from where they stand: one number
	// has to produce one outcome, and the same relative drop is deep bedrock from
	// a cliff top and possibly not even below the floor from a canyon bottom.
	//
	// The habitat, if asked for, is spawned around the body once it has arrived
	// rather than around where it started -- getting that order wrong is what
	// used to make the shelter appear at the player's feet instead of under them.
	//
	// Returns false if the player could not be resolved (nothing is changed).
	bool Engage(double stashAltitude, bool spawnHabitat);

	// Maintains the stash for one frame. Call once per game-thread tick while
	// engaged. Re-asserts the stasis and, if something has managed to shove the
	// body off the stash point, puts it back.
	void Hold();

	// Restores the player's transform and movement state, and removes the
	// habitat if one was spawned. Safe to call when not engaged.
	//
	// Does not trust its own work: it reads the body's position and movement mode
	// back afterwards, and if the body is still down the hole or still frozen it
	// arms VerifyRestore() to keep trying.
	void Release();

	// One retry of a restore that did not take. Call every game-thread tick while
	// the plugin is *idle* -- this is the path that runs after the editor has
	// already closed, and it is a no-op unless Release() armed it.
	//
	// Worth the plumbing because the failure it covers is the only one the player
	// cannot work around: a body left buried in MOVE_None is indistinguishable
	// from the plugin having broken their save.
	void VerifyRestore();

	bool IsEngaged();

	// Where the body is, for the editor's debug marker.
	Vec3 StashLocation();

	// Drops cached pointers without touching the game, for when the world has
	// already gone away underneath us.
	void ForgetWorldState();
}
