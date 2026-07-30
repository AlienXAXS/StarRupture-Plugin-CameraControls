#include "ui_timeline.h"
#include "ui_theme.h"
#include "input_binds.h"

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

		// The band along the top of the track that func frames hang from, above
		// the clips. Keyframes hang below the clips; the two never share a row,
		// which is what lets a single click be unambiguous without any modifier.
		constexpr float kFuncRowHeight = 18.0f;
		constexpr float kFuncHalfWidth = 6.0f;

		// One popup id for the whole track. ImGui matches OpenPopup to BeginPopup
		// by string, so this has to be spelled the same in both places.
		constexpr const char* kContextPopup = "##cc_timeline_ctx";

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

		// A downward pennant, so a func frame reads as hanging from the top of
		// the track rather than standing on the bottom of it like a keyframe.
		void DrawPennant(IModLoaderImGui* ui, PluginDrawList dl,
		                 float cx, float cy, float half, unsigned int fill, unsigned int outline)
		{
			const float top = cy - half;
			const float bot = cy + half;

			ui->DL_AddTriangleFilled(dl, cx - half, top, cx + half, top, cx, bot, fill);
			ui->DL_AddLine(dl, cx - half, top, cx + half, top, outline, 1.5f);
			ui->DL_AddLine(dl, cx + half, top, cx, bot, outline, 1.5f);
			ui->DL_AddLine(dl, cx, bot, cx - half, top, outline, 1.5f);
		}

		// The word drawn beside a func marker. The frame's own name wins when it
		// has one, because someone who bothered to name a cue meant that name to
		// be the label.
		const char* FuncLabel(const FuncFrame& frame)
		{
			if (!frame.name.empty())
				return frame.name.c_str();

			switch (frame.action)
			{
				case FuncAction::StartRupture:       return "start";
				case FuncAction::SetRupturePhase:    return "phase";
				case FuncAction::SetRuptureProgress: return "advance";
				case FuncAction::CancelRupture:      return "cancel";
				case FuncAction::PauseRupture:       return "pause";
				case FuncAction::ResumeRupture:      return "resume";
				default:                             return "cue";
			}
		}

		// Records where everything that is about to move currently sits, so the
		// drag can be replayed as "original + delta" every frame.
		//
		// `grabbed` is what the cursor actually went down on. If it is part of a
		// multi-selection the whole set comes along; otherwise the drag is just
		// that one thing, whatever else happens to be selected.
		void BeginDrag(const State& state, ViewState& view, uint32_t grabbed, double grabTime)
		{
			view.dragOrigins.clear();
			view.dragGrabTime = grabTime;

			const bool group = !state.multiSelection.empty() && IsSelected(state, grabbed);

			auto record = [&](uint32_t id)
			{
				if (const Keyframe* key = state.timeline.Find(id))
				{
					const int index = state.timeline.IndexOf(key->id);
					view.dragOrigins.push_back({ id, state.timeline.AbsoluteTime(index) });
				}
				else if (const FuncFrame* frame = state.timeline.FindFunc(id))
				{
					view.dragOrigins.push_back({ id, frame->time });
				}
			};

			if (group)
			{
				for (uint32_t id : state.multiSelection)
					record(id);
			}
			else
			{
				record(grabbed);
			}
		}

		// Applies a drag: every recorded item back to its original time plus the
		// distance the cursor has travelled.
		//
		// Keyframes are applied in ascending time order because retiming one works
		// by setting the duration of the segment *leading* to it -- so its
		// predecessor has to be final before its turn comes. Descending order
		// would have each key fighting the one before it.
		bool ApplyDrag(Timeline& timeline, const ViewState& view, bool ripple, double cursorTime)
		{
			if (view.dragOrigins.empty())
				return false;

			const double delta = cursorTime - view.dragGrabTime;

			std::vector<const ViewState::DragOrigin*> ordered;
			ordered.reserve(view.dragOrigins.size());
			for (const auto& origin : view.dragOrigins)
				ordered.push_back(&origin);

			std::sort(ordered.begin(), ordered.end(),
			          [](const ViewState::DragOrigin* a, const ViewState::DragOrigin* b)
			          { return a->time < b->time; });

			bool moved = false;

			for (const ViewState::DragOrigin* origin : ordered)
			{
				const double target = origin->time + delta;

				if (FuncFrame* frame = timeline.FindFunc(origin->id))
				{
					frame->time = std::max(target, 0.0);
					moved = true;
					continue;
				}

				const int index = timeline.IndexOf(origin->id);
				if (index > 0)   // the first keyframe defines t = 0 and cannot move
				{
					// Ripple only makes sense for a single key. Applied to a group
					// it would shift the members that have not been placed yet, so
					// each one would be aiming at a target that moved while the
					// previous one was applied.
					timeline.SetAbsoluteTime(index, target,
					                         ripple && view.dragOrigins.size() == 1);
					moved = true;
				}
			}

			return moved;
		}

		// Adds a func frame at `time` and selects it, so the inspector opens on
		// the thing that was just created and its parameters are one glance away.
		void AddFuncAt(State& state, double time, FuncAction action, double now)
		{
			FuncFrame frame;
			frame.time   = std::max(time, 0.0);
			frame.action = action;

			SelectOnly(state, state.timeline.AddFunc(frame));
			state.dirty = true;

			char message[96];
			snprintf(message, sizeof(message), "%s cue added", FuncActionName(action));
			SetStatus(state, now, message);
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

		// Three rows, top to bottom: func frames, clips, keyframes.
		const float funcRowY   = trackTop + 4.0f + kFuncHalfWidth;
		const float clipTop    = trackTop + 8.0f + kFuncRowHeight;
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

			const bool segmentSelected = state.selection == Selection::Segment &&
			                             state.selectedId == keys[i].id &&
			                             state.multiSelection.empty();

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

			// IsSelected rather than a comparison against selectedId, so every
			// member of a Ctrl+click group pulses, not just the last one touched.
			const bool selected = IsSelected(state, keys[i].id) &&
			                      state.selection != Selection::Segment;

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

		// --- Func frames ---------------------------------------------------------
		//
		// Their own row along the top of the track, and the hover test is bounded
		// at BOTH ends of it.
		//
		// The floor is the one that is easy to leave out and expensive to leave
		// out: without it the band runs all the way up through the ruler, which
		// is the playhead's own grab zone, and a cue silently eats every click
		// near its column there -- so the playhead becomes unpickable wherever a
		// cue happens to sit. The marker is nowhere near those pixels, which is
		// what makes it look like nothing is wrong.
		const auto& funcs     = timeline.Funcs();
		const int   funcCount = static_cast<int>(funcs.size());

		int    hoveredFunc     = -1;
		double hoveredFuncDist = 1e9;

		for (int i = 0; i < funcCount; ++i)
		{
			const float x = TimeToX(funcs[i].time, originX, view);
			if (!hovered || x < originX - kGrabPixels || x > rightEdge + kGrabPixels)
				continue;

			const double dist = std::abs(mouseX - x);
			if (dist <= kGrabPixels && dist < hoveredFuncDist &&
			    mouseY >= trackTop && mouseY < clipTop - 4.0f)
			{
				hoveredFuncDist = dist;
				hoveredFunc     = i;
			}
		}

		// A hovered func frame owns the cursor outright: the keyframe row is a
		// long way below it, so a keyframe that also thinks it is hovered is a
		// bug rather than a tie worth resolving.
		if (hoveredFunc >= 0)
			hoveredIndex = -1;

		for (int i = 0; i < funcCount; ++i)
		{
			const FuncFrame& frame = funcs[i];

			const float x = TimeToX(frame.time, originX, view);
			if (x < originX - kGrabPixels || x > rightEdge + kGrabPixels)
				continue;

			const bool selected = IsSelected(state, frame.id);

			Rgba fill = frame.enabled ? kFuncFrame : kDisabled;
			if (i == hoveredFunc)
				fill = kFuncFrameHover;

			float half = kFuncHalfWidth;
			if (selected)
			{
				// The same breathing highlight the keyframes and the world gizmo
				// use, so one selection reads as one thing wherever it shows up.
				const double phase = std::fmod(now * 1.6, 2.0);
				const float  pulse = static_cast<float>(phase < 1.0 ? phase : 2.0 - phase);
				fill = Rgba{ Lerp(fill.r, 1.0f, pulse), Lerp(fill.g, 1.0f, pulse),
				             Lerp(fill.b, 1.0f, pulse), 1.0f };
				half += 2.0f * pulse;
			}

			// A progress ramp is the one cue with a duration, so it gets a bar
			// showing the stretch it covers. Without it the span lives only in
			// the inspector, and lining a ramp up against the camera move it is
			// supposed to happen under means reading two numbers instead of
			// looking at the track.
			if (frame.action == FuncAction::SetRuptureProgress && frame.ramp &&
			    frame.rampDuration > 0.0)
			{
				const float bx0 = std::max(x, originX);
				const float bx1 = std::min(TimeToX(frame.time + frame.rampDuration, originX, view),
				                           rightEdge);

				if (bx1 > bx0)
				{
					ui->DL_AddRectFilled(dl, bx0, funcRowY - 2.0f, bx1, funcRowY + 2.0f,
					                     Pack(ui, fill, 0.5f), 2.0f, PluginDrawFlags_RoundCornersAll);
					ui->DL_AddLine(dl, bx1, funcRowY - half, bx1, funcRowY + half,
					               Pack(ui, fill, 0.8f), 1.5f);
				}
			}

			// Down the whole track, so a cue can be lined up against the clip it
			// is meant to land on.
			ui->DL_AddLine(dl, x, funcRowY, x, clipBottom, Pack(ui, fill, 0.45f), 1.0f);
			DrawPennant(ui, dl, x, funcRowY, half, Pack(ui, fill),
			            Pack(ui, selected ? kSelected : kTrackBg));

			ui->DL_AddText(dl, x + half + 3.0f, funcRowY - half,
			               Pack(ui, fill, frame.enabled ? 0.95f : 0.6f), FuncLabel(frame));
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
		const bool ctrl = Input::CtrlHeld();

		if (hovered && ui->IsMouseClicked(0, false))
		{
			if (hoveredFunc >= 0)
			{
				const uint32_t id = funcs[hoveredFunc].id;

				if (ctrl)
				{
					ToggleSelection(state, id);
				}
				else
				{
					// Clicking something already in the set keeps the set, so a
					// group can be dragged without Ctrl held. Clicking outside it
					// collapses to that one thing, as an unmodified click should.
					if (!IsSelected(state, id))
						SelectOnly(state, id);

					// Every func frame is draggable, including one at t=0: unlike
					// the first keyframe it does not define the origin of anything.
					view.draggingFunc = id;
					BeginDrag(state, view, id, XToTime(mouseX, originX, view));
				}
			}
			else if (hoveredIndex >= 0)
			{
				const uint32_t id = keys[hoveredIndex].id;

				if (ctrl)
				{
					ToggleSelection(state, id);
				}
				else
				{
					if (!IsSelected(state, id))
						SelectOnly(state, id);

					// Only the keys after the first can be retimed by dragging --
					// the first one defines t = 0. It can still lead a group drag,
					// where the rest move around it.
					view.draggingKey = id;
					BeginDrag(state, view, id, XToTime(mouseX, originX, view));
				}
			}
			else if (mouseY < trackTop ||
			         std::abs(mouseX - TimeToX(state.playhead, originX, view)) <= kGrabPixels)
			{
				// The ruler is the playhead's own band, plus anywhere on the track
				// within grabbing distance of the line itself. The ruler alone is
				// 22 pixels of a track four times that tall, and the playhead is
				// the one thing here you reach for constantly -- a two-pixel line
				// is a poor target when the whole column below it is dead space.
				//
				// Last in the chain, so a cue or a keyframe sitting on top of the
				// playhead still wins the click.
				view.draggingPlayhead = true;
				state.playhead = Clamp(XToTime(mouseX, originX, view), 0.0, std::max(total, 0.0));
				state.playing  = false;
				state.playheadScrubbed = true;
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
					SelectOnly(state, keys[segment].id);
					state.selection = Selection::Segment;
				}
				else
				{
					ClearSelection(state);
				}
			}
		}

		// Double-clicking a keyframe puts the camera there, so the shot can be
		// picked back up from an existing pose.
		if (hovered && ui->IsMouseDoubleClicked(0))
		{
			if (hoveredFunc >= 0)
			{
				// The analogue of flying to a keyframe: park the playhead on the
				// cue so the preview shows the moment it fires.
				view.draggingFunc = 0;
				state.playhead    = Clamp(funcs[hoveredFunc].time, 0.0, std::max(total, 0.0));
				state.playing     = false;
			}
			else if (hoveredIndex >= 0)
			{
				SelectOnly(state, keys[hoveredIndex].id);
				view.draggingKey = 0;
				view.dragOrigins.clear();
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
					SelectOnly(state, keys[segment].id);
					view.draggingKey = 0;
					view.dragOrigins.clear();
				}
			}
		}

		if (view.draggingKey != 0)
		{
			if (!ui->IsMouseDown(0))
			{
				view.draggingKey = 0;
				view.dragOrigins.clear();
			}
			else if (ApplyDrag(timeline, view, state.options.rippleEdits,
			                   XToTime(mouseX, originX, view)))
			{
				state.dirty = true;
			}
		}

		if (view.draggingFunc != 0)
		{
			if (!ui->IsMouseDown(0) || !timeline.FindFunc(view.draggingFunc))
			{
				// Released, or deleted out from under the drag.
				view.draggingFunc = 0;
				view.dragOrigins.clear();
			}
			else if (ApplyDrag(timeline, view, state.options.rippleEdits,
			                   XToTime(mouseX, originX, view)))
			{
				// No clamp to the end of the shot: a func frame stores an absolute
				// time, and one parked past the last keyframe is a cue that simply
				// never gets reached rather than an invalid state.
				state.dirty = true;
			}
		}

		if (view.draggingPlayhead)
		{
			if (!ui->IsMouseDown(0))
			{
				view.draggingPlayhead = false;
			}
			else
			{
				state.playhead = Clamp(XToTime(mouseX, originX, view), 0.0, std::max(total, 0.0));

				// Says the *user* moved it, which is what lets the game thread
				// fire cues for a scrub without also firing them for a playhead
				// that jumped because the selection changed. See State.
				state.playheadScrubbed = true;
			}
		}

		// --- Right-click menu ------------------------------------------------------
		if (hovered && ui->IsMouseClicked(1, false))
		{
			view.contextTime   = std::max(XToTime(mouseX, originX, view), 0.0);
			view.contextFuncId = hoveredFunc  >= 0 ? funcs[hoveredFunc].id : 0;
			view.contextKeyId  = hoveredIndex >= 0 ? keys[hoveredIndex].id : 0;
			ui->OpenPopup(kContextPopup, 0);
		}

		// Submitted every frame, not only while open -- BeginPopup returns false
		// when it is closed, and skipping the call is how a popup never appears.
		if (ui->BeginPopup(kContextPopup, 0))
		{
			char header[64];
			char label[128];

			FormatTime(view.contextTime, header, sizeof(header));
			ui->SeparatorText(header);

			if (ui->MenuItem(CC_ICON_ADD "  Add keyframe here", nullptr, false, true))
			{
				// From the live fly pose, the same as every other capture. The
				// pose is plain data written by the game thread, so reading it
				// here is one of the few things the render thread may do with
				// something the world owns.
				SelectOnly(state, timeline.InsertAtTime(view.contextTime,
				                                        MakeKeyframeFromPose(state.flyPose)));
				state.dirty = true;
				SetStatus(state, now, "Keyframe added");
			}
			ItemTooltip(ui, "Records the free camera's current pose as a keyframe at this "
			                "point on the timeline, splitting whichever clip is there so "
			                "nothing after it moves.");

			if (ui->BeginMenu(CC_ICON_BOLT "  Add cue", true))
			{
				for (int i = 1; i < static_cast<int>(FuncAction::Count); ++i)
				{
					const FuncAction action = static_cast<FuncAction>(i);
					if (ui->MenuItem(FuncActionName(action), nullptr, false, true))
						AddFuncAt(state, view.contextTime, action, now);
					ItemTooltip(ui, FuncActionSummary(action));
				}
				ui->EndMenu();
			}
			ItemTooltip(ui, "Cues change the world rather than the camera, and only fire "
			                "during a take.");

			ui->Separator();

			if (ui->MenuItem("Move the playhead here", nullptr, false, true))
			{
				state.playhead = Clamp(view.contextTime, 0.0, std::max(total, 0.0));
				state.playing  = false;
			}

			// Delete offered only for whatever was actually under the cursor when
			// the menu opened, and named so it is obvious which one that is --
			// a bare "Delete" on a crowded track is a coin toss.
			if (const FuncFrame* frame = timeline.FindFunc(view.contextFuncId))
			{
				ui->Separator();
				if (frame->name.empty())
					snprintf(label, sizeof(label), CC_ICON_DELETE "  Delete the '%s' cue",
					         FuncActionName(frame->action));
				else
					snprintf(label, sizeof(label), CC_ICON_DELETE "  Delete cue '%s'",
					         frame->name.c_str());

				if (ui->MenuItem(label, nullptr, false, true))
				{
					if (IsSelected(state, view.contextFuncId))
						ClearSelection(state);
					timeline.RemoveFunc(view.contextFuncId);
					state.dirty = true;
					SetStatus(state, now, "Cue deleted");
				}
			}
			else if (view.contextKeyId != 0 && timeline.Find(view.contextKeyId))
			{
				ui->Separator();
				snprintf(label, sizeof(label), CC_ICON_DELETE "  Delete keyframe %d",
				         timeline.IndexOf(view.contextKeyId) + 1);
				if (ui->MenuItem(label, nullptr, false, true))
				{
					if (IsSelected(state, view.contextKeyId))
						ClearSelection(state);
					timeline.Remove(view.contextKeyId);
					state.dirty = true;
					SetStatus(state, now, "Keyframe deleted");
				}
			}

			ui->EndPopup();
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
		//
		// Not while a look-drag is running: the wheel belongs to the fly speed then
		// (see `HandleFlySpeedWheel`), and the cursor is pinned to the middle of the
		// picture, so "hovered" could be true without anyone having pointed at the
		// track. Both handlers only read the wheel, so without this guard one notch
		// would do both jobs at once.
		if (hovered && !state.lookActive)
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

		// Both hover indices were worked out before the menu above could add or
		// delete anything, so they are re-checked against the live counts rather
		// than trusted -- a stale index here reads off the end of the vector.
		if (hovered && hoveredIndex >= 0 && hoveredIndex < timeline.Count())
		{
			char tip[320];
			const Keyframe& k = keys[hoveredIndex];
			snprintf(tip, sizeof(tip),
			         "Keyframe %d%s%s\n"
			         "t = %.2fs   FOV %.0f\n"
			         "%.0f, %.0f, %.0f\n\n"
			         "Click to select, drag to retime\n"
			         "Double-click to fly the camera here\n"
			         "Right-click for the track menu",
			         hoveredIndex + 1,
			         k.name.empty() ? "" : "  ",
			         k.name.c_str(),
			         timeline.AbsoluteTime(hoveredIndex), k.fov,
			         k.location.x, k.location.y, k.location.z);
			Tooltip(ui, tip);
		}
		else if (hovered && hoveredFunc >= 0 && hoveredFunc < timeline.FuncCount())
		{
			char tip[384];
			const FuncFrame& f = funcs[hoveredFunc];
			snprintf(tip, sizeof(tip),
			         "%s%s%s\n"
			         "t = %.2fs%s\n\n"
			         "%s\n\n"
			         "Click to select, drag to retime\n"
			         "Double-click to put the playhead here",
			         FuncActionName(f.action),
			         f.name.empty() ? "" : "  --  ",
			         f.name.c_str(),
			         f.time,
			         f.enabled ? "" : "   (disabled)",
			         FuncActionSummary(f.action));
			Tooltip(ui, tip);
		}
	}
}
