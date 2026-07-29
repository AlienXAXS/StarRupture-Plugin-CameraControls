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

		// Segment leaving this keyframe. Ignored on the last keyframe.
		double duration   = 3.0;    // seconds at speed 1.0
		float  speed      = 1.0f;   // >1 = faster (shorter), <1 = slower (longer)

		// 0 = sharp corner (straight line in/out), 1 = full Catmull-Rom curve.
		float smoothness  = 1.0f;

		Ease easeIn       = Ease::Auto;
		Ease easeOut      = Ease::Auto;

		// Screen fades. fadeIn dips out of the fade colour over the duration
		// *after* this keyframe's time; fadeOut dips into it over the duration
		// *before* it. Typically fadeIn on the first key, fadeOut on the last.
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

	// The evaluated camera state at some point in time.
	struct CameraPose
	{
		Vec3  location;
		Rot   rotation;
		float fov = 90.0f;
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

		std::vector<Keyframe> m_keys;
		uint32_t              m_nextId = 1;
	};

	// Applies the combined leaving/arriving ease to a normalised segment
	// parameter. Exposed for the property panel's curve preview.
	double ApplyEase(double u, Ease easeOut, Ease easeIn);
}
