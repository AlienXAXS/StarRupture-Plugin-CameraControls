#include "ui_editor.h"
#include "ui_overlay.h"
#include "ui_properties.h"
#include "ui_theme.h"
#include "ui_timeline.h"
#include "ui_viewport.h"
#include "editor_state.h"
#include "plugin_helpers.h"

#include <algorithm>
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

		// Recomputed every frame from GetDisplaySize.
		float g_propertiesWidth = 380.0f;
		float g_timelineHeight  = 260.0f;

		ScreenRect g_gameRect;

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

			// --- Where the game itself gets to draw -----------------------
			const bool squeeze = state.options.fitViewport &&
			                     state.mode == Mode::Editor &&
			                     !state.uiHidden;

			if (squeeze)
			{
				const float freeW = screenW - g_propertiesWidth - kViewerMargin * 2.0f;
				const float freeH = screenH - g_timelineHeight  - kViewerMargin * 2.0f;

				if (freeW > 200.0f && freeH > 150.0f)
					g_gameRect = FitAspect(kViewerMargin, kViewerMargin, freeW, freeH, screenW / screenH);
				else
					g_gameRect = ScreenRect{ 0.0f, 0.0f, screenW, screenH };
			}
			else
			{
				g_gameRect = ScreenRect{ 0.0f, 0.0f, screenW, screenH };
			}

			state.gameView.x = g_gameRect.x / screenW;
			state.gameView.y = g_gameRect.y / screenH;
			state.gameView.w = g_gameRect.w / screenW;
			state.gameView.h = g_gameRect.h / screenH;
			state.gameViewValid = true;
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

			const ScreenRect& r = g_gameRect;
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

			if (ui->ButtonSized("|<", w, h))
			{
				state.playhead = 0.0;
				state.playing  = false;
			}
			ui->SetItemTooltip("Jump to the start");

			ui->SameLine(0.0f, 4.0f);
			if (ui->ButtonSized("<", w, h))
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
			ui->SetItemTooltip("Previous keyframe");

			ui->SameLine(0.0f, 4.0f);
			if (ToggleButton(ui, state.playing ? "||" : ">", state.playing, w, h))
			{
				state.playing = !state.playing;
				if (state.playing && state.playhead >= total - 1e-6)
					state.playhead = 0.0;
			}
			ui->SetItemTooltip("Preview play / pause. The camera follows the playhead "
			                   "if 'Camera follows the playhead' is on.");

			ui->SameLine(0.0f, 4.0f);
			if (ui->ButtonSized(">", w, h))
			{
				double target = total;
				for (int i = 0; i < timeline.Count(); ++i)
				{
					const double t = timeline.AbsoluteTime(i);
					if (t > state.playhead + 1e-4) { target = t; break; }
				}
				state.playhead = target;
			}
			ui->SetItemTooltip("Next keyframe");

			ui->SameLine(0.0f, 4.0f);
			if (ui->ButtonSized(">|", w, h))
			{
				state.playhead = total;
				state.playing  = false;
			}
			ui->SetItemTooltip("Jump to the end");

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
			if (AccentButton(ui, "Add keyframe", 130.0f, h))
				Post(state, Request::CaptureAppend);
			ui->SetItemTooltip("Records the free camera's current position, rotation and FOV "
			                   "as a new keyframe at the end of the timeline.");

			ui->SameLine(0.0f, 4.0f);
			ui->BeginDisabled(state.selection == Selection::None || state.selectedId == 0);
			if (ui->ButtonSized("Insert after", 110.0f, h))
				Post(state, Request::CaptureInsertAfterSelected);
			ui->SetItemTooltip("Splices a keyframe in after the selected one without changing "
			                   "when any later keyframe happens.");
			ui->EndDisabled();

			// --- Record --------------------------------------------------------
			ui->SameLine(0.0f, 18.0f);
			ui->BeginDisabled(timeline.Count() < 2);
			ui->PushStyleColor(Col_Button,        kDanger.r * 0.65f, kDanger.g * 0.65f, kDanger.b * 0.65f, 1.0f);
			ui->PushStyleColor(Col_ButtonHovered, kDanger.r, kDanger.g, kDanger.b, 1.0f);
			ui->PushStyleColor(Col_ButtonActive,  kDanger.r, kDanger.g, kDanger.b, 1.0f);
			if (ui->ButtonSized("Playback mode", 130.0f, h))
				Post(state, Request::StartPlayback);
			ui->PopStyleColor(3);
			ui->SetItemTooltip("Counts down, hides the editor (and optionally the game HUD), "
			                   "then runs the timeline start to finish.");
			ui->EndDisabled();

			// --- Nudge ----------------------------------------------------------
			// Dragging on the track is fine for roughing timing out, but not for
			// "half a second later" -- these are the fine adjustment.
			{
				const int  index = timeline.IndexOf(state.selectedId);
				const bool canNudge = index > 0 && state.selection != Selection::None;

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
						ui->SetTooltip(tip);
					}
				}

				ui->EndDisabled();
			}

			// --- View controls -------------------------------------------------
			float availX = 0.0f, availY = 0.0f;
			ui->GetContentRegionAvail(&availX, &availY);

			// Right-align the zoom cluster so it stays put as the middle grows.
			const float clusterWidth = 34.0f * 3.0f + 8.0f + 90.0f;
			if (availX > clusterWidth + 20.0f)
			{
				ui->SameLine(0.0f, availX - clusterWidth);

				if (ToggleButton(ui, "Ripple", state.options.rippleEdits, 90.0f, h))
					state.options.rippleEdits = !state.options.rippleEdits;
				ui->SetItemTooltip("On: dragging a keyframe pushes every later one along.\n"
				                   "Off: only that keyframe moves; the next segment absorbs it.");

				ui->SameLine(0.0f, 8.0f);
				if (ui->ButtonSized("-", 34.0f, h))
					g_view.pixelsPerSecond = Clamp(g_view.pixelsPerSecond / 1.4, 3.0, 1200.0);
				ui->SetItemTooltip("Zoom out");

				ui->SameLine(0.0f, 2.0f);
				if (ui->ButtonSized("+", 34.0f, h))
					g_view.pixelsPerSecond = Clamp(g_view.pixelsPerSecond * 1.4, 3.0, 1200.0);
				ui->SetItemTooltip("Zoom in");

				ui->SameLine(0.0f, 2.0f);
				if (ui->ButtonSized("Fit", 34.0f, h))
					g_view.pendingFit = true;
				ui->SetItemTooltip("Frame the whole timeline");
			}

			(void)now;
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

			// Mask first -- it goes on the background list, so it sits under
			// everything else regardless, but keeping the order honest makes
			// the intent readable.
			DrawViewerMask(ui, state);

			// Viewport picking next: it draws under the overlay chrome and
			// bails out on its own if the cursor is over an editor window.
			Viewport::Render(ui, state, now);
			Overlay::Render(ui, state, now);
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

		g_timelineWidget   = uiEvents->RegisterWidget(&timelineDesc);
		g_propertiesWidget = uiEvents->RegisterWidget(&propertiesDesc);
		g_overlayWidget    = uiEvents->RegisterWidget(&overlayDesc);

		SetVisible(false);
		LOG_INFO("Editor: widgets registered (timeline=%p properties=%p overlay=%p)",
		         g_timelineWidget, g_propertiesWidget, g_overlayWidget);
	}

	void Unregister(IPluginSelf* self)
	{
		if (!self || !self->hooks || !self->hooks->UI)
			return;

		auto* uiEvents = self->hooks->UI;

		if (g_timelineWidget)   { uiEvents->UnregisterWidget(g_timelineWidget);   g_timelineWidget   = nullptr; }
		if (g_propertiesWidget) { uiEvents->UnregisterWidget(g_propertiesWidget); g_propertiesWidget = nullptr; }
		if (g_overlayWidget)    { uiEvents->UnregisterWidget(g_overlayWidget);    g_overlayWidget    = nullptr; }
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

	ScreenRect CurrentGameRect()
	{
		return g_gameRect;
	}

	void ResetView()
	{
		// Deliberately separate from SetVisible: toggling the UI off and back on
		// mid-edit must not throw away the zoom the user just set up.
		g_view.pendingFit    = true;
		g_view.scrollSeconds = 0.0;
	}
}
