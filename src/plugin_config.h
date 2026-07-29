#pragma once

#include "plugin_interface.h"

namespace CameraControlsConfig
{
	// Config schema — the .ini is created from this on first run.
	// Keybind entries are rebindable from the modloader's plugin config UI;
	// the modloader re-registers them automatically when the user changes one.
	static const ConfigEntry CONFIG_ENTRIES[] = {
		{ "General", "Enabled", ConfigValueType::Boolean, "true",
		  "Enable or disable the plugin" },

		// --- Mode keys -------------------------------------------------------
		{ "Keybinds", "ToggleEditor", ConfigValueType::Keybind, "F7",
		  "Enter / leave the camera timeline editor" },
		{ "Keybinds", "TogglePlayback", ConfigValueType::Keybind, "F8",
		  "Start / stop timeline playback" },
		{ "Keybinds", "ToggleUI", ConfigValueType::Keybind, "F9",
		  "Hide / show the editor windows without leaving editor mode" },
		{ "Keybinds", "ExitPlayback", ConfigValueType::Keybind, "Escape",
		  "Stop playback and return to the editor" },

		// --- Flying ----------------------------------------------------------
		{ "Keybinds", "FlyForward", ConfigValueType::Keybind, "W", "Fly camera forward" },
		{ "Keybinds", "FlyBack",    ConfigValueType::Keybind, "S", "Fly camera backward" },
		{ "Keybinds", "FlyLeft",    ConfigValueType::Keybind, "A", "Fly camera left" },
		{ "Keybinds", "FlyRight",   ConfigValueType::Keybind, "D", "Fly camera right" },
		{ "Keybinds", "FlyDown",    ConfigValueType::Keybind, "Q", "Fly camera down" },
		{ "Keybinds", "FlyUp",      ConfigValueType::Keybind, "E", "Fly camera up" },
		{ "Keybinds", "FlyBoost",   ConfigValueType::Keybind, "LeftShift",   "Hold to fly faster" },
		{ "Keybinds", "FlyCrawl",   ConfigValueType::Keybind, "LeftControl", "Hold to fly slower" },
		{ "Keybinds", "RollLeft",   ConfigValueType::Keybind, "Z", "Roll the camera anticlockwise" },
		{ "Keybinds", "RollRight",  ConfigValueType::Keybind, "C", "Roll the camera clockwise" },
		{ "Keybinds", "FovIn",      ConfigValueType::Keybind, "R", "Narrow the field of view" },
		{ "Keybinds", "FovOut",     ConfigValueType::Keybind, "F", "Widen the field of view" },

		// --- Editing ---------------------------------------------------------
		{ "Keybinds", "CaptureKeyframe", ConfigValueType::Keybind, "K",
		  "Add a keyframe at the current camera pose" },
		{ "Keybinds", "InsertKeyframe",  ConfigValueType::Keybind, "Shift+K",
		  "Insert a keyframe after the selected one" },
		{ "Keybinds", "UpdateKeyframe",  ConfigValueType::Keybind, "U",
		  "Overwrite the selected keyframe with the current camera pose" },
		{ "Keybinds", "GotoKeyframe",    ConfigValueType::Keybind, "G",
		  "Move the camera to the selected keyframe" },
		{ "Keybinds", "DeleteKeyframe",  ConfigValueType::Keybind, "Delete",
		  "Delete the selected keyframe" },
		{ "Keybinds", "PrevKeyframe",    ConfigValueType::Keybind, "Comma",
		  "Select the previous keyframe" },
		{ "Keybinds", "NextKeyframe",    ConfigValueType::Keybind, "Period",
		  "Select the next keyframe" },
		{ "Keybinds", "PlayPause",       ConfigValueType::Keybind, "SpaceBar",
		  "Preview play / pause on the timeline" },

		// --- Behaviour -------------------------------------------------------
		{ "Camera", "FlySpeed", ConfigValueType::Float, "1200",
		  "Base fly speed in Unreal units per second", 50.0f, 20000.0f },
		{ "Camera", "MouseSensitivity", ConfigValueType::Float, "0.25",
		  "Degrees of camera rotation per pixel of mouse movement", 0.01f, 2.0f },
		{ "Camera", "CountdownSeconds", ConfigValueType::Float, "3",
		  "Pre-roll countdown shown before playback starts", 0.0f, 10.0f },

		{ "Editor", "FitViewport", ConfigValueType::Boolean, "true",
		  "Shrink the game's 3D view into the area the timeline and inspector are "
		  "not covering, like an editing program's viewer. The picture keeps the "
		  "window's shape, so what you frame is what a full-screen playback records." },
		{ "Editor", "ShowGizmos", ConfigValueType::Boolean, "true",
		  "Draw the camera path and keyframes in the 3D world" },
		{ "Editor", "SplineSamples", ConfigValueType::Integer, "16",
		  "Line segments drawn per timeline segment (higher = smoother path)", 2.0f, 64.0f },
		{ "Editor", "GizmoScale", ConfigValueType::Float, "3",
		  "Size of the keyframe camera icons drawn in the world "
		  "(multiplier -- 3 is roughly a half-metre camera body)", 0.25f, 40.0f },
		{ "Editor", "GizmoNearCull", ConfigValueType::Float, "150",
		  "Hide path lines and gizmos closer than this many units to the camera. "
		  "A line crossing the near plane smears across the whole screen.", 0.0f, 5000.0f },
		{ "Editor", "HideGameHud", ConfigValueType::Boolean, "true",
		  "Hide the game's own interface for as long as the editor camera is flying, "
		  "not just during a take. This is the same switch the game's own cinematic-mode "
		  "key uses, so it is put back exactly as it was on the way out." },
		{ "Editor", "GizmosDuringPlayback", ConfigValueType::Boolean, "false",
		  "Keep drawing the path gizmos while the timeline is playing" },
		{ "Editor", "PassthroughInput", ConfigValueType::Boolean, "false",
		  "Let keyboard and mouse still reach the game while the editor is open. "
		  "Off (default) freezes game input so stray clicks cannot fire weapons." },

		{ "Safety", "ProtectPlayer", ConfigValueType::Boolean, "true",
		  "Move the player somewhere safe and hold them still while the camera is detached" },
		{ "Safety", "SpawnHabitat", ConfigValueType::Boolean, "false",
		  "Also spawn a habitat shelter around the stashed player. Experimental -- "
		  "this spawns a full building actor, which not every game build likes." },
		{ "Safety", "FollowOffsetZ", ConfigValueType::Float, "-400",
		  "Height of the stashed player relative to the camera, in Unreal units. "
		  "Negative keeps them below it. They ride along with the camera so the "
		  "world keeps streaming in around the shot.", -20000.0f, 20000.0f },
		{ "Safety", "ShowPlayerMarker", ConfigValueType::Boolean, "true",
		  "Draw a marker in the world where the stashed player is" },
		{ "Safety", "LockVitals", ConfigValueType::Boolean, "true",
		  "Hold health, food, water, oxygen, energy and shield at full -- and the "
		  "hazard meters at zero -- while the editor is open. They stay topped up "
		  "when you leave." },
	};

	static const ConfigSchema SCHEMA = {
		CONFIG_ENTRIES,
		sizeof(CONFIG_ENTRIES) / sizeof(ConfigEntry)
	};

	// Typed accessors — the only sanctioned way to read plugin settings.
	class Config
	{
	public:
		static void Initialize(IPluginSelf* self);

		static bool IsEnabled();

		// Copies the configured key name for `key` into `outBuffer`.
		// Returns outBuffer for convenient inline use.
		static const char* GetKeybind(const char* key, const char* fallback,
		                              char* outBuffer, int bufferSize);

		static float FlySpeed();
		static float MouseSensitivity();
		static float CountdownSeconds();

		static bool  FitViewport();
		static bool  ShowGizmos();
		static int   SplineSamples();
		static float GizmoScale();
		static float GizmoNearCull();
		static bool  HideGameHud();
		static bool  GizmosDuringPlayback();
		static bool  PassthroughInput();

		static bool  ProtectPlayer();
		static bool  SpawnHabitat();
		static float FollowOffsetZ();
		static bool  ShowPlayerMarker();
		static bool  LockVitals();

	private:
		static IPluginSelf* s_self;
	};
}
