#pragma once

// ---------------------------------------------------------------------------
// Shared editor state.
//
// THREADING -- the single most important rule in this plugin:
//
//   * ImGui render callbacks run on the RENDER thread. Touching a UObject
//     there (UWorld::GetWorld(), a pawn, a widget) crashes intermittently
//     inside the D3D12 RHI with no useful callstack. The UI may therefore only
//     read and write the plain data in this struct.
//
//   * The engine-tick callback runs on the GAME thread. Everything that talks
//     to the SDK -- moving the camera, spawning actors, hiding the HUD --
//     happens there.
//
// The two meet here, under `Mutex()`. Anything the UI wants done to the game
// world is posted as a Request and drained by the game thread on its next tick.
// ---------------------------------------------------------------------------

#include "timeline.h"

#include <mutex>
#include <string>
#include <vector>

namespace CameraControls
{
	enum class Mode
	{
		Off = 0,     // plugin idle, game plays normally
		Editor,      // free-fly camera, timeline UI on screen
		Playback     // running the timeline, UI suppressed
	};

	// Work the UI asks the game thread to perform on its next tick.
	enum class Request
	{
		EnterEditor,
		ExitEditor,
		StartPlayback,
		StopPlayback,

		CaptureAppend,             // new keyframe at the live camera pose, at the end
		CaptureInsertAfterSelected,// same, but spliced in after the selection
		UpdateSelectedFromCamera,  // re-record the selected key's pose
		GotoSelected,              // fly camera jumps to the selected key
		SnapCameraToPlayhead,      // fly camera jumps to the evaluated pose
		LookAtSelectionFromCamera, // set the selected key's look-at to the camera's aim

		// Run the selected func frame right now. Queued rather than done in the
		// inspector because a func frame changes the *world*, and the inspector
		// is on the render thread.
		FireSelectedFunc,

		// Log a full control_probe snapshot. Queued rather than done in the
		// keybind callback because the probe reads UObjects and keybinds arrive on
		// the modloader's WndProc thread. Handled before the tick's mode gate, so
		// it works while the plugin is idle -- which is the only time it is useful.
		DumpControlProbe,
	};

	// Which timeline element the property panel is inspecting.
	enum class Selection
	{
		None = 0,
		Keyframe,
		Segment,   // the gap between the selected key and the next one
		Timeline,  // project-wide settings

		// A func frame. Named Function rather than FuncFrame so it does not read
		// as the FuncFrame struct at a glance -- `selectedId` means a different
		// lookup depending on which of these is set, and that is exactly the
		// place a misread costs something.
		Function
	};

	struct FlyInput
	{
		// Held-key axes, each -1, 0 or +1, derived from keybind press/release.
		int forward  = 0;
		int right    = 0;
		int up       = 0;
		int rollAxis = 0;
		int fovAxis  = 0;

		bool boost = false;   // hold to move faster
		bool crawl = false;   // hold to move slower

		// Mouse look, in degrees, accumulated since the game thread last
		// consumed them.
		double yawDelta   = 0.0;
		double pitchDelta = 0.0;

		void ClearDeltas() { yawDelta = pitchDelta = 0.0; }

		void ClearAll()
		{
			forward = right = up = rollAxis = fovAxis = 0;
			boost = crawl = false;
			ClearDeltas();
		}
	};

	// A normalised (0-1) rectangle of the game window, origin top-left.
	struct ViewRect
	{
		float x = 0.0f, y = 0.0f, w = 1.0f, h = 1.0f;
	};

	// Where a keyframe's world position lands on screen, projected by the engine
	// on the game thread for the render thread to draw handles at.
	//
	// This crosses the thread boundary as plain numbers precisely because the
	// projection cannot: it needs the player controller, and the render thread may
	// not touch one. Keyed by keyframe id rather than index, because the UI can
	// add or delete a keyframe between the tick that filled this in and the frame
	// that draws it -- and an index would then label the wrong handle.
	struct KeyScreen
	{
		uint32_t id       = 0;
		float    x        = 0.0f;
		float    y        = 0.0f;
		double   distance = 0.0;   // camera to keyframe, for handle sizing
	};

	// Bounds on the fly speed, shared by the inspector's slider and the
	// mouse-wheel adjustment so the two cannot drift apart -- a wheel that can
	// push the speed past the end of its own slider looks broken from both ends.
	//
	// Narrower than the ini's own 50..20000 range on purpose: those are the limits
	// on what someone may deliberately write in a config file, these are the
	// limits on what a control should hand out by accident.
	inline constexpr float kFlySpeedMin = 100.0f;
	inline constexpr float kFlySpeedMax = 12000.0f;

	struct Options
	{
		bool  fitViewport          = true;   // shrink the game view out from under the panels
		bool  showGizmos           = true;   // draw the path/keyframes in-world
		bool  gizmosDuringPlayback = false;  // keep drawing them while recording
		// Hide the game's own interface for as long as the editor camera is
		// flying, not just during a take -- the HUD is drawn for a player who
		// is not looking through this camera, and it is in the way of framing
		// a shot just as much as it is in the way of recording one.
		bool  hideGameHud          = true;
		bool  protectPlayer        = true;   // stash the body somewhere safe
		bool  spawnHabitat         = true;   // ...inside a spawned habitat shelter

		// World Z the body is dropped to, keeping its X and Y. Absolute, not a
		// drop from where the player stands -- see player_safeguard.h. Well below
		// any terrain, where nothing can see it and no volume contains it.
		float stashAltitude        = -3500.0f;

		// Switch off the world's out-of-bounds kill and the pawn's damage flag
		// for as long as the editor is open. See death_guard.
		bool  preventDeath         = true;

		// Additionally turn on the game's own Immortal cheat. Off by default:
		// it works, but it leaves the session running with the game's cheat
		// manager instantiated, which is not a side effect to inflict on
		// somebody who only asked to fly a camera.
		bool  gameImmortality      = false;
		bool  rippleEdits          = true;   // dragging a key pushes later keys
		bool  scrubPreview         = true;   // camera follows the playhead
		float countdownSeconds     = 3.0f;   // pre-roll before playback starts
		float mouseSensitivity     = 0.25f;
		float flySpeed             = 1200.0f;// uu/s at 1x -- see kFlySpeed{Min,Max}
		int   splineSamples        = 16;     // path samples per segment
		float gizmoScale           = 3.0f;   // keyframe camera icon size multiplier
		float gizmoNearCull        = 150.0f; // hide gizmo lines closer than this to the camera

		bool  showPlayerMarker     = true;   // draw a box where the body is
		bool  lockVitals           = true;   // pin health/food/water while editing

		// Paint fades for real while editing, rather than showing them as an
		// indicator. Off by default: a fade-to-black on the first keyframe is
		// the most common thing to set, and it would cover the shot you are
		// composing every time the playhead sat on it.
		bool  previewFades         = false;

		// Draws the world-to-screen self-test. Session-only, never written to
		// the ini -- it is a diagnostic, not a preference.
		bool  projectionDebug      = false;
	};

	struct State
	{
		Mode      mode      = Mode::Off;
		Timeline  timeline;

		Selection selection   = Selection::None;
		uint32_t  selectedId  = 0;

		// Everything selected when Ctrl+clicking has gathered more than one thing,
		// keyframes and cues mixed freely -- they share an id space, so one list
		// covers both and an id can only ever mean one of them.
		//
		// Invariant: either empty, or holding *at least two* ids, one of which is
		// `selectedId`. Never one. A single-item multi-selection would be a second
		// representation of the state `selectedId` already describes, and every
		// piece of code downstream would then have to handle both -- so the toggle
		// collapses back to a plain selection the moment it drops to one.
		std::vector<uint32_t> multiSelection;

		// --- Transport -------------------------------------------------------
		double playhead  = 0.0;
		bool   playing   = false;

		// Set by the timeline when the *user* dragged the playhead, and consumed
		// (and cleared) by the game thread on its next tick.
		//
		// Cues fire on a forward scrub, and this is what separates a scrub from
		// every other way the playhead moves while stopped. Selecting the next
		// keyframe with `.`, flying to one, or stopping a take all reposition it
		// too -- and firing every cue in between because somebody changed the
		// selection would be a surprise with no undo behind it.
		bool   playheadScrubbed = false;

		// Counts down before Playback actually starts rolling. > 0 means the
		// pre-roll overlay is on screen and the playhead is parked at 0.
		double countdown = 0.0;

		// --- Live camera (game thread writes, UI reads) ----------------------
		CameraPose flyPose;
		bool       rigActive = false;   // the camera actor exists and owns the view

		// Whether the FOV we ask for is actually being rendered. False means the
		// game's camera manager is overriding it and `fov_override` could not arm,
		// which the inspector says out loud -- a FOV slider that edits the keyframe
		// but never changes the picture is exactly the bug that module exists for.
		bool       fovLive   = false;

		// The view the engine is *actually* rendering, read back from the
		// camera manager every tick.
		//
		// Not the same thing as flyPose. The camera manager is the last word on
		// what reaches the screen -- it blends between view targets, and games
		// routinely scale FOV underneath it for sprinting, aiming and the like.
		// Anything projecting world space onto the screen has to use this, or
		// the handles it draws will not sit on top of what the player sees.
		CameraPose renderView;
		bool       renderViewValid = false;

		// Engine-projected screen positions for every keyframe that is currently
		// in front of the camera. Refilled from scratch every editor tick, so a
		// keyframe missing from here is one the engine declined to project --
		// behind the camera, or no projection data yet. The UI draws nothing for
		// those rather than guessing.
		std::vector<KeyScreen> keyScreen;

		FlyInput flyInput;
		Options  options;

		// --- UI -> game thread ------------------------------------------------
		std::vector<Request> requests;

		// --- Feedback ---------------------------------------------------------
		std::string status;        // one-line message shown in the editor chrome
		double      statusExpiry = 0.0;

		// True while an ImGui text field has focus, so the fly keybinds can stop
		// treating "W" as a movement key while the user is naming a keyframe.
		bool textInputActive = false;

		// True while the cursor is over one of the editor's ImGui windows.
		// Written by the render thread; the game thread reads it to decide
		// whether a right-mouse press starts a look-drag or belongs to the UI.
		bool uiHovered = false;

		// True while a right-mouse look-drag owns the camera.
		//
		// Lives here rather than as a static in `input_binds` because three threads
		// want it: the loader's input thread sets it, the game thread's mouse pump
		// consumes it, and the render thread reads it to route the scroll wheel to
		// the fly speed instead of to the timeline's zoom. The mutex was already
		// covering every one of those accesses, so this is the honest home for it.
		bool lookActive = false;

		// Set while the editor windows are collapsed to the overlay only.
		bool uiHidden = false;

		// Where the game's 3D view should render, as a fraction of the window.
		// The render thread derives it from the panel layout; the game thread
		// pushes it into the local player. Full-screen until the UI has laid
		// itself out at least once.
		ViewRect gameView;
		bool     gameViewValid = false;

		// Reported back by the game thread once the squeeze has actually taken
		// effect. The UI only masks off the area outside the picture when this
		// is set -- if the engine-side write failed, the game is still filling
		// the whole window and painting over it would black out the view.
		bool gameViewApplied = false;

		// Cleared by the game thread if the engine-side probe failed, so the
		// inspector can grey the option out instead of offering something that
		// will never do anything.
		bool gameViewSupported = true;

		// ImGui's display size in pixels, so the game thread can report the game
		// view rect in the same units the render thread laid it out in. Only the
		// render thread knows this, and only the game thread can ask the engine
		// what rect it is really using -- comparing the two needs both here.
		float displayW = 0.0f;
		float displayH = 0.0f;

		// Project bookkeeping.
		std::string projectName = "Untitled";
		bool        dirty       = false;
	};

	// The one instance. Always take Mutex() before touching anything in it.
	State&      Get();
	std::mutex& Mutex();

	inline std::unique_lock<std::mutex> Lock() { return std::unique_lock<std::mutex>(Mutex()); }

	// Queues a request for the game thread. Caller must already hold the lock.
	void Post(State& state, Request request);

	// --- Selection ---------------------------------------------------------
	// All of these take the already-locked State; none of them touch the SDK, so
	// the timeline widget drives them straight from the render thread.

	// The kind of thing `id` names, derived from the timeline rather than
	// remembered -- an id is a keyframe or a cue and cannot be both.
	Selection KindOf(const State& state, uint32_t id);

	// Is `id` part of the current selection, single or multi?
	bool IsSelected(const State& state, uint32_t id);

	// Plain click: `id` alone, and any multi-selection is dropped.
	void SelectOnly(State& state, uint32_t id);

	// Ctrl+click: adds `id` if it is not already in, removes it if it is.
	//
	// Seeds from the current single selection on the first Ctrl+click, so
	// clicking one thing and Ctrl+clicking a second gives you two rather than
	// silently discarding the first.
	void ToggleSelection(State& state, uint32_t id);

	// Drops everything. Used by delete, load, new project and Escape.
	void ClearSelection(State& state);

	// Sets the transient status line. Caller must already hold the lock.
	// `now` is the plugin's monotonic clock (seconds).
	void SetStatus(State& state, double now, const char* text);

	// Seconds since plugin init, safe to call from any thread.
	double Now();
}
