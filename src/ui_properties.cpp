#include "ui_properties.h"
#include "ui_theme.h"
#include "project_io.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace CameraControls::UI::Properties
{
	using namespace CameraControls::Theme;

	namespace
	{
		// --- Edit buffers ----------------------------------------------------
		// ImGui's text widgets want a raw char buffer. Each is synced from the
		// model when the thing being inspected changes, and written back on
		// edit -- never the other way round, so typing is never clobbered.
		char     g_keyNameBuf[64]     = {};
		uint32_t g_keyNameBufOwner    = 0;

		char     g_projectNameBuf[64] = {};
		bool     g_projectNameSynced  = false;

		// Project browser state.
		std::vector<std::string> g_projectList;
		bool                     g_projectListStale = true;
		int                      g_selectedProject  = -1;
		std::string              g_ioMessage;
		bool                     g_ioMessageIsError = false;

		void SyncKeyName(const Keyframe& key)
		{
			if (g_keyNameBufOwner == key.id)
				return;

			g_keyNameBufOwner = key.id;
			strncpy_s(g_keyNameBuf, sizeof(g_keyNameBuf), key.name.c_str(), _TRUNCATE);
		}

		void RefreshProjectList()
		{
			g_projectList = ProjectIO::ListProjects();
			g_projectListStale = false;
			if (g_selectedProject >= static_cast<int>(g_projectList.size()))
				g_selectedProject = -1;
		}

		bool EaseCombo(IModLoaderImGui* ui, const char* label, Ease& value)
		{
			bool changed = false;

			FullWidthItem(ui);
			if (ui->BeginCombo(label, EaseName(value)))
			{
				for (int i = 0; i < static_cast<int>(Ease::Count); ++i)
				{
					const Ease option = static_cast<Ease>(i);
					if (ui->Selectable(EaseName(option), option == value))
					{
						value   = option;
						changed = true;
					}
				}
				ui->EndCombo();
			}

			return changed;
		}

		void LabelRow(IModLoaderImGui* ui, const char* label)
		{
			ui->AlignTextToFramePadding();
			ui->TextDisabled(label);
		}

		// -------------------------------------------------------------------
		// Keyframe
		// -------------------------------------------------------------------
		void RenderKeyframe(IModLoaderImGui* ui, State& state, Keyframe& key, int index, double now)
		{
			char header[96];
			snprintf(header, sizeof(header), "Keyframe %d", index + 1);
			ui->SeparatorText(header);

			// --- Identity ---------------------------------------------------
			SyncKeyName(key);
			FullWidthItem(ui);
			if (ui->InputTextWithHint("##name", "Shot name (optional)", g_keyNameBuf, sizeof(g_keyNameBuf)))
			{
				key.name    = g_keyNameBuf;
				state.dirty = true;
			}

			bool enabled = key.enabled;
			if (ui->Checkbox("Enabled", &enabled))
			{
				key.enabled = enabled;
				state.dirty = true;
			}
			HelpMarker(ui, "Unchecked keyframes stay on the timeline but are skipped "
			               "during playback -- handy for trying a shot without deleting anything.");

			ui->Spacing();

			// --- Transform ----------------------------------------------------
			ui->SeparatorText("Transform");

			float location[3] = {
				static_cast<float>(key.location.x),
				static_cast<float>(key.location.y),
				static_cast<float>(key.location.z)
			};
			FullWidthItem(ui);
			if (ui->DragFloat3("##loc", location, 5.0f, 0.0f, 0.0f, "%.0f"))
			{
				key.location = Vec3{ location[0], location[1], location[2] };
				state.dirty  = true;
			}
			LabelRow(ui, "Location  X / Y / Z (cm)");

			float rotation[3] = {
				static_cast<float>(key.rotation.pitch),
				static_cast<float>(key.rotation.yaw),
				static_cast<float>(key.rotation.roll)
			};
			FullWidthItem(ui);
			if (ui->DragFloat3("##rot", rotation, 0.25f, -360.0f, 360.0f, "%.1f"))
			{
				key.rotation = Rot{ rotation[0], rotation[1], rotation[2] };
				state.dirty  = true;
			}
			LabelRow(ui, "Rotation  pitch / yaw / roll");

			FullWidthItem(ui);
			if (ui->SliderFloat("##fov", &key.fov, 5.0f, 170.0f, "FOV  %.1f deg"))
				state.dirty = true;

			ui->Spacing();

			// --- Aim ----------------------------------------------------------
			ui->SeparatorText("Aim");

			bool lookAt = key.lookAt;
			if (ui->Checkbox("Look at a point", &lookAt))
			{
				key.lookAt  = lookAt;
				state.dirty = true;
			}
			HelpMarker(ui, "The camera aims at a fixed world point instead of using the "
			               "stored rotation. Roll still comes from the rotation above.");

			if (key.lookAt)
			{
				float target[3] = {
					static_cast<float>(key.lookAtTarget.x),
					static_cast<float>(key.lookAtTarget.y),
					static_cast<float>(key.lookAtTarget.z)
				};
				FullWidthItem(ui);
				if (ui->DragFloat3("##lookat", target, 5.0f, 0.0f, 0.0f, "%.0f"))
				{
					key.lookAtTarget = Vec3{ target[0], target[1], target[2] };
					state.dirty      = true;
				}
				LabelRow(ui, "Target  X / Y / Z");

				if (ui->Button("Aim target from camera"))
					Post(state, Request::LookAtSelectionFromCamera);
				HelpMarker(ui, "Puts the target 10 m in front of wherever the fly camera "
				               "is currently pointing.");
			}

			ui->Spacing();

			// --- Motion --------------------------------------------------------
			ui->SeparatorText("Motion");

			FullWidthItem(ui);
			if (ui->SliderFloat("##smooth", &key.smoothness, 0.0f, 1.0f, "Smoothness  %.2f"))
				state.dirty = true;
			HelpMarker(ui, "0 makes the path turn a sharp corner here; 1 curves through it. "
			               "Neighbouring keyframes are averaged, so a single sharp key is "
			               "enough to break a curve.");

			LabelRow(ui, "Ease out of this key");
			if (EaseCombo(ui, "##easeout", key.easeOut))
				state.dirty = true;

			LabelRow(ui, "Ease into this key");
			if (EaseCombo(ui, "##easein", key.easeIn))
				state.dirty = true;

			HelpMarker(ui, "Auto is almost always what you want: the camera eases away from "
			               "the first keyframe of the shot and settles into the last one, and "
			               "carries its speed straight through every keyframe in between.\n\n"
			               "Every other curve flattens to zero slope at its ends, so putting "
			               "one on both sides of a middle keyframe brings the camera to a "
			               "dead stop as it passes through. That is worth doing deliberately "
			               "to hold on a subject, and jarring by accident.");

			if (key.easeIn == Ease::Auto || key.easeOut == Ease::Auto)
			{
				const bool first = (index == 0);
				const bool last  = (index == state.timeline.Count() - 1);

				ui->TextDisabled(first ? "Auto here: eases out of the start of the shot."
				                       : last ? "Auto here: settles into the end of the shot."
				                              : "Auto here: flows through without slowing down.");
			}

			ui->Spacing();

			// --- Fades ----------------------------------------------------------
			ui->SeparatorText("Fades");

			bool fadeIn = key.fadeIn;
			if (ui->Checkbox("Fade in from colour", &fadeIn))
			{
				key.fadeIn  = fadeIn;
				state.dirty = true;
			}
			if (key.fadeIn)
			{
				FullWidthItem(ui);
				if (ui->SliderFloat("##fadeindur", &key.fadeInDuration, 0.1f, 10.0f, "%.2f s"))
					state.dirty = true;
			}

			bool fadeOut = key.fadeOut;
			if (ui->Checkbox("Fade out to colour", &fadeOut))
			{
				key.fadeOut = fadeOut;
				state.dirty = true;
			}
			if (key.fadeOut)
			{
				FullWidthItem(ui);
				if (ui->SliderFloat("##fadeoutdur", &key.fadeOutDuration, 0.1f, 10.0f, "%.2f s"))
					state.dirty = true;
			}

			if (key.fadeIn || key.fadeOut)
			{
				if (ui->ColorEdit3("Fade colour", key.fadeColor))
					state.dirty = true;
			}

			ui->Spacing();
			ui->Separator();
			ui->Spacing();

			// --- Actions ----------------------------------------------------------
			float availX = 0.0f, availY = 0.0f;
			ui->GetContentRegionAvail(&availX, &availY);
			const float halfWidth = (availX - 6.0f) * 0.5f;
			const float rowHeight = ui->GetFrameHeight() * 1.15f;

			if (AccentButton(ui, "Fly camera here", halfWidth, rowHeight))
				Post(state, Request::GotoSelected);
			ui->SetItemTooltip("Moves the free camera to this keyframe's exact pose, so you can "
			                   "carry on composing from it. Same as double-clicking it.");

			ui->SameLine(0.0f, 6.0f);

			if (AccentButton(ui, "Record from camera", halfWidth, rowHeight))
				Post(state, Request::UpdateSelectedFromCamera);
			ui->SetItemTooltip("Overwrites this keyframe's position, rotation and FOV with "
			                   "wherever the free camera is now.");

			if (ui->ButtonSized("Move earlier", halfWidth, rowHeight))
			{
				if (state.timeline.MoveEarlier(key.id))
					state.dirty = true;
			}
			ui->SameLine(0.0f, 6.0f);
			if (ui->ButtonSized("Move later", halfWidth, rowHeight))
			{
				if (state.timeline.MoveLater(key.id))
					state.dirty = true;
			}

			ui->PushStyleColor(Col_Button,        kDanger.r * 0.6f, kDanger.g * 0.6f, kDanger.b * 0.6f, 1.0f);
			ui->PushStyleColor(Col_ButtonHovered, kDanger.r, kDanger.g, kDanger.b, 1.0f);
			ui->PushStyleColor(Col_ButtonActive,  kDanger.r, kDanger.g, kDanger.b, 1.0f);
			if (ui->ButtonSized("Delete keyframe", availX, rowHeight))
			{
				const uint32_t doomed = key.id;
				state.selectedId = 0;
				state.selection  = Selection::None;
				state.timeline.Remove(doomed);
				state.dirty = true;
				SetStatus(state, now, "Keyframe deleted");
			}
			ui->PopStyleColor(3);
		}

		// -------------------------------------------------------------------
		// Segment
		// -------------------------------------------------------------------
		void RenderSegment(IModLoaderImGui* ui, State& state, int index)
		{
			Timeline& timeline = state.timeline;
			auto& keys = timeline.Keys();

			char header[96];
			snprintf(header, sizeof(header), "Segment %d  ->  %d", index + 1, index + 2);
			ui->SeparatorText(header);

			Keyframe& key = keys[index];

			float baseDuration = static_cast<float>(key.duration);
			FullWidthItem(ui);
			if (ui->DragFloat("##dur", &baseDuration, 0.05f, 0.05f, 600.0f, "Base duration  %.2f s"))
			{
				key.duration = std::max(static_cast<double>(baseDuration), Timeline::kMinDuration);
				state.dirty  = true;
			}

			FullWidthItem(ui);
			if (ui->DragFloat("##speed", &key.speed, 0.01f, 0.05f, 20.0f, "Speed  %.2fx"))
			{
				key.speed   = Clamp(key.speed, 0.05f, 20.0f);
				state.dirty = true;
			}
			HelpMarker(ui, "Playback rate for this segment, exactly like clip speed in a video "
			               "editor: 2x covers the same path in half the time and pulls every "
			               "later keyframe earlier.");

			ui->Spacing();

			// --- Read-outs -------------------------------------------------------
			const double effective = timeline.EffectiveDuration(index);
			const Vec3   delta     = keys[index + 1].location - key.location;
			const double distance  = delta.Length();

			char line[128];
			snprintf(line, sizeof(line), "Plays for   %.2f s", effective);
			ui->Text(line);

			snprintf(line, sizeof(line), "Straight-line distance   %.1f m", distance / 100.0);
			ui->Text(line);

			if (effective > 0.0)
			{
				snprintf(line, sizeof(line), "Average speed   %.1f m/s", (distance / 100.0) / effective);
				ui->Text(line);
			}

			snprintf(line, sizeof(line), "Starts at   %.2f s", timeline.AbsoluteTime(index));
			ui->TextDisabled(line);

			ui->Spacing();
			ui->Separator();
			ui->Spacing();

			float availX = 0.0f, availY = 0.0f;
			ui->GetContentRegionAvail(&availX, &availY);
			const float rowHeight = ui->GetFrameHeight() * 1.15f;

			if (AccentButton(ui, "Insert keyframe here from camera", availX, rowHeight))
			{
				state.selectedId = key.id;
				Post(state, Request::CaptureInsertAfterSelected);
			}
			ui->SetItemTooltip("Splits this segment in two at the current camera pose, "
			                   "without changing when any later keyframe happens.");

			if (ui->ButtonSized("Select the keyframe that starts it", availX, rowHeight))
			{
				state.selection  = Selection::Keyframe;
				state.selectedId = key.id;
			}
		}

		// -------------------------------------------------------------------
		// Project / options
		// -------------------------------------------------------------------
		void RenderProject(IModLoaderImGui* ui, State& state, double now)
		{
			Timeline& timeline = state.timeline;

			ui->SeparatorText("Project");

			if (!g_projectNameSynced)
			{
				strncpy_s(g_projectNameBuf, sizeof(g_projectNameBuf), timeline.name.c_str(), _TRUNCATE);
				g_projectNameSynced = true;
			}

			FullWidthItem(ui);
			if (ui->InputTextWithHint("##projname", "Project name", g_projectNameBuf, sizeof(g_projectNameBuf)))
			{
				timeline.name = g_projectNameBuf;
				state.dirty   = true;
			}

			char line[160];
			snprintf(line, sizeof(line), "%d keyframes   %.2f s total",
			         timeline.Count(), timeline.TotalDuration());
			ui->TextDisabled(line);

			if (state.dirty)
				TextColored(ui, kKeyframe, "Unsaved changes");

			ui->Spacing();

			float availX = 0.0f, availY = 0.0f;
			ui->GetContentRegionAvail(&availX, &availY);
			const float rowHeight = ui->GetFrameHeight() * 1.15f;
			const float halfWidth = (availX - 6.0f) * 0.5f;

			if (AccentButton(ui, "Save", halfWidth, rowHeight))
			{
				std::string error;
				if (ProjectIO::Save(timeline.name, timeline, error))
				{
					state.dirty        = false;
					g_projectListStale = true;
					g_ioMessage        = "Saved " + timeline.name;
					g_ioMessageIsError = false;
					SetStatus(state, now, g_ioMessage.c_str());
				}
				else
				{
					g_ioMessage        = "Save failed: " + error;
					g_ioMessageIsError = true;
				}
			}

			ui->SameLine(0.0f, 6.0f);

			if (ui->ButtonSized("New", halfWidth, rowHeight))
			{
				timeline.Clear();
				timeline.name       = "Untitled";
				state.selectedId    = 0;
				state.selection     = Selection::None;
				state.playhead      = 0.0;
				state.playing       = false;
				state.dirty         = false;
				g_projectNameSynced = false;
				g_keyNameBufOwner   = 0;
				SetStatus(state, now, "New project");
			}

			ui->Spacing();

			// --- Browser -----------------------------------------------------
			if (g_projectListStale)
				RefreshProjectList();

			ui->SeparatorText("Saved projects");

			if (g_projectList.empty())
			{
				ui->TextDisabled("Nothing saved yet.");
			}
			else if (ui->BeginListBox("##projects", -1.0f, ui->GetTextLineHeightWithSpacing() * 5.0f))
			{
				for (int i = 0; i < static_cast<int>(g_projectList.size()); ++i)
				{
					if (ui->Selectable(g_projectList[i].c_str(), i == g_selectedProject))
						g_selectedProject = i;
				}
				ui->EndListBox();
			}

			const bool haveSelection = g_selectedProject >= 0 &&
			                           g_selectedProject < static_cast<int>(g_projectList.size());

			ui->BeginDisabled(!haveSelection);

			if (ui->ButtonSized("Load", halfWidth, rowHeight))
			{
				std::string error;
				Timeline loaded;
				if (ProjectIO::Load(g_projectList[g_selectedProject], loaded, error))
				{
					timeline            = std::move(loaded);
					state.selectedId    = 0;
					state.selection     = Selection::None;
					state.playhead      = 0.0;
					state.playing       = false;
					state.dirty         = false;
					g_projectNameSynced = false;
					g_keyNameBufOwner   = 0;
					g_ioMessage         = "Loaded " + timeline.name;
					g_ioMessageIsError  = false;
					SetStatus(state, now, g_ioMessage.c_str());
				}
				else
				{
					g_ioMessage        = "Load failed: " + error;
					g_ioMessageIsError = true;
				}
			}

			ui->SameLine(0.0f, 6.0f);

			if (ui->ButtonSized("Delete file", halfWidth, rowHeight))
				ui->OpenPopup("##confirmdelete", 0);

			ui->EndDisabled();

			if (ui->BeginPopup("##confirmdelete", 0))
			{
				ui->Text("Delete this project file permanently?");
				if (haveSelection)
					ui->TextDisabled(g_projectList[g_selectedProject].c_str());

				if (ui->Button("Delete"))
				{
					if (haveSelection)
					{
						std::string error;
						if (ProjectIO::Delete(g_projectList[g_selectedProject], error))
						{
							g_ioMessage        = "Deleted";
							g_ioMessageIsError = false;
						}
						else
						{
							g_ioMessage        = "Delete failed: " + error;
							g_ioMessageIsError = true;
						}
						g_projectListStale = true;
						g_selectedProject  = -1;
					}
					ui->CloseCurrentPopup();
				}
				ui->SameLine(0.0f, 6.0f);
				if (ui->Button("Cancel"))
					ui->CloseCurrentPopup();

				ui->EndPopup();
			}

			if (!g_ioMessage.empty())
				TextColored(ui, g_ioMessageIsError ? kDanger : kPlayhead, g_ioMessage.c_str());

			if (ui->SmallButton("Refresh list"))
				g_projectListStale = true;
			ui->SameLine(0.0f, 8.0f);
			if (ui->SmallButton("Where are these?"))
				SetStatus(state, now, ProjectIO::ProjectsDirectory().c_str());

			ui->Spacing();

			// --- Playback settings ---------------------------------------------
			ui->SeparatorText("Playback");

			FullWidthItem(ui);
			if (ui->SliderFloat("##globalspeed", &timeline.globalSpeed, 0.1f, 4.0f, "Global speed  %.2fx"))
				state.dirty = true;
			HelpMarker(ui, "Scales the whole timeline on top of each segment's own speed.");

			if (ui->Checkbox("Loop", &timeline.loop))
				state.dirty = true;

			FullWidthItem(ui);
			if (ui->DragFloat("##framerate", &timeline.frameRate, 1.0f, 1.0f, 240.0f,
			                  "Frame rate  %.0f fps"))
			{
				timeline.frameRate = Clamp(timeline.frameRate, 1.0f, 240.0f);
				state.dirty = true;
			}
			HelpMarker(ui, "Only the unit for the timeline's nudge buttons -- playback itself "
			               "is continuous and frame-rate independent. Set it to whatever you "
			               "record at, or to 1 to nudge in whole seconds.");

			FullWidthItem(ui);
			ui->SliderFloat("##countdown", &state.options.countdownSeconds, 0.0f, 10.0f,
			                "Countdown  %.0f s");
			HelpMarker(ui, "Pre-roll before playback starts, so there is time to get a "
			               "recorder running.");

			ui->Checkbox("Hide the game HUD", &state.options.hideGameHud);
			HelpMarker(ui, "Hides the game's own interface for as long as the editor camera "
			               "is flying, not just during a take -- the HUD belongs to a player "
			               "who is not looking through this camera.\n\n"
			               "This is the same switch the game's own cinematic-mode key throws, "
			               "and whatever state you had it in is restored when you leave.");
			ui->Checkbox("Keep drawing gizmos while playing", &state.options.gizmosDuringPlayback);

			ui->Spacing();

			// --- Editor settings ------------------------------------------------
			ui->SeparatorText("Editor");

			ui->BeginDisabled(!state.gameViewSupported);
			ui->Checkbox("Fit the game view beside the panels", &state.options.fitViewport);
			ui->EndDisabled();
			HelpMarker(ui, "Squeezes the game's 3D view into the space the timeline and this "
			               "inspector are not covering, the way an editing program frames its "
			               "viewer -- so nothing you are composing is hidden behind a panel.\n\n"
			               "The picture keeps the window's shape rather than the free area's, "
			               "so the framing you see is exactly what a full-screen playback "
			               "records. Playback always goes back to the whole window.");

			if (!state.gameViewSupported)
				ui->TextDisabled("Unavailable: this game build lays its viewport out differently.");

			ui->Checkbox("Show path gizmos in the world", &state.options.showGizmos);

			ui->Checkbox("Paint fades while editing", &state.options.previewFades);
			HelpMarker(ui, "Off (default): a fade shows as a coloured bar across the top of the "
			               "picture, so you can see one is happening without losing sight of the "
			               "shot. On: the fade is painted for real.\n\n"
			               "Either way it only ever covers the picture, never the panels, and "
			               "playback always shows the real thing.");

			ui->Checkbox("Camera follows the playhead", &state.options.scrubPreview);
			HelpMarker(ui, "Scrubbing the timeline moves the camera live. Turn off to keep "
			               "the free camera where it is while inspecting timing.");

			ui->Checkbox("Ripple edits", &state.options.rippleEdits);
			HelpMarker(ui, "On: dragging a keyframe pushes every later one along. "
			               "Off: the next segment absorbs the change, so only this keyframe moves.");

			FullWidthItem(ui);
			ui->SliderInt("##splinesamples", &state.options.splineSamples, 2, 64,
			              "Path detail  %d per segment");

			FullWidthItem(ui);
			ui->SliderFloat("##gizmoscale", &state.options.gizmoScale, 0.25f, 40.0f,
			                "Gizmo size  %.2f");
			HelpMarker(ui, "How big the keyframe camera icons are drawn in the world. "
			               "3 is roughly a half-metre camera body -- turn it up for wide "
			               "landscape shots, down when working inside a building.");

			FullWidthItem(ui);
			ui->SliderFloat("##nearcull", &state.options.gizmoNearCull, 0.0f, 2000.0f,
			                "Hide gizmos within  %.0f");
			HelpMarker(ui, "How big the keyframe camera icons are drawn in the world. "
			               "3 is roughly a half-metre camera body -- turn it up for wide "
			               "landscape shots, down when working inside a building.");

			FullWidthItem(ui);
			ui->SliderFloat("##flyspeed", &state.options.flySpeed, 100.0f, 12000.0f,
			                "Fly speed  %.0f u/s");

			FullWidthItem(ui);
			ui->SliderFloat("##sensitivity", &state.options.mouseSensitivity, 0.02f, 1.5f,
			                "Mouse sensitivity  %.2f");

			ui->Checkbox("Projection self-test", &state.options.projectionDebug);
			HelpMarker(ui, "Diagnostic. Draws a grey cross at the centre of the picture and a "
			               "green ring where a point straight ahead of the camera projects to. "
			               "They should sit exactly on top of each other at every angle.\n\n"
			               "If they drift apart, the camera basis or the view rectangle is "
			               "wrong. If they stay together but the keyframe handles are still "
			               "misplaced, it is the field of view or the aspect ratio.");

			ui->Spacing();

			// --- Safety -----------------------------------------------------------
			ui->SeparatorText("Player safety");

			ui->Checkbox("Stash the player and carry them along", &state.options.protectPlayer);
			HelpMarker(ui, "While the camera is detached your body is left standing in the open. "
			               "This freezes it and tows it along under the camera -- which also "
			               "keeps the world streaming in around the shot, since StarRupture "
			               "streams around the player, not around the view. Everything is put "
			               "back exactly as it was on exit.");

			ui->BeginDisabled(!state.options.protectPlayer);

			FullWidthItem(ui);
			ui->DragFloat("##followz", &state.options.followOffsetZ, 5.0f, -20000.0f, 20000.0f,
			              "Height vs camera  %.0f");
			HelpMarker(ui, "Negative keeps the body below the camera -- under the terrain when "
			               "filming near the ground, and out of frame when filming from above. "
			               "Positive puts it overhead instead.");

			ui->Checkbox("Also spawn a habitat shelter", &state.options.spawnHabitat);
			HelpMarker(ui, "Experimental. Spawns a habitat building around the body, which then "
			               "travels with it. It is a full building actor, so not every game build "
			               "takes kindly to one appearing out of nowhere -- leave off if anything "
			               "misbehaves.");

			ui->Checkbox("Show a marker where the body is", &state.options.showPlayerMarker);
			HelpMarker(ui, "Draws a person-sized box in the world at the stash point, so you can "
			               "see it is keeping up.");

			ui->EndDisabled();

			if (state.options.protectPlayer)
				ui->TextDisabled("The stash is applied when you enter the editor.");

			ui->Spacing();

			ui->Checkbox("Lock health, food and water", &state.options.lockVitals);
			HelpMarker(ui, "Holds health, food, water, oxygen, energy and shield at full, and "
			               "the hazard meters (toxicity, radiation, heat, drain, corrosion, "
			               "infection) at zero, for as long as the editor is open. Survival "
			               "still ticks down while you compose a shot, and a long session "
			               "will starve you otherwise.\n\n"
			               "Temperature is left alone -- it is a comfortable band rather than "
			               "a bar, so there is no single safe value to pin it to.\n\n"
			               "Values are not put back when you leave: you come out topped up.");
		}
	}

	void Reset()
	{
		g_keyNameBufOwner   = 0;
		g_projectNameSynced = false;
		g_projectListStale  = true;
		g_selectedProject   = -1;
		g_ioMessage.clear();
	}

	void Render(IModLoaderImGui* ui, State& state, double now)
	{
		switch (state.selection)
		{
			case Selection::Keyframe:
			{
				const int index = state.timeline.IndexOf(state.selectedId);
				if (index >= 0)
				{
					RenderKeyframe(ui, state, state.timeline.Keys()[index], index, now);
					return;
				}
				break;
			}

			case Selection::Segment:
			{
				const int index = state.timeline.IndexOf(state.selectedId);
				if (index >= 0 && index + 1 < state.timeline.Count())
				{
					RenderSegment(ui, state, index);
					return;
				}
				break;
			}

			default:
				break;
		}

		// Nothing (or something stale) selected -- fall back to the project.
		RenderProject(ui, state, now);
	}
}
