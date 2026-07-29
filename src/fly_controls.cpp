#include "fly_controls.h"

#include <algorithm>

namespace CameraControls::Fly
{
	namespace
	{
		// Retained between frames so movement eases in and out instead of
		// snapping between full speed and a dead stop.
		Vec3 g_velocity;

		constexpr double kAccelPerSecond = 8.0;   // how fast we reach target speed
		constexpr double kBoostFactor    = 4.0;
		constexpr double kCrawlFactor    = 0.15;
		constexpr double kRollDegPerSec  = 45.0;
		constexpr double kFovDegPerSec   = 25.0;
		constexpr double kPitchLimit     = 89.0;  // stop short of gimbal flip
	}

	void ResetMomentum()
	{
		g_velocity = Vec3{};
	}

	void Integrate(CameraPose& pose, FlyInput& input, const Options& options, double dt)
	{
		dt = Clamp(dt, 0.0, 0.1);   // a hitch must not fling the camera across the map

		// --- Look ------------------------------------------------------------
		pose.rotation.yaw   = NormalizeAngle(pose.rotation.yaw + input.yawDelta);
		pose.rotation.pitch = Clamp(pose.rotation.pitch + input.pitchDelta, -kPitchLimit, kPitchLimit);
		input.ClearDeltas();

		// --- Held roll / zoom keys -------------------------------------------
		pose.rotation.roll = NormalizeAngle(
			pose.rotation.roll + input.rollAxis * kRollDegPerSec * dt);

		pose.fov = Clamp(
			pose.fov + static_cast<float>(input.fovAxis * kFovDegPerSec * dt), 5.0f, 170.0f);

		// --- Movement --------------------------------------------------------
		double speed = options.flySpeed;
		if (input.boost) speed *= kBoostFactor;
		if (input.crawl) speed *= kCrawlFactor;

		Vec3 wish;
		wish += ForwardVector(pose.rotation) * static_cast<double>(input.forward);
		wish += RightVector(pose.rotation)   * static_cast<double>(input.right);
		wish += Vec3{ 0.0, 0.0, 1.0 }        * static_cast<double>(input.up);

		if (wish.LengthSquared() > 1e-8)
			wish = wish.Normalized() * speed;
		else
			wish = Vec3{};

		const double blend = Clamp(kAccelPerSecond * dt, 0.0, 1.0);
		g_velocity = Lerp(g_velocity, wish, blend);

		// Below a millimetre per second the camera is stopped; zero it so a
		// keyframe captured after releasing the keys is exactly where it looks.
		if (g_velocity.LengthSquared() < 0.1)
			g_velocity = Vec3{};

		pose.location += g_velocity * dt;
	}
}
