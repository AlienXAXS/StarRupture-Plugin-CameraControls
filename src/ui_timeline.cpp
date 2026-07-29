#include "ui_timeline.h"
#include "ui_theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace CameraControls::UI::TimelineView
{
	using namespace CameraControls::Theme;

	namespace
	{
		constexpr float kRulerHeight   = 22.0f;
		constexpr float kTrackPadding  = 6.0f;
		constexpr float kKeyHalfWidth  = 7.0f;
		constexpr float kGrabPixels    = 9.0f;   // click tolerance around a keyframe

		// Tick spacings the ruler is allowed to use, so labels land on times a
		// person would actually name rather than 0.37s intervals.
		constexpr double kTickLadder[] = {
			0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0
		};

		double ChooseTickStep(double pixelsPerSecond, float minPixelsPerTick)
		{
			for (double step : kTickLadder)
			{
				if (step * pixelsPerSecond >= minPixelsPerTick)
					return step;
			}
			return kTickLadder[sizeof(kTickLadder) / sizeof(kTickLadder[0]) - 1];
		}

		float TimeToX(double t, float originX, const ViewState& view)
		{
			return originX + static_cast<float>((t - view.scrollSeconds) * view.pixelsPerSecond);
		}

		double XToTime(float x, float originX, const ViewState& view)
		{
			return view.scrollSeconds + (x - originX) / view.pixelsPerSecond;
		}

		void DrawDiamond(IModLoaderImGui* ui, PluginDrawList dl,
		                 float cx, float cy, float half, unsigned int fill, unsigned int outline)
		{
			ui->DL_AddQuadFilled(dl, cx, cy - half, cx + half, cy, cx, cy + half, cx - half, cy, fill);
			ui->DL_AddQuad(dl, cx, cy - half, cx + half, cy, cx, cy + half, cx - half, cy, outline, 1.5f);
		}
	}

	void Render(IModLoaderImGui* ui, State& state, ViewState& view, double now)
	{
		Timeline& timeline = state.timeline;

		float availX = 0.0f, availY = 0.0f;
		ui->GetContentRegionAvail(&availX, &availY);

		if (availX < 40.0f || availY < 40.0f)
			return;

		float originX = 0.0f, originY = 0.0f;
		ui->GetCursorScreenPos(&originX, &originY);

		const float trackTop    = originY + kRulerHeight;
		const float trackHeight = std::max(availY - kRulerHeight - kTrackPadding, 30.0f);
		const float trackBottom = trackTop + trackHeight;
		const float rightEdge   = originX + availX;

		const double total = timeline.TotalDuration();

		// Frame the whole shot the first time the editor opens, and whenever
		// the caller asks for it (the "Fit" button).
		if (view.pendingFit)
		{
			view.pendingFit   = false;
			view.scrollSeconds = 0.0;
			view.pixelsPerSecond = total > 0.01
				? Clamp(static_cast<double>(availX - 40.0f) / total, 4.0, 600.0)
				: 90.0;
		}

		PluginDrawList dl = ui->GetWindowDrawList();

		// One invisible button covers the whole strip so the mouse state below
		// is scoped to this widget instead of the whole window.
		ui->InvisibleButton("##cc_timeline", availX, availY);
		const bool hovered = ui->IsItemHovered();
		const bool active  = ui->IsItemActive();

		float mouseX = 0.0f, mouseY = 0.0f;
		ui->GetMousePos(&mouseX, &mouseY);

		// --- Background --------------------------------------------------------
		ui->DL_AddRectFilled(dl, originX, originY, rightEdge, trackBottom,
		                     Pack(ui, kTrackBg), 4.0f, PluginDrawFlags_RoundCornersAll);

		// --- Ruler -------------------------------------------------------------
		{
			const double step = ChooseTickStep(view.pixelsPerSecond, 70.0f);
			const double firstTick = std::floor(view.scrollSeconds / step) * step;
			const unsigned int rulerCol = Pack(ui, kRuler);
			const unsigned int lineCol  = Pack(ui, kTrackLine);

			for (double t = firstTick; ; t += step)
			{
				const float x = TimeToX(t, originX, view);
				if (x > rightEdge) break;
				if (x < originX - 40.0f) continue;

				ui->DL_AddLine(dl, x, originY + kRulerHeight - 5.0f, x, trackBottom, lineCol, 1.0f);

				char label[32];
				FormatTime(t, label, sizeof(label));
				ui->DL_AddText(dl, x + 3.0f, originY + 2.0f, rulerCol, label);
			}

			// Minor ticks, at a fifth of the labelled step.
			const double minor = step / 5.0;
			for (double t = firstTick; ; t += minor)
			{
				const float x = TimeToX(t, originX, view);
				if (x > rightEdge) break;
				if (x < originX) continue;
				ui->DL_AddLine(dl, x, originY + kRulerHeight - 3.0f, x, originY + kRulerHeight,
				               lineCol, 1.0f);
			}
		}

		// --- Segments ----------------------------------------------------------
		const auto& keys = timeline.Keys();
		const int   count = static_cast<int>(keys.size());

		const float clipTop    = trackTop + 8.0f;
		const float clipBottom = trackBottom - 22.0f;
		const float keyRowY    = clipBottom + 11.0f;

		for (int i = 0; i + 1 < count; ++i)
		{
			const double t0 = timeline.AbsoluteTime(i);
			const double t1 = timeline.AbsoluteTime(i + 1);

			float x0 = TimeToX(t0, originX, view);
			float x1 = TimeToX(t1, originX, view);
			if (x1 < originX || x0 > rightEdge)
				continue;

			x0 = std::max(x0, originX);
			x1 = std::min(x1, rightEdge);

			const bool live = keys[i].enabled && keys[i + 1].enabled;

			// Speed reads as colour temperature: at 1x the clip is the plain
			// accent, faster runs hotter, slower runs cooler.
			const float speed = keys[i].speed;
			Rgba fill = live ? kAccentDim : kDisabled;
			if (live && speed > 1.01f)
			{
				const float k = Clamp((speed - 1.0f) / 3.0f, 0.0f, 1.0f);
				fill = Rgba{ Lerp(fill.r, 0.80f, k), Lerp(fill.g, 0.35f, k), Lerp(fill.b, 0.20f, k), 1.0f };
			}
			else if (live && speed < 0.99f)
			{
				const float k = Clamp((1.0f - speed) / 0.9f, 0.0f, 1.0f);
				fill = Rgba{ Lerp(fill.r, 0.25f, k), Lerp(fill.g, 0.25f, k), Lerp(fill.b, 0.65f, k), 1.0f };
			}

			const bool segmentSelected =
				state.selection == Selection::Segment && state.selectedId == keys[i].id;

			ui->DL_AddRectFilled(dl, x0 + 1.0f, clipTop, x1 - 1.0f, clipBottom,
			                     Pack(ui, fill, segmentSelected ? 1.0f : 0.75f),
			                     3.0f, PluginDrawFlags_RoundCornersAll);

			if (segmentSelected)
			{
				ui->DL_AddRect(dl, x0 + 1.0f, clipTop, x1 - 1.0f, clipBottom,
				               Pack(ui, kSelected), 3.0f, PluginDrawFlags_RoundCornersAll, 2.0f);
			}

			// Only label a clip that has room for it -- half a word of text is
			// worse than none.
			if (x1 - x0 > 74.0f)
			{
				char label[64];
				snprintf(label, sizeof(label), "%.2fs  %.2fx",
				         timeline.EffectiveDuration(i), speed);
				ui->DL_AddText(dl, x0 + 6.0f, clipTop + 4.0f, Pack(ui, kSelected, 0.85f), label);
			}

			// Fade ribbons, drawn along the top edge of the clip they cover.
			auto drawFade = [&](double from, double to, const char* tag)
			{
				const float fx0 = std::max(TimeToX(from, originX, view), originX);
				const float fx1 = std::min(TimeToX(to,   originX, view), rightEdge);
				if (fx1 <= fx0) return;

				ui->DL_AddRectFilled(dl, fx0, clipTop, fx1, clipTop + 5.0f,
				                     Pack(ui, kFadeMarker, 0.9f), 2.0f, PluginDrawFlags_RoundCornersAll);
				if (fx1 - fx0 > 30.0f)
					ui->DL_AddText(dl, fx0 + 2.0f, clipTop + 6.0f, Pack(ui, kFadeMarker), tag);
			};

			if (keys[i].fadeIn)
				drawFade(t0, t0 + keys[i].fadeInDuration, "fade in");
			if (keys[i + 1].fadeOut)
				drawFade(t1 - keys[i + 1].fadeOutDuration, t1, "fade out");
		}

		// --- Keyframe markers ---------------------------------------------------
		int    hoveredIndex = -1;
		double hoveredDist  = 1e9;

		for (int i = 0; i < count; ++i)
		{
			const float x = TimeToX(timeline.AbsoluteTime(i), originX, view);
			if (x < originX - kGrabPixels || x > rightEdge + kGrabPixels)
				continue;

			if (hovered)
			{
				const double dist = std::abs(mouseX - x);
				if (dist <= kGrabPixels && dist < hoveredDist && mouseY >= clipTop - 4.0f)
				{
					hoveredDist  = dist;
					hoveredIndex = i;
				}
			}
		}

		for (int i = 0; i < count; ++i)
		{
			const float x = TimeToX(timeline.AbsoluteTime(i), originX, view);
			if (x < originX - kGrabPixels || x > rightEdge + kGrabPixels)
				continue;

			const bool selected = (state.selectedId == keys[i].id &&
			                       state.selection == Selection::Keyframe);

			Rgba fill = keys[i].enabled ? kKeyframe : kDisabled;
			if (i == hoveredIndex)
				fill = kKeyframeHover;

			float half = kKeyHalfWidth;
			if (selected)
			{
				// Same breathing highlight as the world gizmo, so the two read
				// as one selection rather than two unrelated highlights.
				const double phase = std::fmod(now * 1.6, 2.0);
				const float  pulse = static_cast<float>(phase < 1.0 ? phase : 2.0 - phase);
				fill = Rgba{ Lerp(fill.r, 1.0f, pulse), Lerp(fill.g, 1.0f, pulse),
				             Lerp(fill.b, 1.0f, pulse), 1.0f };
				half += 2.0f * pulse;
			}

			// A stem down the whole track makes it easy to line a keyframe up
			// with the ruler above it.
			ui->DL_AddLine(dl, x, clipTop, x, keyRowY, Pack(ui, fill, 0.5f), 1.0f);
			DrawDiamond(ui, dl, x, keyRowY, half, Pack(ui, fill),
			            Pack(ui, selected ? kSelected : kTrackBg));

			char index[8];
			snprintf(index, sizeof(index), "%d", i + 1);
			ui->DL_AddText(dl, x - 3.0f, keyRowY + half + 1.0f, Pack(ui, fill, 0.9f), index);
		}

		// --- Playhead ------------------------------------------------------------
		{
			const float x = TimeToX(state.playhead, originX, view);
			if (x >= originX - 2.0f && x <= rightEdge + 2.0f)
			{
				const unsigned int col = Pack(ui, kPlayhead);
				ui->DL_AddLine(dl, x, originY, x, trackBottom, col, 2.0f);
				ui->DL_AddTriangleFilled(dl, x - 6.0f, originY, x + 6.0f, originY, x, originY + 9.0f, col);
			}
		}

		// --- Interaction ----------------------------------------------------------
		if (hovered && ui->IsMouseClicked(0, false))
		{
			if (hoveredIndex >= 0)
			{
				state.selectedId = keys[hoveredIndex].id;
				state.selection  = Selection::Keyframe;

				// Only the keys after the first can be retimed by dragging --
				// the first one defines t = 0.
				if (hoveredIndex > 0)
					view.draggingKey = keys[hoveredIndex].id;
			}
			else if (mouseY < trackTop)
			{
				view.draggingPlayhead = true;
				state.playhead = Clamp(XToTime(mouseX, originX, view), 0.0, std::max(total, 0.0));
				state.playing  = false;
			}
			else
			{
				// Clicking the body of a clip selects the segment, which is what
				// the property panel needs to edit duration and speed.
				const double t = XToTime(mouseX, originX, view);
				int segment = -1;
				for (int i = 0; i + 1 < count; ++i)
				{
					if (t >= timeline.AbsoluteTime(i) && t < timeline.AbsoluteTime(i + 1))
					{
						segment = i;
						break;
					}
				}

				if (segment >= 0)
				{
					state.selectedId = keys[segment].id;
					state.selection  = Selection::Segment;
				}
				else
				{
					state.selection  = Selection::None;
					state.selectedId = 0;
				}
			}
		}

		// Double-clicking a keyframe puts the camera there, so the shot can be
		// picked back up from an existing pose.
		if (hovered && ui->IsMouseDoubleClicked(0))
		{
			if (hoveredIndex >= 0)
			{
				state.selectedId = keys[hoveredIndex].id;
				state.selection  = Selection::Keyframe;
				view.draggingKey = 0;
				Post(state, Request::GotoSelected);
			}
			else if (mouseY >= trackTop)
			{
				// Double-clicking the body of a clip selects the keyframe that
				// starts it. The clip's own properties are a click away, but
				// the thing you actually want to edit after looking at a stretch
				// of the move is the pose it leaves from.
				const double t = XToTime(mouseX, originX, view);
				int segment = -1;
				for (int i = 0; i + 1 < count; ++i)
				{
					if (t >= timeline.AbsoluteTime(i) && t < timeline.AbsoluteTime(i + 1))
					{
						segment = i;
						break;
					}
				}

				if (segment >= 0)
				{
					state.selectedId = keys[segment].id;
					state.selection  = Selection::Keyframe;
					view.draggingKey = 0;
				}
			}
		}

		if (view.draggingKey != 0)
		{
			if (!ui->IsMouseDown(0))
			{
				view.draggingKey = 0;
			}
			else
			{
				const int index = timeline.IndexOf(view.draggingKey);
				if (index > 0)
				{
					timeline.SetAbsoluteTime(index, XToTime(mouseX, originX, view),
					                         state.options.rippleEdits);
					state.dirty = true;
				}
			}
		}

		if (view.draggingPlayhead)
		{
			if (!ui->IsMouseDown(0))
				view.draggingPlayhead = false;
			else
				state.playhead = Clamp(XToTime(mouseX, originX, view), 0.0, std::max(total, 0.0));
		}

		// Middle-drag pans, the same as every other timeline in existence.
		if (hovered && ui->IsMouseDragging(2, -1.0f))
		{
			float dx = 0.0f, dy = 0.0f;
			ui->GetMouseDragDelta(2, -1.0f, &dx, &dy);
			ui->ResetMouseDragDelta(2);
			view.scrollSeconds -= dx / view.pixelsPerSecond;
		}

		// Wheel zooms about the cursor, so the moment you are looking at stays
		// under the pointer instead of sliding away as the scale changes.
		if (hovered)
		{
			const float wheel = ui->GetMouseWheel();
			if (wheel != 0.0f)
			{
				const double anchorTime = XToTime(mouseX, originX, view);
				const double factor     = std::pow(1.25, static_cast<double>(wheel));

				view.pixelsPerSecond = Clamp(view.pixelsPerSecond * factor, 3.0, 2000.0);
				view.scrollSeconds   = anchorTime - (mouseX - originX) / view.pixelsPerSecond;
			}
		}

		// Never scroll so far that the timeline leaves the window entirely.
		view.scrollSeconds = Clamp(view.scrollSeconds, -1.0, std::max(total, 1.0));

		if (hovered && hoveredIndex >= 0)
		{
			char tip[256];
			const Keyframe& k = keys[hoveredIndex];
			snprintf(tip, sizeof(tip),
			         "Keyframe %d%s%s\n"
			         "t = %.2fs   FOV %.0f\n"
			         "%.0f, %.0f, %.0f\n\n"
			         "Click to select, drag to retime\n"
			         "Double-click to fly the camera here",
			         hoveredIndex + 1,
			         k.name.empty() ? "" : "  ",
			         k.name.c_str(),
			         timeline.AbsoluteTime(hoveredIndex), k.fov,
			         k.location.x, k.location.y, k.location.z);
			ui->SetTooltip(tip);
		}
	}
}
