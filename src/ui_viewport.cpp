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
		// Handle size against distance.
		//
		// These are UI furniture, not objects in the world. A true 1/distance
		// falloff is the honest perspective law and it is the wrong one here: a
		// camera path routinely spans hundreds of metres, and 1/d has the handle
		// down to a few pixels by the time you have backed off far enough to see
		// the whole shot -- which is exactly when you want to click one.
		//
		// So the falloff is 1/sqrt(distance): enough of a depth cue to tell which
		// handles are nearer, nowhere near enough to lose them. The floor is a
		// comfortably clickable dot rather than a speck.
		constexpr float  kMinHandleRadius   = 9.0f;
		constexpr float  kMaxHandleRadius   = 26.0f;
		constexpr double kHandleRefDistance = 4000.0;   // 40 m -> natural size
		constexpr float  kHandleRefRadius   = 14.0f;

		// Slack around the drawn circle for grabbing it. The pick target tracks
		// the handle you can see, plus a little -- a fixed radius unrelated to the
		// drawing means either invisible dots that are somehow clickable or fat
		// circles that are not.
		constexpr float kPickSlack     = 5.0f;
		constexpr float kMinPickRadius = 14.0f;

		// One drawable handle: a screen position the *engine* projected, paired
		// back up with the keyframe it belongs to.
		struct Handle
		{
			int    index      = 0;      // into Timeline::Keys()
			float  x          = 0.0f;
			float  y          = 0.0f;
			double distance   = 0.0;
			float  radius     = kMinHandleRadius;
			float  pickRadius = kMinPickRadius;
		};

		float HandleRadius(double distance)
		{
			const double d = distance > 1.0 ? distance : 1.0;
			return Clamp(kHandleRefRadius * static_cast<float>(std::sqrt(kHandleRefDistance / d)),
			             kMinHandleRadius, kMaxHandleRadius);
		}

		// Turns the game thread's projected positions into handles, dropping
		// anything that is off the picture or whose keyframe has since gone away.
		void CollectHandles(const State& state, const Editor::ScreenRect& rect,
		                    std::vector<Handle>& out)
		{
			out.clear();
			out.reserve(state.keyScreen.size());

			for (const KeyScreen& projected : state.keyScreen)
			{
				// Matched by id, not by position in the list: the UI may have
				// added or deleted a keyframe since the tick that projected these,
				// and an index would then label the wrong handle.
				const int index = state.timeline.IndexOf(projected.id);
				if (index < 0)
					continue;

				// Clipped to the picture, not to the window: a handle that has
				// left frame must not float about on the matte beside it.
				if (projected.x < rect.x || projected.x > rect.x + rect.w ||
				    projected.y < rect.y || projected.y > rect.y + rect.h)
					continue;

				Handle handle;
				handle.index      = index;
				handle.x          = projected.x;
				handle.y          = projected.y;
				handle.distance   = projected.distance;
				handle.radius     = HandleRadius(projected.distance);
				handle.pickRadius = std::max(handle.radius + kPickSlack, kMinPickRadius);
				out.push_back(handle);
			}
		}

		// Which handle the cursor is over, or -1. Index into `handles`.
		int PickHandle(const std::vector<Handle>& handles, float mouseX, float mouseY)
		{
			int   best      = -1;
			float bestScore = 1.0f;

			for (int i = 0; i < static_cast<int>(handles.size()); ++i)
			{
				const Handle& h = handles[i];

				const float dx = mouseX - h.x;
				const float dy = mouseY - h.y;

				// Scored as a fraction of the handle's own pick radius, not in raw
				// pixels. Handles do not all have the same size, and unnormalised
				// pixels would hand every contest to the biggest -- so grazing a
				// fat near handle would beat sitting dead centre on a small far
				// one.
				const float score = (dx * dx + dy * dy) / (h.pickRadius * h.pickRadius);
				if (score > 1.0f)
					continue;

				// Best score wins; near-ties go to whichever is closer to the
				// camera, so a handle in front is picked over one behind it.
				if (best < 0 || score < bestScore ||
				    (score < bestScore * 1.25f && h.distance < handles[best].distance))
				{
					bestScore = std::min(score, bestScore);
					best      = i;
				}
			}

			return best;
		}

		// A readout of the things that decide where a handle lands, drawn on
		// demand.
		//
		// The projection itself is no longer ours to get wrong -- the engine does
		// it. What is still ours is the rectangle we clip to and the assumption
		// that the picture on screen is where we think it is, so that is what this
		// reports: the rect, the window, and whether the viewport squeeze actually
		// took. If "squeeze" says off while the picture is clearly inset (or the
		// other way round) then the rect below is not the engine's and handles are
		// being clipped against the wrong frame.
		void DrawSelfTest(IModLoaderImGui* ui, const State& state,
		                  const Editor::ScreenRect& rect, int handleCount)
		{
			PluginDrawList dl = ui->GetForegroundDrawList();

			const float cx = rect.x + rect.w * 0.5f;
			const float cy = rect.y + rect.h * 0.5f;

			const unsigned int centreCol = Pack(ui, kDisabled);
			ui->DL_AddLine(dl, cx - 22.0f, cy, cx + 22.0f, cy, centreCol, 1.0f);
			ui->DL_AddLine(dl, cx, cy - 22.0f, cx, cy + 22.0f, centreCol, 1.0f);

			float screenW = 0.0f, screenH = 0.0f;
			ui->GetDisplaySize(&screenW, &screenH);

			char info[256];
			snprintf(info, sizeof(info),
			         "projection (engine)\n"
			         "rect %.0f,%.0f %.0fx%.0f | window %.0fx%.0f\n"
			         "squeeze %s | supported %s\n"
			         "projected %d of %d | on picture %d",
			         rect.x, rect.y, rect.w, rect.h, screenW, screenH,
			         state.gameViewApplied ? "on" : "OFF",
			         state.gameViewSupported ? "yes" : "NO",
			         static_cast<int>(state.keyScreen.size()), state.timeline.Count(),
			         handleCount);
			ui->DL_AddText(dl, rect.x + 12.0f, rect.y + rect.h - 74.0f, Pack(ui, kPlayhead), info);
		}
	}

	void Render(IModLoaderImGui* ui, State& state, double now)
	{
		if (state.mode != Mode::Editor)
			return;

		const Editor::ScreenRect rect = Editor::CurrentGameRect();
		if (rect.w < 1.0f || rect.h < 1.0f)
			return;

		std::vector<Handle> handles;
		CollectHandles(state, rect, handles);

		if (state.options.projectionDebug)
			DrawSelfTest(ui, state, rect, static_cast<int>(handles.size()));

		if (handles.empty())
			return;

		// Clicks that land on the timeline or the inspector belong to them. The
		// handles are still drawn -- only the picking is suppressed.
		float mouseX = 0.0f, mouseY = 0.0f;
		ui->GetMousePos(&mouseX, &mouseY);

		const int hovered = state.uiHovered ? -1 : PickHandle(handles, mouseX, mouseY);

		// --- Draw the handles ---------------------------------------------------
		PluginDrawList dl = ui->GetForegroundDrawList();

		const double phase = std::fmod(now * 1.6, 2.0);
		const float  pulse = static_cast<float>(phase < 1.0 ? phase : 2.0 - phase);

		const auto& keys = state.timeline.Keys();

		for (int i = 0; i < static_cast<int>(handles.size()); ++i)
		{
			const Handle&   h   = handles[i];
			const Keyframe& key = keys[h.index];

			const bool selected = (key.id == state.selectedId &&
			                       state.selection == Selection::Keyframe);

			Rgba color = key.enabled ? kKeyframe : kDisabled;
			if (i == hovered) color = kKeyframeHover;
			if (selected)     color = Rgba{ Lerp(color.r, 1.0f, pulse), Lerp(color.g, 1.0f, pulse),
			                                Lerp(color.b, 1.0f, pulse), 1.0f };

			// Enough segments not to read as a polygon at the larger sizes.
			const int segments = h.radius > 14.0f ? 24 : 16;

			ui->DL_AddCircleFilled(dl, h.x, h.y, h.radius, Pack(ui, color, 0.35f), segments);
			ui->DL_AddCircle(dl, h.x, h.y, h.radius, Pack(ui, color), segments,
			                 selected ? 2.5f : 1.5f);

			char label[8];
			snprintf(label, sizeof(label), "%d", h.index + 1);
			ui->DL_AddText(dl, h.x + h.radius + 3.0f, h.y - h.radius, Pack(ui, color), label);
		}

		if (hovered < 0)
			return;

		// --- Interaction ---------------------------------------------------------
		const Handle&   picked = handles[hovered];
		const Keyframe& key    = keys[picked.index];

		char tip[192];
		snprintf(tip, sizeof(tip),
		         "Keyframe %d%s%s\n%.1f m away\n\nClick to select, double-click to fly here",
		         picked.index + 1,
		         key.name.empty() ? "" : "  ",
		         key.name.c_str(),
		         picked.distance / 100.0);
		Tooltip(ui, tip);

		if (ui->IsMouseDoubleClicked(0))
		{
			state.selectedId = key.id;
			state.selection  = Selection::Keyframe;
			Post(state, Request::GotoSelected);
		}
		else if (ui->IsMouseClicked(0, false))
		{
			state.selectedId = key.id;
			state.selection  = Selection::Keyframe;
		}
	}
}
