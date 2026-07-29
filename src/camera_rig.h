#pragma once

// ---------------------------------------------------------------------------
// The camera the plugin drives.
//
// A plain ACameraActor is spawned and made the local player's view target;
// every tick its transform and its UCameraComponent's FOV are written from the
// pose the editor produced. Nothing hooks the player camera manager, so
// tearing down is just "put the old view target back and destroy the actor".
//
// GAME THREAD ONLY. Every function here touches UObjects.
// ---------------------------------------------------------------------------

#include "cc_math.h"
#include "timeline.h"

namespace CameraControls::Rig
{
	// Spawns the camera actor at `startPose` and blends the local player's view
	// onto it. Safe to call when already active (no-op). Returns false if the
	// world or local player controller could not be resolved.
	bool Activate(const CameraPose& startPose);

	// Restores the previous view target and destroys the camera actor.
	// Safe to call when inactive.
	//
	// The restore is a hard cut, not a blend, and deliberately so: a blend
	// keeps the camera manager reading the *outgoing* view target for the
	// blend's duration, and the outgoing target here is the actor we are about
	// to destroy. Entering the editor cuts too, so this is symmetrical anyway.
	void Deactivate();

	bool IsActive();

	// Writes a pose onto the camera actor. No-op while inactive.
	void ApplyPose(const CameraPose& pose);

	// The local player's current point of view, used to seed the fly camera
	// when the editor opens so it starts exactly where the player was looking.
	// Returns false if no player camera could be read.
	bool GetPlayerViewpoint(CameraPose& outPose);

	// Drops every cached pointer without touching the game -- for use when the
	// world has already been torn down under us.
	void ForgetWorldState();
}
