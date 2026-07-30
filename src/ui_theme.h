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

	// --- Icons ------------------------------------------------------------
	//
	// Material Icons glyphs, from the font the modloader merges into the shared
	// ImGui atlas. Nothing has to be registered to use them: ImGui 1.92 bakes
	// glyphs on demand and the modloader's DX12 backend supports that, so
	// `GlyphRanges` over in `imgui_backend.cpp` is only a preload hint. Any of the
	// font's 2188 glyphs works from here.
	//
	// Macros rather than constants on purpose: every one of these is concatenated
	// with an "##id" suffix at the call site, and only string *literals* can be
	// concatenated at compile time. See the ImGui identity rule in CLAUDE.md for
	// why the suffix is not optional.
	//
	// The font's `post` table is version 3.0 and stores no glyph names, so a wrong
	// codepoint is an invisible button rather than a compile error. Every value
	// below was verified by eye against the contact sheets in the modloader's
	// MaterialIcons.md -- check there, not memory, before adding one.
	#define CC_ICON_SKIP_START  "\xEE\x81\x85"   // U+E045 skip_previous
	#define CC_ICON_REWIND      "\xEE\x80\xA0"   // U+E020 fast_rewind
	#define CC_ICON_PLAY        "\xEE\x80\xB7"   // U+E037 play_arrow
	#define CC_ICON_PAUSE       "\xEE\x80\xB4"   // U+E034 pause
	#define CC_ICON_FORWARD     "\xEE\x80\x9F"   // U+E01F fast_forward
	#define CC_ICON_SKIP_END    "\xEE\x81\x84"   // U+E044 skip_next
	#define CC_ICON_RECORD      "\xEE\x81\xA1"   // U+E061 fiber_manual_record
	#define CC_ICON_LINK        "\xEE\x85\x97"   // U+E157 link
	#define CC_ICON_LINK_OFF    "\xEE\x85\xAF"   // U+E16F link_off
	#define CC_ICON_ADD         "\xEE\x85\x85"   // U+E145 add
	#define CC_ICON_PLAYLIST_ADD "\xEE\x80\xBB"  // U+E03B playlist_add
	#define CC_ICON_ZOOM_IN     "\xEE\xA3\xBF"   // U+E8FF zoom_in
	#define CC_ICON_ZOOM_OUT    "\xEE\xA4\x80"   // U+E900 zoom_out
	#define CC_ICON_FIT_SCREEN  "\xEE\xA8\x90"   // U+EA10 fit_screen
	#define CC_ICON_RESET       "\xEE\x97\x95"   // U+E5D5 refresh
	#define CC_ICON_SAVE        "\xEE\x85\xA1"   // U+E161 save
	#define CC_ICON_FOLDER_OPEN "\xEE\x8B\x88"   // U+E2C8 folder_open
	#define CC_ICON_NEW_FILE    "\xEE\xA2\x9C"   // U+E89C note_add
	#define CC_ICON_DELETE      "\xEE\xA1\xB2"   // U+E872 delete
	#define CC_ICON_TARGET      "\xEE\x86\xB3"   // U+E1B3 gps_fixed
	#define CC_ICON_CAMERA      "\xEE\x90\x92"   // U+E412 photo_camera
	#define CC_ICON_FOCUS       "\xEE\x8E\xB4"   // U+E3B4 center_focus_strong
	#define CC_ICON_EARLIER     "\xEE\x97\x84"   // U+E5C4 arrow_back
	#define CC_ICON_LATER       "\xEE\x97\x88"   // U+E5C8 arrow_forward
	#define CC_ICON_CANCEL      "\xEE\x97\x8D"   // U+E5CD close
	#define CC_ICON_BOLT        "\xEE\x8F\xA7"   // U+E3E7 flash_on -- func frames

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

	// Func frames. Far enough from the amber keyframe diamond to be told apart
	// at a glance without reading the row it is on -- the two live on the same
	// track and mean entirely different things.
	constexpr Rgba kFuncFrame     { 0.98f, 0.36f, 0.62f, 1.00f };
	constexpr Rgba kFuncFrameHover{ 1.00f, 0.62f, 0.80f, 1.00f };

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

	// Tooltips that wrap.
	//
	// SetTooltip and SetItemTooltip do not: ImGui auto-sizes the tooltip window to
	// the longest line, so a two-sentence explanation becomes a tooltip wider than
	// the screen. Wrapping at a font-relative width fixes every tooltip at once,
	// and it changes what the "\n\n" breaks in the strings are *for* -- they now
	// separate ideas rather than manually holding lines to a readable length.
	//
	// Tooltip() draws unconditionally (use inside your own hover test);
	// ItemTooltip() gates on IsItemHovered() and replaces SetItemTooltip.
	void Tooltip(IModLoaderImGui* ui, const char* text);
	void ItemTooltip(IModLoaderImGui* ui, const char* text);

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
