#include "input_binds.h"
#include "editor_state.h"
#include "plugin_config.h"
#include "plugin_helpers.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace CameraControls::Input
{
	namespace
	{
		// -------------------------------------------------------------------
		// Bind table
		//
		// Every entry is registered by *name* so the modloader tracks it and
		// re-registers automatically when the user rebinds it in the config UI.
		// The name string has to outlive registration, so the resolved names
		// are kept here for the plugin's lifetime.
		// -------------------------------------------------------------------
		enum class Action
		{
			ToggleEditor, TogglePlayback, ToggleUI, ExitPlayback,
			FlyForward, FlyBack, FlyLeft, FlyRight, FlyDown, FlyUp,
			FlyBoost, FlyCrawl,
			RollLeft, RollRight, FovIn, FovOut,
			CaptureKeyframe, InsertKeyframe, UpdateKeyframe, GotoKeyframe,
			DeleteKeyframe, PrevKeyframe, NextKeyframe, PlayPause,
			Count
		};

		struct BindDef
		{
			Action      action;
			const char* configKey;
			const char* fallback;
			bool        wantsRelease;   // held keys need both edges
		};

		const BindDef kBinds[] = {
			{ Action::ToggleEditor,    "ToggleEditor",    "F7",          false },
			{ Action::TogglePlayback,  "TogglePlayback",  "F8",          false },
			{ Action::ToggleUI,        "ToggleUI",        "F9",          false },
			{ Action::ExitPlayback,    "ExitPlayback",    "Escape",      false },

			{ Action::FlyForward,      "FlyForward",      "W",           true  },
			{ Action::FlyBack,         "FlyBack",         "S",           true  },
			{ Action::FlyLeft,         "FlyLeft",         "A",           true  },
			{ Action::FlyRight,        "FlyRight",        "D",           true  },
			{ Action::FlyDown,         "FlyDown",         "Q",           true  },
			{ Action::FlyUp,           "FlyUp",           "E",           true  },
			{ Action::FlyBoost,        "FlyBoost",        "LeftShift",   true  },
			{ Action::FlyCrawl,        "FlyCrawl",        "LeftControl", true  },
			{ Action::RollLeft,        "RollLeft",        "Z",           true  },
			{ Action::RollRight,       "RollRight",       "C",           true  },
			{ Action::FovIn,           "FovIn",           "R",           true  },
			{ Action::FovOut,          "FovOut",          "F",           true  },

			{ Action::CaptureKeyframe, "CaptureKeyframe", "K",           false },
			{ Action::InsertKeyframe,  "InsertKeyframe",  "Shift+K",     false },
			{ Action::UpdateKeyframe,  "UpdateKeyframe",  "U",           false },
			{ Action::GotoKeyframe,    "GotoKeyframe",    "G",           false },
			{ Action::DeleteKeyframe,  "DeleteKeyframe",  "Delete",      false },
			{ Action::PrevKeyframe,    "PrevKeyframe",    "Comma",       false },
			{ Action::NextKeyframe,    "NextKeyframe",    "Period",      false },
			{ Action::PlayPause,       "PlayPause",       "SpaceBar",    false },
		};

		constexpr int kBindCount = static_cast<int>(sizeof(kBinds) / sizeof(kBinds[0]));

		std::string g_resolvedNames[kBindCount];
		bool        g_held[static_cast<int>(Action::Count)] = {};

		// --- Mouse look ------------------------------------------------------
		bool  g_looking     = false;   // right button held and the drag owns the camera
		POINT g_lookAnchor  = {};      // screen position the cursor is pinned to
		HWND  g_gameWindow  = nullptr;

		HWND ResolveGameWindow()
		{
			if (g_gameWindow && IsWindow(g_gameWindow))
				return g_gameWindow;

			// The engine tick runs on the thread that owns the game window, so
			// its active window is the one we want. Fall back to the foreground
			// window if that comes back null (e.g. during a mode change).
			HWND hwnd = GetActiveWindow();
			if (!hwnd)
				hwnd = GetForegroundWindow();

			if (hwnd)
			{
				DWORD pid = 0;
				GetWindowThreadProcessId(hwnd, &pid);
				if (pid == GetCurrentProcessId())
					g_gameWindow = hwnd;
			}

			return g_gameWindow;
		}

		// -------------------------------------------------------------------
		// Held-key axes
		// -------------------------------------------------------------------
		void RecomputeAxes(State& state)
		{
			auto down = [](Action a) { return g_held[static_cast<int>(a)]; };

			state.flyInput.forward  = (down(Action::FlyForward) ? 1 : 0) - (down(Action::FlyBack)  ? 1 : 0);
			state.flyInput.right    = (down(Action::FlyRight)   ? 1 : 0) - (down(Action::FlyLeft)  ? 1 : 0);
			state.flyInput.up       = (down(Action::FlyUp)      ? 1 : 0) - (down(Action::FlyDown)  ? 1 : 0);
			state.flyInput.rollAxis = (down(Action::RollRight)  ? 1 : 0) - (down(Action::RollLeft) ? 1 : 0);
			state.flyInput.fovAxis  = (down(Action::FovOut)     ? 1 : 0) - (down(Action::FovIn)    ? 1 : 0);
			state.flyInput.boost    = down(Action::FlyBoost);
			state.flyInput.crawl    = down(Action::FlyCrawl);
		}

		// -------------------------------------------------------------------
		// Action handling
		// -------------------------------------------------------------------
		void SelectNeighbour(State& state, int direction)
		{
			const auto& keys = state.timeline.Keys();
			if (keys.empty())
				return;

			int index = state.timeline.IndexOf(state.selectedId);
			if (index < 0)
				index = direction > 0 ? -1 : static_cast<int>(keys.size());

			index = Clamp(index + direction, 0, static_cast<int>(keys.size()) - 1);

			state.selectedId = keys[index].id;
			state.selection  = Selection::Keyframe;
			state.playhead   = state.timeline.AbsoluteTime(index);
		}

		void HandlePress(Action action)
		{
			auto lock = Lock();
			State& state = Get();
			const double now = Now();

			// Mode keys work from anywhere; everything else needs the editor.
			switch (action)
			{
				case Action::ToggleEditor:
					Post(state, state.mode == Mode::Off ? Request::EnterEditor : Request::ExitEditor);
					return;

				case Action::TogglePlayback:
					if (state.mode == Mode::Playback)
						Post(state, Request::StopPlayback);
					else if (state.mode == Mode::Editor)
						Post(state, Request::StartPlayback);
					return;

				// Escape by default, and only meaningful mid-take. Playback
				// holds exclusive input, so the key never reaches the game's
				// pause menu -- without this there is no reflex-level way out
				// of a take that has gone wrong.
				case Action::ExitPlayback:
					if (state.mode == Mode::Playback)
					{
						LOG_TRACE("Input: escape out of playback");
						Post(state, Request::StopPlayback);
					}
					return;

				default:
					break;
			}

			// Playback deliberately answers to nothing but its own stop key --
			// a stray keypress must not derail a take being recorded. Mode is
			// also the gate that keeps every editing key inert outside the main
			// game world, since the editor cannot be entered there at all.
			if (state.mode != Mode::Editor)
				return;

			// While the user is typing into an ImGui field, letter keys belong
			// to the text box, not to the editor.
			if (state.textInputActive)
				return;

			LOG_TRACE("Input: action %d in mode %d", static_cast<int>(action),
			          static_cast<int>(state.mode));

			switch (action)
			{
				case Action::ToggleUI:
					state.uiHidden = !state.uiHidden;
					SetStatus(state, now, state.uiHidden ? "Editor UI hidden" : "Editor UI shown");
					break;

				case Action::CaptureKeyframe:
					Post(state, Request::CaptureAppend);
					break;

				case Action::InsertKeyframe:
					Post(state, Request::CaptureInsertAfterSelected);
					break;

				case Action::UpdateKeyframe:
					Post(state, Request::UpdateSelectedFromCamera);
					break;

				case Action::GotoKeyframe:
					Post(state, Request::GotoSelected);
					break;

				case Action::DeleteKeyframe:
					if (state.selectedId != 0 && state.timeline.Remove(state.selectedId))
					{
						state.selectedId = 0;
						state.selection  = Selection::None;
						state.dirty      = true;
						SetStatus(state, now, "Keyframe deleted");
					}
					break;

				case Action::PrevKeyframe: SelectNeighbour(state, -1); break;
				case Action::NextKeyframe: SelectNeighbour(state, +1); break;

				case Action::PlayPause:
					if (state.timeline.TotalDuration() > 0.0)
					{
						state.playing = !state.playing;
						if (state.playing && state.playhead >= state.timeline.TotalDuration() - 1e-6)
							state.playhead = 0.0;
					}
					break;

				default:
					break;
			}
		}

		// -------------------------------------------------------------------
		// Callback plumbing
		//
		// The modloader's callback signature carries no user data, so each bind
		// needs its own function. A template instantiated per index gives us
		// that without twenty near-identical copy-pasted functions.
		// -------------------------------------------------------------------
		template <int Index>
		void OnBind(EModKey /*key*/, EModKeyEvent event)
		{
			static_assert(Index >= 0 && Index < kBindCount, "bind index out of range");
			const BindDef& def = kBinds[Index];

			if (def.wantsRelease)
			{
				g_held[static_cast<int>(def.action)] = (event == EModKeyEvent::Pressed);
				auto lock = Lock();
				RecomputeAxes(Get());
				return;
			}

			if (event == EModKeyEvent::Pressed)
				HandlePress(def.action);
		}

		using BindCallback = void (*)(EModKey, EModKeyEvent);

		// Builds the per-index callback table at compile time.
		template <int... Indices>
		constexpr auto MakeCallbackTable(std::integer_sequence<int, Indices...>)
		{
			return std::array<BindCallback, sizeof...(Indices)>{ &OnBind<Indices>... };
		}

		const auto g_callbacks = MakeCallbackTable(std::make_integer_sequence<int, kBindCount>{});

		// --- Mouse look button ------------------------------------------------
		void OnLookButton(EModKey /*key*/, EModKeyEvent event)
		{
			auto lock = Lock();
			State& state = Get();

			if (event == EModKeyEvent::Pressed)
			{
				// A press that lands on one of the editor windows belongs to
				// the UI -- dragging a slider must not also spin the camera.
				if (state.mode == Mode::Editor && !state.uiHovered)
				{
					g_looking = true;
					LOG_TRACE("Input: mouse-look started");
				}
				return;
			}

			if (g_looking)
				LOG_TRACE("Input: mouse-look ended");

			g_looking = false;
		}
	}

	void ReleaseAllKeys(State& state)
	{
		// No Lock() here on purpose -- every caller is a teardown path running
		// inside the engine tick, which already holds it. Taking it again on
		// the same thread is undefined behaviour, and it used to throw out of
		// ExitEditor before the camera and the player had been restored.
		for (bool& held : g_held)
			held = false;

		g_looking    = false;
		g_lookAnchor = POINT{};

		state.flyInput.ClearAll();
	}

	void Register(IPluginSelf* self)
	{
		if (!self || !self->hooks || !self->hooks->Input)
		{
			LOG_WARN("Input: hooks->Input unavailable (server build?) -- no keybinds registered");
			return;
		}

		auto* input = self->hooks->Input;

		for (int i = 0; i < kBindCount; ++i)
		{
			char buffer[64] = {};
			g_resolvedNames[i] = CameraControlsConfig::Config::GetKeybind(
				kBinds[i].configKey, kBinds[i].fallback, buffer, sizeof(buffer));

			LOG_DEBUG("Input: '%s' -> %s%s", g_resolvedNames[i].c_str(), kBinds[i].configKey,
			          kBinds[i].wantsRelease ? " (held)" : "");

			input->RegisterKeybindByName(g_resolvedNames[i].c_str(),
			                             EModKeyEvent::Pressed, g_callbacks[i]);

			if (kBinds[i].wantsRelease)
			{
				input->RegisterKeybindByName(g_resolvedNames[i].c_str(),
				                             EModKeyEvent::Released, g_callbacks[i]);
			}
		}

		// The look button is not rebindable: right-drag-to-look is the one
		// convention every camera tool shares, and remapping it would collide
		// with ImGui's own use of the other buttons.
		input->RegisterKeybind(EModKey::RightMouseButton, EModKeyEvent::Pressed,  &OnLookButton);
		input->RegisterKeybind(EModKey::RightMouseButton, EModKeyEvent::Released, &OnLookButton);

		LOG_INFO("Input: registered %d keybinds", kBindCount);
	}

	void Unregister(IPluginSelf* self)
	{
		if (!self || !self->hooks || !self->hooks->Input)
			return;

		auto* input = self->hooks->Input;

		for (int i = 0; i < kBindCount; ++i)
		{
			if (g_resolvedNames[i].empty())
				continue;

			input->UnregisterKeybindByName(g_resolvedNames[i].c_str(),
			                               EModKeyEvent::Pressed, g_callbacks[i]);

			if (kBinds[i].wantsRelease)
			{
				input->UnregisterKeybindByName(g_resolvedNames[i].c_str(),
				                               EModKeyEvent::Released, g_callbacks[i]);
			}
		}

		input->UnregisterKeybind(EModKey::RightMouseButton, EModKeyEvent::Pressed,  &OnLookButton);
		input->UnregisterKeybind(EModKey::RightMouseButton, EModKeyEvent::Released, &OnLookButton);
	}

	void PumpMouseLook()
	{
		float sensitivity = 0.25f;
		bool  wantLook    = false;

		{
			auto lock = Lock();
			State& state = Get();
			sensitivity = state.options.mouseSensitivity;
			wantLook    = g_looking && state.mode == Mode::Editor;
		}

		if (!wantLook)
		{
			g_lookAnchor = POINT{};
			return;
		}

		HWND hwnd = ResolveGameWindow();
		if (!hwnd)
			return;

		// Pin the cursor to the centre of the client area and measure how far
		// it moved away each tick. Without this the cursor walks off the window
		// during a long pan and the drag stops dead.
		RECT client{};
		if (!GetClientRect(hwnd, &client))
			return;

		POINT centre{ (client.right - client.left) / 2, (client.bottom - client.top) / 2 };
		if (!ClientToScreen(hwnd, &centre))
			return;

		POINT cursor{};
		if (!GetCursorPos(&cursor))
			return;

		// First tick of a drag only establishes the anchor -- reporting the
		// distance from wherever the cursor happened to be would snap the view.
		if (g_lookAnchor.x == 0 && g_lookAnchor.y == 0)
		{
			g_lookAnchor = centre;
			SetCursorPos(centre.x, centre.y);
			return;
		}

		const double dx = static_cast<double>(cursor.x - centre.x);
		const double dy = static_cast<double>(cursor.y - centre.y);

		SetCursorPos(centre.x, centre.y);

		if (dx == 0.0 && dy == 0.0)
			return;

		auto lock = Lock();
		State& state = Get();
		state.flyInput.yawDelta   += dx * sensitivity;
		state.flyInput.pitchDelta -= dy * sensitivity;   // screen Y grows downward
	}
}
