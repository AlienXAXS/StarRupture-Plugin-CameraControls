#include "ui_viewport.h"
#include "ui_editor.h"
#include "ui_theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace CameraControls::UI::Viewport
{
	using namespace CameraControls::Theme;

	namespace
	{
		// How close the cursor has to get, in pixels, to grab a handle.
		constexpr float kPickRadius = 18.0f;

		// Handles smaller than this are still drawn, just at a floor size, so a
		// keyframe half a kilometre away stays clickable.
		constexpr float kMinHandleRadius = 4.0f;
		constexpr float kMaxHandleRadius = 22.0f;

		// Reference distance at which a handle is drawn at its natural size.
		constexpr double kHandleRefDistance = 2000.0;

		struct Projected
		{
			bool  visible = false;
			float x = 0.0f;
			float y = 0.0f;
			double distance = 0.0;
		};

		// World -> screen for the pose the editor camera is currently at.
		//
		// Unreal's FOVAngle is the *horizontal* field of view for the usual
		// wider-than-tall viewport, so the vertical half-angle is derived from
		// it by the aspect ratio rather than the other way round.
		//
		// `rect` is where the game is actually drawing, which is not the whole
		// window once the view has been squeezed out from under the panels.
		Projected Project(const Vec3& world, const CameraPose& view, const Editor::ScreenRect& rect)
		{
			Projected out;

			if (rect.w < 1.0f || rect.h < 1.0f)
				return out;

			const Vec3 forward = ForwardVector(view.rotation);

			// RightVector() is deliberately roll-free -- it is the fly camera's
			// strafe axis, which should stay horizontal however the shot is
			// tilted. That makes it the wrong basis to project with, so the
			// roll is put back here: Unreal rotates the right and up axes about
			// forward, leaving forward itself alone.
			Vec3 right = RightVector(view.rotation);
			Vec3 up    = Cross(forward, right);

			const double roll = view.rotation.roll * kPi / 180.0;
			if (std::fabs(roll) > 1e-9)
			{
				const double cr = std::cos(roll);
				const double sr = std::sin(roll);

				const Vec3 rolledRight = right * cr + up * sr;
				const Vec3 rolledUp    = up * cr - right * sr;

				right = rolledRight;
				up    = rolledUp;
			}

			const Vec3 relative = world - view.location;

			const double depth = Dot(relative, forward);
			if (depth <= 1.0)
				return out;   // behind the camera (or on the near plane)

			const double tanHalfH = std::tan(Clamp(view.fov, 5.0f, 170.0f) * 0.5 * kPi / 180.0);
			if (tanHalfH < 1e-6)
				return out;

			const double aspect   = static_cast<double>(rect.w) / static_cast<double>(rect.h);
			const double tanHalfV = tanHalfH / aspect;

			const double ndcX = (Dot(relative, right) / depth) / tanHalfH;
			const double ndcY = (Dot(relative, up)    / depth) / tanHalfV;

			out.x = static_cast<float>(rect.x + (0.5 + 0.5 * ndcX) * rect.w);
			out.y = static_cast<float>(rect.y + (0.5 - 0.5 * ndcY) * rect.h);
			out.distance = depth;
			out.visible  = true;
			return out;
		}

		// A visual proof of the projection, drawn on demand.
		//
		// Two markers: a cross at the geometric centre of the picture, and the
		// projection of a point ten metres straight down the camera's forward
		// axis. If the projection is right they sit exactly on top of each
		// other at every angle. If they drift apart the basis or the view rect
		// is wrong; if they stay together but the keyframe handles are still
		// off, it is the FOV or the aspect. Two different bugs, one glance.
		void DrawSelfTest(IModLoaderImGui* ui, const CameraPose& view,
		                  const Editor::ScreenRect& rect, bool usingRenderView)
		{
			PluginDrawList dl = ui->GetForegroundDrawList();

			const float cx = rect.x + rect.w * 0.5f;
			const float cy = rect.y + rect.h * 0.5f;

			const unsigned int centreCol = Pack(ui, kDisabled);
			ui->DL_AddLine(dl, cx - 22.0f, cy, cx + 22.0f, cy, centreCol, 1.0f);
			ui->DL_AddLine(dl, cx, cy - 22.0f, cx, cy + 22.0f, centreCol, 1.0f);

			const Vec3 ahead = view.location + ForwardVector(view.rotation) * 1000.0;
			const Projected p = Project(ahead, view, rect);

			if (p.visible)
			{
				ui->DL_AddCircle(dl, p.x, p.y, 12.0f, Pack(ui, kPlayhead), 20, 2.0f);
				ui->DL_AddLine(dl, cx, cy, p.x, p.y, Pack(ui, kPlayhead), 1.0f);
			}

			char info[160];
			snprintf(info, sizeof(info),
			         "projection self-test\nview %s | fov %.1f | rect %.0f,%.0f %.0fx%.0f\n"
			         "error %.1f px",
			         usingRenderView ? "engine" : "requested",
			         view.fov, rect.x, rect.y, rect.w, rect.h,
			         p.visible ? std::sqrt((p.x - cx) * (p.x - cx) + (p.y - cy) * (p.y - cy)) : -1.0f);
			ui->DL_AddText(dl, rect.x + 12.0f, rect.y + rect.h - 58.0f, Pack(ui, kPlayhead), info);
		}
	}

	void Render(IModLoaderImGui* ui, State& state, double now)
	{
		if (state.mode != Mode::Editor)
			return;

		const Editor::ScreenRect rect = Editor::CurrentGameRect();
		if (rect.w < 1.0f || rect.h < 1.0f)
			return;

		// What the engine is rendering, not what we asked it to render. Falls
		// back to the pose we drove the rig with if the read-back has not
		// happened yet -- which is only ever true for the first frame or two.
		const CameraPose& view = state.renderViewValid ? state.renderView : state.flyPose;

		if (state.options.projectionDebug)
			DrawSelfTest(ui, view, rect, state.renderViewValid);

		if (state.timeline.Empty())
			return;

		// Clicks that land on the timeline or the inspector belong to them.
		if (state.uiHovered)
			return;

		float mouseX = 0.0f, mouseY = 0.0f;
		ui->GetMousePos(&mouseX, &mouseY);

		PluginDrawList dl = ui->GetForegroundDrawList();

		const auto& keys = state.timeline.Keys();

		int   bestIndex = -1;
		float bestDistSq = kPickRadius * kPickRadius;

		// Two passes: find what the cursor is over first, so the handle under it
		// can be drawn differently without a frame of lag.
		std::vector<Projected> projected;
		projected.reserve(keys.size());

		for (int i = 0; i < static_cast<int>(keys.size()); ++i)
		{
			const Projected p = Project(keys[i].location, view, rect);
			projected.push_back(p);

			if (!p.visible ||
			    p.x < rect.x || p.x > rect.x + rect.w ||
			    p.y < rect.y || p.y > rect.y + rect.h)
				continue;

			const float dx = mouseX - p.x;
			const float dy = mouseY - p.y;
			const float distSq = dx * dx + dy * dy;

			// Nearest to the cursor wins; ties broken by whichever is closer to
			// the camera, so a handle in front is picked over one behind it.
			if (distSq < bestDistSq ||
			    (bestIndex >= 0 && distSq < kPickRadius * kPickRadius &&
			     p.distance < projected[bestIndex].distance && distSq <= bestDistSq * 1.2f))
			{
				bestDistSq = std::min(distSq, bestDistSq);
				bestIndex  = i;
			}
		}

		// --- Draw the handles ---------------------------------------------------
		const double phase = std::fmod(now * 1.6, 2.0);
		const float  pulse = static_cast<float>(phase < 1.0 ? phase : 2.0 - phase);

		for (int i = 0; i < static_cast<int>(keys.size()); ++i)
		{
			const Projected& p = projected[i];
			if (!p.visible)
				continue;

			// Clipped to the picture, not to the window: a handle that has left
			// frame must not float about on the matte beside it.
			if (p.x < rect.x || p.x > rect.x + rect.w ||
			    p.y < rect.y || p.y > rect.y + rect.h)
				continue;

			const bool selected = (keys[i].id == state.selectedId &&
			                       state.selection == Selection::Keyframe);
			const bool hovered  = (i == bestIndex);

			// Shrink with distance so the handles read as being in the world,
			// but never below a clickable size.
			const float radius = Clamp(
				static_cast<float>(kHandleRefDistance / p.distance) * 10.0f,
				kMinHandleRadius, kMaxHandleRadius);

			Rgba color = keys[i].enabled ? kKeyframe : kDisabled;
			if (hovered)  color = kKeyframeHover;
			if (selected) color = Rgba{ Lerp(color.r, 1.0f, pulse), Lerp(color.g, 1.0f, pulse),
			                            Lerp(color.b, 1.0f, pulse), 1.0f };

			ui->DL_AddCircleFilled(dl, p.x, p.y, radius, Pack(ui, color, 0.35f), 16);
			ui->DL_AddCircle(dl, p.x, p.y, radius, Pack(ui, color), 16, selected ? 2.5f : 1.5f);

			char label[8];
			snprintf(label, sizeof(label), "%d", i + 1);
			ui->DL_AddText(dl, p.x + radius + 3.0f, p.y - radius, Pack(ui, color), label);
		}

		if (bestIndex < 0)
			return;

		// --- Interaction ---------------------------------------------------------
		char tip[192];
		snprintf(tip, sizeof(tip),
		         "Keyframe %d%s%s\n%.1f m away\n\nClick to select, double-click to fly here",
		         bestIndex + 1,
		         keys[bestIndex].name.empty() ? "" : "  ",
		         keys[bestIndex].name.c_str(),
		         projected[bestIndex].distance / 100.0);
		ui->SetTooltip(tip);

		if (ui->IsMouseDoubleClicked(0))
		{
			state.selectedId = keys[bestIndex].id;
			state.selection  = Selection::Keyframe;
			Post(state, Request::GotoSelected);
		}
		else if (ui->IsMouseClicked(0, false))
		{
			state.selectedId = keys[bestIndex].id;
			state.selection  = Selection::Keyframe;
		}
	}
}
