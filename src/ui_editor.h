#pragma once

// ---------------------------------------------------------------------------
// The editor shell: registers the modloader widgets and lays out the timeline
// window (bottom), the property inspector (right) and the overlays.
//
// Window geometry is recomputed from the viewport every frame rather than
// hard-coded, so the layout is sane at 1080p and at ultrawide alike.
//
// RENDER THREAD.
// ---------------------------------------------------------------------------

#include "plugin_interface.h"

namespace CameraControls::UI::Editor
{
	void Register(IPluginSelf* self);
	void Unregister(IPluginSelf* self);

	// Shows or hides the editor's widget windows. Called by the game thread on
	// a mode change, and when the user toggles the UI off with F9.
	void SetVisible(bool visible);

	// Shows or hides the pre-open notice. Separate from SetVisible because it is
	// the one piece of this UI that appears while the mode is still Off -- and it
	// must not come back with the editor windows when F9 shows them again.
	void SetNoticeVisible(bool visible);

	// Re-frames the timeline view. Called when the editor is opened, not every
	// time the windows come back, so a hide/show round trip keeps the zoom.
	void ResetView();

	// The screen rectangle, in pixels, the game's 3D view occupies -- the whole
	// window unless it has been squeezed out from under the panels. Everything
	// that projects world space onto the screen has to go through this rather
	// than assuming the picture fills the window.
	struct ScreenRect
	{
		float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
	};

	// Where the game's 3D view is *actually* being drawn this frame, in screen
	// pixels. The whole window unless the viewport squeeze has taken effect --
	// never the rect we merely asked for, because anything mapping world space
	// onto the screen has to agree with the picture the player is looking at.
	ScreenRect CurrentGameRect();
}
