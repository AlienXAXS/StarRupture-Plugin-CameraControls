#pragma once

// ---------------------------------------------------------------------------
// Func frame execution -- the only module here that changes the *world* rather
// than the camera or the player's own body.
//
// GAME THREAD ONLY. Everything below goes through the game SDK.
//
// The rupture is `UCrEnviroWaveSubsystem` in the generated headers -- the game
// calls the event an "enviro wave" internally and a rupture everywhere a player
// can see it, so the code says wave and the UI says rupture. Every call is a
// UFunction on a real U* class, which is the safe half of the SDK access rule
// (see CLAUDE.md section 6); nothing here resolves a soft pointer or touches an
// interface class.
//
// AUTHORITY: this is a client plugin, and starting a rupture is a world event.
// Single-player and hosting are the cases it is written for. As a connected
// client the server has the final say, so a cue may report success and produce
// nothing -- which is why Execute always hands back a line for the status bar
// rather than failing silently.
// ---------------------------------------------------------------------------

#include "timeline.h"

#include <string>

namespace CameraControls::WorldFunc
{
	// Runs one func frame. Returns false if it could not, and fills outMessage
	// either way with a line short enough for the editor's status bar. The
	// caller shows it *and* logs it, so "the cue fired and nothing happened" is
	// never a thing anyone has to guess at.
	bool Execute(const FuncFrame& frame, std::string& outMessage);

	// The per-frame half, for a ramping Advance cue: pushes one interpolated
	// stage progress at the world and says whether it landed.
	//
	// Silent by design. It runs from the tick, and CLAUDE.md's logging rules put
	// an unguarded line in a per-frame path off limits -- so the caller
	// edge-triggers on the return value instead. Nothing else here is allowed to
	// be called sixty times a second.
	bool HoldRuptureProgress(float progress);

	// Drops the cached subsystem pointer. Called from world end-play, where the
	// object it points at is being torn down with the world -- deliberately
	// without dereferencing it.
	void ForgetWorldState();
}
