#pragma once

// ---------------------------------------------------------------------------
// Holding the player's survival attributes steady while the editor is open.
//
// The safeguard keeps the body out of harm's way, but it cannot stop the clock:
// hunger, thirst and oxygen tick down on their own, and a long composing session
// will happily starve you to death behind the camera.
//
// So while the editor (or a playback take) is running, the survival attributes
// are pinned every tick -- resources held at their maximum, hazards held at
// their minimum. Same technique as BetterCheats' attribute locks: write both
// BaseValue *and* CurrentValue, because gameplay effects re-evaluate from the
// base value and would otherwise drag the current value straight back down.
//
// Nothing is snapshotted or restored. You leave the editor topped up, which is
// the point -- you were in stasis and consuming nothing anyway.
//
// GAME THREAD ONLY.
// ---------------------------------------------------------------------------

namespace CameraControls::Vitals
{
	// Pins every survival attribute for one frame. Call once per game-thread
	// tick while the editor is active. Cheap: a dozen struct writes, no
	// allocation and no UFunction calls.
	void Pin();

	// True if the last Pin() found a character with attribute sets to write to,
	// so the UI can say whether the lock is actually doing anything.
	//
	// Only meaningful while something is still calling Pin(): the flag records
	// what the last call managed, not whether pinning is ongoing. Stop() is what
	// makes it stop claiming otherwise.
	bool IsActive();

	// Clears the active flag when the editor stops driving Pin().
	//
	// There is nothing to restore here -- pinned attributes are deliberately left
	// topped up -- but the flag is read by the inspector and by control_probe, and
	// left set it reports attributes being held steady when nothing has touched
	// them since the editor closed. A diagnostic reporting stale state is worse
	// than one reporting nothing, because it sends the reader somewhere real to
	// look for a problem that is not there.
	void Stop();

	void ForgetWorldState();
}
