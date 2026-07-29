#include "plugin_config.h"

#include <cstring>

namespace CameraControlsConfig
{
	IPluginSelf* Config::s_self = nullptr;

	void Config::Initialize(IPluginSelf* self)
	{
		s_self = self;
		if (s_self)
			s_self->config->InitializeFromSchema(s_self, &SCHEMA);
	}

	bool Config::IsEnabled()
	{
		return s_self ? s_self->config->ReadBool(s_self, "General", "Enabled", true) : true;
	}

	const char* Config::GetKeybind(const char* key, const char* fallback,
	                               char* outBuffer, int bufferSize)
	{
		if (!outBuffer || bufferSize <= 0)
			return fallback;

		outBuffer[0] = '\0';

		if (s_self &&
		    s_self->config->ReadString(s_self, "Keybinds", key, outBuffer, bufferSize, fallback) &&
		    outBuffer[0] != '\0')
		{
			return outBuffer;
		}

		strncpy_s(outBuffer, bufferSize, fallback, _TRUNCATE);
		return outBuffer;
	}

	float Config::FlySpeed()
	{
		return s_self ? s_self->config->ReadFloat(s_self, "Camera", "FlySpeed", 1200.0f) : 1200.0f;
	}

	float Config::MouseSensitivity()
	{
		return s_self ? s_self->config->ReadFloat(s_self, "Camera", "MouseSensitivity", 0.25f) : 0.25f;
	}

	float Config::CountdownSeconds()
	{
		return s_self ? s_self->config->ReadFloat(s_self, "Camera", "CountdownSeconds", 3.0f) : 3.0f;
	}

	bool Config::FitViewport()
	{
		return s_self ? s_self->config->ReadBool(s_self, "Editor", "FitViewport", true) : true;
	}

	bool Config::ShowGizmos()
	{
		return s_self ? s_self->config->ReadBool(s_self, "Editor", "ShowGizmos", true) : true;
	}

	int Config::SplineSamples()
	{
		return s_self ? s_self->config->ReadInt(s_self, "Editor", "SplineSamples", 16) : 16;
	}

	float Config::GizmoScale()
	{
		return s_self ? s_self->config->ReadFloat(s_self, "Editor", "GizmoScale", 3.0f) : 3.0f;
	}

	float Config::GizmoNearCull()
	{
		return s_self ? s_self->config->ReadFloat(s_self, "Editor", "GizmoNearCull", 150.0f) : 150.0f;
	}

	bool Config::HideGameHud()
	{
		return s_self ? s_self->config->ReadBool(s_self, "Editor", "HideGameHud", true) : true;
	}

	bool Config::GizmosDuringPlayback()
	{
		return s_self ? s_self->config->ReadBool(s_self, "Editor", "GizmosDuringPlayback", false) : false;
	}

	bool Config::PassthroughInput()
	{
		return s_self ? s_self->config->ReadBool(s_self, "Editor", "PassthroughInput", false) : false;
	}

	bool Config::ProtectPlayer()
	{
		return s_self ? s_self->config->ReadBool(s_self, "Safety", "ProtectPlayer", true) : true;
	}

	bool Config::SpawnHabitat()
	{
		return s_self ? s_self->config->ReadBool(s_self, "Safety", "SpawnHabitat", false) : false;
	}

	float Config::FollowOffsetZ()
	{
		return s_self ? s_self->config->ReadFloat(s_self, "Safety", "FollowOffsetZ", -400.0f) : -400.0f;
	}

	bool Config::ShowPlayerMarker()
	{
		return s_self ? s_self->config->ReadBool(s_self, "Safety", "ShowPlayerMarker", true) : true;
	}

	bool Config::LockVitals()
	{
		return s_self ? s_self->config->ReadBool(s_self, "Safety", "LockVitals", true) : true;
	}
}
