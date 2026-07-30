#include "ui_editor.h"
#include "ui_overlay.h"
#include "ui_properties.h"
#include "ui_theme.h"
#include "ui_timeline.h"
#include "ui_viewport.h"
#include "editor_state.h"
#include "plugin_helpers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace CameraControls::UI::Editor
{
	using namespace CameraControls::Theme;

	namespace
	{
		// ImGuiHoveredFlags_AnyWindow
		constexpr int kHoveredAnyWindow = 1 << 2;

		WidgetHandle g_timelineWidget   = nullptr;
		WidgetHandle g_propertiesWidget = nullptr;
		WidgetHandle g_overlayWidget    = nullptr;
		WidgetHandle g_noticeWidget     = nullptr;

		TimelineView::ViewState g_view;

		// Hints are re-read by the modloader every frame, so recomputing them
		// from the viewport at the end of each frame keeps both windows docked
		// to the screen edges at any resolution -- one frame behind a resize,
		// which nobody can see.
		PluginWindowHints g_timelineHints = {
			900.0f, 260.0f, 0.0f, 500.0f, 0.0f, 0.0f, /*size_cond*/0, /*pos_cond*/0,
			PluginWindowFlags_NoResize | PluginWindowFlags_NoMove | PluginWindowFlags_NoSavedSettings
		};

		PluginWindowHints g_propertiesHints = {
			380.0f, 700.0f, 900.0f, 0.0f, 0.0f, 0.0f, 0, 0,
			PluginWindowFlags_NoResize | PluginWindowFlags_NoMove | PluginWindowFlags_NoSavedSettings
		};

		PluginWindowHints g_overlayHints = {
			1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0,
			PluginWindowFlags_NoTitleBar | PluginWindowFlags_NoResize | PluginWindowFlags_NoMove |
			PluginWindowFlags_NoBackground | PluginWindowFlags_NoScrollbar |
			PluginWindowFlags_NoMouseInputs | PluginWindowFlags_NoSavedSettings
		};

		// The pre-open notice, shown once per session before the editor is first
		// opened. A dialog, so it keeps its title bar and sits in the
		// middle of the window rather than docking to an edge like the panels --
		// and it is a widget of its own rather than something drawn on the overlay
		// because the overlay is NoMouseInputs. A notice whose buttons cannot be
		// clicked would be worse than no notice at all.
		PluginWindowHints g_noticeHints = {
			520.0f, 300.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0,
			PluginWindowFlags_NoResize | PluginWindowFlags_NoMove |
			PluginWindowFlags_NoSavedSettings
		};

		// Recomputed every frame from GetDisplaySize.
		float g_propertiesWidth = 380.0f;
		float g_timelineHeight  = 260.0f;

		// Two rectangles, and the difference between them is load-bearing.
		//
		// `g_requestedGameRect` is where we have *asked* the engine to draw. It
		// drives state.gameView and the matte we paint around the picture.
		//
		// `g_actualGameRect` is where the engine is really drawing, which is the
		// whole window whenever the squeeze did not take -- an unsupported build,
		// a failed offset probe, or simply the frame before the game thread has
		// applied it. Anything mapping world space onto the screen has to use this
		// one. Projecting into a rect the engine is not honouring puts every
		// handle up and to the left of the thing it is supposed to be on, by
		// exactly the margin between the two.
		ScreenRect g_requestedGameRect;
		ScreenRect g_actualGameRect;

		// Breathing room between the picture and the panels, so the frame edge
		// reads as an edge rather than as the panel's border.
		constexpr float kViewerMargin = 8.0f;

		// Fits a rectangle of `aspect` inside the free area, centred.
		//
		// Keeping the *window's* aspect rather than the free area's is the whole
		// point: playback hides the panels and fills the screen, so anything
		// framed against a differently-shaped preview would be a lie. This is a
		// safe-frame viewer, not a stretched one.
		ScreenRect FitAspect(float freeX, float freeY, float freeW, float freeH, float aspect)
		{
			ScreenRect out;

			float w = freeW;
			float h = w / aspect;
			if (h > freeH)
			{
				h = freeH;
				w = h * aspect;
			}

			out.x = freeX + (freeW - w) * 0.5f;
			out.y = freeY + (freeH - h) * 0.5f;
			out.w = w;
			out.h = h;
			return out;
		}

		void UpdateLayout(IModLoaderImGui* ui, State& state)
		{
			float screenW = 0.0f, screenH = 0.0f;
			ui->GetDisplaySize(&screenW, &screenH);

			if (screenW < 100.0f || screenH < 100.0f)
				return;

			// A fifth of the width for the inspector, clamped so it stays
			// usable on a 1280-wide window and does not become a canyon on an
			// ultrawide. Same idea vertically for the timeline.
			g_propertiesWidth = Clamp(screenW * 0.21f, 330.0f, 560.0f);
			g_timelineHeight  = Clamp(screenH * 0.225f, 165.0f, 345.0f);

			g_timelineHints.width  = screenW - g_propertiesWidth;
			g_timelineHints.height = g_timelineHeight;
			g_timelineHints.pos_x  = 0.0f;
			g_timelineHints.pos_y  = screenH - g_timelineHeight;

			g_propertiesHints.width  = g_propertiesWidth;
			g_propertiesHints.height = screenH;
			g_propertiesHints.pos_x  = screenW - g_propertiesWidth;
			g_propertiesHints.pos_y  = 0.0f;

			// Centred rather than docked. Sized from its own top-left rather than
			// through the hints' pivot, so it does not depend on the modloader
			// honouring one -- the panels have never used it either.
			g_noticeHints.width  = Clamp(screenW * 0.34f, 440.0f, 640.0f);
			g_noticeHints.height = Clamp(screenH * 0.38f, 280.0f, 400.0f);
			g_noticeHints.pos_x  = (screenW - g_noticeHints.width)  * 0.5f;
			g_noticeHints.pos_y  = (screenH - g_noticeHints.height) * 0.5f;

			// --- Where the game itself gets to draw -----------------------
			const bool squeeze = state.options.fitViewport &&
			                     state.mode == Mode::Editor &&
			                     !state.uiHidden;

			const ScreenRect fullWindow{ 0.0f, 0.0f, screenW, screenH };

			if (squeeze)
			{
				const float freeW = screenW - g_propertiesWidth - kViewerMargin * 2.0f;
				const float freeH = screenH - g_timelineHeight  - kViewerMargin * 2.0f;

				if (freeW > 200.0f && freeH > 150.0f)
					g_requestedGameRect =
						FitAspect(kViewerMargin, kViewerMargin, freeW, freeH, screenW / screenH);
				else
					g_requestedGameRect = fullWindow;
			}
			else
			{
				g_requestedGameRect = fullWindow;
			}

			// The engine only renders into the sub-rect once the game thread has
			// pushed it into the local player and confirmed it took. Until then --
			// and for good on a build where the probe refused -- the picture is
			// still the whole window, and that is what has to be projected into.
			g_actualGameRect = state.gameViewApplied ? g_requestedGameRect : fullWindow;

			state.gameView.x = g_requestedGameRect.x / screenW;
			state.gameView.y = g_requestedGameRect.y / screenH;
			state.gameView.w = g_requestedGameRect.w / screenW;
			state.gameView.h = g_requestedGameRect.h / screenH;
			state.gameViewValid = true;

			state.displayW = screenW;
			state.displayH = screenH;
		}

		// Blacks out everything the game is no longer drawing into.
		//
		// The engine clears the unused part of the backbuffer itself, but only
		// reliably on the frames it actually renders -- and a paused or
		// loading frame will happily leave the last full-screen image smeared
		// around the edges. Painting it ourselves also gives the picture a
		// visible frame, which is what makes it read as a viewer.
		void DrawViewerMask(IModLoaderImGui* ui, const State& state)
		{
			if (!state.gameViewApplied || state.mode != Mode::Editor || state.uiHidden)
				return;

			float screenW = 0.0f, screenH = 0.0f;
			ui->GetDisplaySize(&screenW, &screenH);

			const ScreenRect& r = g_requestedGameRect;
			if (r.w <= 0.0f || r.h <= 0.0f)
				return;

			// Below every window, so the panels still draw over the top of it.
			PluginDrawList dl = ui->GetBackgroundDrawList();
			const unsigned int mask = Pack(ui, kViewerBackdrop);

			ui->DL_AddRectFilled(dl, 0.0f, 0.0f, screenW, r.y, mask, 0.0f, 0);
			ui->DL_AddRectFilled(dl, 0.0f, r.y + r.h, screenW, screenH, mask, 0.0f, 0);
			ui->DL_AddRectFilled(dl, 0.0f, r.y, r.x, r.y + r.h, mask, 0.0f, 0);
			ui->DL_AddRectFilled(dl, r.x + r.w, r.y, screenW, r.y + r.h, mask, 0.0f, 0);

			// A hairline round the picture, so the edge of frame is obvious
			// when the shot itself is dark.
			ui->DL_AddRect(dl, r.x - 1.0f, r.y - 1.0f, r.x + r.w + 1.0f, r.y + r.h + 1.0f,
			               Pack(ui, kViewerFrame), 0.0f, 0, 1.0f);
		}

		// -------------------------------------------------------------------
		// Transport bar
		// -------------------------------------------------------------------
		void RenderTransport(IModLoaderImGui* ui, State& state, double now)
		{
			const float h = ui->GetFrameHeight() * 1.2f;
			const float w = 34.0f;

			Timeline& timeline = state.timeline;
			const double total = timeline.TotalDuration();
			const bool   hasShot = total > 0.0;

			ui->BeginDisabled(!hasShot);

			// Every transport button carries an explicit "##id" after its glyph.
			//
			// ImGui derives a widget's identity from its label, so two buttons
			// labelled the same thing *are* the same widget -- ">" used to be both
			// play and next-keyframe, and clicking one drove the other. Icons make
			// that trap easier to fall into, not harder: rewind and forward are a
			// mirrored pair and play/pause swaps its glyph on press, so identity has
			// to come from the id rather than from what is drawn.
			if (ui->ButtonSized(CC_ICON_SKIP_START "##transportStart", w, h))
			{
				state.playhead = 0.0;
				state.playing  = false;
			}
			ItemTooltip(ui, "Jump to the start");

			ui->SameLine(0.0f, 4.0f);
			if (ui->ButtonSized(CC_ICON_REWIND "##transportPrev", w, h))
			{
				// Step back to the previous keyframe boundary.
				double target = 0.0;
				for (int i = timeline.Count() - 1; i >= 0; --i)
				{
					const double t = timeline.AbsoluteTime(i);
					if (t < state.playhead - 1e-4) { target = t; break; }
				}
				state.playhead = target;
			}
			ItemTooltip(ui, "Previous keyframe");

			ui->SameLine(0.0f, 4.0f);
			if (ToggleButton(ui, state.playing ? CC_ICON_PAUSE "##transportPlay"
			                                   : CC_ICON_PLAY  "##transportPlay",
			                 state.playing, w, h))
			{
				state.playing = !state.playing;
				if (state.playing && state.playhead >= total - 1e-6)
					state.playhead = 0.0;
			}
			ItemTooltip(ui, "Preview play / pause.\n\n"
			                "The camera follows the playhead if 'Camera follows the playhead' "
			                "is on in the inspector.");

			ui->SameLine(0.0f, 4.0f);
			if (ui->ButtonSized(CC_ICON_FORWARD "##transportNext", w, h))
			{
				double target = total;
				for (int i = 0; i < timeline.Count(); ++i)
				{
					const double t = timeline.AbsoluteTime(i);
					if (t > state.playhead + 1e-4) { target = t; break; }
				}
				state.playhead = target;
			}
			ItemTooltip(ui, "Next keyframe");

			ui->SameLine(0.0f, 4.0f);
			if (ui->ButtonSized(CC_ICON_SKIP_END "##transportEnd", w, h))
			{
				state.playhead = total;
				state.playing  = false;
			}
			ItemTooltip(ui, "Jump to the end");

			ui->EndDisabled();

			// --- Time read-out ---------------------------------------------
			ui->SameLine(0.0f, 14.0f);
			ui->AlignTextToFramePadding();

			char current[32], length[32], readout[80];
			FormatTime(state.playhead, current, sizeof(current));
			FormatTime(total, length, sizeof(length));
			snprintf(readout, sizeof(readout), "%s / %s", current, length);
			TextColored(ui, kPlayhead, readout);

			// --- Capture ------------------------------------------------------
			ui->SameLine(0.0f, 18.0f);
			if (AccentButton(ui, CC_ICON_ADD "##addkey", w, h))
				Post(state, Request::CaptureAppend);
			ItemTooltip(ui, "Add keyframe.\n\n"
			                "Records the free camera's current position, rotation and FOV as a "
			                "new keyframe at the end of the timeline.");

			ui->SameLine(0.0f, 4.0f);
			// Keyframe or segment specifically, not "anything selected": with a
			// cue selected there is no index to insert after, and InsertAfter(-1)
			// prepends -- which puts the new keyframe at the *front* of the shot.
			// A multi-selection is out for the same reason: "after the selection"
			// names no single place.
			ui->BeginDisabled(state.selectedId == 0 ||
			                  !state.multiSelection.empty() ||
			                  (state.selection != Selection::Keyframe &&
			                   state.selection != Selection::Segment));
			if (ui->ButtonSized(CC_ICON_PLAYLIST_ADD "##insertkey", w, h))
				Post(state, Request::CaptureInsertAfterSelected);
			ItemTooltip(ui, "Insert after the selection.\n\n"
			                "Splices a keyframe in after the selected one without changing when "
			                "any later keyframe happens.");
			ui->EndDisabled();

			// --- Record --------------------------------------------------------
			ui->SameLine(0.0f, 18.0f);
			ui->BeginDisabled(timeline.Count() < 2);
			ui->PushStyleColor(Col_Button,        kDanger.r * 0.65f, kDanger.g * 0.65f, kDanger.b * 0.65f, 1.0f);
			ui->PushStyleColor(Col_ButtonHovered, kDanger.r, kDanger.g, kDanger.b, 1.0f);
			ui->PushStyleColor(Col_ButtonActive,  kDanger.r, kDanger.g, kDanger.b, 1.0f);
			if (ui->ButtonSized(CC_ICON_RECORD "##playbackmode", w, h))
				Post(state, Request::StartPlayback);
			ui->PopStyleColor(3);
			ItemTooltip(ui, "Playback mode.\n\n"
			                "Counts down, hides the editor (and optionally the game HUD), then "
			                "runs the timeline start to finish.");
			ui->EndDisabled();

			// --- Ripple ---------------------------------------------------------
			// On the bar rather than buried in the inspector because it changes what
			// every drag on the track does -- it belongs where you can see its state
			// while you are dragging, not two panels away. Unconditionally, too: the
			// zoom cluster further right is skipped entirely on a narrow window, and
			// a toggle that silently disappears is worse than no toggle.
			//
			// The glyph carries the state as well as the highlight does -- a whole
			// chain link when later keyframes come along, a broken one when they do
			// not -- so this reads correctly even at a glance.
			ui->SameLine(0.0f, 18.0f);
			if (ToggleButton(ui, state.options.rippleEdits ? CC_ICON_LINK     "##ripple"
			                                               : CC_ICON_LINK_OFF "##ripple",
			                 state.options.rippleEdits, w, h))
				state.options.rippleEdits = !state.options.rippleEdits;
			ItemTooltip(ui, state.options.rippleEdits
				? "Ripple is ON.\n\n"
				  "Retiming a keyframe moves every later keyframe with it, so the gaps you "
				  "already set stay put.\n\n"
				  "Click to turn off."
				: "Ripple is OFF.\n\n"
				  "Retiming a keyframe only changes that keyframe, stretching or squashing "
				  "the segments either side of it.\n\n"
				  "Click to turn on.");

			// --- Nudge ----------------------------------------------------------
			// Dragging on the track is fine for roughing timing out, but not for
			// "half a second later" -- these are the fine adjustment.
			{
				const int  index = timeline.IndexOf(state.selectedId);
				// Single selection only. Nudging a group is what dragging it does,
				// and a nudge that silently moved one member of a set would be
				// worse than one that is greyed out.
				const bool canNudge = index > 0 && state.selection != Selection::None &&
				                      state.multiSelection.empty();

				ui->SameLine(0.0f, 18.0f);
				ui->AlignTextToFramePadding();
				ui->TextDisabled("Nudge");

				ui->BeginDisabled(!canNudge);

				static const int kNudgeFrames[] = { -60, -30, -10, -5, 5, 10, 30, 60 };

				for (int frames : kNudgeFrames)
				{
					char label[16];
					snprintf(label, sizeof(label), "%+d", frames);

					ui->SameLine(0.0f, 2.0f);
					if (ui->ButtonSized(label, 38.0f, h) && canNudge)
					{
						const double delta = frames / static_cast<double>(std::max(timeline.frameRate, 1.0f));
						timeline.SetAbsoluteTime(index,
						                         timeline.AbsoluteTime(index) + delta,
						                         state.options.rippleEdits);
						state.playhead = timeline.AbsoluteTime(index);
						state.dirty    = true;
					}

					if (ui->IsItemHovered())
					{
						char tip[96];
						snprintf(tip, sizeof(tip), "Move this keyframe %+d frames (%+.2f s at %.0f fps)",
						         frames, frames / std::max(timeline.frameRate, 1.0f), timeline.frameRate);
						Tooltip(ui, tip);
					}
				}

				ui->EndDisabled();
			}

			// --- View controls -------------------------------------------------
			float availX = 0.0f, availY = 0.0f;
			ui->GetContentRegionAvail(&availX, &availY);

			// Right-align the zoom cluster so it stays put as the middle grows.
			//
			// Ripple used to live here too, and this cluster is skipped entirely
			// when the window is narrow -- so the one control that changes what
			// every drag does was the first thing to vanish. It now sits on the
			// transport bar instead, which is always drawn.
			const float clusterWidth = 34.0f * 3.0f + 4.0f;
			if (availX > clusterWidth + 20.0f)
			{
				ui->SameLine(0.0f, availX - clusterWidth);

				if (ui->ButtonSized(CC_ICON_ZOOM_OUT "##zoomout", 34.0f, h))
					g_view.pixelsPerSecond = Clamp(g_view.pixelsPerSecond / 1.4, 3.0, 1200.0);
				ItemTooltip(ui, "Zoom out");

				ui->SameLine(0.0f, 2.0f);
				if (ui->ButtonSized(CC_ICON_ZOOM_IN "##zoomin", 34.0f, h))
					g_view.pixelsPerSecond = Clamp(g_view.pixelsPerSecond * 1.4, 3.0, 1200.0);
				ItemTooltip(ui, "Zoom in");

				ui->SameLine(0.0f, 2.0f);
				if (ui->ButtonSized(CC_ICON_FIT_SCREEN "##zoomfit", 34.0f, h))
					g_view.pendingFit = true;
				ItemTooltip(ui, "Frame the whole timeline");
			}

			(void)now;
		}

		// -------------------------------------------------------------------
		// Wheel while looking = fly speed
		//
		// The convention every flycam shares, and it wants no keybind: the hand
		// that is already holding right-mouse to steer is on the wheel.
		//
		// It has to be read here, on the render thread. `WM_MOUSEWHEEL` never
		// reaches the plugin -- ImGui consumes it -- so `GetMouseWheel` off ImGuiIO
		// is the only view of it there is, which is also why this cannot live in
		// `input_binds` next to the rest of the camera input.
		//
		// Gated on the look drag rather than on "the cursor is not over a panel",
		// because during a drag the cursor is pinned to the middle of the window
		// and its position has stopped meaning anything.
		// -------------------------------------------------------------------
		void HandleFlySpeedWheel(IModLoaderImGui* ui, State& state, double now)
		{
			if (state.mode != Mode::Editor || !state.lookActive)
				return;

			const float wheel = ui->GetMouseWheel();
			if (wheel == 0.0f)
				return;

			// Multiplicative, because the range spans two orders of magnitude: a
			// fixed step big enough to be useful at 8000 u/s makes the bottom of
			// the range unreachable, and one fine enough at 200 u/s never gets
			// anywhere near the top.
			const float factor = std::pow(1.15f, wheel);

			state.options.flySpeed =
				Clamp(state.options.flySpeed * factor, kFlySpeedMin, kFlySpeedMax);

			// Worth a status line: the speed is otherwise invisible unless the
			// inspector happens to be scrolled to it, and F9 may have hidden the
			// inspector entirely. The number is the feedback.
			char message[48];
			snprintf(message, sizeof(message), "Fly speed  %.0f u/s", state.options.flySpeed);
			SetStatus(state, now, message);
		}

		// -------------------------------------------------------------------
		// Widget callbacks
		// -------------------------------------------------------------------
		void RenderTimelineWindow(IModLoaderImGui* ui)
		{
			const double now = Now();
			auto lock = Lock();
			State& state = Get();

			UpdateLayout(ui, state);

			// The editor's own bookkeeping about what the mouse and keyboard
			// are doing, consumed by the game thread and the keybind handlers.
			state.uiHovered       = ui->IsWindowHovered(kHoveredAnyWindow);
			state.textInputActive = ui->IsAnyItemActive();

			if (state.mode != Mode::Editor || state.uiHidden)
				return;

			if (state.timeline.Empty())
			{
				ui->Spacing();
				TextColored(ui, kAccent, "Nothing on the timeline yet.");
				ui->TextWrapped("Fly the camera to where you want the shot to start, then press "
				                "Add keyframe (or K). Add a few more along the path and the "
				                "camera will move smoothly between them.");
				ui->Spacing();
				RenderTransport(ui, state, now);
				return;
			}

			RenderTransport(ui, state, now);
			ui->Separator();
			TimelineView::Render(ui, state, g_view, now);
		}

		void RenderPropertiesWindow(IModLoaderImGui* ui)
		{
			const double now = Now();
			auto lock = Lock();
			State& state = Get();

			if (state.mode != Mode::Editor || state.uiHidden)
				return;

			// Hovering the inspector counts as hovering the UI too, so a
			// right-click on a slider never doubles as a camera look-drag.
			if (ui->IsWindowHovered(kHoveredAnyWindow))
				state.uiHovered = true;
			if (ui->IsAnyItemActive())
				state.textInputActive = true;

			Properties::Render(ui, state, now);
		}

		void RenderOverlayWindow(IModLoaderImGui* ui)
		{
			const double now = Now();
			auto lock = Lock();
			State& state = Get();

			// The overlay is the one widget that renders in every mode, so it
			// is also the only place the game rect can be kept honest during
			// playback -- the timeline window is unregistered by then, and a
			// stale squeezed rect would letterbox the fade and the chrome into
			// a picture that is no longer there.
			UpdateLayout(ui, state);

			// Before the timeline widget gets a chance to read the same wheel for
			// its zoom. Both only ever read it, so neither consumes it from the
			// other -- the timeline is guarded on its own side too.
			HandleFlySpeedWheel(ui, state, now);

			// Mask first -- it goes on the background list, so it sits under
			// everything else regardless, but keeping the order honest makes
			// the intent readable.
			DrawViewerMask(ui, state);

			// Viewport picking next: it draws under the overlay chrome and
			// bails out on its own if the cursor is over an editor window.
			Viewport::Render(ui, state, now);
			Overlay::Render(ui, state, now);
		}

		// -------------------------------------------------------------------
		// The pre-open notice
		//
		// Both buttons only post. Opening the editor is a page of UObject work and
		// this is the render thread; even Cancel has to be queued, because the
		// input token the notice is holding is released by the same code that
		// releases the editor's.
		//
		// The window is hidden whenever this is not pending, so the early return
		// is belt and braces -- the modloader begins and ends the window around
		// this callback, and a visible window that drew nothing would be an empty
		// title bar in the middle of the screen.
		// -------------------------------------------------------------------
		void RenderNoticeWindow(IModLoaderImGui* ui)
		{
			auto lock = Lock();
			State& state = Get();

			if (!state.confirmPending)
				return;

			ui->Spacing();
			TextColored(ui, kKeyframe, "Open this on a save you can go back to.");
			ui->Spacing();

			ui->TextWrapped("While the editor is open your character is taken out of the world: "
			                "the body is moved somewhere safe, the things that can kill it are "
			                "switched off, and the camera comes off the pawn. Leaving the editor "
			                "puts all of that back.");
			ui->Spacing();
			ui->TextWrapped("Timeline cues go further. A rupture one starts is a real rupture, in "
			                "the save you are actually playing, and nothing here can undo it.");
			ui->Spacing();
			ui->TextWrapped("Save the game first, or fly on a save you would not mind reloading.");

			ui->Spacing();
			ui->Separator();
			ui->Spacing();

			const float h = ui->GetFrameHeight() * 1.4f;

			float availX = 0.0f, availY = 0.0f;
			ui->GetContentRegionAvail(&availX, &availY);
			const float w = (availX - 8.0f) * 0.5f;

			if (AccentButton(ui, "I have saved -- open it##noticeOk", w, h))
				Post(state, Request::ConfirmOpenEditor);

			ui->SameLine(0.0f, 8.0f);
			if (ui->ButtonSized("Not now##noticeCancel", w, h))
				Post(state, Request::CancelOpenEditor);

			ui->Spacing();
			ui->TextDisabled("Shown once per session.");
		}
	}

	void Register(IPluginSelf* self)
	{
		if (!self || !self->hooks || !self->hooks->UI)
		{
			LOG_WARN("Editor: hooks->UI unavailable -- no editor UI registered");
			return;
		}

		auto* uiEvents = self->hooks->UI;

		static PluginWidgetDesc timelineDesc   = { "Camera Timeline",   &RenderTimelineWindow,   &g_timelineHints };
		static PluginWidgetDesc propertiesDesc = { "Camera Properties", &RenderPropertiesWindow, &g_propertiesHints };
		static PluginWidgetDesc overlayDesc    = { "CameraControlsOverlay", &RenderOverlayWindow, &g_overlayHints };
		static PluginWidgetDesc noticeDesc     = { "Camera Controls",   &RenderNoticeWindow,     &g_noticeHints };

		g_timelineWidget   = uiEvents->RegisterWidget(&timelineDesc);
		g_propertiesWidget = uiEvents->RegisterWidget(&propertiesDesc);
		g_overlayWidget    = uiEvents->RegisterWidget(&overlayDesc);
		g_noticeWidget     = uiEvents->RegisterWidget(&noticeDesc);

		SetVisible(false);
		SetNoticeVisible(false);
		LOG_INFO("Editor: widgets registered (timeline=%p properties=%p overlay=%p notice=%p)",
		         g_timelineWidget, g_propertiesWidget, g_overlayWidget, g_noticeWidget);
	}

	void Unregister(IPluginSelf* self)
	{
		if (!self || !self->hooks || !self->hooks->UI)
			return;

		auto* uiEvents = self->hooks->UI;

		if (g_timelineWidget)   { uiEvents->UnregisterWidget(g_timelineWidget);   g_timelineWidget   = nullptr; }
		if (g_propertiesWidget) { uiEvents->UnregisterWidget(g_propertiesWidget); g_propertiesWidget = nullptr; }
		if (g_overlayWidget)    { uiEvents->UnregisterWidget(g_overlayWidget);    g_overlayWidget    = nullptr; }
		if (g_noticeWidget)     { uiEvents->UnregisterWidget(g_noticeWidget);     g_noticeWidget     = nullptr; }
	}

	void SetVisible(bool visible)
	{
		auto* hooks = GetHooks();
		if (!hooks || !hooks->UI)
			return;

		// The overlay stays registered but only draws while a mode is active,
		// so the countdown and fade survive the editor windows being hidden.
		LOG_DEBUG("Editor: widgets %s", visible ? "shown" : "hidden");

		if (g_timelineWidget)   hooks->UI->SetWidgetVisible(g_timelineWidget,   visible);
		if (g_propertiesWidget) hooks->UI->SetWidgetVisible(g_propertiesWidget, visible);
		if (g_overlayWidget)    hooks->UI->SetWidgetVisible(g_overlayWidget,    true);
	}

	void SetNoticeVisible(bool visible)
	{
		auto* hooks = GetHooks();
		if (!hooks || !hooks->UI || !g_noticeWidget)
			return;

		LOG_DEBUG("Editor: notice %s", visible ? "shown" : "hidden");
		hooks->UI->SetWidgetVisible(g_noticeWidget, visible);
	}

	ScreenRect CurrentGameRect()
	{
		// The rect the engine is actually drawing into, not the one we asked for.
		// Both callers -- world-to-screen projection and the playback fade --
		// need to line up with the picture on screen, and lining up with a
		// request the engine ignored is worse than not squeezing at all.
		return g_actualGameRect;
	}

	void ResetView()
	{
		// Deliberately separate from SetVisible: toggling the UI off and back on
		// mid-edit must not throw away the zoom the user just set up.
		g_view.pendingFit    = true;
		g_view.scrollSeconds = 0.0;
	}
}
