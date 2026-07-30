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

#include <vector>

namespace CameraControls::UI::TimelineView
{
	// Purely visual, persists across frames, not part of the saved project.
	struct ViewState
	{
		double pixelsPerSecond = 90.0;
		double scrollSeconds   = 0.0;

		// Live drag bookkeeping.
		uint32_t draggingKey    = 0;
		uint32_t draggingFunc   = 0;
		bool     draggingPlayhead = false;
		bool     pendingFit     = true;   // frame the whole timeline on first show

		// Where everything being dragged started, captured once when the drag
		// begins.
		//
		// A group drag has to be applied as "original + delta" rather than by
		// accumulating per-frame movement: a keyframe that hits a clamp against an
		// unselected neighbour would otherwise fall permanently behind the rest of
		// the group, and the shape you selected would quietly deform as you drag.
		// Replaying from the originals means a blocked item springs back into
		// formation the moment there is room again.
		struct DragOrigin
		{
			uint32_t id   = 0;
			double   time = 0.0;
		};

		std::vector<DragOrigin> dragOrigins;
		double                  dragGrabTime = 0.0;

		// What the right-click menu was opened on. Latched at the click rather
		// than re-read while the popup is up, because the cursor has moved onto
		// the menu by then and the track no longer knows what was under it.
		double   contextTime   = 0.0;
		uint32_t contextFuncId = 0;   // 0 = the click was not on a func frame
		uint32_t contextKeyId  = 0;   // 0 = the click was not on a keyframe
	};

	// Renders the ruler + track into the remaining content region of the
	// current window. `now` is the plugin clock, used for the selection pulse.
	void Render(IModLoaderImGui* ui, State& state, ViewState& view, double now);
}
