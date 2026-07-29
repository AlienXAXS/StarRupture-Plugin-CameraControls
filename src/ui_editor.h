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

	ScreenRect CurrentGameRect();
}
