#pragma once

// ---------------------------------------------------------------------------
// Timeline persistence.
//
// Projects are JSON files under <game>\Plugins\CameraControls\Projects\, one
// per timeline, named after the project. The format is deliberately plain and
// hand-editable: a version stamp, the project settings, and a keyframe array.
//
// Pure file + JSON work; no SDK, no ImGui. Called from either thread, but the
// caller must copy the timeline out from under the state lock first rather
// than holding it across a disk write.
// ---------------------------------------------------------------------------

#include "timeline.h"

#include <string>
#include <vector>

namespace CameraControls::ProjectIO
{
	// Resolves (and creates) <plugin dir>\CameraControls\Projects. Call once
	// during PluginInit.
	void Initialize();

	// Absolute path of the projects folder, for showing the user where their
	// files went.
	const std::string& ProjectsDirectory();

	// Project names (file stems), sorted, without the .json extension.
	std::vector<std::string> ListProjects();

	// Writes `timeline` to <projects>\<name>.json. Returns false and fills
	// outError on failure.
	bool Save(const std::string& name, const Timeline& timeline, std::string& outError);

	// Reads <projects>\<name>.json into `outTimeline`. Returns false and fills
	// outError on failure; outTimeline is left untouched in that case.
	bool Load(const std::string& name, Timeline& outTimeline, std::string& outError);

	bool Delete(const std::string& name, std::string& outError);

	bool Exists(const std::string& name);

	// Strips characters that cannot appear in a file name. Returns "Untitled"
	// for an empty result.
	std::string SanitizeName(const std::string& name);
}
