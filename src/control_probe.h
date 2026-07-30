#pragma once

// ---------------------------------------------------------------------------
// Control probe -- "why can I not move my character?"
//
// Leaving the editor is a dozen separate restores across four subsystems, and
// the failure we are chasing is not one of them throwing: every step reports
// success and the player still cannot move. That means something we restored is
// being un-restored, or something we never looked at is holding control -- and
// neither shows up in a log that only records failures.
//
// So this module logs *state*, not events. It reads every field that can stop a
// UE character responding to input, formats them into a flat list of named
// values, and can diff two of those lists. The teardown then narrates itself:
// each step logs exactly which observable values it changed, including "none".
//
// Whatever is breaking control has to appear either as a value that was already
// wrong before we touched anything, a value one of our steps changed and should
// not have, or a value that never changed back. There is no fourth possibility,
// which is the point of doing it this way rather than adding more log lines to
// the paths we already suspect. Anything that goes wrong *later* is what the
// on-demand dump is for.
//
// Everything here is read-only. It never writes to the game, so a probe can
// never be the reason a session goes wrong, and every value in the log is what
// the engine actually had rather than what we asked it for.
//
// Cost: a probe formats ~50 short strings, so it is far too expensive for an
// unguarded per-frame path. Everything here is once per user action -- the
// teardown steps, and the keybind. See the logging table in CLAUDE.md.
//
// GAME THREAD ONLY -- it reads UObjects.
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

namespace CameraControls
{
	struct State;
}

namespace CameraControls::Probe
{
	// One observed value. `group` and `name` are literals from the capture code,
	// so a Field is cheap to copy and the pair identifies it across snapshots.
	struct Field
	{
		const char* group;
		const char* name;
		std::string value;
	};

	struct Snapshot
	{
		std::vector<Field> fields;
		bool               valid = false;   // false if there was no world to read
	};

	// Reads the world. Never throws -- an unreadable value is recorded as such
	// rather than abandoning the snapshot, because a field that cannot be read
	// is itself a finding.
	void Capture(const State& state, Snapshot& out);

	// Logs every field, grouped, at TRACE. Use for the two snapshots worth
	// having in full: what the world looked like before we touched it, and what
	// it looked like once we had finished putting it back.
	void Log(const char* label, const Snapshot& snapshot);

	// Logs only what differs. Returns true if anything did.
	bool LogDiff(const char* label, const Snapshot& before, const Snapshot& after);

	// --- The narrated teardown --------------------------------------------
	//
	// Baseline: called before the editor changes anything, so there is something
	// to compare the end state against. Also logged in full -- if control is
	// already half-broken when the editor opens, that is the answer and no
	// amount of looking at the teardown would have found it.
	void CaptureBaseline(const State& state);

	// One step of a sequence. Logs what changed since the previous Step (or
	// since the baseline, for the first one) and nothing else. "Nothing changed"
	// is logged too: a restore step that provably changes no observable state is
	// evidence, not noise.
	void Step(const char* what, const State& state);

	// Called once the teardown is done. Logs the end state in full and diffs it
	// against the baseline.
	void FinishExit(const State& state);

	// A full snapshot on demand, logged with a diff against the last one taken.
	//
	// This is the probe that matters most in practice: the automatic ones all fire
	// during the teardown, which is long before anyone has finished pressing keys to
	// work out what is broken. Bound to a keybind so the state can be sampled at the
	// moment the symptom is being reproduced, with a movement key actually held down.
	//
	// Works in every mode, including Off. Diffs against the previous dump, so
	// pressing the key twice -- once with a key held and once without -- shows
	// exactly what the keypress did or did not reach.
	void DumpNow(const State& state);

	// Logs UE's Enhanced Input mapping counts whenever they change.
	//
	// A working/broken pair showed the engine dropping 62 of 73 action mappings and
	// 40 of 50 action instances somewhere inside an editor session, and never
	// restoring them -- which is the input pipeline for gameplay being torn down
	// and not rebuilt. The snapshots bracket the session too widely to say *which*
	// step did it, and the answer is almost certainly one specific call rather than
	// the session as a whole.
	//
	// So this samples the counts every tick and logs only on a change, putting a
	// timestamped line right next to whichever of our own log lines caused it.
	// Cheap enough for a per-tick path: two integer reads and a comparison, no
	// formatting unless something moved.
	//
	// Call from the tick in every mode -- the change may well land after the editor
	// has closed, and a watcher that stops looking when the plugin goes idle would
	// miss exactly that.
	void WatchInputMappings(const State& state);

	// The number of live Enhanced Input action mappings, or -1 if it cannot be
	// read. `camera_rig` uses it to tell a healthy input set from a stripped one
	// without duplicating the SDK walk.
	int ActionMappingCount();

	// Drops everything without reading the world. For world teardown, where the
	// objects a snapshot names are already going away.
	void Forget();

	// The plugin's own view of the input token, so every snapshot carries it
	// next to the engine state it is supposed to be affecting.
	//
	// This is deliberately what *we* think we hold rather than what the modloader
	// thinks: the two disagreeing is one of the specific failures being hunted,
	// and it can only be seen by recording both independently. The other half is
	// the modloader's own token count, which is why that was added to its debug
	// panel in the same change. Safe from any thread.
	void NoteInputToken(void* token, bool passthrough);

}
