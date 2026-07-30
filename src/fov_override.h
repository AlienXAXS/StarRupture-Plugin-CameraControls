#pragma once

// ---------------------------------------------------------------------------
// Makes the editor's FOV actually reach the screen.
//
// THE BUG THIS EXISTS FOR: setting `UCameraComponent::FieldOfView` on our camera
// actor did nothing at all. Not "nothing much" -- the FOV slider, the R/F zoom
// keys and every keyframe's FOV during playback were all completely inert, while
// the frustum gizmo and the read-out (which come from our own numbers) moved
// perfectly. That combination is what makes it look like a UI bug.
//
// It is not. The write reaches the engine exactly as intended and the game throws
// it away one call later. The view target chain is
// `ACrPlayerCameraManager` -> a thunk -> `AAuPlayerCameraManager::UpdateViewTarget`
// (0x14690A2C0), which calls the base implementation -- that is where our camera
// component's FOV lands in `OutVT.POV.FOV` -- and then afterwards does:
//
//     14690a7d0  movss xmm0, [rbx+299Ch]   ; AdditionalFOVOffset
//     14690a7d8  addss xmm0, [rbx+2998h]   ; this->FOV
//     14690a7e0  movss [rdi+40h], xmm0     ; OutVT->POV.FOV
//
// `this->FOV` is the manager's own value, interpolated toward
// `clamp(StackedAdditiveModifiers.FOV + CurrentConfig.FOV, MinPossibleFOV,
// MaxPossibleFOV)`. The only guards on that store are "has an owning player
// controller" and "GetProjectionData succeeded", both always true in play.
//
// It stomps *only* POV.FOV. Location and rotation come through the base call
// untouched, which is precisely why flying works and zooming does not -- and why
// this presented as a broken widget rather than as a broken camera.
//
// THE FIX: write `AdditionalFOVOffset` ourselves. It is added *after* the clamp
// and *after* the interpolation, so `AdditionalFOVOffset = wanted - FOV` puts the
// exact angle on screen, the same frame, across the whole 5-170 range.
//
// GAME THREAD ONLY.
// ---------------------------------------------------------------------------

namespace CameraControls::FovOverride
{
	// Validates the offsets against the engine's own reported FOV and snapshots
	// `AdditionalFOVOffset`. Returns false if the probe failed, in which case the
	// override stays off for the rest of the session and the game keeps its FOV.
	//
	// Call once, from Rig::Activate, before the first pose is applied.
	bool Engage();

	// Puts `AdditionalFOVOffset` back to what the game had. Safe when not engaged.
	void Release();

	// Drives the rendered FOV to `fov` degrees. Per-frame, silent, no-op unless
	// engaged. Clamped to the same 5-170 as the rest of the plugin.
	void Apply(float fov);

	// False once the offset probe has failed on this build -- the FOV controls
	// cannot do anything and the UI should say so rather than offering a dead
	// slider. Distinct from IsEngaged(), which is merely "not in a session".
	bool IsSupported();
	bool IsEngaged();

	// Drops cached state without touching the game, for world teardown.
	// `IsSupported` deliberately survives: a failed probe is a property of the
	// build, not of the world that happened to be loaded at the time.
	void ForgetWorldState();
}
