#pragma once

// ---------------------------------------------------------------------------
// Timeline data model and evaluation.
//
// Pure data + maths -- no game SDK, no ImGui, no threading. The owner
// (editor_state) is responsible for locking; camera_rig converts the evaluated
// pose into SDK types on the game thread.
//
// TIME MODEL
// ----------
// A keyframe does not store an absolute timeline position. It stores the
// *base duration* of the segment leaving it, plus a *speed* multiplier, and
// absolute times are derived:
//
//     effectiveDuration(i) = keys[i].duration / keys[i].speed
//     absoluteTime(i)      = sum of effectiveDuration(0 .. i-1)
//
// This is how a real NLE models clip speed: dropping a segment to 0.5x makes
// it take twice as long and pushes everything after it later, and there is
// never a stored absolute time that can disagree with the stored speed. The
// last keyframe's duration is unused (nothing leaves it).
// ---------------------------------------------------------------------------

#include "cc_math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace CameraControls
{
	// Velocity profile applied across a segment. easeOut is taken from the key
	// the camera is leaving, easeIn from the key it is arriving at, and the two
	// are combined into a single curve -- so "Cubic out" into "Cubic in" gives
	// the usual slow-fast-slow shot.
	//
	// Every non-linear curve has zero slope at its ends, so an explicit ease on
	// both sides of an interior keyframe brings the camera to a dead stop as it
	// passes through. That is occasionally what you want (a beat on a subject)
	// and almost never what you want by default, which is what Auto is for.
	enum class Ease : int
	{
		Linear = 0,
		Sine,
		Quad,
		Cubic,
		Expo,

		// Ease only where the whole move starts and ends, and sail straight
		// through every keyframe in between. Appended rather than inserted:
		// the integer is what project files store.
		Auto,

		Count
	};

	const char* EaseName(Ease e);

	// Turns Auto into a concrete curve. `atShotEnd` is true only at the very
	// start or the very end of the whole move -- the two boundaries where the
	// camera has nowhere to carry its momentum to.
	Ease ResolveEase(Ease e, bool atShotEnd);

	// A single camera pose on the timeline.
	struct Keyframe
	{
		uint32_t id       = 0;      // stable across insert/delete/sort; 0 = invalid
		std::string name;           // free-text label shown on the clip

		Vec3  location;
		Rot   rotation;
		float fov         = 90.0f;

		// Depth of field. Both are animated exactly like fov, so a focus pull is
		// just two keyframes with different distances -- which is the whole reason
		// they live on the keyframe rather than on the project. Only applied when
		// the project's `depthOfField` is on; see there.
		//
		// Distance is in centimetres, UE's unit, and is blended linearly in
		// distance. Real focus pulls are closer to linear in 1/distance, so a pull
		// across a big range will feel back-loaded; the fix if that ever matters is
		// to blend the reciprocal here, not to add another control.
		float focusDistance = 1000.0f;   // 10 m
		float aperture      = 4.0f;      // f-stop; lower is shallower

		// Segment leaving this keyframe. Ignored on the last keyframe.
		double duration   = 3.0;    // seconds at speed 1.0
		float  speed      = 1.0f;   // >1 = faster (shorter), <1 = slower (longer)

		// 0 = sharp corner (straight line in/out), 1 = full Catmull-Rom curve.
		float smoothness  = 1.0f;

		Ease easeIn       = Ease::Auto;
		Ease easeOut      = Ease::Auto;

		// Screen fades. fadeIn dips *out* of the fade colour over the duration
		// after this keyframe's time; fadeOut dips *into* it over the duration
		// before it. Typically fadeIn on the first key and fadeOut on the last,
		// but not only: they are events, and the picture holds whatever the last
		// one left it at, so a fade-out followed by a later fade-in is a
		// transition, and both flags on one keyframe is a dip through the colour
		// and back. See Timeline::EvaluateFade -- treating these as independent
		// ramps instead of as events is what used to leave the screen stuck on
		// black for the rest of the shot.
		bool  fadeIn          = false;
		float fadeInDuration  = 1.0f;
		bool  fadeOut         = false;
		float fadeOutDuration = 1.0f;
		float fadeColor[3]    = { 0.0f, 0.0f, 0.0f };

		// When set, rotation is derived by aiming at lookAtTarget instead of
		// interpolating the stored rotation -- useful for orbiting a building.
		bool lookAt           = false;
		Vec3 lookAtTarget;

		// Unchecked keys stay on the timeline but are skipped entirely by
		// evaluation, so a shot can be tried without deleting anything.
		bool enabled          = true;
	};

	// -------------------------------------------------------------------
	// Function frames
	//
	// A keyframe says where the camera is. A func frame says what the *world*
	// does, at one instant, while a take is rolling -- "the rupture starts
	// here", "it is halfway across the map by here". They share the timeline
	// with keyframes and are drawn from its top edge rather than its bottom,
	// because they are a different kind of thing and reading one as the other
	// is the one mistake this feature makes expensive.
	//
	// Unlike a keyframe, a func frame stores an ABSOLUTE time. Keyframe times
	// are derived from the segment durations, so retiming a shot carries them
	// along; a func frame stays exactly where it was put. That is deliberate:
	// a cue belongs to a moment in the recording rather than to a camera move,
	// and there is no anchor for it to ride on that would survive the keyframe
	// it was anchored to being deleted.
	//
	// Func frames fire during *playback* only, never while scrubbing. Scrubbing
	// back and forth over a "start the rupture" cue would fire it once per pass.
	// The inspector's Trigger now button is the way to test one by hand.
	enum class FuncAction : int
	{
		None = 0,

		StartRupture,        // uses waveType
		SetRupturePhase,     // uses waveType + stage
		SetRuptureProgress,  // uses progress
		CancelRupture,
		PauseRupture,
		ResumeRupture,

		Count
	};

	const char* FuncActionName(FuncAction action);

	// One line for a tooltip. Kept next to the name so the two cannot drift.
	const char* FuncActionSummary(FuncAction action);

	// What to run when the playhead is dragged back over this cue, or
	// FuncAction::None where there is no honest inverse.
	//
	// Only three actions have one. Everything else returns None rather than a
	// best guess -- see the comment on the implementation for why inventing one
	// is worse than doing nothing.
	FuncAction ReverseOf(FuncAction action);

	// waveType and stage mirror the game's own EEnviroWave / EEnviroWaveStage
	// and are held as plain ints so this header stays free of the game SDK.
	// `world_func` is the only place that casts them back, and it range-checks
	// first.
	const char* RuptureTypeName(int waveType);   // 1 Heat, 2 Cold
	const char* RuptureStageName(int stage);     // 1 PreWave .. 4 Growback

	struct FuncFrame
	{
		uint32_t    id      = 0;        // shares the keyframe id space; 0 = invalid
		std::string name;               // free-text label drawn beside the marker
		double      time    = 0.0;      // ABSOLUTE seconds -- see above
		bool        enabled = true;

		FuncAction  action  = FuncAction::StartRupture;

		int   waveType = 1;             // EEnviroWave::Heat
		int   stage    = 2;             // EEnviroWaveStage::Moving
		float progress = 0.5f;          // 0..1 through the current stage

		// SetRuptureProgress only: interpolate to `progressEnd` over
		// `rampDuration` seconds instead of jumping once.
		//
		// A ramping cue is the one thing here that is not an event -- it has a
		// span, so it is driven from where the playhead *is* rather than fired
		// when the playhead crosses it. That difference is load-bearing
		// everywhere it is handled; see WorldFunc and the tick.
		bool   ramp         = false;
		float  progressEnd  = 1.0f;
		double rampDuration = 3.0;
	};

	// The evaluated camera state at some point in time.
	struct CameraPose
	{
		Vec3  location;
		Rot   rotation;
		float fov = 90.0f;

		// `depthOfField` is stamped from the project rather than interpolated --
		// it is a property of the shot, and a pose that carried a *blend* of "on"
		// and "off" would mean nothing. Carrying it here anyway keeps `ApplyPose`
		// able to work from the pose alone.
		bool  depthOfField  = false;
		float focusDistance = 1000.0f;
		float aperture      = 4.0f;
	};

	// Screen fade state at some point in time.
	struct FadeState
	{
		float alpha    = 0.0f;                 // 0 = clear, 1 = fully covered
		float color[3] = { 0.0f, 0.0f, 0.0f };
	};

	class Timeline
	{
	public:
		std::string name = "Untitled";

		// Global playback rate applied on top of per-keyframe speed.
		float globalSpeed = 1.0f;
		bool  loop        = false;

		// Whether the shot overrides the game's depth of field at all. One switch
		// for the whole timeline, because "on for this keyframe, off for the next"
		// has no meaningful in-between and would pop mid-move. Off by default: the
		// game has its own DOF and silently replacing it is not a favour.
		bool  depthOfField = false;

		// Only used as the unit for the editor's nudge buttons and the ruler's
		// frame read-out -- evaluation itself is continuous and frame-rate
		// independent. Set it to whatever you are recording at (or to 1 to make
		// the nudge buttons work in whole seconds).
		float frameRate   = 30.0f;

		// --- Keyframe access -------------------------------------------------
		const std::vector<Keyframe>& Keys() const { return m_keys; }
		std::vector<Keyframe>&       Keys()       { return m_keys; }

		int  Count() const { return static_cast<int>(m_keys.size()); }
		bool Empty() const { return m_keys.empty(); }

		Keyframe*       Find(uint32_t id);
		const Keyframe* Find(uint32_t id) const;
		int             IndexOf(uint32_t id) const;

		// Appends a keyframe at the end of the timeline and returns its id.
		uint32_t Append(const Keyframe& key);

		// Inserts a keyframe after `index`, splitting that segment's duration in
		// half so nothing after it moves. index < 0 prepends. Returns the new id.
		uint32_t InsertAfter(int index, const Keyframe& key);

		// Inserts a keyframe so that it lands at `time`, splitting whichever
		// segment contains that moment (or extending the shot if it is past the
		// end). This is what the timeline's right-click menu uses -- everywhere
		// else keyframes are placed by index, but a context menu only knows
		// where the cursor was.
		uint32_t InsertAtTime(double time, const Keyframe& key);

		bool Remove(uint32_t id);
		void Clear();

		// Swaps a keyframe with its neighbour, keeping segment durations with
		// the slots rather than the keys so the overall timing is preserved.
		bool MoveEarlier(uint32_t id);
		bool MoveLater(uint32_t id);

		// --- Timing ----------------------------------------------------------
		double EffectiveDuration(int index) const;
		double AbsoluteTime(int index) const;
		double TotalDuration() const;

		// Sets the absolute time of `index` by adjusting the preceding segment.
		// ripple: everything after keeps its own durations and slides along.
		// !ripple: the following segment absorbs the change, so only this
		//          keyframe moves (a trim). Clamped so no segment goes below
		//          kMinDuration.
		void SetAbsoluteTime(int index, double newTime, bool ripple);

		// --- Evaluation ------------------------------------------------------
		// Clamped to [0, TotalDuration()]. Returns false (leaving pose
		// untouched) if there is nothing enabled to evaluate.
		bool Evaluate(double time, CameraPose& outPose) const;

		// Combined fade contribution of every enabled keyframe at `time`.
		FadeState EvaluateFade(double time) const;

		// --- Function frames -------------------------------------------------
		//
		// Deliberately NOT kept sorted. The inspector edits a func frame through
		// a reference, and re-ordering the vector under one of those is a
		// dangling reference waiting to happen. Nothing here needs storage order
		// anyway: drawing does not care, and the only consumer that needs time
		// order (the playback trigger) sorts a copy of what it is about to fire.
		const std::vector<FuncFrame>& Funcs() const { return m_funcs; }
		std::vector<FuncFrame>&       Funcs()       { return m_funcs; }

		int FuncCount() const { return static_cast<int>(m_funcs.size()); }

		FuncFrame*       FindFunc(uint32_t id);
		const FuncFrame* FindFunc(uint32_t id) const;

		uint32_t AddFunc(const FuncFrame& frame);
		bool     RemoveFunc(uint32_t id);

		// Samples the path into world-space points for the spline gizmo.
		// samplesPerSegment is clamped to a sane range internally.
		void SamplePath(int samplesPerSegment, std::vector<Vec3>& outPoints) const;

		// Next unused keyframe id. Exposed so project_io can restore ids and
		// keep the counter ahead of them.
		uint32_t NextId() const { return m_nextId; }
		void     SetNextId(uint32_t id) { m_nextId = id; }

		static constexpr double kMinDuration = 0.05;

	private:
		// Indices of enabled keyframes, in order. Evaluation walks this rather
		// than m_keys so disabled keys are skipped without disturbing timing of
		// the ones that remain.
		void BuildEnabled(std::vector<int>& out) const;

		std::vector<Keyframe>  m_keys;
		std::vector<FuncFrame> m_funcs;

		// One counter for both, so a keyframe and a func frame can never share
		// an id. The editor holds a single `selectedId` alongside a kind, and a
		// collision there would inspect the wrong thing the moment those two
		// disagreed.
		uint32_t              m_nextId = 1;
	};

	// Applies the combined leaving/arriving ease to a normalised segment
	// parameter. Exposed for the property panel's curve preview.
	double ApplyEase(double u, Ease easeOut, Ease easeIn);

	// A fresh keyframe carrying an evaluated pose. Shared because both the
	// game thread (the capture requests) and the timeline's right-click menu
	// need it, and a second copy would be one to forget when a field is added.
	Keyframe MakeKeyframeFromPose(const CameraPose& pose);
}
