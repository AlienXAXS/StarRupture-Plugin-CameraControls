# Camera Controls

A keyframe camera timeline for [StarRupture](https://store.steampowered.com/app/1750870/StarRupture/), built as a
[StarRupture ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader) plugin.

Fly a free camera around your base, drop keyframes wherever you want the shot to pass through, then scrub, tune and
play the whole move back — the way you would cut a camera move in DaVinci Resolve, except the timeline drives the
game's actual camera.

---

## What it does

- **Free-fly camera.** Detaches the view from your character and gives you a proper fly cam: WASD + QE, mouse look,
  boost and crawl modifiers, roll, and live FOV. Scroll the wheel while holding right mouse to change how fast it flies,
  without letting go of the camera.
- **Keyframes at the press of a key.** Position, rotation and FOV are recorded from wherever the camera is standing.
- **A real timeline.** Clips between keyframes, a draggable playhead, ripple and trim editing, zoom and pan.
- **Per-keyframe control.** Speed, smoothness, ease in/out, FOV, focus, fade in/out, and an optional look-at target.
- **Depth of field, animated.** Turn it on for a shot and every keyframe carries its own focus distance and aperture, so
  a focus pull is just two keyframes with different distances. One click focuses a keyframe on whatever it is aiming at.
  Off by default — the game keeps its own look until you ask for yours.
- **Cues that change the world.** Alongside keyframes, the timeline carries *func frames* — markers that hang from the
  top of the track and fire when the playhead reaches them during a take. Start a rupture on cue, set its phase, jump it
  forward, pause it, cancel it. Right-click the track to place one.
- **Scrubbing moves the camera.** Drag the playhead and the camera flies along the path with you.
- **In-world gizmos.** The path is drawn as a spline in the 3D world, with a camera frustum at every keyframe. The
  selected keyframe pulses so you can pick it out of a dense path.
- **Double-click to resume.** Double-clicking a keyframe flies the camera to that exact pose, so you can carry on
  composing from a shot you already set up.
- **Ctrl+click to select several.** Keyframes and cues together, in any mix. Drag any one of them and the whole group
  moves, keeping its spacing; `Delete` removes the lot.
- **Playback mode.** A countdown, then the timeline runs start to finish with the game HUD and the editor out of the
  way, for a clean capture.
- **Projects saved to disk.** Plain, hand-editable JSON under `Plugins\CameraControls\Projects\`.
- **Click keyframes in the 3D view.** Every keyframe gets an on-screen handle you can click and double-click, not just
  a row on the timeline.
- **The game view moves out from under the panels.** Rather than covering your shot with the timeline and the
  inspector, the editor frames the game's own 3D view into the space they leave free, like the viewer in an editing
  program. Note that it **crops** rather than scales — see the caveat under Layout — so playback shows more than the
  preview. Turn it off when the exact framing matters.
- **Your body is looked after.** While the camera is detached your character is frozen, buried far below the terrain in
  a habitat shelter, and made unkillable — the world's out-of-bounds kill and the pawn's damage flag are switched off,
  so a radiation field or a lethal drop cannot end the take on the respawn screen. Everything is put back exactly where
  it was.

---

## Getting started

1. Load a save.
2. Press **F7** to open the editor.
3. Fly to where the shot should start and press **K** to drop the first keyframe.
4. Fly somewhere else, press **K** again. Repeat.
5. Press **Space** to preview, or **F8** to run it properly.
6. Press **F7** again to put everything back.

The keybind cheat-sheet stays pinned to the top-left corner the whole time you are in the editor, showing your actual
bindings rather than the defaults. **Right-click the timeline track** for a menu that adds a keyframe or a world cue
wherever you clicked.

---

## Default keybinds

All of these are rebindable from the ModLoader's plugin config UI (or `CameraControls.ini`).

| Key | Action |
|---|---|
| `F7` | Enter / leave the editor |
| `F8` | Start / stop playback mode |
| `F9` | Hide / show the editor windows (the camera still flies) |
| `Esc` | Stop a take and go back to the editor |
| `W` `A` `S` `D` | Fly forward / left / back / right |
| `E` / `Q` | Fly up / down |
| `Left Shift` / `Left Ctrl` | Fly faster / slower (hold) |
| Right mouse (hold) | Look around |
| Mouse wheel (while looking) | Fly faster / slower — the same setting as the Fly speed slider |
| `Z` / `C` | Roll left / right |
| `X` | Level the camera — zero all rotation without moving it |
| `R` / `F` | Zoom in / out (FOV) |
| `K` | Add a keyframe at the camera |
| `I` | Insert a keyframe after the selected one |
| `U` | Re-record the selected keyframe from the camera |
| `G` | Fly the camera to the selected keyframe |
| `Delete` | Delete the selected keyframe |
| `,` / `.` | Select the previous / next keyframe |
| `Space` | Preview play / pause |
| `Home` | Write a diagnostic snapshot of the player's control state to the log (works any time, see below) |

**On the timeline:** click a keyframe to select it, drag it to retime, double-click it to fly there, click a clip to
select the segment, wheel to zoom about the cursor, middle-drag to pan. The `-60 … +60` nudge buttons move the
selected keyframe by whole frames (at the project's frame rate — set that to 1 to nudge in seconds).

**In the 3D view:** every keyframe gets a clickable on-screen handle. Click to select, double-click to fly there. The
plugin projects each keyframe to screen space using the camera pose it is already driving, so this needs no picking
API and no render hooks.

---

## The timeline model

Keyframes do not store an absolute time. Each one stores the **base duration** of the segment leaving it plus a
**speed** multiplier, and the timeline derives absolute positions from those:

```
effective duration = base duration / speed
keyframe time      = sum of every effective duration before it
```

This is how clip speed works in a video editor. Dropping a segment to `0.5x` makes it take twice as long and pushes
everything after it later; there is never a stored time that can disagree with the stored speed.

**Smoothness** blends between a straight line and a Catmull-Rom curve through the keyframe. `0` gives a hard corner,
`1` a full curve; the two ends of a segment are averaged, so one sharp keyframe is enough to break a curve. The spline
uses non-uniform tangents, so a long segment next to a short one does not overshoot.

**Ease** shapes the velocity within a segment: the leaving keyframe's *ease out* governs the first half, the arriving
keyframe's *ease in* the second.

**Ctrl+click** adds a keyframe or a cue to the selection, and removes it again if it is already in. Dragging any member
moves the whole group and keeps its spacing — a keyframe that runs into an unselected neighbour stops there and rejoins
the group as soon as there is room, rather than falling behind for good. `Delete` removes everything selected. The
inspector shows only a count and a delete button while several things are selected: editing "the keyframe's rotation"
means nothing when there are five of them, so click one on its own to go back to editing it.

**Fades** dip to (or out of) a colour around a keyframe's time. Fade in on the first keyframe and fade out on the last
is the usual pairing.

---

## Func frames — cues that change the world

A keyframe says where the camera is. A **func frame** says what the *world* does at one instant of the take. They live
on the same timeline and are drawn from its **top** edge, in pink, where keyframes hang from the bottom in amber —
different row, different colour, so the two never have to be told apart by reading a label.

Right-click anywhere on the track for the menu: add a keyframe at the click, add a cue, move the playhead there, or
delete whatever was under the cursor. Click a cue to select it and drag it to retime; double-click to put the playhead
on it.

The actions available today all drive the rupture (the game calls it an "enviro wave" internally):

| Cue | What it does |
|---|---|
| Start rupture | Begins a Heat or Cold rupture, exactly as the world's own timer would |
| Set rupture phase | Restarts the running rupture at a chosen stage — pre-wave, moving, fade-out or growback |
| Set rupture progress | Jumps the running rupture to a point within its current stage — or **ramps** it smoothly between a start and end percentage over a duration you set |
| Cancel rupture | Ends it immediately |
| Pause / Resume rupture | Holds it where it is, and lets it go again |

**Ramping the rupture across the shot.** Tick **Ramp over time** on a *Set rupture progress* cue and it stops being an
instant: give it a start percentage, an end percentage and a duration, and the rupture is driven smoothly between them
while the playhead is inside that span. The span is drawn as a bar running right from the cue's marker, so you can line
it up against the camera move it happens under. Because it follows the playhead rather than firing as the playhead
passes, scrubbing into the middle of a ramp puts the rupture exactly where that moment says it should be — which makes
it the one cue you can compose against by dragging.

Three things worth knowing:

- **Cues fire whenever the playhead moves forward over them** — during a take, during preview play (`Space`), and while
  you scrub the timeline live, so the world keeps up with wherever you have dragged to.
- **Scrubbing backwards takes them back.** Dragging the playhead back past a *Start rupture* cue cancels it; back past a
  *Pause* resumes it, and vice versa. Drag out over a cue and back again and the world is where it started. Three of the
  six actions have an inverse like that; **Set phase**, **Set progress** and **Cancel** do not, and the inspector says so
  under each one rather than leaving you to find out by dragging. Untick a cue's **Enabled** box to work on camera
  timing without it, or use **Trigger now** in the inspector to fire one on its own.
- **A cue changes the save you are playing.** There is no undo for a rupture. This is the one part of the plugin whose
  effects outlive the session, which is why every cue writes what it did to both the status line and the log.
- **You have to be the one running the world** — single player or hosting. As a connected client the server has the
  final say, and a cue may report success while nothing happens.

Unlike keyframes, a func frame stores an **absolute** time rather than deriving one from segment durations. Retiming
the camera move leaves cues exactly where they were put: they belong to a moment in the recording, not to a camera move.

---

## Player safety, and why the body travels with you

Two problems, one solution.

The obvious one: the moment the camera detaches, your body is left standing in the open and fully simulated. A
five-minute shot is a five-minute free hit for anything nearby.

On entering the editor the plugin snapshots your transform, movement mode and gravity scale, freezes you (movement mode
`None`, no gravity, zero velocity) and **buries you**: your X and Y are kept, your Z is set to `-3500`, and a habitat
shelter is spawned around you so you have a floor and four walls. Several thousand units under the terrain there is
nothing to shoot you, nothing to walk into you and no volume containing you. On exit you are teleported back and your
movement state restored exactly.

That altitude is absolute, not a drop from where you were standing — the same relative drop is deep bedrock from a
cliff top and may not clear the floor from a canyon bottom. One number should mean one place.

The body **does not follow the camera.** It used to, on the theory that the world streams around the *player*, so the
body had to chase the shot or you would be filming unstreamed terrain. That turned out to be both unnecessary and
harmful:

- Unnecessary, because a UE `APlayerController` is itself a World Partition streaming source, and
  `GetStreamingSourceLocationAndRotation` is `GetPlayerViewPoint` — which follows the view target, which is our camera.
  The shot already pulls the world in around itself. `camera_rig::Activate` forces `bEnableStreamingSource` on and logs
  the prior value, so this stays verifiable rather than assumed.
- Harmful, because towing walked the body through every radiation field, kill volume and lethal drop the camera flew
  through, and a kill trigger ends the take on the respawn screen.

A person-sized marker is drawn at the stash point by default so you can see where it went.

### Not dying

The game has kill triggers that do not drain a meter, they just kill: the world's own out-of-bounds plane, pain
volumes, hazard fields. Locking the survival meters cannot help with those, because an instant kill happens inside a
single frame and writing health back to full on the next tick does not un-die you — you are already on the respawn
screen with the take lost.

So `death_guard` removes the mechanisms instead of repairing the damage, and puts every one of them back verbatim on
exit:

| Layer | What it does | Default |
|---|---|---|
| World bounds | Clears `AWorldSettings::bEnableWorldBoundsChecks` and floors `KillZ` — the "went too low" death | on |
| Damage | Clears `AActor::bCanBeDamaged` on the pawn, re-asserted every tick because the game rewrites it | on |
| Game immortality | Calls the game's own `UCrCheatManager::Immortal` exec, via `APlayerController::EnableCheats` | **off** |

The third layer is the only one that also covers damage applied through the game's ability system, because it is the
game's own code rather than our guess at it. It is off by default because getting there means asking the player
controller to instantiate the game's cheat manager, and it stays instantiated for the rest of the session. Turn it on
if you are still dying with everything else enabled.

---

## Input handling

By default the editor takes **exclusive** input capture: the game receives no keyboard or mouse while you are
composing, so a stray click cannot fire the weapon you are still holding and `Escape` cannot open the pause menu
mid-shot. The fly camera keeps working regardless, because the ModLoader dispatches plugin keybinds from its WndProc
hook whether or not input is captured.

If you would rather the game kept receiving input — co-op, or triggering something in-world while filming — set
`[Editor] PassthroughInput = true`. That switches to the ModLoader's **cooperative** input mode (interface v51, added
for this plugin): ImGui still owns the cursor and your editor windows stay clickable, but the game only loses the
input ImGui is actually using that frame — mouse while the cursor is over a window, keyboard while a text field has
focus. Everything else, including raw mouse deltas, reaches the game.

Playback mode always takes exclusive capture, whatever this is set to.

### Getting your keys back after leaving the editor

Taking the camera off your character makes the game strip that character's key mappings — 73 of them drop to 11,
leaving only menu and cheat keys — and it never puts them back. That is why the character used to stop responding to
WASD after a session.

The plugin repairs it on the way out, and the repair is on by default (`[Editor] RestoreInputConfigs`). You should
not have to do anything. If you ever do lose your keys, closing and re-opening the editor retries the repair.

### If something still will not respond

Press **`Home`**. It writes a full snapshot of everything that can stop a character responding to input — the
controller's ignore-input counters, view target, possessed pawn, input component, movement mode, the game's own
character state, the live input mappings, and whether the game window even has OS keyboard focus — to the log at
`VeryVerbose`, diffed against the previous snapshot and against the state before the editor was opened.

If the on-screen status does *not* say "Control probe written to the log" when you press it, that is itself the
finding: keybinds are not reaching the plugin, so the problem is in front of anything the log could tell you.

The most useful line is `InputMappings: live set`, which names every mapping as `Action/Key`. Diffing that against a
working one is how every version of this bug was actually found — the counts only ever said "wrong", never *what*.

The other half lives in the ModLoader: turn on **ModLoader Debug Values** in its Global Settings to see whether a
plugin is still holding an input token, and which one. The plugin and the loader record that independently, because
the two disagreeing is one of the specific things worth catching.

---

## Requirements

- StarRupture with [ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader) **interface v52 or newer**.
  Three loader-side changes were made for this plugin: `hooks->HUD->DebugDraw` (v50) draws the in-world path,
  `AcquireInputPassthrough` (v51) is the cooperative input mode, and `GetMouseWheel` (v52) is what the timeline zooms
  with. v52 also fixed a keybind bug where binding a bare `LeftShift`/`LeftControl` never fired, because Windows
  reports those as the generic `VK_SHIFT`/`VK_CONTROL` and the loader's key table only held the sided codes.
- Visual Studio 2022 (MSVC v143, C++20) to build.
- [StarRupture-Game-SDK](https://github.com/AlienXAXS/StarRupture-Game-SDK) and
  [StarRupture-Plugin-SDK](https://github.com/AlienXAXS/StarRupture-Plugin-SDK) as sibling checkouts.

## Building

```bat
msbuild StarRupture-Plugin-CameraControls.sln /p:Configuration="Client Release" /p:Platform=x64
```

Output lands at `build\Client Release\Plugins\CameraControls.dll`. Copy it into the game's `Plugins\` directory
alongside `dwmapi.dll` and launch. On first run `CameraControls.ini` is generated in `<game_dir>\Plugins\config\`.

This is a client-only plugin — there is no camera to drive on a dedicated server.

## Layout

| Path | Role |
|---|---|
| [`src/plugin.cpp`](src/plugin.cpp) | Exports, mode state machine, the game-thread tick |
| [`src/timeline.{h,cpp}`](src/timeline.h) | Keyframes, timing model, spline and ease evaluation |
| [`src/editor_state.h`](src/editor_state.h) | Shared state and the render/game thread contract |
| [`src/camera_rig.{h,cpp}`](src/camera_rig.h) | The `ACameraActor` and view-target handling |
| [`src/fly_controls.{h,cpp}`](src/fly_controls.h) | Free-fly integration |
| [`src/input_binds.{h,cpp}`](src/input_binds.h) | Keybind registration and mouse look |
| [`src/player_safeguard.{h,cpp}`](src/player_safeguard.h) | Stashing and restoring the player |
| [`src/death_guard.{h,cpp}`](src/death_guard.h) | Switching off the kill triggers while filming |
| [`src/hud_visibility.{h,cpp}`](src/hud_visibility.h) | Hiding the game's UMG HUD |
| [`src/world_draw.{h,cpp}`](src/world_draw.h) | In-world spline and keyframe gizmos |
| [`src/viewport_fit.{h,cpp}`](src/viewport_fit.h) | Masking the game's 3D view in beside the panels |
| [`src/project_io.{h,cpp}`](src/project_io.h) | JSON project save / load |
| [`src/ui_*.{h,cpp}`](src/ui_editor.h) | Timeline widget, property inspector, viewport picking, overlays, theme |
