#include "ui_theme.h"

#include <cmath>
#include <cstdio>

namespace CameraControls::Theme
{
	void TextColored(IModLoaderImGui* ui, const Rgba& c, const char* text)
	{
		ui->TextColored(c.r, c.g, c.b, c.a, text);
	}

	bool AccentButton(IModLoaderImGui* ui, const char* label, float width, float height)
	{
		ui->PushStyleColor(Col_Button,        kAccentDim.r, kAccentDim.g, kAccentDim.b, 1.0f);
		ui->PushStyleColor(Col_ButtonHovered, kAccent.r,    kAccent.g,    kAccent.b,    1.0f);
		ui->PushStyleColor(Col_ButtonActive,  kAccent.r * 1.1f, kAccent.g * 1.1f, kAccent.b * 1.1f, 1.0f);

		const bool clicked = ui->ButtonSized(label, width, height);

		ui->PopStyleColor(3);
		return clicked;
	}

	bool ToggleButton(IModLoaderImGui* ui, const char* label, bool active, float width, float height)
	{
		if (active)
		{
			ui->PushStyleColor(Col_Button,        kAccentDim.r, kAccentDim.g, kAccentDim.b, 1.0f);
			ui->PushStyleColor(Col_ButtonHovered, kAccent.r,    kAccent.g,    kAccent.b,    1.0f);
			ui->PushStyleColor(Col_ButtonActive,  kAccent.r,    kAccent.g,    kAccent.b,    1.0f);
		}

		const bool clicked = ui->ButtonSized(label, width, height);

		if (active)
			ui->PopStyleColor(3);

		return clicked;
	}

	void Tooltip(IModLoaderImGui* ui, const char* text)
	{
		if (!ui->BeginTooltip())
			return;

		// Font-relative rather than a pixel constant: the modloader lets the user
		// scale the UI font, and a fixed 340px that reads well at 100% is a narrow
		// column of two-word lines at 150%. Roughly 42 characters of body text,
		// which is about where a tooltip stops being comfortable to read.
		ui->PushTextWrapPos(ui->GetFontSize() * 24.0f);
		ui->TextWrapped(text);
		ui->PopTextWrapPos();

		ui->EndTooltip();
	}

	void ItemTooltip(IModLoaderImGui* ui, const char* text)
	{
		if (ui->IsItemHovered())
			Tooltip(ui, text);
	}

	float HelpGutter(IModLoaderImGui* ui)
	{
		float w = 0.0f, h = 0.0f;
		ui->CalcTextSize("(?)", &w, &h, false, -1.0f);

		// The text plus the spacing SameLine puts in front of it.
		return w + 10.0f;
	}

	void FullWidthItem(IModLoaderImGui* ui)
	{
		// Negative widths in ImGui mean "stop this far short of the right edge
		// of the content region", and the content region already excludes the
		// scrollbar -- so reserving the gutter here is all it takes for the
		// marker to survive the panel growing a scrollbar.
		ui->SetNextItemWidth(-HelpGutter(ui));
	}

	void HelpMarker(IModLoaderImGui* ui, const char* text)
	{
		ui->SameLine(0.0f, -1.0f);

		// Pin the marker to the right-hand edge rather than letting it sit
		// wherever the previous item happened to end. Long checkbox labels and
		// full-width sliders both push it out of the panel otherwise, and the
		// fixed column reads better than a ragged one regardless.
		float availX = 0.0f, availY = 0.0f;
		ui->GetContentRegionAvail(&availX, &availY);

		float markerW = 0.0f, markerH = 0.0f;
		ui->CalcTextSize("(?)", &markerW, &markerH, false, -1.0f);

		ui->SetCursorPosX(ui->GetCursorPosX() + availX - markerW);

		ui->TextDisabled("(?)");
		ItemTooltip(ui, text);
	}

	void FormatTime(double seconds, char* buffer, int bufferSize)
	{
		if (seconds < 0.0)
			seconds = 0.0;

		const int minutes = static_cast<int>(seconds / 60.0);
		const double rest = seconds - minutes * 60.0;

		snprintf(buffer, static_cast<size_t>(bufferSize), "%d:%05.2f", minutes, rest);
	}
}
