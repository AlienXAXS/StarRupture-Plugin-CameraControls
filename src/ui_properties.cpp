#include "ui_properties.h"
#include "ui_theme.h"
#include "project_io.h"

#include <algorithm>
#include <cmath>
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

		char     g_funcNameBuf[64]    = {};
		uint32_t g_funcNameBufOwner   = 0;

		char     g_projectNameBuf[64] = {};
		bool     g_projectNameSynced  = false;

		// Project browser state.
		std::vector<std::string> g_projectList;
		bool                     g_projectListStale = true;
		int                      g_selectedProject  = -1;
		std::string              g_ioMessage;
		bool                     g_ioMessageIsError = false;

		// Width of the little [R] reset button that sits on the rotation row.
		constexpr float kResetButtonWidth = 26.0f;

		// Is the fly camera still sitting exactly on this keyframe's pose?
		//
		// The tolerances are tight rather than generous on purpose. GotoSelected
		// copies the keyframe's pose into flyPose verbatim, so a camera that flew
		// here and has not been touched matches to within float conversion. Fly
		// even slightly and it stops matching, which is the intent: the moment the
		// user is composing somewhere else, editing a keyframe must not drag their
		// view back to it.
		bool CameraIsOnKeyframe(const State& state, const Keyframe& key)
		{
			// Compare against the same *effective* rotation GotoSelected applies,
			// not against key.rotation: for a look-at keyframe the camera is aimed
			// at the target instead, so comparing the stored rotation would say
			// "not here" for every look-at key and the live update would silently
			// only work on half of them.
			const Rot aim = key.lookAt ? LookAtRotation(key.location, key.lookAtTarget)
			                           : key.rotation;

			const Vec3   delta = state.flyPose.location - key.location;
			const double drift = std::fabs(state.flyPose.rotation.pitch - aim.pitch) +
			                     std::fabs(state.flyPose.rotation.yaw   - aim.yaw) +
			                     std::fabs(state.flyPose.rotation.roll  - aim.roll);

			return delta.Length() < 1.0 &&
			       drift < 0.1 &&
			       std::fabs(state.flyPose.fov - key.fov) < 0.1f;
		}

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

			// Whether the fly camera is still parked on this keyframe's pose --
			// i.e. the user flew to it (or just made it here) and has not moved
			// since. Sampled *before* the edits below, because they are about to
			// change what "this keyframe's pose" means.
			//
			// When it holds, every edit is fed back to the live camera so the
			// viewport shows the change as it is dragged. When it does not, the
			// user is composing somewhere else and yanking their camera to the
			// keyframe would be the last thing they want.
			const bool cameraOnKey = CameraIsOnKeyframe(state, key);
			bool        poseEdited = false;

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
				poseEdited   = true;
			}
			LabelRow(ui, "Location  X / Y / Z (cm)");

			float rotation[3] = {
				static_cast<float>(key.rotation.pitch),
				static_cast<float>(key.rotation.yaw),
				static_cast<float>(key.rotation.roll)
			};

			// Room for the [R] on the same row, on top of the help gutter.
			ui->SetNextItemWidth(-(HelpGutter(ui) + kResetButtonWidth + 4.0f));
			if (ui->DragFloat3("##rot", rotation, 0.25f, -360.0f, 360.0f, "%.1f"))
			{
				key.rotation = Rot{ rotation[0], rotation[1], rotation[2] };
				state.dirty  = true;
				poseEdited   = true;
			}

			ui->SameLine(0.0f, 4.0f);
			if (ui->ButtonSized(CC_ICON_RESET "##resetrot", kResetButtonWidth, 0.0f))
			{
				key.rotation = Rot{ 0.0, 0.0, 0.0 };
				state.dirty  = true;
				poseEdited   = true;
			}
			if (ui->IsItemHovered())
				ui->SetTooltip("Reset pitch, yaw and roll to zero (level, facing world +X)");

			LabelRow(ui, "Rotation  pitch / yaw / roll");

			FullWidthItem(ui);
			if (ui->SliderFloat("##fov", &key.fov, 5.0f, 170.0f, "FOV  %.1f deg"))
			{
				state.dirty = true;
				poseEdited  = true;
			}

			// Said out loud rather than left to be discovered. This slider edits the
			// keyframe either way -- the gizmo and the saved project honour it -- but
			// with the override unarmed the game's camera manager keeps overwriting
			// the rendered FOV, and a control that moves the number without moving
			// the picture is the exact bug `fov_override` exists to fix.
			if (state.rigActive && !state.fovLive)
			{
				TextColored(ui, kDanger, "FOV is not reaching the screen on this build");
				ItemTooltip(ui, "The game's camera manager is overriding the FOV and the "
				                "plugin could not hook it -- see the log for what the probe "
				                "found.\n\n"
				                "Keyframes still store the FOV, so nothing is lost, but the "
				                "view will not change.");
			}

			ui->Spacing();

			// --- Aim ----------------------------------------------------------
			ui->SeparatorText("Aim");

			bool lookAt = key.lookAt;
			if (ui->Checkbox("Look at a point", &lookAt))
			{
				key.lookAt  = lookAt;
				state.dirty = true;
				poseEdited  = true;
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
					poseEdited       = true;
				}
				LabelRow(ui, "Target  X / Y / Z");

				if (ui->Button(CC_ICON_FOCUS "  Aim from camera"))
					Post(state, Request::LookAtSelectionFromCamera);
				HelpMarker(ui, "Puts the target 10 m in front of wherever the fly camera "
				               "is currently pointing.");
			}

			ui->Spacing();

			// --- Focus ----------------------------------------------------------
			ui->SeparatorText("Focus");

			// The switch is the *project's*, shown here rather than in the project
			// panel on purpose: this is where someone is looking when they want
			// depth of field, and splitting the on/off from the two numbers it
			// governs across two panels would be worse than the small impurity.
			bool dof = state.timeline.depthOfField;
			if (ui->Checkbox("Depth of field", &dof))
			{
				state.timeline.depthOfField = dof;
				state.dirty                 = true;
				poseEdited                  = true;
			}
			HelpMarker(ui, "Overrides the game's depth of field for the whole shot, so the "
			               "focus distance and aperture below do something.\n\n"
			               "One switch for every keyframe, because focus fading in and out of "
			               "existence partway through a move would just pop. The distance and "
			               "aperture themselves are per-keyframe and animate, which is what a "
			               "focus pull is.");

			if (state.timeline.depthOfField)
			{
				// Metres in the UI, centimetres in the model. A drag rather than a
				// slider because the useful range spans three orders of magnitude --
				// a linear slider from 0.1 m to 500 m gives about two pixels to
				// everything closer than arm's length.
				float focusMetres = key.focusDistance / 100.0f;
				FullWidthItem(ui);
				if (ui->DragFloat("##focus", &focusMetres, 0.1f, 0.05f, 10000.0f, "Focus  %.2f m"))
				{
					key.focusDistance = std::max(focusMetres, 0.05f) * 100.0f;
					state.dirty       = true;
					poseEdited        = true;
				}

				// Lower f-stop = shallower depth of field. A slider here because the
				// range is small, bounded and evenly useful across all of it.
				FullWidthItem(ui);
				if (ui->SliderFloat("##aperture", &key.aperture, 0.5f, 32.0f, "Aperture  f/%.1f"))
				{
					state.dirty = true;
					poseEdited  = true;
				}
				HelpMarker(ui, "How shallow the focus is. f/1.4 throws everything but the focus "
				               "distance out; f/22 is sharp almost throughout.\n\n"
				               "Sensor width is left at the game's own value, so this number "
				               "means the same thing here as it does everywhere else in the "
				               "game's look.");

				if (key.lookAt)
				{
					const double toTarget = (key.lookAtTarget - key.location).Length();

					char focusOnTarget[64];
					snprintf(focusOnTarget, sizeof(focusOnTarget),
					         CC_ICON_FOCUS "  Focus on the target (%.2f m)", toTarget / 100.0);

					if (ui->Button(focusOnTarget))
					{
						key.focusDistance = static_cast<float>(std::max(toTarget, 5.0));
						state.dirty       = true;
						poseEdited        = true;
					}
					ItemTooltip(ui, "Sets the focus distance to exactly where this keyframe is "
					                "aiming, so the subject is sharp.");
				}
			}

			// Feed the edit back to the live camera, so the viewport tracks the
			// drag instead of only showing it once you fly here again. One request
			// per frame while a slider is held down, which is exactly the point.
			//
			// Placed after the Aim section so a look-at target counts as a pose
			// edit too -- moving the target re-aims the camera just as surely as
			// dragging the rotation does.
			if (poseEdited && cameraOnKey)
				Post(state, Request::GotoSelected);

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
			               "dead stop as it passes through.\n\n"
			               "That is worth doing deliberately to hold on a subject, and jarring "
			               "by accident.");

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

			// Icon plus a word for these, not icon alone. They are half-panel-wide
			// buttons in a dense inspector, and "fly to it" versus "overwrite it
			// from the camera" is the pair most expensive to get wrong -- one of
			// them destroys the pose you spent time on.
			if (AccentButton(ui, CC_ICON_TARGET "  Fly here", halfWidth, rowHeight))
				Post(state, Request::GotoSelected);
			ItemTooltip(ui, "Moves the free camera to this keyframe's exact pose, so you can "
			                "carry on composing from it.\n\n"
			                "Same as double-clicking the keyframe.");

			ui->SameLine(0.0f, 6.0f);

			if (AccentButton(ui, CC_ICON_CAMERA "  Re-record", halfWidth, rowHeight))
				Post(state, Request::UpdateSelectedFromCamera);
			ItemTooltip(ui, "Overwrites this keyframe's position, rotation and FOV with wherever "
			                "the free camera is now.\n\n"
			                "The old pose is not recoverable, so fly somewhere you like first.");

			if (ui->ButtonSized(CC_ICON_EARLIER "  Earlier", halfWidth, rowHeight))
			{
				if (state.timeline.MoveEarlier(key.id))
					state.dirty = true;
			}
			ItemTooltip(ui, "Swap this keyframe with the one before it, keeping the timing.");

			ui->SameLine(0.0f, 6.0f);
			if (ui->ButtonSized(CC_ICON_LATER "  Later", halfWidth, rowHeight))
			{
				if (state.timeline.MoveLater(key.id))
					state.dirty = true;
			}
			ItemTooltip(ui, "Swap this keyframe with the one after it, keeping the timing.");

			ui->PushStyleColor(Col_Button,        kDanger.r * 0.6f, kDanger.g * 0.6f, kDanger.b * 0.6f, 1.0f);
			ui->PushStyleColor(Col_ButtonHovered, kDanger.r, kDanger.g, kDanger.b, 1.0f);
			ui->PushStyleColor(Col_ButtonActive,  kDanger.r, kDanger.g, kDanger.b, 1.0f);
			if (ui->ButtonSized(CC_ICON_DELETE "  Delete keyframe", availX, rowHeight))
			{
				const uint32_t doomed = key.id;
				ClearSelection(state);
				state.timeline.Remove(doomed);
				state.dirty = true;
				SetStatus(state, now, "Keyframe deleted");
			}
			ui->PopStyleColor(3);
		}

		// -------------------------------------------------------------------
		// Multi-selection
		//
		// Deliberately thin. Every other panel here edits one thing, and the
		// controls are worded for one thing -- "this keyframe's" rotation, "the
		// running rupture". Offering a subset of them across a mixed bag of
		// keyframes and cues would mean inventing an answer for every field where
		// the members disagree, which is a lot of design for an editor whose real
		// multi-item operations are *drag them* and *delete them*.
		//
		// So: say what is selected, and offer the one action that means the same
		// thing for all of them.
		// -------------------------------------------------------------------
		void RenderMultiSelection(IModLoaderImGui* ui, State& state, double now)
		{
			int keyCount = 0;
			int cueCount = 0;
			for (uint32_t id : state.multiSelection)
			{
				if (state.timeline.Find(id))          ++keyCount;
				else if (state.timeline.FindFunc(id)) ++cueCount;
			}

			char header[96];
			snprintf(header, sizeof(header), "%d selected",
			         static_cast<int>(state.multiSelection.size()));
			ui->SeparatorText(header);

			char line[128];
			if (keyCount > 0 && cueCount > 0)
				snprintf(line, sizeof(line), "%d keyframe%s and %d cue%s",
				         keyCount, keyCount == 1 ? "" : "s",
				         cueCount, cueCount == 1 ? "" : "s");
			else if (keyCount > 0)
				snprintf(line, sizeof(line), "%d keyframe%s", keyCount, keyCount == 1 ? "" : "s");
			else
				snprintf(line, sizeof(line), "%d cue%s", cueCount, cueCount == 1 ? "" : "s");
			ui->TextDisabled(line);

			ui->Spacing();
			ui->TextWrapped("Drag any one of them on the track to move the whole group, keeping "
			                "the spacing between them.");
			ui->Spacing();
			ui->TextDisabled("Ctrl+click to add or remove one.");
			ui->TextDisabled("Click anything on its own to go back to editing it.");

			ui->Spacing();
			ui->Separator();
			ui->Spacing();

			float availX = 0.0f, availY = 0.0f;
			ui->GetContentRegionAvail(&availX, &availY);
			const float rowHeight = ui->GetFrameHeight() * 1.15f;

			ui->PushStyleColor(Col_Button,        kDanger.r * 0.6f, kDanger.g * 0.6f, kDanger.b * 0.6f, 1.0f);
			ui->PushStyleColor(Col_ButtonHovered, kDanger.r, kDanger.g, kDanger.b, 1.0f);
			ui->PushStyleColor(Col_ButtonActive,  kDanger.r, kDanger.g, kDanger.b, 1.0f);

			snprintf(line, sizeof(line), CC_ICON_DELETE "  Delete all %d",
			         static_cast<int>(state.multiSelection.size()));

			if (ui->ButtonSized(line, availX, rowHeight))
			{
				// Copy the ids out first: ClearSelection empties the very vector
				// this would otherwise be iterating.
				const std::vector<uint32_t> doomed = state.multiSelection;
				ClearSelection(state);

				for (uint32_t id : doomed)
				{
					if (!state.timeline.Remove(id))
						state.timeline.RemoveFunc(id);
				}

				state.dirty = true;
				snprintf(line, sizeof(line), "Deleted %d items", static_cast<int>(doomed.size()));
				SetStatus(state, now, line);
			}
			ui->PopStyleColor(3);
		}

		// -------------------------------------------------------------------
		// Func frame
		// -------------------------------------------------------------------
		void RenderFuncFrame(IModLoaderImGui* ui, State& state, FuncFrame& frame, double now)
		{
			ui->SeparatorText(CC_ICON_BOLT "  Cue");

			if (g_funcNameBufOwner != frame.id)
			{
				g_funcNameBufOwner = frame.id;
				strncpy_s(g_funcNameBuf, sizeof(g_funcNameBuf), frame.name.c_str(), _TRUNCATE);
			}

			FullWidthItem(ui);
			if (ui->InputTextWithHint("##funcname", "Cue name (optional)",
			                          g_funcNameBuf, sizeof(g_funcNameBuf)))
			{
				frame.name  = g_funcNameBuf;
				state.dirty = true;
			}
			HelpMarker(ui, "Drawn beside the marker on the track, in place of the action's own "
			               "short label.");

			bool enabled = frame.enabled;
			if (ui->Checkbox("Enabled", &enabled))
			{
				frame.enabled = enabled;
				state.dirty   = true;
			}
			HelpMarker(ui, "A disabled cue stays on the timeline and never fires.");

			// Seconds, absolute. Unlike a keyframe this is stored rather than
			// derived, which is why it is an ordinary number field here and a
			// retime operation over on the track.
			float seconds = static_cast<float>(frame.time);
			FullWidthItem(ui);
			if (ui->DragFloat("##functime", &seconds, 0.05f, 0.0f, 100000.0f, "Fires at  %.2f s"))
			{
				frame.time  = std::max(static_cast<double>(seconds), 0.0);
				state.dirty = true;
			}
			HelpMarker(ui, "An absolute position in the take, not a position relative to any "
			               "keyframe -- retiming the camera move leaves cues where they are.");

			if (frame.time > state.timeline.TotalDuration())
				ui->TextDisabled("After the end of the shot: this will never fire.");

			ui->Spacing();

			// --- Action -------------------------------------------------------
			ui->SeparatorText("Action");

			FullWidthItem(ui);
			if (ui->BeginCombo("##funcaction", FuncActionName(frame.action)))
			{
				for (int i = 0; i < static_cast<int>(FuncAction::Count); ++i)
				{
					const FuncAction option = static_cast<FuncAction>(i);
					if (ui->Selectable(FuncActionName(option), option == frame.action))
					{
						frame.action = option;
						state.dirty  = true;
					}
				}
				ui->EndCombo();
			}

			ui->TextWrapped(FuncActionSummary(frame.action));

			// What scrubbing back over it does, per action rather than as one
			// general note. Whether a cue can be taken back is the thing you want
			// to know while choosing it, and for three of the six the answer is
			// "nothing" -- which is worth saying out loud here instead of being
			// discovered by dragging.
			{
				const FuncAction undo = ReverseOf(frame.action);

				char line[160];
				if (undo == FuncAction::None)
				{
					ui->TextDisabled("Dragging back over this does nothing -- no inverse.");
					ItemTooltip(ui, "Undoing it would need the value it replaced, or the rupture it "
					                "ended put back, and a cue carries neither.\n\n"
					                "An approximate undo that quietly left the world somewhere it "
					                "had never been would be harder to spot than no undo at all.");
				}
				else
				{
					snprintf(line, sizeof(line), "Dragging back over this runs %s.",
					         FuncActionName(undo));
					ui->TextDisabled(line);
					ItemTooltip(ui, "Scrubbing the playhead back past this cue runs its inverse, so "
					                "dragging out over it and back again leaves the world where it "
					                "started.");
				}
			}

			ui->Spacing();

			// Only the parameters this action actually reads. A control that is
			// on screen but ignored is worse than one that is missing -- it looks
			// like the cue is configured when it is not.
			const bool usesType = frame.action == FuncAction::StartRupture ||
			                      frame.action == FuncAction::SetRupturePhase;

			if (usesType)
			{
				LabelRow(ui, "Kind");
				FullWidthItem(ui);
				if (ui->BeginCombo("##wavetype", RuptureTypeName(frame.waveType)))
				{
					for (int t = 1; t <= 2; ++t)
					{
						if (ui->Selectable(RuptureTypeName(t), t == frame.waveType))
						{
							frame.waveType = t;
							state.dirty    = true;
						}
					}
					ui->EndCombo();
				}
			}

			if (frame.action == FuncAction::SetRupturePhase)
			{
				LabelRow(ui, "Phase");
				FullWidthItem(ui);
				if (ui->BeginCombo("##wavestage", RuptureStageName(frame.stage)))
				{
					for (int s = 1; s <= 4; ++s)
					{
						if (ui->Selectable(RuptureStageName(s), s == frame.stage))
						{
							frame.stage = s;
							state.dirty = true;
						}
					}
					ui->EndCombo();
				}
				HelpMarker(ui, "Restarts the running rupture at this stage, keeping the timings "
				               "it already has.\n\n"
				               "There has to be one running: the stage durations come from it, "
				               "and a made-up set would give you a rupture that starts and then "
				               "never moves.");
			}

			if (frame.action == FuncAction::SetRuptureProgress)
			{
				bool ramp = frame.ramp;
				if (ui->Checkbox("Ramp over time", &ramp))
				{
					frame.ramp  = ramp;
					state.dirty = true;
				}
				HelpMarker(ui, "Off: the rupture jumps to one progress value the moment the "
				               "playhead reaches this cue.\n\n"
				               "On: it is driven smoothly from the start value to the end value "
				               "over the duration below, so the rupture crosses the map at a pace "
				               "you choose rather than the world's.\n\n"
				               "A ramp is the one cue that is not an instant. It follows the "
				               "playhead rather than firing as it passes, so scrubbing into the "
				               "middle of one puts the rupture exactly where that moment says it "
				               "should be.");

				float start = Clamp(frame.progress, 0.0f, 1.0f) * 100.0f;
				FullWidthItem(ui);
				if (ui->SliderFloat("##waveprogress", &start, 0.0f, 100.0f,
				                    frame.ramp ? "Start  %.0f%%" : "Progress  %.0f%%"))
				{
					frame.progress = Clamp(start / 100.0f, 0.0f, 1.0f);
					state.dirty    = true;
				}

				if (frame.ramp)
				{
					float end = Clamp(frame.progressEnd, 0.0f, 1.0f) * 100.0f;
					FullWidthItem(ui);
					if (ui->SliderFloat("##waveprogressend", &end, 0.0f, 100.0f, "End  %.0f%%"))
					{
						frame.progressEnd = Clamp(end / 100.0f, 0.0f, 1.0f);
						state.dirty       = true;
					}
					HelpMarker(ui, "End below start runs the rupture backwards across its stage, "
					               "which the game is perfectly willing to do.");

					float seconds = static_cast<float>(frame.rampDuration);
					FullWidthItem(ui);
					if (ui->DragFloat("##waverampdur", &seconds, 0.05f, 0.05f, 600.0f,
					                  "Over  %.2f s"))
					{
						frame.rampDuration = std::max(static_cast<double>(seconds), 0.05);
						state.dirty        = true;
					}
					HelpMarker(ui, "How long the ramp takes. It is drawn on the track as a bar "
					               "running right from the cue's marker, so you can line it up "
					               "against the camera move it is meant to happen under.");

					char span[96];
					snprintf(span, sizeof(span), "Runs %.2f s  ->  %.2f s",
					         frame.time, frame.time + frame.rampDuration);
					ui->TextDisabled(span);
				}
				else
				{
					HelpMarker(ui, "How far through its current stage the rupture is jumped to, so "
					               "a shot does not have to wait for it to arrive.");
				}
			}

			ui->Spacing();
			ui->Separator();
			ui->Spacing();

			// --- Actions --------------------------------------------------------
			float availX = 0.0f, availY = 0.0f;
			ui->GetContentRegionAvail(&availX, &availY);
			const float rowHeight = ui->GetFrameHeight() * 1.15f;

			if (AccentButton(ui, CC_ICON_BOLT "  Trigger now", availX, rowHeight))
				Post(state, Request::FireSelectedFunc);
			ItemTooltip(ui, "Runs this cue against the world immediately, so it can be tried "
			                "without recording a take.\n\n"
			                "It changes the actual save you are playing -- there is no undo for a "
			                "rupture.");

			ui->TextDisabled("Cues fire whenever the playhead moves forward over them.");
			HelpMarker(ui, "During a take, during preview play, and while you scrub -- so the "
			               "world keeps up with wherever you have dragged the playhead to.\n\n"
			               "Scrubbing backwards runs each cue's inverse where it has one, so "
			               "dragging out over a cue and back again leaves the world where it "
			               "started.\n\n"
			               "Untick Enabled above to work on the camera timing without it, or use "
			               "Trigger now to fire this one on its own.\n\n"
			               "They also need you to be the one running the world -- single player or "
			               "hosting. As a connected client the server decides, and the cue may "
			               "report success while nothing happens.");

			ui->Spacing();

			ui->PushStyleColor(Col_Button,        kDanger.r * 0.6f, kDanger.g * 0.6f, kDanger.b * 0.6f, 1.0f);
			ui->PushStyleColor(Col_ButtonHovered, kDanger.r, kDanger.g, kDanger.b, 1.0f);
			ui->PushStyleColor(Col_ButtonActive,  kDanger.r, kDanger.g, kDanger.b, 1.0f);
			if (ui->ButtonSized(CC_ICON_DELETE "  Delete cue", availX, rowHeight))
			{
				const uint32_t doomed = frame.id;
				ClearSelection(state);
				state.timeline.RemoveFunc(doomed);
				state.dirty = true;
				SetStatus(state, now, "Cue deleted");
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

			if (AccentButton(ui, CC_ICON_PLAYLIST_ADD "  Insert here from camera",
			                 availX, rowHeight))
			{
				SelectOnly(state, key.id);
				Post(state, Request::CaptureInsertAfterSelected);
			}
			ItemTooltip(ui, "Splits this segment in two at the current camera pose, without "
			                "changing when any later keyframe happens.");

			if (ui->ButtonSized(CC_ICON_TARGET "  Select the keyframe that starts it",
			                    availX, rowHeight))
			{
				SelectOnly(state, key.id);
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
			if (timeline.FuncCount() > 0)
			{
				snprintf(line, sizeof(line), "%d keyframes   %d cues   %.2f s total",
				         timeline.Count(), timeline.FuncCount(), timeline.TotalDuration());
			}
			else
			{
				snprintf(line, sizeof(line), "%d keyframes   %.2f s total",
				         timeline.Count(), timeline.TotalDuration());
			}
			ui->TextDisabled(line);

			if (state.dirty)
				TextColored(ui, kKeyframe, "Unsaved changes");

			ui->Spacing();

			float availX = 0.0f, availY = 0.0f;
			ui->GetContentRegionAvail(&availX, &availY);
			const float rowHeight = ui->GetFrameHeight() * 1.15f;
			const float halfWidth = (availX - 6.0f) * 0.5f;

			if (AccentButton(ui, CC_ICON_SAVE "  Save", halfWidth, rowHeight))
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

			if (ui->ButtonSized(CC_ICON_NEW_FILE "  New", halfWidth, rowHeight))
			{
				timeline.Clear();
				timeline.name       = "Untitled";
				ClearSelection(state);
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

			if (ui->ButtonSized(CC_ICON_FOLDER_OPEN "  Load", halfWidth, rowHeight))
			{
				std::string error;
				Timeline loaded;
				if (ProjectIO::Load(g_projectList[g_selectedProject], loaded, error))
				{
					timeline            = std::move(loaded);
					ClearSelection(state);
					state.playhead      = 0.0;
					state.playing       = false;
					state.dirty         = false;
					g_projectNameSynced = false;
					g_keyNameBufOwner   = 0;
					g_funcNameBufOwner  = 0;
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

			if (ui->ButtonSized(CC_ICON_DELETE "  Delete file", halfWidth, rowHeight))
				ui->OpenPopup("##confirmdelete", 0);

			ui->EndDisabled();

			if (ui->BeginPopup("##confirmdelete", 0))
			{
				ui->Text("Delete this project file permanently?");
				if (haveSelection)
					ui->TextDisabled(g_projectList[g_selectedProject].c_str());

				if (ui->Button(CC_ICON_DELETE "  Delete"))
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
				if (ui->Button(CC_ICON_CANCEL "  Cancel"))
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

			// Migration aid. New keyframes default to Auto, which flows through
			// interior keys on its own -- but a project saved before Auto existed
			// has an explicit curve on every key, and explicit curves mean a stop
			// at every one of them. This is the one-click fix.
			{
				int explicitEases = 0;
				for (const Keyframe& k : timeline.Keys())
				{
					if (k.easeIn != Ease::Auto)  ++explicitEases;
					if (k.easeOut != Ease::Auto) ++explicitEases;
				}

				ui->BeginDisabled(explicitEases == 0);
				if (ui->Button("Flow through every keyframe"))
				{
					for (Keyframe& k : timeline.Keys())
					{
						k.easeIn  = Ease::Auto;
						k.easeOut = Ease::Auto;
					}
					state.dirty = true;
					SetStatus(state, now, "All keyframes set to Auto easing");
				}
				ui->EndDisabled();

				HelpMarker(ui, "Sets every keyframe's ease to Auto, so the camera eases away "
				               "from the first and settles into the last, and carries its speed "
				               "straight through everything in between.\n\n"
				               "New keyframes are already Auto. This is for projects saved before "
				               "Auto existed, which have an explicit curve on every keyframe.\n\n"
				               "An explicit curve on both sides of a middle keyframe stops the "
				               "camera dead as it passes through.");

				if (explicitEases > 0)
				{
					char note[96];
					snprintf(note, sizeof(note), "%d ease setting%s not on Auto",
					         explicitEases, explicitEases == 1 ? "" : "s");
					ui->TextDisabled(note);
				}
			}

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
			ui->Checkbox("Mask the game view beside the panels", &state.options.fitViewport);
			ui->EndDisabled();
			HelpMarker(ui, "Frames the game's 3D view into the space the timeline and this "
			               "inspector are not covering, the way an editing program frames its "
			               "viewer -- so nothing you are composing is hidden behind a panel.\n\n"
			               "It CROPS rather than scales.\n\n"
			               "The engine keeps rendering the full "
			               "window underneath: writing the player's viewport rectangle moves "
			               "where the engine projects, not where it draws.\n\n"
			               "So playback shows "
			               "more than this preview does, on all four sides.\n\n"
			               "Turn it off when the exact framing matters. The keyframe handles "
			               "line up correctly either way.");

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
			ui->SliderFloat("##flyspeed", &state.options.flySpeed, kFlySpeedMin, kFlySpeedMax,
			                "Fly speed  %.0f u/s");
			HelpMarker(ui, "How fast the free camera flies at 1x, before the boost and crawl "
			               "modifiers.\n\n"
			               "Quicker to reach with the scroll wheel while holding right mouse to "
			               "look around -- this slider and the wheel are the same setting, and it "
			               "is saved when you leave the editor.");

			FullWidthItem(ui);
			ui->SliderFloat("##sensitivity", &state.options.mouseSensitivity, 0.02f, 1.5f,
			                "Mouse sensitivity  %.2f");

			ui->Checkbox("Projection readout", &state.options.projectionDebug);
			HelpMarker(ui, "Diagnostic. Marks the centre of the picture and prints the rectangle "
			               "the handles are being clipped to, the window size, whether the "
			               "squeeze actually took, and how many keyframes the engine "
			               "managed to project.\n\n"
			               "The projection itself comes from the engine, so it cannot disagree "
			               "with the in-world gizmos. What this checks is the frame around it.\n\n"
			               "If "
			               "'squeeze' reads off while the picture is clearly inset -- or on while "
			               "it is not -- then handles are being clipped against the wrong "
			               "rectangle and will vanish near the edges.");

			ui->Spacing();

			// --- Safety -----------------------------------------------------------
			ui->SeparatorText("Player safety");

			ui->Checkbox("Bury the player while filming", &state.options.protectPlayer);
			HelpMarker(ui, "While the camera is detached your body is left standing in the open, "
			               "still fully simulated.\n\n"
			               "This freezes it, drops it far below the terrain "
			               "and leaves it there for the session -- nothing can see it, walk into "
			               "it or contain it down there.\n\n"
			               "It does not follow the camera. Towing it along was what used to walk "
			               "it through every radiation field and kill volume the shot flew "
			               "through; the world streams from the view instead.\n\n"
			               "Everything is put back exactly as it was on exit.");

			ui->BeginDisabled(!state.options.protectPlayer);

			FullWidthItem(ui);
			ui->DragFloat("##stashz", &state.options.stashAltitude, 25.0f, -200000.0f, 200000.0f,
			              "Stash height  %.0f");
			HelpMarker(ui, "The world height (Z) the body is dropped to, keeping the X and Y it "
			               "was standing on.\n\n"
			               "An absolute altitude, not a distance down: the same "
			               "drop measured from a cliff top and from a canyon floor land nowhere "
			               "near each other, and one number should mean one place.\n\n"
			               "Lower it if your map has anything at this depth.");

			ui->Checkbox("Spawn a habitat shelter around them", &state.options.spawnHabitat);
			HelpMarker(ui, "Gives the body a floor to stand on and four walls between it and the "
			               "world.\n\n"
			               "It is a full building actor, so not every game build takes "
			               "kindly to one appearing out of nowhere -- turn it off if anything "
			               "misbehaves.");

			ui->Checkbox("Show a marker where the body is", &state.options.showPlayerMarker);
			HelpMarker(ui, "Draws a person-sized box in the world at the stash point, so you can "
			               "see where it ended up.");

			ui->EndDisabled();

			if (state.options.protectPlayer)
				ui->TextDisabled("The stash is applied when you enter the editor.");

			ui->Spacing();

			ui->Checkbox("Make the player unkillable", &state.options.preventDeath);
			HelpMarker(ui, "Switches off the two things that can kill the body outright rather "
			               "than by draining a meter: the world's out-of-bounds kill (KillZ and "
			               "the world bounds checks -- the \"went too low\" death).\n\n"
			               "And the pawn's "
			               "own damage flag, which is what pain volumes go through.\n\n"
			               "Both are read before they are written and restored exactly as they "
			               "were when you leave.\n\n"
			               "Locking the meters below cannot do this job on its own: an instant "
			               "kill happens inside one frame, and writing health back to full on the "
			               "next tick does not undo it.");

			ui->BeginDisabled(!state.options.preventDeath);
			ui->Checkbox("Use the game's own Immortal cheat", &state.options.gameImmortality);
			HelpMarker(ui, "Belt and braces, for damage the engine flags above cannot reach -- "
			               "anything the game applies through its own ability system.\n\n"
			               "This calls the game's `Immortal` cheat, which means asking the player "
			               "controller to create the game's cheat manager.\n\n"
			               "It stays instantiated "
			               "for the rest of the session, so leave this off unless you are still "
			               "dying with everything else on.");
			ui->EndDisabled();

			ui->Spacing();

			ui->Checkbox("Lock health, food and water", &state.options.lockVitals);
			HelpMarker(ui, "Holds health, food, water, oxygen, energy and shield at full, and "
			               "the hazard meters (toxicity, radiation, heat, drain, corrosion, "
			               "infection) at zero, for as long as the editor is open.\n\n"
			               "Survival "
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
		g_funcNameBufOwner  = 0;
		g_projectNameSynced = false;
		g_projectListStale  = true;
		g_selectedProject   = -1;
		g_ioMessage.clear();
	}

	namespace
	{
		// A clickable trail back out of a selection.
		//
		// The inspector is driven entirely by what the timeline has selected, so
		// without this the only way back to the project settings is to find an
		// empty stretch of track and click it -- which on a full timeline may
		// not exist at all.
		void RenderBreadcrumb(IModLoaderImGui* ui, State& state)
		{
			const int index = state.timeline.IndexOf(state.selectedId);

			// Root crumb: always live, and always takes you home.
			if (ui->Button("Project"))
				ClearSelection(state);
			ItemTooltip(ui, "Back to the project: name, length, save and load");

			if (!state.multiSelection.empty())
			{
				ui->SameLine(0.0f, 6.0f);
				ui->TextDisabled(">");
				ui->SameLine(0.0f, 6.0f);

				char crumb[64];
				snprintf(crumb, sizeof(crumb), "%d selected",
				         static_cast<int>(state.multiSelection.size()));
				ui->TextDisabled(crumb);

				ui->Separator();
				return;
			}

			// A func frame is not in the keyframe list, so it gets its own crumb
			// rather than falling through the index lookup below.
			if (state.selection == Selection::Function)
			{
				if (const FuncFrame* frame = state.timeline.FindFunc(state.selectedId))
				{
					ui->SameLine(0.0f, 6.0f);
					ui->TextDisabled(">");
					ui->SameLine(0.0f, 6.0f);

					char crumb[96];
					snprintf(crumb, sizeof(crumb), "Cue  %s", FuncActionName(frame->action));
					ui->TextDisabled(crumb);
				}

				ui->Separator();
				return;
			}

			if (state.selection == Selection::None || index < 0)
			{
				ui->Separator();
				return;
			}

			const Keyframe& key = state.timeline.Keys()[index];

			char crumb[96];

			// Middle crumb for a segment selection, so "clip 2 -> its keyframe"
			// is one click each way rather than a hunt on the track.
			if (state.selection == Selection::Segment)
			{
				ui->SameLine(0.0f, 6.0f);
				ui->TextDisabled(">");
				ui->SameLine(0.0f, 6.0f);

				snprintf(crumb, sizeof(crumb), "Clip %d-%d", index + 1, index + 2);
				ui->TextDisabled(crumb);

				ui->SameLine(0.0f, 6.0f);
				ui->TextDisabled(">");
				ui->SameLine(0.0f, 6.0f);

				snprintf(crumb, sizeof(crumb), "Keyframe %d", index + 1);
				if (ui->Button(crumb))
					state.selection = Selection::Keyframe;
				ItemTooltip(ui, "Inspect the keyframe this clip leaves from");
			}
			else if (state.selection == Selection::Keyframe)
			{
				ui->SameLine(0.0f, 6.0f);
				ui->TextDisabled(">");
				ui->SameLine(0.0f, 6.0f);

				// The clip is only a thing if there is something after this key
				// for it to run to.
				if (index + 1 < state.timeline.Count())
				{
					snprintf(crumb, sizeof(crumb), "Clip %d-%d", index + 1, index + 2);
					if (ui->Button(crumb))
						state.selection = Selection::Segment;
					ItemTooltip(ui, "Inspect the timing of the clip leaving this keyframe");

					ui->SameLine(0.0f, 6.0f);
					ui->TextDisabled(">");
					ui->SameLine(0.0f, 6.0f);
				}

				snprintf(crumb, sizeof(crumb), "Keyframe %d%s%s", index + 1,
				         key.name.empty() ? "" : "  ", key.name.c_str());
				ui->TextDisabled(crumb);
			}

			ui->Separator();
		}
	}

	void Render(IModLoaderImGui* ui, State& state, double now)
	{
		RenderBreadcrumb(ui, state);

		// Before the per-kind panels: with several things selected there is no
		// single "the keyframe" for them to edit.
		if (!state.multiSelection.empty())
		{
			RenderMultiSelection(ui, state, now);
			return;
		}

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

			case Selection::Function:
			{
				if (FuncFrame* frame = state.timeline.FindFunc(state.selectedId))
				{
					RenderFuncFrame(ui, state, *frame, now);
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
