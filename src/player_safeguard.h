#pragma once

// ---------------------------------------------------------------------------
// Keeping the player alive -- and useful -- while nobody is driving them.
//
// Two problems, one solution.
//
// The obvious one: the moment the view target moves to our camera actor the
// player's body is left standing in the open, still fully simulated, and a
// five-minute shot is a five-minute free hit for anything nearby.
//
// The less obvious one: StarRupture streams its world around the *player*, not
// around the view. Park the body at the base and fly the camera a kilometre
// away and the shot is of empty terrain, because the Mass subsystem never
// spawned anything out there.
//
// So the body is not parked -- it rides along. It is teleported to a fixed
// offset from the camera every tick (below it by default, so it stays out of
// frame and under the terrain when filming at ground level), held in stasis so
// it cannot fall or be pushed, and put back exactly where it started on exit.
// An optional habitat shelter rides along with it.
//
// GAME THREAD ONLY.
// ---------------------------------------------------------------------------

#include "cc_math.h"

namespace CameraControls::Safeguard
{
	// Snapshots the player's transform and movement state, puts them into
	// stasis and moves them straight to `initialStash` -- spawning the habitat
	// around that point rather than around where they were standing.
	// Call Follow() every tick afterwards to keep them with the camera.
	// Returns false if the player could not be resolved (nothing is changed).
	bool Engage(const Vec3& initialStash, bool spawnHabitat);

	// Moves the stashed player (and the habitat, if there is one) towards
	// `cameraLocation + offset`. Call once per game-thread tick while engaged.
	// The move is rate-limited using deltaSeconds so World Partition can stream
	// ahead of the body instead of blocking on a jump.
	void Follow(const Vec3& cameraLocation, const Vec3& offset, double deltaSeconds);

	// Restores the player's transform and movement state, and removes the
	// habitat if one was spawned. Safe to call when not engaged.
	void Release();

	bool IsEngaged();

	// Where the body currently is, for the editor's debug marker.
	Vec3 StashLocation();

	// Drops cached pointers without touching the game, for when the world has
	// already gone away underneath us.
	void ForgetWorldState();
}
