#include "timeline.h"

#include <algorithm>

namespace CameraControls
{
	namespace
	{
		// Half of an ease curve, mapping [0,1] -> [0,1] with zero slope at u=1.
		// The segment ease combines the leaving key's curve (mirrored) with the
		// arriving key's curve, so both ends can be shaped independently.
		double EaseHalf(double u, Ease type)
		{
			switch (type)
			{
				case Ease::Sine:  return 1.0 - std::cos(u * kPi * 0.5);
				case Ease::Quad:  return u * u;
				case Ease::Cubic: return u * u * u;
				case Ease::Expo:  return u <= 0.0 ? 0.0 : std::pow(2.0, 10.0 * (u - 1.0));
				case Ease::Linear:
				default:          return u;
			}
		}

		// Non-uniform cubic Hermite through p1 -> p2, with tangents derived from
		// the surrounding points and the real segment durations. Uniform
		// Catmull-Rom overshoots badly when one segment is much longer than its
		// neighbours, which on a camera path shows up as the shot lurching
		// sideways before a slow move.
		double CatmullRom1D(double p0, double p1, double p2, double p3,
		                    double d0, double d1, double d2, double u)
		{
			// Guard against zero-length neighbour segments (first/last key).
			d0 = std::max(d0, 1e-6);
			d1 = std::max(d1, 1e-6);
			d2 = std::max(d2, 1e-6);

			const double m1 = ((p2 - p0) / (d0 + d1)) * d1;
			const double m2 = ((p3 - p1) / (d1 + d2)) * d1;

			const double u2 = u * u;
			const double u3 = u2 * u;

			return (2.0 * u3 - 3.0 * u2 + 1.0) * p1
			     + (u3 - 2.0 * u2 + u)         * m1
			     + (-2.0 * u3 + 3.0 * u2)      * p2
			     + (u3 - u2)                   * m2;
		}

		// Blends the spline result back towards a straight line by `smoothness`,
		// so 0 gives a hard corner at the keyframe and 1 the full curve.
		double SmoothBlend1D(double p0, double p1, double p2, double p3,
		                     double d0, double d1, double d2, double u, double smoothness)
		{
			const double linear = Lerp(p1, p2, u);
			if (smoothness <= 0.0001)
				return linear;

			const double curved = CatmullRom1D(p0, p1, p2, p3, d0, d1, d2, u);
			return Lerp(linear, curved, smoothness);
		}
	}

	const char* EaseName(Ease e)
	{
		switch (e)
		{
			case Ease::Linear: return "Linear";
			case Ease::Sine:   return "Sine";
			case Ease::Quad:   return "Quad";
			case Ease::Cubic:  return "Cubic";
			case Ease::Expo:   return "Expo";
			case Ease::Auto:   return "Auto (flow through)";
			default:           return "?";
		}
	}

	Ease ResolveEase(Ease e, bool atShotEnd)
	{
		if (e != Ease::Auto)
			return e;

		// Linear in the middle is what makes the camera carry its speed through
		// a keyframe. The path itself is already smooth there -- the Hermite
		// tangents are scaled by segment length, so position is continuous in
		// *time* across the joint -- and it is only the ease curve flattening
		// to zero slope that was stopping the move dead.
		return atShotEnd ? Ease::Sine : Ease::Linear;
	}

	const char* FuncActionName(FuncAction action)
	{
		switch (action)
		{
			case FuncAction::None:               return "Nothing";
			case FuncAction::StartRupture:       return "Start rupture";
			case FuncAction::SetRupturePhase:    return "Set rupture phase";
			case FuncAction::SetRuptureProgress: return "Set rupture progress";
			case FuncAction::CancelRupture:      return "Cancel rupture";
			case FuncAction::PauseRupture:       return "Pause rupture";
			case FuncAction::ResumeRupture:      return "Resume rupture";
			default:                             return "?";
		}
	}

	const char* FuncActionSummary(FuncAction action)
	{
		switch (action)
		{
			case FuncAction::None:
				return "Placeholder. Fires nothing.";
			case FuncAction::StartRupture:
				return "Begins a rupture of the chosen kind, exactly as the world's own "
				       "timer would.";
			case FuncAction::SetRupturePhase:
				return "Restarts the running rupture at a different stage, keeping the "
				       "settings it is already using. Needs a rupture in progress -- there "
				       "is no sensible set of durations to invent for one that is not.";
			case FuncAction::SetRuptureProgress:
				return "Drives how far the running rupture is through its current stage. Jumps "
				       "to one value, or ramps smoothly between two over a duration you set -- "
				       "which is how you make it cross the map on the shot's schedule rather "
				       "than the world's.";
			case FuncAction::CancelRupture:
				return "Ends the running rupture immediately.";
			case FuncAction::PauseRupture:
				return "Holds the rupture where it is -- useful for framing a shot against "
				       "one without it moving on.";
			case FuncAction::ResumeRupture:
				return "Lets a paused rupture carry on.";
			default:
				return "";
		}
	}

	FuncAction ReverseOf(FuncAction action)
	{
		switch (action)
		{
			case FuncAction::StartRupture:  return FuncAction::CancelRupture;
			case FuncAction::PauseRupture:  return FuncAction::ResumeRupture;
			case FuncAction::ResumeRupture: return FuncAction::PauseRupture;

			// Everything else has no inverse, and a plausible-looking guess would
			// be worse than nothing -- an approximate undo that silently leaves
			// the world somewhere it has never been is harder to notice, and
			// harder to reason about, than no undo at all:
			//
			//  * SetRupturePhase and SetRuptureProgress would need the value they
			//    overwrote, and nothing records it. The obvious "put back the
			//    previous stage" needs a history this model does not keep, and the
			//    previous stage is not even well defined once another cue has run.
			//  * CancelRupture would have to restore the rupture it ended -- type,
			//    stage and elapsed progress -- which is strictly more than a cue
			//    carries. Restarting from scratch would look like an undo and
			//    would actually be a second rupture.
			default: return FuncAction::None;
		}
	}

	const char* RuptureTypeName(int waveType)
	{
		switch (waveType)
		{
			case 1:  return "Heat";
			case 2:  return "Cold";
			default: return "None";
		}
	}

	const char* RuptureStageName(int stage)
	{
		switch (stage)
		{
			case 1:  return "Pre-wave";
			case 2:  return "Moving";
			case 3:  return "Fade-out";
			case 4:  return "Growback";
			default: return "None";
		}
	}

	Keyframe MakeKeyframeFromPose(const CameraPose& pose)
	{
		Keyframe key;
		key.location      = pose.location;
		key.rotation      = pose.rotation;
		key.fov           = pose.fov;
		key.focusDistance = pose.focusDistance;
		key.aperture      = pose.aperture;
		return key;
	}

	double ApplyEase(double u, Ease easeOut, Ease easeIn)
	{
		u = Clamp(u, 0.0, 1.0);

		// First half is governed by the key being left (accelerating away from
		// it), second half by the key being approached (decelerating into it).
		if (u < 0.5)
			return 0.5 * EaseHalf(u * 2.0, easeOut);

		return 1.0 - 0.5 * EaseHalf((1.0 - u) * 2.0, easeIn);
	}

	// -----------------------------------------------------------------------
	// Keyframe storage
	// -----------------------------------------------------------------------

	Keyframe* Timeline::Find(uint32_t id)
	{
		for (auto& k : m_keys)
			if (k.id == id) return &k;
		return nullptr;
	}

	const Keyframe* Timeline::Find(uint32_t id) const
	{
		for (const auto& k : m_keys)
			if (k.id == id) return &k;
		return nullptr;
	}

	int Timeline::IndexOf(uint32_t id) const
	{
		for (int i = 0; i < static_cast<int>(m_keys.size()); ++i)
			if (m_keys[i].id == id) return i;
		return -1;
	}

	uint32_t Timeline::Append(const Keyframe& key)
	{
		Keyframe k = key;
		k.id = m_nextId++;
		m_keys.push_back(k);
		return k.id;
	}

	uint32_t Timeline::InsertAfter(int index, const Keyframe& key)
	{
		Keyframe k = key;
		k.id = m_nextId++;

		if (index < 0 || m_keys.empty())
		{
			m_keys.insert(m_keys.begin(), k);
			return k.id;
		}

		index = Clamp(index, 0, static_cast<int>(m_keys.size()) - 1);

		// Split the host segment so the keys after the insertion point keep
		// their absolute times -- inserting a key mid-shot should refine the
		// path, not stretch the timeline.
		if (index < static_cast<int>(m_keys.size()) - 1)
		{
			const double half = std::max(m_keys[index].duration * 0.5, kMinDuration);
			m_keys[index].duration = half;
			k.duration             = half;
			k.speed                = m_keys[index].speed;
		}

		m_keys.insert(m_keys.begin() + index + 1, k);
		return k.id;
	}

	uint32_t Timeline::InsertAtTime(double time, const Keyframe& key)
	{
		if (m_keys.empty())
			return Append(key);   // the first key defines t = 0 whatever was asked for

		const int    last  = static_cast<int>(m_keys.size()) - 1;
		const double total = AbsoluteTime(last);

		// Past the end of the shot: append, then stretch the segment that now
		// leads to it so the new key lands where it was asked for rather than
		// wherever the default duration happened to put it.
		if (time >= total)
		{
			const uint32_t id = Append(key);
			SetAbsoluteTime(static_cast<int>(m_keys.size()) - 1,
			                std::max(time, total + kMinDuration), /*ripple=*/true);
			return id;
		}

		// Otherwise it splits whichever segment contains the moment. InsertAfter
		// halves that segment; SetAbsoluteTime then trims the two halves against
		// each other, so nothing past the pair moves.
		int host = 0;
		for (int i = 0; i < last; ++i)
		{
			if (time < AbsoluteTime(i + 1))
			{
				host = i;
				break;
			}
		}

		const uint32_t id = InsertAfter(host, key);
		SetAbsoluteTime(host + 1, time, /*ripple=*/false);
		return id;
	}

	bool Timeline::Remove(uint32_t id)
	{
		const int index = IndexOf(id);
		if (index < 0)
			return false;

		// Hand the removed key's segment to its predecessor so the following
		// keys keep their absolute times.
		if (index > 0 && index < static_cast<int>(m_keys.size()) - 1)
			m_keys[index - 1].duration += m_keys[index].duration;

		m_keys.erase(m_keys.begin() + index);
		return true;
	}

	void Timeline::Clear()
	{
		m_keys.clear();
		m_funcs.clear();
		m_nextId = 1;
	}

	// -----------------------------------------------------------------------
	// Function frames
	// -----------------------------------------------------------------------

	FuncFrame* Timeline::FindFunc(uint32_t id)
	{
		for (auto& f : m_funcs)
			if (f.id == id) return &f;
		return nullptr;
	}

	const FuncFrame* Timeline::FindFunc(uint32_t id) const
	{
		for (const auto& f : m_funcs)
			if (f.id == id) return &f;
		return nullptr;
	}

	uint32_t Timeline::AddFunc(const FuncFrame& frame)
	{
		FuncFrame f = frame;
		f.id   = m_nextId++;
		f.time = std::max(f.time, 0.0);
		m_funcs.push_back(f);
		return f.id;
	}

	bool Timeline::RemoveFunc(uint32_t id)
	{
		for (auto it = m_funcs.begin(); it != m_funcs.end(); ++it)
		{
			if (it->id == id)
			{
				m_funcs.erase(it);
				return true;
			}
		}
		return false;
	}

	bool Timeline::MoveEarlier(uint32_t id)
	{
		const int index = IndexOf(id);
		if (index <= 0)
			return false;

		Keyframe& earlier = m_keys[index - 1];
		Keyframe& later   = m_keys[index];

		// Durations belong to timeline slots, not to the keys sitting in them:
		// swap the whole keyframes, then put the timing back where it was, so
		// reordering poses never changes the overall shot length.
		std::swap(earlier, later);
		std::swap(earlier.duration, later.duration);
		std::swap(earlier.speed,    later.speed);
		return true;
	}

	bool Timeline::MoveLater(uint32_t id)
	{
		const int index = IndexOf(id);
		if (index < 0 || index >= static_cast<int>(m_keys.size()) - 1)
			return false;

		return MoveEarlier(m_keys[index + 1].id);
	}

	// -----------------------------------------------------------------------
	// Timing
	// -----------------------------------------------------------------------

	double Timeline::EffectiveDuration(int index) const
	{
		if (index < 0 || index >= static_cast<int>(m_keys.size()))
			return 0.0;

		const float speed = m_keys[index].speed > 0.01f ? m_keys[index].speed : 0.01f;
		return std::max(m_keys[index].duration / speed, kMinDuration);
	}

	double Timeline::AbsoluteTime(int index) const
	{
		double t = 0.0;
		const int clamped = Clamp(index, 0, static_cast<int>(m_keys.size()));
		for (int i = 0; i < clamped; ++i)
			t += EffectiveDuration(i);
		return t;
	}

	double Timeline::TotalDuration() const
	{
		if (m_keys.size() < 2)
			return 0.0;

		// The final key ends the timeline; its own duration never plays.
		return AbsoluteTime(static_cast<int>(m_keys.size()) - 1);
	}

	void Timeline::SetAbsoluteTime(int index, double newTime, bool ripple)
	{
		if (index <= 0 || index >= static_cast<int>(m_keys.size()))
			return; // the first keyframe defines t=0 and cannot be moved

		const double prevStart = AbsoluteTime(index - 1);
		double       newEff    = newTime - prevStart;

		Keyframe& prev = m_keys[index - 1];
		const float prevSpeed = prev.speed > 0.01f ? prev.speed : 0.01f;

		if (!ripple && index < static_cast<int>(m_keys.size()) - 1)
		{
			// Trim: the following segment gives up (or absorbs) exactly what
			// this one gains, so every key past the next one stays put.
			Keyframe& next = m_keys[index];
			const float nextSpeed = next.speed > 0.01f ? next.speed : 0.01f;

			const double prevEff  = EffectiveDuration(index - 1);
			const double nextEff  = EffectiveDuration(index);
			const double budget   = prevEff + nextEff;

			newEff = Clamp(newEff, kMinDuration, budget - kMinDuration);

			prev.duration = newEff * prevSpeed;
			next.duration = (budget - newEff) * nextSpeed;
			return;
		}

		newEff        = std::max(newEff, kMinDuration);
		prev.duration = newEff * prevSpeed;
	}

	// -----------------------------------------------------------------------
	// Evaluation
	// -----------------------------------------------------------------------

	void Timeline::BuildEnabled(std::vector<int>& out) const
	{
		out.clear();
		out.reserve(m_keys.size());
		for (int i = 0; i < static_cast<int>(m_keys.size()); ++i)
			if (m_keys[i].enabled)
				out.push_back(i);
	}

	bool Timeline::Evaluate(double time, CameraPose& outPose) const
	{
		std::vector<int> live;
		BuildEnabled(live);

		if (live.empty())
			return false;

		outPose.depthOfField = depthOfField;

		if (live.size() == 1)
		{
			const Keyframe& only = m_keys[live[0]];
			outPose.location      = only.location;
			outPose.rotation      = only.lookAt ? LookAtRotation(only.location, only.lookAtTarget) : only.rotation;
			outPose.fov           = only.fov;
			outPose.focusDistance = only.focusDistance;
			outPose.aperture      = only.aperture;
			return true;
		}

		const double total = AbsoluteTime(live.back());
		time = Clamp(time, 0.0, total);

		// Locate the live segment containing `time`. Segment n spans
		// live[n] -> live[n+1], and its length is the sum of the effective
		// durations of every raw slot in between (disabled keys are skipped,
		// but the time they occupied is not thrown away).
		int    segment = 0;
		double segStart = 0.0;
		double segLen   = 0.0;

		for (int n = 0; n + 1 < static_cast<int>(live.size()); ++n)
		{
			double len = 0.0;
			for (int raw = live[n]; raw < live[n + 1]; ++raw)
				len += EffectiveDuration(raw);

			segment = n;
			segLen  = len;

			if (time < segStart + len || n + 2 == static_cast<int>(live.size()))
				break;

			segStart += len;
		}

		segLen = std::max(segLen, kMinDuration);

		const double u = Clamp((time - segStart) / segLen, 0.0, 1.0);

		const Keyframe& a = m_keys[live[segment]];
		const Keyframe& b = m_keys[live[segment + 1]];

		// Auto easing resolves against the shot, not against the segment: only
		// the very first and very last boundaries are places the camera has to
		// come to rest. Everything in between flows through.
		const bool leavingShotStart = (segment == 0);
		const bool arrivingShotEnd  = (segment + 2 == static_cast<int>(live.size()));

		const double e = ApplyEase(u,
		                           ResolveEase(a.easeOut, leavingShotStart),
		                           ResolveEase(b.easeIn,  arrivingShotEnd));

		// Neighbours for the spline, clamped at the ends.
		const Keyframe& p0 = m_keys[live[segment > 0 ? segment - 1 : segment]];
		const Keyframe& p3 = m_keys[live[segment + 2 < static_cast<int>(live.size()) ? segment + 2 : segment + 1]];

		// Neighbouring segment lengths, for the non-uniform tangents.
		auto liveSegLen = [&](int n) -> double
		{
			if (n < 0 || n + 1 >= static_cast<int>(live.size()))
				return segLen;
			double len = 0.0;
			for (int raw = live[n]; raw < live[n + 1]; ++raw)
				len += EffectiveDuration(raw);
			return std::max(len, kMinDuration);
		};

		const double d0 = liveSegLen(segment - 1);
		const double d1 = segLen;
		const double d2 = liveSegLen(segment + 1);

		const double smoothness = Clamp((a.smoothness + b.smoothness) * 0.5, 0.0, 1.0);

		outPose.location.x = SmoothBlend1D(p0.location.x, a.location.x, b.location.x, p3.location.x, d0, d1, d2, e, smoothness);
		outPose.location.y = SmoothBlend1D(p0.location.y, a.location.y, b.location.y, p3.location.y, d0, d1, d2, e, smoothness);
		outPose.location.z = SmoothBlend1D(p0.location.z, a.location.z, b.location.z, p3.location.z, d0, d1, d2, e, smoothness);

		outPose.fov = static_cast<float>(
			SmoothBlend1D(p0.fov, a.fov, b.fov, p3.fov, d0, d1, d2, e, smoothness));

		// Same spline as fov, so a focus pull eases with the move rather than
		// fighting it. Clamped because the spline can overshoot past the
		// neighbouring keys and a negative or zero focus distance is not a value
		// the renderer can do anything sensible with.
		outPose.focusDistance = static_cast<float>(std::max(
			SmoothBlend1D(p0.focusDistance, a.focusDistance, b.focusDistance, p3.focusDistance,
			              d0, d1, d2, e, smoothness),
			1.0));

		outPose.aperture = static_cast<float>(Clamp(
			SmoothBlend1D(p0.aperture, a.aperture, b.aperture, p3.aperture,
			              d0, d1, d2, e, smoothness),
			0.5, 32.0));

		// Rotation. If either end aims at a target, blend between the two aimed
		// rotations so a look-at key transitions cleanly into a free one.
		const Rot ra = a.lookAt ? LookAtRotation(outPose.location, a.lookAtTarget) : a.rotation;
		const Rot rb = b.lookAt ? LookAtRotation(outPose.location, b.lookAtTarget) : b.rotation;

		if (a.lookAt || b.lookAt)
		{
			// Aimed rotations are already a function of the current position, so
			// splining them would fight the aim -- plain shortest-path blend.
			outPose.rotation = LerpRot(ra, rb, e);
		}
		else
		{
			// Spline each angle in a frame anchored at `a`, so wrapping across
			// +/-180 never sends the camera the long way round.
			auto angleSpline = [&](double a0, double a1, double a2, double a3) -> double
			{
				const double v1 = 0.0;
				const double v2 = DeltaAngle(a1, a2);
				const double v0 = DeltaAngle(a1, a0);
				const double v3 = v2 + DeltaAngle(a2, a3);
				return a1 + SmoothBlend1D(v0, v1, v2, v3, d0, d1, d2, e, smoothness);
			};

			const Rot r0 = p0.lookAt ? LookAtRotation(p0.location, p0.lookAtTarget) : p0.rotation;
			const Rot r3 = p3.lookAt ? LookAtRotation(p3.location, p3.lookAtTarget) : p3.rotation;

			outPose.rotation.pitch = angleSpline(r0.pitch, ra.pitch, rb.pitch, r3.pitch);
			outPose.rotation.yaw   = angleSpline(r0.yaw,   ra.yaw,   rb.yaw,   r3.yaw);
			outPose.rotation.roll  = angleSpline(r0.roll,  ra.roll,  rb.roll,  r3.roll);
		}

		return true;
	}

	// The fade is a state machine, not a set of independent ramps, and that
	// distinction is the whole of the bug this replaced.
	//
	// The old version gave every fade-out key a hard "alpha = 1 for all time after
	// it" and every fade-in key a hard "alpha = 1 for all time before it", then
	// combined every key with max(). That is correct for the two cases it was
	// written for -- a fade-in on the first keyframe, a fade-out on the last -- and
	// wrong for every other one:
	//
	//   * A fade-out anywhere but the end pinned the screen to the fade colour for
	//     the entire rest of the timeline, and no later fade-in could lift it,
	//     because a hard 1.0 always wins a max() against a ramp coming down.
	//   * A fade-in anywhere but the start did the same to everything before it.
	//   * So the obvious way to write a transition -- fade out here, fade back in
	//     there -- blacked out the whole shot from both ends at once. Which also
	//     made every *later* fade look like it did nothing, since the screen was
	//     already at full opacity before it began.
	//   * Both flags on one keyframe, which should dip to the colour and come back,
	//     could never come back for the same reason.
	//
	// So each flag is an *event*: a fade-out covers the picture and it stays
	// covered, a fade-in uncovers it and it stays uncovered. What is on screen at
	// any moment comes from the last event to have started, and from nothing else.
	FadeState Timeline::EvaluateFade(double time) const
	{
		struct Event
		{
			double          start = 0.0;
			double          end   = 0.0;
			bool            cover = false;   // fade-out covers; fade-in uncovers
			const Keyframe* key   = nullptr;
		};

		Event first;         // earliest event anywhere on the timeline
		Event governing;     // latest event to have started by `time`
		bool  haveFirst     = false;
		bool  haveGoverning = false;

		auto consider = [&](const Event& e)
		{
			if (!haveFirst || e.start < first.start)
			{
				first     = e;
				haveFirst = true;
			}

			// `>=` so that when two events start at the same instant the later one
			// in scan order wins -- which is what makes a keyframe carrying both
			// flags dip and recover rather than stopping at the bottom.
			if (e.start <= time && (!haveGoverning || e.start >= governing.start))
			{
				governing     = e;
				haveGoverning = true;
			}
		};

		for (int i = 0; i < static_cast<int>(m_keys.size()); ++i)
		{
			const Keyframe& k = m_keys[i];
			if (!k.enabled || (!k.fadeIn && !k.fadeOut))
				continue;

			const double keyTime = AbsoluteTime(i);

			// Cover before uncover, because a key with both is a dip *through* the
			// colour: the fade-out ramp arrives at the keyframe and the fade-in ramp
			// leaves from it.
			if (k.fadeOut)
			{
				Event e;
				e.start = keyTime - std::max(static_cast<double>(k.fadeOutDuration), 0.0);
				e.end   = keyTime;
				e.cover = true;
				e.key   = &k;
				consider(e);
			}

			if (k.fadeIn)
			{
				Event e;
				e.start = keyTime;
				e.end   = keyTime + std::max(static_cast<double>(k.fadeInDuration), 0.0);
				e.cover = false;
				e.key   = &k;
				consider(e);
			}
		}

		FadeState result;

		auto takeColour = [&result](const Keyframe& k)
		{
			result.color[0] = k.fadeColor[0];
			result.color[1] = k.fadeColor[1];
			result.color[2] = k.fadeColor[2];
		};

		if (!haveGoverning)
		{
			// Nothing has started yet. The shot opens on the colour only when the
			// first thing due to happen is a fade-*in* -- that is what fading in
			// from black means. A shot whose first fade event is a fade-out opens on
			// the picture, as it should.
			if (haveFirst && !first.cover)
			{
				result.alpha = 1.0f;
				takeColour(*first.key);
			}
			return result;
		}

		// A zero-length fade is a cut, so it is already fully applied at its own
		// instant rather than dividing by zero.
		const double span = governing.end - governing.start;
		const double u    = span > 0.0
			? Clamp((time - governing.start) / span, 0.0, 1.0)
			: 1.0;

		result.alpha = static_cast<float>(governing.cover ? u : 1.0 - u);
		result.alpha = Clamp(result.alpha, 0.0f, 1.0f);
		takeColour(*governing.key);
		return result;
	}

	void Timeline::SamplePath(int samplesPerSegment, std::vector<Vec3>& outPoints) const
	{
		outPoints.clear();

		std::vector<int> live;
		BuildEnabled(live);
		if (live.size() < 2)
			return;

		samplesPerSegment = Clamp(samplesPerSegment, 2, 128);

		const double total = AbsoluteTime(live.back());
		const int    steps = samplesPerSegment * (static_cast<int>(live.size()) - 1);

		outPoints.reserve(static_cast<size_t>(steps) + 1);

		CameraPose pose;
		for (int i = 0; i <= steps; ++i)
		{
			const double t = total * (static_cast<double>(i) / steps);
			if (Evaluate(t, pose))
				outPoints.push_back(pose.location);
		}
	}
}
