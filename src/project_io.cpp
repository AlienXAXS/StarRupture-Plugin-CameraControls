#include "project_io.h"
#include "plugin_helpers.h"
#include "json.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

using nlohmann::json;

namespace CameraControls::ProjectIO
{
	namespace
	{
		// Bumped only on a breaking format change. Loaders accept anything at
		// or below the current version and fill in missing fields with the
		// Keyframe defaults, so additive changes need no bump.
		constexpr int kFormatVersion = 1;

		std::string g_projectsDir;

		// The plugin DLL's own directory, resolved from the address of a
		// function inside it rather than a DllMain-captured HMODULE.
		std::string ModuleDirectory()
		{
			HMODULE module = nullptr;
			GetModuleHandleExA(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(&ModuleDirectory), &module);

			char path[MAX_PATH] = {};
			GetModuleFileNameA(module, path, MAX_PATH);

			return std::filesystem::path(path).parent_path().string();
		}

		std::string PathFor(const std::string& name)
		{
			return g_projectsDir + "\\" + SanitizeName(name) + ".json";
		}

		json ToJson(const Vec3& v) { return json{ v.x, v.y, v.z }; }

		Vec3 VecFrom(const json& node, const Vec3& fallback)
		{
			if (!node.is_array() || node.size() != 3)
				return fallback;
			return Vec3{ node[0].get<double>(), node[1].get<double>(), node[2].get<double>() };
		}

		// Reads `key` from `node` if it is present and of the right type,
		// otherwise leaves `target` alone -- which is what makes older project
		// files load cleanly against a newer Keyframe.
		template <typename T>
		void ReadInto(const json& node, const char* key, T& target)
		{
			auto it = node.find(key);
			if (it == node.end() || it->is_null())
				return;

			try { target = it->get<T>(); }
			catch (...) { /* wrong type in a hand-edited file -- keep the default */ }
		}
	}

	const std::string& ProjectsDirectory() { return g_projectsDir; }

	std::string SanitizeName(const std::string& name)
	{
		std::string result;
		result.reserve(name.size());

		for (char c : name)
		{
			switch (c)
			{
				case '<': case '>': case ':': case '"':
				case '/': case '\\': case '|': case '?': case '*':
					result += '_';
					break;
				default:
					if (static_cast<unsigned char>(c) >= 0x20)
						result += c;
					break;
			}
		}

		// Trailing dots and spaces are legal in the string but not in a Windows
		// file name, so strip them rather than producing an unopenable path.
		while (!result.empty() && (result.back() == ' ' || result.back() == '.'))
			result.pop_back();

		return result.empty() ? "Untitled" : result;
	}

	void Initialize()
	{
		try
		{
			g_projectsDir = ModuleDirectory() + "\\CameraControls\\Projects";
			std::filesystem::create_directories(g_projectsDir);
			LOG_INFO("ProjectIO: projects directory is %s", g_projectsDir.c_str());
		}
		catch (const std::exception& e)
		{
			LOG_ERROR("ProjectIO: could not create the projects directory: %s", e.what());
			g_projectsDir.clear();
		}
	}

	bool Exists(const std::string& name)
	{
		if (g_projectsDir.empty())
			return false;

		std::error_code ec;
		return std::filesystem::exists(PathFor(name), ec);
	}

	std::vector<std::string> ListProjects()
	{
		std::vector<std::string> names;
		if (g_projectsDir.empty())
			return names;

		try
		{
			for (const auto& entry : std::filesystem::directory_iterator(g_projectsDir))
			{
				if (!entry.is_regular_file())
					continue;
				if (entry.path().extension() != ".json")
					continue;

				names.push_back(entry.path().stem().string());
			}
		}
		catch (const std::exception& e)
		{
			LOG_WARN("ProjectIO: listing projects failed: %s", e.what());
		}

		std::sort(names.begin(), names.end());
		return names;
	}

	bool Save(const std::string& name, const Timeline& timeline, std::string& outError)
	{
		if (g_projectsDir.empty())
		{
			outError = "projects directory is unavailable";
			return false;
		}

		try
		{
			json doc;
			doc["version"]     = kFormatVersion;
			doc["name"]        = timeline.name;
			doc["globalSpeed"] = timeline.globalSpeed;
			doc["loop"]        = timeline.loop;
			doc["frameRate"]   = timeline.frameRate;

			json keys = json::array();
			for (const Keyframe& k : timeline.Keys())
			{
				json j;
				j["id"]              = k.id;
				j["name"]            = k.name;
				j["location"]        = ToJson(k.location);
				j["rotation"]        = json{ k.rotation.pitch, k.rotation.yaw, k.rotation.roll };
				j["fov"]             = k.fov;
				j["duration"]        = k.duration;
				j["speed"]           = k.speed;
				j["smoothness"]      = k.smoothness;
				j["easeIn"]          = static_cast<int>(k.easeIn);
				j["easeOut"]         = static_cast<int>(k.easeOut);
				j["fadeIn"]          = k.fadeIn;
				j["fadeInDuration"]  = k.fadeInDuration;
				j["fadeOut"]         = k.fadeOut;
				j["fadeOutDuration"] = k.fadeOutDuration;
				j["fadeColor"]       = json{ k.fadeColor[0], k.fadeColor[1], k.fadeColor[2] };
				j["lookAt"]          = k.lookAt;
				j["lookAtTarget"]    = ToJson(k.lookAtTarget);
				j["enabled"]         = k.enabled;
				keys.push_back(std::move(j));
			}
			doc["keyframes"] = std::move(keys);

			std::ofstream file(PathFor(name), std::ios::trunc);
			if (!file)
			{
				outError = "could not open the project file for writing";
				return false;
			}

			file << doc.dump(2);
			LOG_DEBUG("ProjectIO: saved %d keyframes to %s",
			          timeline.Count(), PathFor(name).c_str());
			return true;
		}
		catch (const std::exception& e)
		{
			outError = e.what();
			return false;
		}
	}

	bool Load(const std::string& name, Timeline& outTimeline, std::string& outError)
	{
		if (g_projectsDir.empty())
		{
			outError = "projects directory is unavailable";
			return false;
		}

		try
		{
			std::ifstream file(PathFor(name));
			if (!file)
			{
				outError = "project not found";
				return false;
			}

			json doc;
			file >> doc;

			const int version = doc.value("version", 0);
			if (version > kFormatVersion)
			{
				outError = "saved by a newer version of the plugin";
				return false;
			}

			// Build into a scratch timeline so a malformed file cannot leave
			// the caller holding a half-loaded one.
			Timeline loaded;
			loaded.name        = doc.value("name", name);
			loaded.globalSpeed = doc.value("globalSpeed", 1.0f);
			loaded.loop        = doc.value("loop", false);
			loaded.frameRate   = doc.value("frameRate", 30.0f);

			uint32_t highestId = 0;

			auto keys = doc.find("keyframes");
			if (keys != doc.end() && keys->is_array())
			{
				for (const json& j : *keys)
				{
					Keyframe k;

					ReadInto(j, "id",              k.id);
					ReadInto(j, "name",            k.name);
					ReadInto(j, "fov",             k.fov);
					ReadInto(j, "duration",        k.duration);
					ReadInto(j, "speed",           k.speed);
					ReadInto(j, "smoothness",      k.smoothness);
					ReadInto(j, "fadeIn",          k.fadeIn);
					ReadInto(j, "fadeInDuration",  k.fadeInDuration);
					ReadInto(j, "fadeOut",         k.fadeOut);
					ReadInto(j, "fadeOutDuration", k.fadeOutDuration);
					ReadInto(j, "lookAt",          k.lookAt);
					ReadInto(j, "enabled",         k.enabled);

					int easeIn  = static_cast<int>(k.easeIn);
					int easeOut = static_cast<int>(k.easeOut);
					ReadInto(j, "easeIn",  easeIn);
					ReadInto(j, "easeOut", easeOut);
					k.easeIn  = static_cast<Ease>(Clamp(easeIn,  0, static_cast<int>(Ease::Count) - 1));
					k.easeOut = static_cast<Ease>(Clamp(easeOut, 0, static_cast<int>(Ease::Count) - 1));

					k.location     = VecFrom(j.value("location", json{}), k.location);
					k.lookAtTarget = VecFrom(j.value("lookAtTarget", json{}), k.lookAtTarget);

					const json rot = j.value("rotation", json{});
					if (rot.is_array() && rot.size() == 3)
					{
						k.rotation.pitch = rot[0].get<double>();
						k.rotation.yaw   = rot[1].get<double>();
						k.rotation.roll  = rot[2].get<double>();
					}

					const json fade = j.value("fadeColor", json{});
					if (fade.is_array() && fade.size() == 3)
					{
						for (int c = 0; c < 3; ++c)
							k.fadeColor[c] = fade[c].get<float>();
					}

					if (k.id == 0)
						k.id = highestId + 1;
					highestId = std::max(highestId, k.id);

					loaded.Keys().push_back(k);
				}
			}

			loaded.SetNextId(highestId + 1);
			LOG_DEBUG("ProjectIO: loaded %d keyframes from %s (format v%d)",
			          loaded.Count(), PathFor(name).c_str(), version);
			outTimeline = std::move(loaded);
			return true;
		}
		catch (const std::exception& e)
		{
			outError = e.what();
			return false;
		}
	}

	bool Delete(const std::string& name, std::string& outError)
	{
		if (g_projectsDir.empty())
		{
			outError = "projects directory is unavailable";
			return false;
		}

		std::error_code ec;
		if (!std::filesystem::remove(PathFor(name), ec))
		{
			outError = ec ? ec.message() : "project not found";
			return false;
		}

		return true;
	}
}
