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
		m_nextId = 1;
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

		if (live.size() == 1)
		{
			const Keyframe& only = m_keys[live[0]];
			outPose.location = only.location;
			outPose.rotation = only.lookAt ? LookAtRotation(only.location, only.lookAtTarget) : only.rotation;
			outPose.fov      = only.fov;
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

	FadeState Timeline::EvaluateFade(double time) const
	{
		FadeState result;

		for (int i = 0; i < static_cast<int>(m_keys.size()); ++i)
		{
			const Keyframe& k = m_keys[i];
			if (!k.enabled || (!k.fadeIn && !k.fadeOut))
				continue;

			const double keyTime = AbsoluteTime(i);
			float alpha = 0.0f;

			if (k.fadeOut && k.fadeOutDuration > 0.0f)
			{
				const double start = keyTime - k.fadeOutDuration;
				if (time >= start && time <= keyTime)
					alpha = std::max(alpha, static_cast<float>((time - start) / k.fadeOutDuration));
			}

			if (k.fadeIn && k.fadeInDuration > 0.0f)
			{
				const double end = keyTime + k.fadeInDuration;
				if (time >= keyTime && time <= end)
					alpha = std::max(alpha, static_cast<float>(1.0 - (time - keyTime) / k.fadeInDuration));
			}

			// Before the first fade-in key, and after the last fade-out key,
			// hold fully opaque so the shot starts and ends on the fade colour
			// rather than snapping.
			if (k.fadeIn && time < keyTime)
				alpha = std::max(alpha, 1.0f);
			if (k.fadeOut && time > keyTime)
				alpha = std::max(alpha, 1.0f);

			if (alpha > result.alpha)
			{
				result.alpha    = alpha;
				result.color[0] = k.fadeColor[0];
				result.color[1] = k.fadeColor[1];
				result.color[2] = k.fadeColor[2];
			}
		}

		result.alpha = Clamp(result.alpha, 0.0f, 1.0f);
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
