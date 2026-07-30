#include "ui_overlay.h"
#include "ui_editor.h"
#include "ui_theme.h"
#include "plugin_config.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace CameraControls::UI::Overlay
{
	using namespace CameraControls::Theme;

	namespace
	{
		constexpr float kMargin      = 14.0f;
		constexpr float kPadding     = 10.0f;
		constexpr float kLineSpacing = 2.0f;

		// The cheat-sheet, resolved from config once so the panel shows the
		// user's actual bindings rather than the shipped defaults.
		enum class RowKind { Section, Bind, Literal };

		struct HintRow
		{
			RowKind     kind;
			const char* configKey;   // Bind rows only
			const char* fallback;    // Bind rows only
			const char* text;        // heading, literal line, or the description
		};

		const HintRow kHints[] = {
			{ RowKind::Section, nullptr,           nullptr,       "CAMERA" },
			{ RowKind::Bind,    "FlyForward",      "W",           "forward" },
			{ RowKind::Bind,    "FlyBack",         "S",           "back" },
			{ RowKind::Bind,    "FlyLeft",         "A",           "left" },
			{ RowKind::Bind,    "FlyRight",        "D",           "right" },
			{ RowKind::Bind,    "FlyUp",           "E",           "up" },
			{ RowKind::Bind,    "FlyDown",         "Q",           "down" },
			{ RowKind::Bind,    "FlyBoost",        "LeftShift",   "faster (hold)" },
			{ RowKind::Bind,    "FlyCrawl",        "LeftControl", "slower (hold)" },
			{ RowKind::Literal, nullptr,           nullptr,       "Right mouse -- look around" },
			{ RowKind::Literal, nullptr,           nullptr,       "Wheel while looking -- fly speed" },
			{ RowKind::Bind,    "RollLeft",        "Z",           "roll left" },
			{ RowKind::Bind,    "RollRight",       "C",           "roll right" },
			{ RowKind::Bind,    "FovIn",           "R",           "zoom in" },
			{ RowKind::Bind,    "FovOut",          "F",           "zoom out" },
			{ RowKind::Bind,    "ResetRotation",   "X",           "level the camera" },

			{ RowKind::Section, nullptr,           nullptr,       "KEYFRAMES" },
			{ RowKind::Bind,    "CaptureKeyframe", "K",           "add at camera" },
			{ RowKind::Bind,    "InsertKeyframe",  "I",           "insert after selected" },
			{ RowKind::Bind,    "UpdateKeyframe",  "U",           "re-record selected" },
			{ RowKind::Bind,    "GotoKeyframe",    "G",           "fly to selected" },
			{ RowKind::Bind,    "DeleteKeyframe",  "Delete",      "delete selected" },
			{ RowKind::Bind,    "PrevKeyframe",    "Comma",       "select previous" },
			{ RowKind::Bind,    "NextKeyframe",    "Period",      "select next" },
			{ RowKind::Literal, nullptr,           nullptr,       "Double-click a clip -- fly there" },
			{ RowKind::Literal, nullptr,           nullptr,       "Right-click the track -- add a keyframe or cue" },
			{ RowKind::Literal, nullptr,           nullptr,       "Ctrl+click -- select several, drag as one" },

			{ RowKind::Section, nullptr,           nullptr,       "TRANSPORT" },
			{ RowKind::Bind,    "PlayPause",       "SpaceBar",    "preview play / pause" },
			{ RowKind::Bind,    "TogglePlayback",  "F8",          "record playback" },
			{ RowKind::Bind,    "ExitPlayback",    "Escape",      "stop a take" },
			{ RowKind::Bind,    "ToggleUI",        "F9",          "hide / show this UI" },
			{ RowKind::Bind,    "ToggleEditor",    "F7",          "leave editor" },
		};

		constexpr int kHintCount = static_cast<int>(sizeof(kHints) / sizeof(kHints[0]));

		std::string g_hintKeys[kHintCount];
		std::string g_playbackKey = "F8";       // shown on the recording chrome
		std::string g_exitPlaybackKey = "Escape";

		// Full-screen rect helper -- the display size is the viewport in the
		// single-viewport case the game always runs in.
		void GetScreen(IModLoaderImGui* ui, float& w, float& h)
		{
			ui->GetDisplaySize(&w, &h);
		}

		void DrawPanel(IModLoaderImGui* ui, PluginDrawList dl,
		               float x0, float y0, float x1, float y1, float alpha)
		{
			ui->DL_AddRectFilled(dl, x0, y0, x1, y1,
			                     ui->GetColorU32FromVec4(0.04f, 0.05f, 0.07f, 0.82f * alpha),
			                     6.0f, PluginDrawFlags_RoundCornersAll);
			ui->DL_AddRect(dl, x0, y0, x1, y1, Pack(ui, kAccentDim, alpha),
			               6.0f, PluginDrawFlags_RoundCornersAll, 1.5f);
		}

		// -----------------------------------------------------------------
		void RenderHints(IModLoaderImGui* ui, State& state)
		{
			PluginDrawList dl = ui->GetForegroundDrawList();

			const float lineHeight = ui->GetTextLineHeight() + kLineSpacing;

			// Two columns: key on the left, what it does on the right.
			float keyColumn = 0.0f;
			for (int i = 0; i < kHintCount; ++i)
			{
				if (kHints[i].kind != RowKind::Bind)
					continue;
				float w = 0.0f, h = 0.0f;
				ui->CalcTextSize(g_hintKeys[i].c_str(), &w, &h, false, -1.0f);
				keyColumn = std::max(keyColumn, w);
			}

			float widest = 0.0f;
			for (int i = 0; i < kHintCount; ++i)
			{
				float w = 0.0f, h = 0.0f;
				ui->CalcTextSize(kHints[i].text, &w, &h, false, -1.0f);
				widest = std::max(widest, w);
			}

			const float panelWidth  = keyColumn + 14.0f + widest + kPadding * 2.0f;
			const float panelHeight = kHintCount * lineHeight + kPadding * 2.0f;

			const float x0 = kMargin;
			const float y0 = kMargin;

			DrawPanel(ui, dl, x0, y0, x0 + panelWidth, y0 + panelHeight, 1.0f);

			float y = y0 + kPadding;
			for (int i = 0; i < kHintCount; ++i)
			{
				switch (kHints[i].kind)
				{
					case RowKind::Section:
						ui->DL_AddText(dl, x0 + kPadding, y, Pack(ui, kAccent), kHints[i].text);
						break;

					case RowKind::Literal:
						ui->DL_AddText(dl, x0 + kPadding, y, Pack(ui, kRuler), kHints[i].text);
						break;

					case RowKind::Bind:
						ui->DL_AddText(dl, x0 + kPadding, y, Pack(ui, kKeyframe), g_hintKeys[i].c_str());
						ui->DL_AddText(dl, x0 + kPadding + keyColumn + 14.0f, y,
						               Pack(ui, kSelected, 0.85f), kHints[i].text);
						break;
				}
				y += lineHeight;
			}

			// Live read-out under the cheat-sheet: where the camera is, and
			// whether the world gizmos are actually going to show up.
			// The FOV is shown twice on purpose: asked-for, then what the camera
			// manager is really rendering. They used to differ silently, because the
			// game overwrites POV.FOV every frame -- one number could never have
			// shown that, and two make it obvious at a glance. Only while they
			// disagree, so a healthy session stays uncluttered.
			char actual[24] = {};
			if (state.renderViewValid && std::fabs(state.renderView.fov - state.flyPose.fov) > 1.0f)
				snprintf(actual, sizeof(actual), " (rendering %.0f)", state.renderView.fov);

			char readout[224];
			snprintf(readout, sizeof(readout),
			         "%.0f  %.0f  %.0f      pitch %.1f  yaw %.1f  roll %.1f      FOV %.0f%s",
			         state.flyPose.location.x, state.flyPose.location.y, state.flyPose.location.z,
			         state.flyPose.rotation.pitch, state.flyPose.rotation.yaw,
			         state.flyPose.rotation.roll, state.flyPose.fov, actual);

			float rw = 0.0f, rh = 0.0f;
			ui->CalcTextSize(readout, &rw, &rh, false, -1.0f);

			const float ry0 = y0 + panelHeight + 6.0f;
			DrawPanel(ui, dl, x0, ry0, x0 + rw + kPadding * 2.0f, ry0 + rh + kPadding, 1.0f);
			ui->DL_AddText(dl, x0 + kPadding, ry0 + kPadding * 0.5f, Pack(ui, kRuler), readout);
		}

		// -----------------------------------------------------------------
		void RenderCountdown(IModLoaderImGui* ui, State& state)
		{
			float screenW = 0.0f, screenH = 0.0f;
			GetScreen(ui, screenW, screenH);

			PluginDrawList dl = ui->GetForegroundDrawList();

			const int whole = static_cast<int>(std::ceil(state.countdown));
			char text[16];
			snprintf(text, sizeof(text), "%d", whole > 0 ? whole : 1);

			// The digit swells and fades across each second, so the beat is
			// readable out of the corner of an eye while framing the shot.
			const double fraction = state.countdown - std::floor(state.countdown);
			const float  scale    = 5.0f + static_cast<float>(fraction) * 3.0f;
			const float  alpha    = 0.25f + static_cast<float>(fraction) * 0.75f;

			const float fontSize = ui->GetFontSize() * scale;

			float tw = 0.0f, th = 0.0f;
			ui->CalcTextSize(text, &tw, &th, false, -1.0f);
			tw *= scale;
			th *= scale;

			ui->DL_AddTextSized(dl, fontSize,
			                    screenW * 0.5f - tw * 0.5f,
			                    screenH * 0.5f - th * 0.5f,
			                    ui->GetColorU32FromVec4(1.0f, 1.0f, 1.0f, alpha),
			                    text);

			const char* caption = "Recording starts in";
			float cw = 0.0f, ch = 0.0f;
			ui->CalcTextSize(caption, &cw, &ch, false, -1.0f);
			ui->DL_AddText(dl, screenW * 0.5f - cw * 0.5f, screenH * 0.5f - th * 0.5f - ch - 12.0f,
			               Pack(ui, kAccent), caption);
		}

		// -----------------------------------------------------------------
		// The fade is a property of the *shot*, so it is painted over the
		// picture and nothing else. Covering the whole window would also
		// cover the timeline and the inspector, which -- with a fade-to-black
		// on the first keyframe, the single most common thing to set -- means
		// parking the playhead there blacks out the editor you are trying to
		// work in.
		void RenderFade(IModLoaderImGui* ui, const FadeState& fade, const Editor::ScreenRect& rect)
		{
			if (fade.alpha <= 0.001f)
				return;

			PluginDrawList dl = ui->GetForegroundDrawList();
			ui->DL_AddRectFilled(dl, rect.x, rect.y, rect.x + rect.w, rect.y + rect.h,
			                     ui->GetColorU32FromVec4(fade.color[0], fade.color[1], fade.color[2],
			                                             fade.alpha),
			                     0.0f, PluginDrawFlags_None);
		}

		// What the editor shows instead of an actual fade, unless asked for
		// one: a swatch at the top of the picture saying what would be
		// happening here in a take, without hiding the shot being composed.
		void RenderFadeIndicator(IModLoaderImGui* ui, const FadeState& fade,
		                         const Editor::ScreenRect& rect)
		{
			if (fade.alpha <= 0.001f)
				return;

			PluginDrawList dl = ui->GetForegroundDrawList();

			const float h  = 6.0f;
			const float y0 = rect.y + 2.0f;
			const float w  = rect.w * fade.alpha;

			ui->DL_AddRectFilled(dl, rect.x, y0, rect.x + rect.w, y0 + h,
			                     ui->GetColorU32FromVec4(0.0f, 0.0f, 0.0f, 0.35f),
			                     0.0f, PluginDrawFlags_None);
			ui->DL_AddRectFilled(dl, rect.x, y0, rect.x + w, y0 + h,
			                     ui->GetColorU32FromVec4(fade.color[0], fade.color[1], fade.color[2], 1.0f),
			                     0.0f, PluginDrawFlags_None);
			ui->DL_AddRect(dl, rect.x, y0, rect.x + rect.w, y0 + h,
			               Pack(ui, kFadeMarker, 0.8f), 0.0f, PluginDrawFlags_None, 1.0f);

			char label[48];
			snprintf(label, sizeof(label), "FADE  %.0f%%", fade.alpha * 100.0f);
			ui->DL_AddText(dl, rect.x + 4.0f, y0 + h + 3.0f, Pack(ui, kFadeMarker), label);
		}

		// -----------------------------------------------------------------
		void RenderRecordingChrome(IModLoaderImGui* ui, State& state, double now)
		{
			float screenW = 0.0f, screenH = 0.0f;
			GetScreen(ui, screenW, screenH);

			PluginDrawList dl = ui->GetForegroundDrawList();

			// A slow pulse on the frame edge -- unmistakably "rolling", without
			// putting anything in the middle of the shot.
			const float pulse = 0.35f + 0.25f * static_cast<float>(std::sin(now * 3.0) * 0.5 + 0.5);
			ui->DL_AddRect(dl, 2.0f, 2.0f, screenW - 2.0f, screenH - 2.0f,
			               ui->GetColorU32FromVec4(kDanger.r, kDanger.g, kDanger.b, pulse),
			               0.0f, PluginDrawFlags_None, 3.0f);

			char line[128];
			snprintf(line, sizeof(line), "PLAYBACK   %.2f / %.2f s      %s or %s to stop",
			         state.playhead, state.timeline.TotalDuration(),
			         g_exitPlaybackKey.c_str(), g_playbackKey.c_str());

			float tw = 0.0f, th = 0.0f;
			ui->CalcTextSize(line, &tw, &th, false, -1.0f);

			const float x0 = screenW * 0.5f - tw * 0.5f - kPadding;
			const float y0 = screenH - th - kPadding * 2.0f - kMargin;
			DrawPanel(ui, dl, x0, y0, x0 + tw + kPadding * 2.0f, y0 + th + kPadding, 0.7f);
			ui->DL_AddText(dl, x0 + kPadding, y0 + kPadding * 0.5f, Pack(ui, kDanger), line);
		}

		// -----------------------------------------------------------------
		void RenderStatus(IModLoaderImGui* ui, State& state, double now)
		{
			if (state.status.empty() || now > state.statusExpiry)
				return;

			float screenW = 0.0f, screenH = 0.0f;
			GetScreen(ui, screenW, screenH);

			PluginDrawList dl = ui->GetForegroundDrawList();

			// Fade the last half second so messages do not just blink out.
			const double remaining = state.statusExpiry - now;
			const float  alpha     = static_cast<float>(Clamp(remaining / 0.5, 0.0, 1.0));

			float tw = 0.0f, th = 0.0f;
			ui->CalcTextSize(state.status.c_str(), &tw, &th, false, -1.0f);

			const float x0 = screenW * 0.5f - tw * 0.5f - kPadding;
			const float y0 = kMargin;

			DrawPanel(ui, dl, x0, y0, x0 + tw + kPadding * 2.0f, y0 + th + kPadding, alpha);
			ui->DL_AddText(dl, x0 + kPadding, y0 + kPadding * 0.5f,
			               Pack(ui, kSelected, alpha), state.status.c_str());
		}
	}

	void CacheKeybindNames()
	{
		for (int i = 0; i < kHintCount; ++i)
		{
			if (kHints[i].kind != RowKind::Bind)
				continue;

			char buffer[64] = {};
			g_hintKeys[i] = CameraControlsConfig::Config::GetKeybind(
				kHints[i].configKey, kHints[i].fallback, buffer, sizeof(buffer));

			if (std::strcmp(kHints[i].configKey, "TogglePlayback") == 0)
				g_playbackKey = g_hintKeys[i];
			if (std::strcmp(kHints[i].configKey, "ExitPlayback") == 0)
				g_exitPlaybackKey = g_hintKeys[i];
		}
	}

	void Render(IModLoaderImGui* ui, State& state, double now)
	{
		if (state.mode == Mode::Off)
		{
			// Still draw the status line. This is the only path that reports a
			// *refused* mode change -- pressing the editor key outside the main
			// game world would otherwise do nothing at all, with no clue why.
			RenderStatus(ui, state, now);
			return;
		}

		if (state.mode == Mode::Editor && !state.uiHidden)
			RenderHints(ui, state);

		// Once the countdown is over, a take that asked for a clean image gets
		// one: if the user is hiding the game's HUD they are recording, and
		// our own chrome would land in the footage just the same.
		const bool wantsCleanImage =
			state.mode == Mode::Playback && state.options.hideGameHud;

		// Fades are confined to the picture, and in the editor they are shown
		// as an indicator rather than painted, unless the user asks to see the
		// real thing. A take gets the real fade regardless.
		//
		// This runs through the countdown as well. The playhead is parked at 0
		// during the pre-roll, so a fade-in on the first keyframe already
		// evaluates to fully opaque there -- which means the screen is covered
		// *before* the take starts and the fade dips out of a settled frame.
		// Skipping it left the shot plainly visible right up to the moment the
		// fade was supposed to be starting from black, and then cut to black on
		// the first frame of the move.
		{
			const FadeState fade = state.timeline.EvaluateFade(state.playhead);
			const Editor::ScreenRect rect = Editor::CurrentGameRect();

			if (state.mode == Mode::Playback || state.options.previewFades)
				RenderFade(ui, fade, rect);
			else
				RenderFadeIndicator(ui, fade, rect);
		}

		// Countdown *after* the fade, so the digit stays readable when the
		// pre-roll is sitting on a full-strength fade to black.
		if (state.mode == Mode::Playback)
		{
			if (state.countdown > 0.0)
				RenderCountdown(ui, state);
			else if (!wantsCleanImage)
				RenderRecordingChrome(ui, state, now);
		}

		if (!wantsCleanImage || state.countdown > 0.0)
			RenderStatus(ui, state, now);
	}
}
