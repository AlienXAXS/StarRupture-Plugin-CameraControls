#pragma once

// ---------------------------------------------------------------------------
// The property inspector -- the right-hand panel of the editor.
//
// What it shows follows the timeline selection: a keyframe, the segment
// between two keyframes, or (with nothing selected) the project itself.
//
// RENDER THREAD. Shared state only; no SDK calls.
// ---------------------------------------------------------------------------

#include "editor_state.h"
#include "plugin_interface.h"

namespace CameraControls::UI::Properties
{
	void Render(IModLoaderImGui* ui, State& state, double now);

	// Drops cached edit buffers -- call when a project is loaded so stale text
	// from the previous selection cannot be written back.
	void Reset();
}
