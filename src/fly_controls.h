#pragma once

// ---------------------------------------------------------------------------
// Free-fly camera integration.
//
// Pure maths on plain data -- no SDK, no ImGui. The game-thread tick calls
// Integrate() once per frame with the input accumulated since the last one.
// ---------------------------------------------------------------------------

#include "editor_state.h"

namespace CameraControls::Fly
{
	// Advances `pose` by `input` over `dt` seconds and clears the accumulated
	// look/roll/FOV deltas. Movement is smoothed so tapping a direction key
	// produces a glide rather than a jolt, which matters because the fly camera
	// is often used to line up a shot by hand.
	void Integrate(CameraPose& pose, FlyInput& input, const Options& options, double dt);

	// Drops the retained velocity -- call when the camera is teleported so it
	// does not keep drifting from before the jump.
	void ResetMomentum();
}
