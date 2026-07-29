#pragma once

// ---------------------------------------------------------------------------
// Plain math types used by the timeline.
//
// Deliberately independent of the game SDK: everything in timeline.h /
// project_io.h / the ImGui layer runs on the render thread, where touching a
// UObject (or anything that pulls in Basic.hpp's FVector) is not safe. The
// game-thread side converts these to SDK::FVector / SDK::FRotator at the
// boundary in camera_rig.cpp.
//
// Units match Unreal: centimetres for positions, degrees for rotations.
// ---------------------------------------------------------------------------

#include <cmath>

namespace CameraControls
{
	constexpr double kPi = 3.14159265358979323846;

	inline float  Lerp(float a, float b, float t)   { return a + (b - a) * t; }
	inline double Lerp(double a, double b, double t) { return a + (b - a) * t; }

	inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
	inline double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
	inline int   Clamp(int v, int lo, int hi)       { return v < lo ? lo : (v > hi ? hi : v); }

	struct Vec3
	{
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;

		Vec3() = default;
		Vec3(double inX, double inY, double inZ) : x(inX), y(inY), z(inZ) {}

		Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
		Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
		Vec3 operator*(double s)      const { return { x * s, y * s, z * s }; }
		Vec3 operator/(double s)      const { return { x / s, y / s, z / s }; }

		Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
		Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
		Vec3& operator*=(double s)      { x *= s; y *= s; z *= s; return *this; }

		double LengthSquared() const { return x * x + y * y + z * z; }
		double Length() const { return std::sqrt(LengthSquared()); }

		Vec3 Normalized() const
		{
			const double len = Length();
			return len > 1e-8 ? Vec3{ x / len, y / len, z / len } : Vec3{};
		}
	};

	inline double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

	inline Vec3 Cross(const Vec3& a, const Vec3& b)
	{
		return { a.y * b.z - a.z * b.y,
		         a.z * b.x - a.x * b.z,
		         a.x * b.y - a.y * b.x };
	}

	inline Vec3 Lerp(const Vec3& a, const Vec3& b, double t)
	{
		return { Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t) };
	}

	// Unreal rotator convention: pitch about Y, yaw about Z, roll about X.
	struct Rot
	{
		double pitch = 0.0;
		double yaw   = 0.0;
		double roll  = 0.0;

		Rot() = default;
		Rot(double inPitch, double inYaw, double inRoll) : pitch(inPitch), yaw(inYaw), roll(inRoll) {}
	};

	// Wraps an angle into (-180, 180].
	inline double NormalizeAngle(double degrees)
	{
		degrees = std::fmod(degrees + 180.0, 360.0);
		if (degrees < 0.0)
			degrees += 360.0;
		return degrees - 180.0;
	}

	// Shortest-path delta from `from` to `to`, so interpolating 350 -> 10 goes
	// forwards through 0 instead of the long way round.
	inline double DeltaAngle(double from, double to) { return NormalizeAngle(to - from); }

	inline double LerpAngle(double a, double b, double t) { return a + DeltaAngle(a, b) * t; }

	inline Rot LerpRot(const Rot& a, const Rot& b, double t)
	{
		return { LerpAngle(a.pitch, b.pitch, t),
		         LerpAngle(a.yaw,   b.yaw,   t),
		         LerpAngle(a.roll,  b.roll,  t) };
	}

	// Unreal's forward vector for a rotator (X forward, Y right, Z up).
	inline Vec3 ForwardVector(const Rot& r)
	{
		const double cp = std::cos(r.pitch * kPi / 180.0);
		const double sp = std::sin(r.pitch * kPi / 180.0);
		const double cy = std::cos(r.yaw   * kPi / 180.0);
		const double sy = std::sin(r.yaw   * kPi / 180.0);
		return { cp * cy, cp * sy, sp };
	}

	// Right vector ignoring roll -- the horizontal strafe axis, which is what a
	// fly-cam wants even when the shot is rolled.
	inline Vec3 RightVector(const Rot& r)
	{
		const double cy = std::cos(r.yaw * kPi / 180.0);
		const double sy = std::sin(r.yaw * kPi / 180.0);
		return { -sy, cy, 0.0 };
	}

	inline Vec3 UpVector(const Rot& r) { return Cross(RightVector(r), ForwardVector(r)) * -1.0; }

	// Shortest distance from `point` to the segment a->b.
	//
	// Used to cull debug lines that pass close to the camera: a line crossing
	// the near plane smears across the whole screen, and testing only the
	// endpoints misses the common case of a long path segment running past the
	// viewer with both ends far away.
	inline double DistanceToSegment(const Vec3& point, const Vec3& a, const Vec3& b)
	{
		const Vec3   ab     = b - a;
		const double lenSq  = ab.LengthSquared();

		if (lenSq < 1e-8)
			return (point - a).Length();

		const double t = Clamp(Dot(point - a, ab) / lenSq, 0.0, 1.0);
		return (point - (a + ab * t)).Length();
	}

	// Rotator that looks from `from` towards `to`. Roll is always zero -- callers
	// that want a rolled shot set it themselves afterwards.
	inline Rot LookAtRotation(const Vec3& from, const Vec3& to)
	{
		const Vec3 dir = to - from;
		const double horizontal = std::sqrt(dir.x * dir.x + dir.y * dir.y);
		if (horizontal < 1e-6 && std::abs(dir.z) < 1e-6)
			return {};

		Rot r;
		r.yaw   = std::atan2(dir.y, dir.x) * 180.0 / kPi;
		r.pitch = std::atan2(dir.z, horizontal) * 180.0 / kPi;
		r.roll  = 0.0;
		return r;
	}
}
