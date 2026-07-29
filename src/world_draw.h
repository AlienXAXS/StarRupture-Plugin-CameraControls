#pragma once

// ---------------------------------------------------------------------------
// In-world gizmos for the camera path, drawn through hooks->HUD->DebugDraw.
//
// The debug-draw interface has no spline primitive, so the path is sampled off
// the timeline and emitted as a chain of straight segments -- dense enough that
// the joins are invisible at any sane sample count.
//
// The draw calls are documented as safe from any thread, but everything here
// reads the timeline, so it is called from the game-thread tick under the
// state lock like the rest of the per-frame work.
// ---------------------------------------------------------------------------

#include "timeline.h"

namespace CameraControls::WorldDraw
{
	// Cheap probe for whether the line batchers resolved on this build. When
	// false every Draw* below is a no-op and the editor says so in its status
	// line rather than silently drawing nothing.
	bool IsAvailable();

	struct DrawParams
	{
		int      splineSamples = 16;
		uint32_t selectedId    = 0;
		double   playhead      = 0.0;
		double   now           = 0.0;   // drives the selection pulse
		bool     showPlayhead  = true;

		// Multiplier passed to DrawCameraAt, which multiplies it by a base of
		// 4.0 with 2:1:1.5 proportions -- so the camera body ends up roughly
		// (8 x 4 x 6) * gizmoScale Unreal units. 3.0 gives a body about half a
		// metre long, which reads at working distances without swallowing the
		// thing being filmed.
		float gizmoScale = 3.0f;

		// Where the viewer is. Any geometry closer than nearCull to this point
		// is skipped: a debug line crossing the near plane is stretched across
		// the entire screen, which is worse than not drawing it at all. The
		// worst offender is the playhead gizmo, which during scrub preview sits
		// exactly on top of the camera.
		Vec3  cameraLocation;
		float nearCull = 150.0f;

		// Where the safeguard has parked the player's body. Drawn as a box so
		// it is obvious where it went and that it is keeping up with the shot.
		bool showPlayerMarker = false;
		Vec3 playerLocation;
	};

	// Draws the whole path: spline, per-keyframe camera gizmos, the selection
	// highlight and the playhead marker. One frame's worth -- call every tick.
	void DrawTimeline(const Timeline& timeline, const DrawParams& params);
}
