#pragma once

// ---------------------------------------------------------------------------
// Clicking keyframes in the 3D view.
//
// The debug-draw gizmos are engine line-batcher geometry -- there is nothing to
// hit-test against and no picking API to ask. But we do not need one: this
// plugin *is* the camera, so it already knows the exact view pose and FOV, and
// can project each keyframe's world position to screen space itself and test
// the mouse against that. No SDK access, no render hooks, and it works on the
// render thread where the rest of the UI lives.
//
// The projection has to match what the engine actually rendered. Since we set
// the camera actor's transform and its component's FOV every tick, and the view
// target is that actor, it does -- to well within the generous pixel radius
// used for hit-testing.
//
// RENDER THREAD.
// ---------------------------------------------------------------------------

#include "editor_state.h"
#include "plugin_interface.h"

namespace CameraControls::UI::Viewport
{
	// Draws the on-screen keyframe handles and handles clicks on them.
	// Call once per frame from a widget render callback, before the editor
	// windows so their hover test wins over the viewport.
	void Render(IModLoaderImGui* ui, State& state, double now);
}
