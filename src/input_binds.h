#pragma once

// ---------------------------------------------------------------------------
// Keybind registration and mouse-look.
//
// Keybind callbacks fire from the modloader's WndProc hook, which runs
// regardless of whether ImGui is capturing input -- that is what lets the fly
// camera keep working with the editor windows on screen. They only ever touch
// the shared state, never the SDK.
//
// PumpMouseLook() is the exception: it reads and re-centres the OS cursor and
// must be called from the game-thread tick.
// ---------------------------------------------------------------------------

#include "editor_state.h"
#include "plugin_interface.h"

namespace CameraControls::Input
{
	void Register(IPluginSelf* self);
	void Unregister(IPluginSelf* self);

	// Reads the mouse while the look button is held, converts it into the
	// yaw/pitch deltas the fly camera consumes, and re-centres the cursor so it
	// cannot wander off the window mid-shot.
	//
	// Game thread only, and it takes the state lock itself -- so it must be
	// called *outside* any lock the caller is already holding. The mutex is not
	// recursive and locking it twice on one thread is undefined behaviour.
	void PumpMouseLook();

	// Clears every held-key axis. Call when leaving editor mode so a key that
	// was down at the time does not stay latched.
	//
	// Takes the already-locked State rather than locking itself, because every
	// caller is a teardown path that is already holding the lock.
	void ReleaseAllKeys(State& state);

	// Logs which movement keys are *physically* down right now, at TRACE.
	//
	// This is the one piece of the input picture no snapshot of the engine can
	// reach. While the modloader holds an exclusive token it drops every key
	// message, releases included -- so a key that was already down when the token
	// was taken never gets its release delivered to UE, and the game is left
	// believing it is still held after we hand input back. The player experiences
	// a character that walks off on its own or will not stop, which is easy to
	// report as "I cannot control my character".
	//
	// UE's own latched key state is not readable from here, so instead both edges
	// of token ownership record what the keyboard looked like. A key listed at
	// acquire and not at release is the exact case above, and the pairing is what
	// makes it identifiable rather than just suspicious.
	//
	// Safe from any thread and takes no locks -- it only reads the OS.
	void LogPhysicalMovementKeys(const char* when);
}
