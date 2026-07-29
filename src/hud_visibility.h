#pragma once

// ---------------------------------------------------------------------------
// Hiding the game's own HUD for a clean shot.
//
// StarRupture's HUD is CommonUI, not a canvas AHUD: toggling AHUD::bShowHUD or
// the ShowHUD console command does nothing visible because everything the
// player sees lives in UMG widgets under a UPrimaryGameLayout. So the layout
// widget itself is found by class name and collapsed, then restored to
// whatever visibility it had before.
//
// GAME THREAD ONLY.
// ---------------------------------------------------------------------------

namespace CameraControls::Hud
{
	// Collapses the game's UI root. No-op if already hidden or if the layout
	// could not be found. Returns true if the HUD is hidden after the call.
	bool Hide();

	// Restores the visibility recorded by Hide().
	void Show();

	bool IsHidden();

	void ForgetWorldState();
}
