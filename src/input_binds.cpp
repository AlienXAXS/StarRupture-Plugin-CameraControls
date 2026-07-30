#include "input_binds.h"
#include "editor_state.h"
#include "plugin_config.h"
#include "plugin_helpers.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdio>
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
			ToggleEditor, TogglePlayback, ToggleUI, ExitPlayback, DumpProbe,
			FlyForward, FlyBack, FlyLeft, FlyRight, FlyDown, FlyUp,
			FlyBoost, FlyCrawl,
			RollLeft, RollRight, FovIn, FovOut, ResetRotation,
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

			// Diagnostic dump. Deliberately bound outside the editor's own keys
			// because the whole point is to sample the world when the plugin is
			// idle and the character will not respond -- at which point every
			// editor key is inert by design.
			//
			// Home rather than the obvious F10: F10 is a Windows *system* key, so
			// pressing it sends WM_SYSKEYDOWN and, unhandled, activates the window
			// menu -- which steals focus and stops input reaching the game. For a
			// key whose entire job is diagnosing input that has stopped reaching
			// the game, that is the one thing it must not do.
			{ Action::DumpProbe,       "DumpProbe",       "Home",        false },

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
			{ Action::ResetRotation,   "ResetRotation",   "X",           false },

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

		// --- Modifier keys, sampled from the OS ------------------------------
		//
		// Boost and crawl default to LeftShift and LeftControl, and the modloader
		// deliberately never dispatches a plain modifier keydown to a plugin:
		// keybind_registry.cpp's ProcessWindowMessage resolves the sided VK
		// correctly and then drops it, so that pressing Shift as part of a
		// "Shift+K" combo cannot also fire a standalone Shift bind. The upshot is
		// that a plain LeftShift bind can never fire, however it is registered.
		//
		// So these two are read straight from the keyboard instead. This file
		// already owns OS-level input for the mouse-look drag, which makes it the
		// right home, and it means boost/crawl work on any loader build rather
		// than only on one carrying a fix. A bind pointed at a non-modifier key
		// resolves to 0 here and goes back through the normal keybind path.
		int g_boostVK = 0;
		int g_crawlVK = 0;

		// PumpModifierKeys runs every tick, so it may only log on a change.
		bool g_lastBoost = false;
		bool g_lastCrawl = false;

		int ModifierVKForName(const std::string& name)
		{
			if (name == "LeftShift")    return VK_LSHIFT;
			if (name == "RightShift")   return VK_RSHIFT;
			if (name == "LeftControl")  return VK_LCONTROL;
			if (name == "RightControl") return VK_RCONTROL;
			if (name == "LeftAlt")      return VK_LMENU;
			if (name == "RightAlt")     return VK_RMENU;
			return 0;
		}

		bool KeyIsDown(int vk)
		{
			return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
		}

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

				// Works in every mode, including Off, because Off is exactly when
				// it is needed: the symptom is a character that will not respond
				// after the editor has closed.
				case Action::DumpProbe:
					LOG_TRACE("Input: control probe dump requested (mode %s)",
					          state.mode == Mode::Off ? "Off"
					        : state.mode == Mode::Editor ? "Editor" : "Playback");
					Post(state, Request::DumpControlProbe);
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
					else if (state.mode == Mode::Editor && state.selection != Selection::None)
					{
						// Escape as "step back out of what I am inspecting". The
						// only other way back to the project settings is to find
						// an empty stretch of track to click, which on a full
						// timeline may not exist at all.
						LOG_TRACE("Input: escape cleared the selection");
						state.selection  = Selection::None;
						state.selectedId = 0;
						SetStatus(state, now, "Selection cleared");
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

				// Levels the camera without moving it. Position is untouched on
				// purpose -- the usual reason to want this is a shot that has
				// drifted off-level from rolling, and re-framing from scratch
				// because the level-out also teleported you would be worse than
				// the tilt.
				//
				// flyPose.rotation is the fly camera's only rotation state (see
				// fly_controls, which integrates deltas straight into it), so
				// zeroing it here is the whole job -- the tick writes it onto the
				// camera actor on its way past.
				case Action::ResetRotation:
					state.flyPose.rotation = Rot{ 0.0, 0.0, 0.0 };
					SetStatus(state, now, "Camera rotation reset");
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

		// What was still down when the editor closed, before it is cleared.
		//
		// Worth a line even when the answer is "nothing", because the interesting
		// case is a key we were holding at the moment the modloader stopped
		// swallowing input: the game never saw our press, so it will never see the
		// matching release, and from the player's chair a latched movement key is
		// hard to tell apart from a character that will not respond.
		int stillHeld = 0;
		for (int i = 0; i < static_cast<int>(Action::Count); ++i)
			if (g_held[i]) ++stillHeld;

		if (stillHeld > 0)
		{
			char list[256] = {};
			int  used = 0;
			for (int i = 0; i < kBindCount && used < static_cast<int>(sizeof(list)) - 24; ++i)
			{
				if (!g_held[static_cast<int>(kBinds[i].action)])
					continue;

				used += snprintf(list + used, sizeof(list) - used, "%s%s",
				                 used > 0 ? "," : "", kBinds[i].configKey);
			}
			LOG_TRACE("Input: releasing %d still-held key(s): %s", stillHeld, list);
		}
		else
		{
			LOG_TRACE("Input: no keys were still held at teardown");
		}

		for (bool& held : g_held)
			held = false;

		g_looking    = false;
		g_lookAnchor = POINT{};
		g_lastBoost  = false;
		g_lastCrawl  = false;

		state.flyInput.ClearAll();
	}

	void LogPhysicalMovementKeys(const char* when)
	{
		// The keys the game moves on, plus the ones it acts on. Hard-coded rather
		// than read from the game's bindings because this is about what the OS
		// sees, and the game's own mapping is not reachable from here anyway.
		struct Probe { int vk; const char* name; };
		static const Probe kProbes[] = {
			{ 'W', "W" }, { 'A', "A" }, { 'S', "S" }, { 'D', "D" },
			{ VK_SPACE,   "Space"    },
			{ VK_LSHIFT,  "LShift"   },
			{ VK_LCONTROL,"LCtrl"    },
			{ VK_LBUTTON, "LMouse"   },
			{ VK_RBUTTON, "RMouse"   },
		};

		char list[128] = {};
		int  used  = 0;
		int  count = 0;

		for (const Probe& probe : kProbes)
		{
			if (!KeyIsDown(probe.vk))
				continue;

			++count;
			const int remaining = static_cast<int>(sizeof(list)) - used;
			const int written = snprintf(list + used, remaining, "%s%s",
			                             used > 0 ? "," : "", probe.name);
			if (written < 0 || written >= remaining)
				break;
			used += written;
		}

		if (count > 0)
			LOG_TRACE("Input: physically down at %s: %s", when, list);
		else
			LOG_TRACE("Input: nothing physically down at %s", when);
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

			// Boost and crawl get polled from the OS instead of dispatched, if
			// they are still pointed at a modifier key. Recorded here, where the
			// resolved name is, and logged so the log says which path a given
			// bind is actually taking.
			const int modifierVK = ModifierVKForName(g_resolvedNames[i]);
			if (modifierVK != 0)
			{
				if (kBinds[i].action == Action::FlyBoost) g_boostVK = modifierVK;
				if (kBinds[i].action == Action::FlyCrawl) g_crawlVK = modifierVK;
			}

			const bool polled = modifierVK != 0 &&
			                    (kBinds[i].action == Action::FlyBoost ||
			                     kBinds[i].action == Action::FlyCrawl);

			LOG_DEBUG("Input: '%s' -> %s%s%s", g_resolvedNames[i].c_str(), kBinds[i].configKey,
			          kBinds[i].wantsRelease ? " (held)" : "",
			          polled ? " (polled -- the loader does not dispatch bare modifiers)" : "");

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

	void PumpModifierKeys()
	{
		if (g_boostVK == 0 && g_crawlVK == 0)
			return;

		const bool boost = KeyIsDown(g_boostVK);
		const bool crawl = KeyIsDown(g_crawlVK);

		// Edge-triggered: this runs 60 times a second and the user asked to see
		// these in the log, which only works if they are not also the loudest
		// thing in it.
		if (boost != g_lastBoost)
		{
			g_lastBoost = boost;
			LOG_TRACE("Input: boost (vk 0x%02X) %s", g_boostVK, boost ? "down" : "up");
		}
		if (crawl != g_lastCrawl)
		{
			g_lastCrawl = crawl;
			LOG_TRACE("Input: crawl (vk 0x%02X) %s", g_crawlVK, crawl ? "down" : "up");
		}

		auto lock = Lock();
		State& state = Get();

		if (state.mode != Mode::Editor)
			return;

		g_held[static_cast<int>(Action::FlyBoost)] = boost;
		g_held[static_cast<int>(Action::FlyCrawl)] = crawl;
		RecomputeAxes(state);
	}

	void PumpMouseLook()
	{
		// Same thread, same place in the tick, and it takes the lock the same way.
		PumpModifierKeys();

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
