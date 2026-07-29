#pragma once

// ---------------------------------------------------------------------------
// The timeline track widget.
//
// Draw-list based rather than assembled from ImGui widgets, because the whole
// point is a continuous time axis with clips and a draggable playhead -- none
// of which the widget API expresses.
//
// RENDER THREAD. Reads and writes the shared state (the caller holds the lock)
// and never touches the SDK.
// ---------------------------------------------------------------------------

#include "editor_state.h"
#include "plugin_interface.h"

namespace CameraControls::UI::TimelineView
{
	// Purely visual, persists across frames, not part of the saved project.
	struct ViewState
	{
		double pixelsPerSecond = 90.0;
		double scrollSeconds   = 0.0;

		// Live drag bookkeeping.
		uint32_t draggingKey    = 0;
		bool     draggingPlayhead = false;
		bool     pendingFit     = true;   // frame the whole timeline on first show
	};

	// Renders the ruler + track into the remaining content region of the
	// current window. `now` is the plugin clock, used for the selection pulse.
	void Render(IModLoaderImGui* ui, State& state, ViewState& view, double now);
}
