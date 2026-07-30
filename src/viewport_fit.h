#pragma once

// IMPORTANT, measured in-game and not what this module was written believing:
// writing ULocalPlayer::Origin/Size moves the rectangle the engine *projects*
// into, but NOT the rectangle it *renders* into. The scene keeps filling the
// whole window. The engine reported a 1459x820 view rect inside a 1920x1080
// window while every world gizmo was still landing at its full-window position.
//
// So this module does not squeeze the picture, it only makes the engine's
// projection think the picture is smaller. What the player sees as a fitted
// viewer is `ui_editor`'s matte cropping a still-full-screen render, which means
// playback shows MORE than the preview on all four sides -- the opposite of the
// "what you frame is what you record" claim this started life with.
//
// Two consequences worth keeping straight:
//   * `Rig::ProjectToScreen` has to rescale the engine's answer out of the
//     projection rect and back into the window, or every handle sits up and to
//     the left of the gizmo it belongs to.
//   * The option's description says "crops", not "fits".


// ---------------------------------------------------------------------------
// Squeezes the game's 3D view into a sub-rectangle of the window, so the
// editor panels sit *beside* the picture rather than on top of it.
//
// This is the same machinery the engine uses for splitscreen: every
// ULocalPlayer owns a normalised Origin/Size pair describing which slice of
// the backbuffer it renders into, and the scene renderer derives the view
// rect and the aspect ratio from them. Handing player 0 a smaller slice gives
// a letterboxed viewer for free, with no render-target work of our own.
//
// Neither field is a UPROPERTY, so the generated SDK cannot see them -- they
// live inside ULocalPlayer's padding and are reached by offset. Apply() sanity
// checks what it finds there before writing anything, and gives up for the
// session if it does not look like a normalised rectangle.
//
// GAME THREAD ONLY.
// ---------------------------------------------------------------------------

namespace CameraControls::ViewportFit
{
	// Applies a normalised sub-rect (0-1, origin top-left). Cheap to call every
	// tick: it only writes when the rect has actually changed.
	// Returns false if the local player could not be reached, or if the offset
	// probe failed.
	bool Apply(float x, float y, float width, float height);

	// Puts back whatever the engine had before the first Apply(). Safe to call
	// when nothing was ever applied.
	void Restore();

	// True while the view is squeezed into a sub-rect.
	bool IsActive();

	// False once the offset probe has failed, so the UI can stop offering it.
	bool IsSupported();

	// Drops cached pointers without touching them -- the world is going away.
	void ForgetWorldState();
}
