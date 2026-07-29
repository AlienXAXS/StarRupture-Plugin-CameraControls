#pragma once

// ---------------------------------------------------------------------------
// Full-screen overlays drawn straight onto ImGui's foreground draw list:
//
//   * the keybind cheat-sheet pinned top-left while the editor is open
//   * the playback pre-roll countdown
//   * the timeline's screen fades
//   * a recording-mode border and status line
//
// These use the foreground list rather than a window so they survive with the
// editor windows hidden, and so the fade genuinely covers everything.
//
// RENDER THREAD.
// ---------------------------------------------------------------------------

#include "editor_state.h"
#include "plugin_interface.h"

namespace CameraControls::UI::Overlay
{
	// Caches the resolved keybind names for the cheat-sheet. Called once from
	// PluginInit, after the config has been initialised.
	void CacheKeybindNames();

	void Render(IModLoaderImGui* ui, State& state, double now);
}
