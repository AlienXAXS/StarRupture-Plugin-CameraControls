# Camera Controls

A keyframe camera timeline for [StarRupture](https://store.steampowered.com/app/1750870/StarRupture/), built as a
[StarRupture ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader) plugin.

Fly a free camera around your base, drop keyframes wherever you want the shot to pass through, then scrub, tune and
play the whole move back — the way you would cut a camera move in DaVinci Resolve, except the timeline drives the
game's actual camera.

---

## What it does

- **Free-fly camera.** Detaches the view from your character and gives you a proper fly cam: WASD + QE, mouse look,
  boost and crawl modifiers, roll, and live FOV.
- **Keyframes at the press of a key.** Position, rotation and FOV are recorded from wherever the camera is standing.
- **A real timeline.** Clips between keyframes, a draggable playhead, ripple and trim editing, zoom and pan.
- **Per-keyframe control.** Speed, smoothness, ease in/out, FOV, fade in/out, and an optional look-at target.
- **Scrubbing moves the camera.** Drag the playhead and the camera flies along the path with you.
- **In-world gizmos.** The path is drawn as a spline in the 3D world, with a camera frustum at every keyframe. The
  selected keyframe pulses so you can pick it out of a dense path.
- **Double-click to resume.** Double-clicking a keyframe flies the camera to that exact pose, so you can carry on
  composing from a shot you already set up.
- **Playback mode.** A countdown, then the timeline runs start to finish with the game HUD and the editor out of the
  way, for a clean capture.
- **Projects saved to disk.** Plain, hand-editable JSON under `Plugins\CameraControls\Projects\`.
- **Click keyframes in the 3D view.** Every keyframe gets an on-screen handle you can click and double-click, not just
  a row on the timeline.
- **The game view moves out from under the panels.** Rather than covering your shot with the timeline and the
  inspector, the editor shrinks the game's own 3D view into the space they leave free, like the viewer in an editing
  program. It keeps the window's shape, so what you frame is exactly what a full-screen playback records.
- **Your body is looked after — and comes with you.** While the camera is detached your character is frozen and towed
  along underneath it, which keeps it out of harm's way *and* keeps the world streaming in around the shot. Everything
  is put back exactly where it was.

---

## Getting started

1. Load a save.
2. Press **F7** to open the editor.
3. Fly to where the shot should start and press **K** to drop the first keyframe.
4. Fly somewhere else, press **K** again. Repeat.
5. Press **Space** to preview, or **F8** to run it properly.
6. Press **F7** again to put everything back.

The keybind cheat-sheet stays pinned to the top-left corner the whole time you are in the editor, showing your actual
bindings rather than the defaults.

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
| `Z` / `C` | Roll left / right |
| `R` / `F` | Zoom in / out (FOV) |
| `K` | Add a keyframe at the camera |
| `Shift+K` | Insert a keyframe after the selected one |
| `U` | Re-record the selected keyframe from the camera |
| `G` | Fly the camera to the selected keyframe |
| `Delete` | Delete the selected keyframe |
| `,` / `.` | Select the previous / next keyframe |
| `Space` | Preview play / pause |

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

**Fades** dip to (or out of) a colour around a keyframe's time. Fade in on the first keyframe and fade out on the last
is the usual pairing.

---

## Player safety, and why the body travels with you

Two problems, one solution.

The obvious one: the moment the camera detaches, your body is left standing in the open and fully simulated. A
five-minute shot is a five-minute free hit for anything nearby.

The less obvious one: StarRupture streams its world around the **player**, not around the view. Park the body at base,
fly the camera a kilometre away, and you are filming empty terrain — the Mass subsystem never spawned anything out
there.

So the body is not parked, it rides along. On entering the editor the plugin snapshots your transform, movement mode
and gravity scale, then freezes you (movement mode `None`, no gravity, zero velocity) and teleports you to a fixed
offset from the camera every tick — 400 units below it by default, which keeps you under the terrain when filming near
the ground and out of frame when filming from the air. Set the offset positive to ride above the camera instead. On
exit everything is restored exactly.

A person-sized marker is drawn at the stash point by default so you can see it keeping up. There is also an
**experimental** option to spawn a `BP_HabitatBig_C` shelter around the body, which travels with it — a full building
actor appearing out of nowhere is not something every game build enjoys, so it is off by default.

This is deliberately *not* god mode — nothing touches your health, and if you turn the safeguard off you are on your
own.

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
| [`src/hud_visibility.{h,cpp}`](src/hud_visibility.h) | Hiding the game's UMG HUD |
| [`src/world_draw.{h,cpp}`](src/world_draw.h) | In-world spline and keyframe gizmos |
| [`src/viewport_fit.{h,cpp}`](src/viewport_fit.h) | Squeezing the game's 3D view in beside the panels |
| [`src/project_io.{h,cpp}`](src/project_io.h) | JSON project save / load |
| [`src/ui_*.{h,cpp}`](src/ui_editor.h) | Timeline widget, property inspector, viewport picking, overlays, theme |
