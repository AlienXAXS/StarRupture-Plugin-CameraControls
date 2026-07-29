#pragma once

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
