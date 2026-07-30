#pragma once

// ---------------------------------------------------------------------------
// The camera the plugin drives.
//
// A plain ACameraActor is spawned and made the local player's view target;
// every tick its transform is written from the pose the editor produced. Tearing
// down is mostly just "put the old view target back and destroy the actor".
//
// FOV is the exception and does not work that way: this game's camera manager
// overwrites `POV.FOV` every frame, so the pose's FOV goes through
// `fov_override` instead. See that header -- it is the reason the FOV controls
// used to do nothing at all.
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

	// Hands keyboard focus and mouse capture back to the game viewport.
	//
	// This is the restore nothing in the plugin was doing, and the reason a
	// character could come back from the editor completely inert while every
	// gameplay field read perfectly healthy. A working/broken snapshot pair came
	// back byte-identical on the engine side -- same pawn, same controller, same
	// view target, nothing ignored, movement mode Walking -- which is only
	// possible if the problem is not gameplay state at all but how the *host*
	// routes input into the engine.
	//
	// UE routes gameplay input through Slate: the game viewport widget has to hold
	// user focus and mouse capture, and `UGameViewportClient::bIgnoreInput` has to
	// be clear. All three live behind Slate and none of them is a UPROPERTY, so
	// they are invisible to the probe and untouchable directly. `FInputModeGameOnly`
	// sets all three, and `UWidgetBlueprintLibrary::SetInputMode_GameOnly` is a
	// BlueprintCallable wrapper for it -- so the fix goes through the engine's own
	// code rather than our guess at what it does, the same argument as using
	// ProjectWorldLocationToScreen instead of hand-rolling a projection.
	//
	// Input is flushed as part of it, which also clears the stale-latched-key
	// hazard: while the modloader holds an exclusive token it drops key *releases*
	// too, so a key held when the editor opened would otherwise still read as down
	// to the game afterwards.
	//
	// Safe and idempotent. Does not require the rig to be active -- it is about the
	// player's input, not our camera.
	void RestoreGameInputMode();

	// Puts the player's Enhanced Input mappings back after the view target has been
	// handed to the pawn. This is the fix for "I cannot walk after leaving the
	// editor".
	//
	// THE BUG: taking the view target off the pawn makes the game strip the pawn's
	// key mappings. The live set drops from 73 to 11 -- the 11 survivors are all
	// controller-level globals (cheats, Menu/Escape, CinematicMode/F1, TextChat,
	// Skip) while everything owned by the pawn goes -- and it never recovers.
	// `ACrCharacterPlayerBase::BecomeViewTarget` is overridden (0x1474EF930) to
	// broadcast `OnPlayerStateReady`, so the game does watch view-target changes on
	// the player pawn, and `EndViewTarget` fires when we take it. Enhanced Input
	// defers its rebuild a frame, which is the one-tick gap between `Rig::Activate`
	// and the drop appearing in the log. Handing the view target back re-broadcasts
	// and the mappings still do not return: the binding is guarded to run once.
	//
	// THE FIX: unbind and re-bind the player's contextual input configs
	// (`kPlayerInputConfigTags` in the .cpp -- see there for why that list is exactly
	// what it is, and for the three wrong answers on the way to it). The unbind is
	// mandatory, not defensive: `BindContextualMapping` returns immediately if the
	// tag is still in the component's `ContextualBindingHandles`, and the strip
	// leaves the handle behind.
	//
	// `repair` comes from `[Editor] RestoreInputConfigs`, default on -- it is the fix
	// for the bug, so shipping it disabled would ship the bug. With it off this still
	// logs the tags the pawn has, which is what would identify a rename.
	void RestorePlayerInputConfigs(bool repair);

	// Re-asserts the input restore for a bounded number of idle ticks.
	//
	// A single re-bind at teardown races the thing it is repairing. The game's
	// removal is asynchronous -- the drop shows up a tick *after* `Rig::Activate`
	// -- so opening and closing the editor quickly puts the re-bind in front of a
	// removal that has not landed yet. We restore 73 mappings, then the pending
	// removal fires and takes them straight back out, and the log looks like a
	// success. That is exactly the "closed it too fast and my keys are gone" case.
	//
	// So the restore is not trusted: this keeps checking, and re-binds again if the
	// mapping count is still below what it was before the editor opened. Same
	// pattern and the same reasoning as `Safeguard::VerifyRestore`, for the same
	// reason -- the failures that matter happen after everything reported success.
	//
	// Call from the tick's idle path. No-op unless armed and unhappy.
	void VerifyInputRestore();

	// Writes a pose onto the camera actor. No-op while inactive.
	void ApplyPose(const CameraPose& pose);

	// Whether the FOV the editor asks for is reaching the screen.
	//
	// Setting the camera component's FOV is not enough on this game: the player
	// camera manager overwrites `POV.FOV` with its own value every frame, so the
	// FOV slider, the zoom keys and keyframe FOV were all completely inert while
	// the gizmos and read-outs -- which come from our own numbers -- moved fine.
	// `fov_override` is the repair; this is false when its offset probe declined
	// to arm, so the UI can say the FOV controls are dead instead of leaving the
	// user to work it out from a slider that moves nothing.
	bool FovIsLive();

	// The local player's current point of view, used to seed the fly camera
	// when the editor opens so it starts exactly where the player was looking.
	// Returns false if no player camera could be read.
	bool GetPlayerViewpoint(CameraPose& outPose);

	// World -> screen, done by the engine rather than by us.
	//
	// This is deliberately not our own maths. Reproducing UE's projection means
	// getting four separate things right -- whether FOVAngle is the horizontal or
	// the vertical angle, which axis the aspect constraint maintains, the origin
	// of the player's sub-rect, and where the principal point lands inside it --
	// and being wrong about any one puts the handles somewhere other than the
	// gizmo they belong to.
	//
	// `scaleX`/`scaleY` convert from the engine's view rectangle to the window the
	// picture is really in: pass `displayW / engineRectW` and the vertical
	// equivalent, from MeasureViewRect. They are 1 unless the viewport has been
	// squeezed, and they are needed because writing ULocalPlayer::Origin/Size
	// moves the engine's *projection* rectangle without moving what it *renders* --
	// see the implementation comment.
	//
	// Returns false when the point is behind the camera or the projection data is
	// not ready. Output is in window pixels, the space the ImGui draw lists use.
	bool ProjectToScreen(const Vec3& world, float scaleX, float scaleY,
	                     float& outX, float& outY);

	// Diagnostic: the rectangle, in viewport pixels, that the engine is actually
	// projecting into.
	//
	// Recovered from two projections of one point rather than from any offset or
	// assumption -- see the implementation. Compare it against the rect we asked
	// `viewport_fit` for and against the ImGui display size, and the three of them
	// between them say whether a handle/gizmo disagreement is a projection we got
	// wrong or a composite that honours neither of us.
	//
	// Returns false if the probe point could not be projected.
	bool MeasureViewRect(const CameraPose& view, float& outX, float& outY,
	                     float& outW, float& outH);

	// Drops every cached pointer without touching the game -- for use when the
	// world has already been torn down under us.
	void ForgetWorldState();
}
