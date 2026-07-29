#pragma once

// ---------------------------------------------------------------------------
// ImGui constants and shared UI helpers.
//
// IModLoaderImGui takes plain ints where ImGui takes its enums, and plugins do
// not (and must not) include imgui.h. The values below are transcribed from the
// modloader's bundled ImGui 1.92.6 -- if the modloader ever bumps ImGui across
// a release that reorders ImGuiCol_ or ImGuiStyleVar_, this is the one file
// that needs revisiting.
// ---------------------------------------------------------------------------

#include "plugin_interface.h"

namespace CameraControls::Theme
{
	// --- ImGuiCol_ -------------------------------------------------------
	enum Col : int
	{
		Col_Text            = 0,
		Col_TextDisabled    = 1,
		Col_WindowBg        = 2,
		Col_ChildBg         = 3,
		Col_PopupBg         = 4,
		Col_Border          = 5,
		Col_FrameBg         = 7,
		Col_FrameBgHovered  = 8,
		Col_FrameBgActive   = 9,
		Col_TitleBg         = 10,
		Col_TitleBgActive   = 11,
		Col_CheckMark       = 18,
		Col_SliderGrab      = 20,
		Col_SliderGrabActive= 21,
		Col_Button          = 22,
		Col_ButtonHovered   = 23,
		Col_ButtonActive    = 24,
		Col_Header          = 25,
		Col_HeaderHovered   = 26,
		Col_HeaderActive    = 27,
		Col_Separator       = 28,
	};

	// --- ImGuiStyleVar_ --------------------------------------------------
	enum StyleVar : int
	{
		Var_Alpha           = 0,
		Var_WindowPadding   = 2,
		Var_WindowRounding  = 3,
		Var_FramePadding    = 11,
		Var_FrameRounding   = 12,
		Var_ItemSpacing     = 14,
		Var_ItemInnerSpacing= 15,
		Var_CellPadding     = 17,
		Var_GrabRounding    = 22,
	};

	// --- ImGuiWindowFlags_ -----------------------------------------------
	enum WindowFlags : int
	{
		Win_None                 = 0,
		Win_NoTitleBar           = 1 << 0,
		Win_NoResize             = 1 << 1,
		Win_NoMove               = 1 << 2,
		Win_NoScrollbar          = 1 << 3,
		Win_NoScrollWithMouse    = 1 << 4,
		Win_NoCollapse           = 1 << 5,
		Win_AlwaysAutoResize     = 1 << 6,
		Win_NoBackground         = 1 << 7,
		Win_NoSavedSettings      = 1 << 8,
		Win_NoMouseInputs        = 1 << 9,
		Win_NoFocusOnAppearing   = 1 << 12,
		Win_NoBringToFrontOnFocus= 1 << 13,
		Win_NoNavInputs          = 1 << 16,
	};

	// --- ImGuiTableFlags_ / ImGuiTableColumnFlags_ ------------------------
	enum TableFlags : int
	{
		Table_Resizable        = 1 << 0,
		Table_SizingStretchProp= 3 << 13,
		Table_SizingFixedFit   = 1 << 13,
		Table_RowBg            = 1 << 6,
		Table_BordersInnerV    = 1 << 9,
	};

	enum TableColumnFlags : int
	{
		Column_WidthStretch = 1 << 3,
		Column_WidthFixed   = 1 << 4,
	};

	// --- Palette ----------------------------------------------------------
	struct Rgba { float r, g, b, a; };

	constexpr Rgba kAccent        { 0.20f, 0.72f, 0.95f, 1.00f };  // cyan, matches the world path
	constexpr Rgba kAccentDim     { 0.14f, 0.42f, 0.55f, 1.00f };
	constexpr Rgba kKeyframe      { 1.00f, 0.65f, 0.10f, 1.00f };  // amber, matches the world gizmo
	constexpr Rgba kKeyframeHover { 1.00f, 0.80f, 0.35f, 1.00f };
	constexpr Rgba kSelected      { 1.00f, 0.98f, 0.90f, 1.00f };
	constexpr Rgba kDisabled      { 0.45f, 0.45f, 0.45f, 1.00f };
	constexpr Rgba kPlayhead      { 0.35f, 1.00f, 0.45f, 1.00f };
	constexpr Rgba kTrackBg       { 0.09f, 0.10f, 0.12f, 1.00f };
	constexpr Rgba kTrackLine     { 0.24f, 0.26f, 0.30f, 1.00f };
	constexpr Rgba kRuler         { 0.55f, 0.58f, 0.64f, 1.00f };
	constexpr Rgba kDanger        { 0.90f, 0.30f, 0.28f, 1.00f };
	constexpr Rgba kFadeMarker    { 0.75f, 0.45f, 1.00f, 1.00f };

	// The matte around the squeezed game view, and the hairline framing it.
	// Opaque on purpose -- it is covering whatever the engine last left in
	// that part of the backbuffer.
	constexpr Rgba kViewerBackdrop{ 0.04f, 0.04f, 0.05f, 1.00f };
	constexpr Rgba kViewerFrame   { 0.30f, 0.33f, 0.38f, 1.00f };

	inline unsigned int Pack(IModLoaderImGui* ui, const Rgba& c, float alphaScale = 1.0f)
	{
		return ui->GetColorU32FromVec4(c.r, c.g, c.b, c.a * alphaScale);
	}

	// Small conveniences the UI files use constantly.
	void TextColored(IModLoaderImGui* ui, const Rgba& c, const char* text);
	bool AccentButton(IModLoaderImGui* ui, const char* label, float width, float height);
	bool ToggleButton(IModLoaderImGui* ui, const char* label, bool active, float width, float height);
	// Trailing "(?)" with a tooltip, right-aligned into the gutter that
	// FullWidthItem reserves so it cannot be pushed off the panel.
	void HelpMarker(IModLoaderImGui* ui, const char* text);

	// Width kept clear at the right-hand edge of a panel for a help marker.
	float HelpGutter(IModLoaderImGui* ui);

	// SetNextItemWidth for a control that should fill the panel. Use this
	// rather than SetNextItemWidth(-1.0f): -1 fills the content region right up
	// to the edge, which leaves a following HelpMarker nowhere to go the moment
	// a scrollbar appears and takes that width away.
	void FullWidthItem(IModLoaderImGui* ui);

	// Formats seconds as m:ss.cc, the notation the timeline ruler uses.
	void FormatTime(double seconds, char* buffer, int bufferSize);
}
