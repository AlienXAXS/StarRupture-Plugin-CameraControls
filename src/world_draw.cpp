#include "world_draw.h"
#include "plugin_helpers.h"

#include <cmath>
#include <vector>

namespace CameraControls::WorldDraw
{
	namespace
	{
		// Colours, chosen to stay readable against StarRupture's rust-and-dust
		// palette: cyan path, amber keyframes, white-hot selection.
		constexpr PluginDebugColor kPathColor      { 0.20f, 0.85f, 1.00f, 1.0f };
		constexpr PluginDebugColor kKeyColor       { 1.00f, 0.65f, 0.10f, 1.0f };
		constexpr PluginDebugColor kDisabledColor  { 0.45f, 0.45f, 0.45f, 1.0f };
		constexpr PluginDebugColor kPlayheadColor  { 0.35f, 1.00f, 0.35f, 1.0f };
		constexpr PluginDebugColor kLookAtColor    { 1.00f, 0.30f, 0.80f, 1.0f };
		constexpr PluginDebugColor kPlayerColor    { 0.30f, 1.00f, 0.80f, 1.0f };

		// DrawCameraAt's scale is a multiplier, not a length: the modloader
		// multiplies it by a base of 4.0 and 2:1:1.5 proportions. The selection
		// ring, the look-at marker and the playhead axes are the opposite --
		// plain Unreal units. Keeping the two straight matters, because a value
		// that looks sane as a radius is enormous as a multiplier.
		constexpr float kSelectedScaleBoost = 1.35f;

		// Ring radius as a multiple of the camera body's half-length
		// (8 * scale), so the highlight tracks whatever gizmo size is in use.
		constexpr float kSelectionRingFactor = 2.2f;
		constexpr float kBodyHalfLengthPerScale = 8.0f;

		IPluginDebugDraw* Draw()
		{
			auto* hooks = GetHooks();
			if (!hooks || !hooks->HUD || !hooks->HUD->DebugDraw)
				return nullptr;
			return hooks->HUD->DebugDraw;
		}

		PluginDebugVector ToDD(const Vec3& v) { return PluginDebugVector{ v.x, v.y, v.z }; }

		PluginDebugRotator ToDD(const Rot& r)
		{
			return PluginDebugRotator{ r.pitch, r.yaw, r.roll };
		}

		PluginDebugDrawStyle MakeStyle(const PluginDebugColor& color, float thickness)
		{
			PluginDebugDrawStyle style{};
			style.color       = color;
			style.duration    = 0.0f;   // single frame -- redrawn every tick
			style.thickness   = thickness;
			style.bPersistent = false;
			style.bForeground = true;   // the path matters more than the wall in front of it
			return style;
		}

		// 0..1 triangle wave, used to make the selected keyframe breathe so it
		// is findable at a glance in a dense path.
		float Pulse(double now)
		{
			const double phase = std::fmod(now * 1.6, 2.0);
			return static_cast<float>(phase < 1.0 ? phase : 2.0 - phase);
		}
	}

	bool IsAvailable()
	{
		IPluginDebugDraw* dd = Draw();
		return dd && dd->IsAvailable();
	}

	void DrawTimeline(const Timeline& timeline, const DrawParams& params)
	{
		IPluginDebugDraw* dd = Draw();
		if (!dd || !dd->IsAvailable() || timeline.Empty())
			return;

		// --- The path itself -------------------------------------------------
		std::vector<Vec3> path;
		timeline.SamplePath(params.splineSamples, path);

		if (path.size() >= 2)
		{
			const PluginDebugDrawStyle pathStyle = MakeStyle(kPathColor, 3.0f);
			const double cull = static_cast<double>(params.nearCull);

			for (size_t i = 0; i + 1 < path.size(); ++i)
			{
				// Segment-distance rather than endpoint-distance: a long
				// segment can run right past the viewer with both of its ends
				// comfortably far away, and that is exactly the one that fills
				// the screen when it clips the near plane.
				if (DistanceToSegment(params.cameraLocation, path[i], path[i + 1]) < cull)
					continue;

				const PluginDebugVector a = ToDD(path[i]);
				const PluginDebugVector b = ToDD(path[i + 1]);
				dd->DrawLine(&a, &b, &pathStyle);
			}
		}

		// --- Keyframe gizmos ---------------------------------------------------
		const float pulse = Pulse(params.now);

		const auto& keys = timeline.Keys();
		for (int i = 0; i < static_cast<int>(keys.size()); ++i)
		{
			const Keyframe& key = keys[i];
			const bool selected = (key.id == params.selectedId);

			PluginDebugColor color = key.enabled ? kKeyColor : kDisabledColor;
			float scale = params.gizmoScale;

			if (selected)
			{
				// Pulse from the key colour up to white so the highlight reads
				// as "this one" rather than "a different kind of keyframe".
				color.r = Lerp(color.r, 1.0f, pulse);
				color.g = Lerp(color.g, 1.0f, pulse);
				color.b = Lerp(color.b, 1.0f, pulse);

				const float selectedScale = params.gizmoScale * kSelectedScaleBoost;
				scale = Lerp(selectedScale, selectedScale * 1.2f, pulse);
			}

			const PluginDebugDrawStyle style = MakeStyle(color, selected ? 4.0f : 2.0f);

			// The camera frustum gizmo shows position, aim and FOV in one shape.
			const Rot aim = key.lookAt ? LookAtRotation(key.location, key.lookAtTarget) : key.rotation;

			const PluginDebugVector  location = ToDD(key.location);
			const PluginDebugRotator rotation = ToDD(aim);

			// Standing inside a gizmo turns its box and frustum into
			// screen-filling streaks, so drop the geometry when the camera is
			// on top of it. The label below is canvas text and does not smear,
			// so it stays -- being able to read which keyframe you are sitting
			// on is exactly when you most want to know.
			const double distance = (params.cameraLocation - key.location).Length();
			const bool   tooClose = distance < static_cast<double>(params.nearCull);

			if (!tooClose)
			{
				dd->DrawCameraAt(&location, &rotation, key.fov, scale, &style);

				if (selected)
				{
					// A ring around the selection so it is visible even when the
					// frustum is pointing straight at (or away from) the viewer.
					// Radius is in world units, unlike `scale` above.
					const float radius = scale * kBodyHalfLengthPerScale * kSelectionRingFactor;
					dd->DrawSphere(&location, radius, 12, &style);
				}
			}

			// Look-at keys get a line to whatever they are aiming at, so an
			// orbit is obvious from outside.
			if (key.lookAt &&
			    DistanceToSegment(params.cameraLocation, key.location, key.lookAtTarget) >= params.nearCull)
			{
				const PluginDebugDrawStyle aimStyle = MakeStyle(kLookAtColor, 1.5f);
				const PluginDebugVector target = ToDD(key.lookAtTarget);
				dd->DrawLine(&location, &target, &aimStyle);
				dd->DrawPoint(&target, params.gizmoScale * kBodyHalfLengthPerScale, &aimStyle);
			}

			// Index label, so the timeline row and the world gizmo can be
			// matched up without counting along the path.
			char label[64];
			if (key.name.empty())
				snprintf(label, sizeof(label), "%d", i + 1);
			else
				snprintf(label, sizeof(label), "%d  %s", i + 1, key.name.c_str());

			// Duration 0 means "this frame only" for HUD text (the opposite of
			// the line batcher's rule), which is what we want -- it is redrawn
			// every tick anyway.
			dd->DrawString(&location, label, nullptr, &color, 0.0f, selected ? 1.35f : 1.0f);
		}

		// --- Stashed player ------------------------------------------------------
		if (params.showPlayerMarker)
		{
			const double distance = (params.cameraLocation - params.playerLocation).Length();
			if (distance >= static_cast<double>(params.nearCull))
			{
				const PluginDebugDrawStyle style = MakeStyle(kPlayerColor, 2.0f);
				const PluginDebugVector  location = ToDD(params.playerLocation);
				const PluginDebugVector  extent { 45.0, 45.0, 95.0 };   // roughly a person
				const PluginDebugRotator upright { 0.0, 0.0, 0.0 };

				dd->DrawBox(&location, &extent, &upright, &style);
				dd->DrawString(&location, "player", nullptr, &kPlayerColor, 0.0f, 1.0f);
			}
		}

		// --- Playhead ----------------------------------------------------------
		if (params.showPlayhead && timeline.TotalDuration() > 0.0)
		{
			CameraPose pose;
			if (timeline.Evaluate(params.playhead, pose))
			{
				// While scrub preview is on, the playhead pose *is* the camera
				// pose -- drawing it would wrap a frustum around the viewer's
				// own eye. The distance check takes care of that case without
				// needing to know whether preview is active.
				const double distance = (params.cameraLocation - pose.location).Length();
				if (distance >= static_cast<double>(params.nearCull))
				{
					const PluginDebugDrawStyle style = MakeStyle(kPlayheadColor, 3.0f);
					const PluginDebugVector  location = ToDD(pose.location);
					const PluginDebugRotator rotation = ToDD(pose.rotation);

					dd->DrawCameraAt(&location, &rotation, pose.fov, params.gizmoScale * 0.8f, &style);
				}
			}
		}
	}
}
