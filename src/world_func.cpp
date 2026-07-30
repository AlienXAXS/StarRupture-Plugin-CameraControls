#include "world_func.h"
#include "plugin_helpers.h"

#include "Chimera_classes.hpp"

#include <cstdio>

namespace CameraControls::WorldFunc
{
	namespace
	{
		// The rupture lives on a UWorldSubsystem, which has no accessor the
		// generated SDK exposes -- so it is found by class name through the
		// object walker, the same way the habitat class and the UI layout widget
		// are found. One per world.
		constexpr const char* kWaveSubsystemClass = "CrEnviroWaveSubsystem";
		constexpr int         kMaxMatches         = 8;

		SDK::UCrEnviroWaveSubsystem* g_wave = nullptr;

		// Cached because the walk is a synchronous pass over tens of thousands
		// of GObjects entries and a cue that fires mid-take must not cost a
		// frame. Dropped on world end-play rather than validated, because there
		// is nothing safe to validate a dead UObject pointer against.
		SDK::UCrEnviroWaveSubsystem* FindWaveSubsystem()
		{
			if (g_wave)
				return g_wave;

			auto* hooks = GetHooks();
			if (!hooks || !hooks->ObjectWalker || !hooks->ObjectWalker->IsReady())
			{
				LOG_WARN("WorldFunc: the object walker is unavailable -- cues cannot run");
				return nullptr;
			}

			PluginObjectInfo matches[kMaxMatches] = {};
			const int found = hooks->ObjectWalker->FindObjectsByClassNameInto(
				kWaveSubsystemClass, PluginObjectLookup_InstanceOnly, matches, kMaxMatches);

			if (found <= 0)
			{
				LOG_WARN("WorldFunc: no live '%s' -- this world has no rupture system",
				         kWaveSubsystemClass);
				return nullptr;
			}

			g_wave = static_cast<SDK::UCrEnviroWaveSubsystem*>(matches[0].object);
			LOG_DEBUG("WorldFunc: resolved %s at %p (%d instance(s))",
			          kWaveSubsystemClass, static_cast<void*>(g_wave), found);
			return g_wave;
		}
	}

	void ForgetWorldState()
	{
		g_wave = nullptr;
	}

	bool HoldRuptureProgress(float progress)
	{
		try
		{
			SDK::UCrEnviroWaveSubsystem* wave = FindWaveSubsystem();
			if (!wave || !wave->IsWaveInProgress())
				return false;

			wave->ForceWaveStageProgress(Clamp(progress, 0.0f, 1.0f));
			return true;
		}
		catch (...)
		{
			// Deliberately mute. This is a per-frame path, and a throw here means
			// the same throw next frame -- one line becomes sixty a second, which
			// is how a log stops being readable at exactly the moment it matters.
			// The caller's edge-trigger on `false` is what reports it.
			return false;
		}
	}

	bool Execute(const FuncFrame& frame, std::string& outMessage)
	{
		try
		{
			SDK::UCrEnviroWaveSubsystem* wave = FindWaveSubsystem();
			if (!wave)
			{
				outMessage = "No rupture system in this world";
				return false;
			}

			// Range-checked here rather than trusted from the project file: the
			// int comes from JSON somebody may have hand-edited, and casting an
			// out-of-range value into a uint8 enum the game switches on is not
			// something to find out about in-game.
			const SDK::EEnviroWave type =
				static_cast<SDK::EEnviroWave>(Clamp(frame.waveType, 1, 2));

			char line[192];

			switch (frame.action)
			{
				case FuncAction::StartRupture:
				{
					wave->StartWave(type);
					snprintf(line, sizeof(line), "Rupture started (%s)",
					         RuptureTypeName(frame.waveType));
					outMessage = line;
					return true;
				}

				case FuncAction::SetRupturePhase:
				{
					// StartWaveCustom needs the settings block the stage runs
					// with, and there is no defensible default for one: a zeroed
					// FCrEnviroWaveSettings describes a rupture with no speed
					// and no stage durations, which starts and then never does
					// anything. So the settings come from the rupture the game
					// is already running -- which means there has to be one.
					if (!wave->IsWaveInProgress())
					{
						outMessage = "No rupture running -- put a Start rupture cue first";
						return false;
					}

					SDK::FCrEnviroWaveSettings settings = wave->GetCurrentStageSettings();
					if (!(settings.WaveSpeed > 0.0f))
					{
						outMessage = "The running rupture reported no settings -- phase unchanged";
						return false;
					}

					const SDK::EEnviroWaveStage stage =
						static_cast<SDK::EEnviroWaveStage>(Clamp(frame.stage, 1, 4));

					wave->StartWaveCustom(type, stage, settings);
					snprintf(line, sizeof(line), "Rupture phase set to %s (%s)",
					         RuptureStageName(frame.stage), RuptureTypeName(frame.waveType));
					outMessage = line;
					return true;
				}

				case FuncAction::SetRuptureProgress:
				{
					if (!wave->IsWaveInProgress())
					{
						outMessage = "No rupture running -- nothing to advance";
						return false;
					}

					const float progress = Clamp(frame.progress, 0.0f, 1.0f);
					wave->ForceWaveStageProgress(progress);
					snprintf(line, sizeof(line), "Rupture advanced to %.0f%% of its stage",
					         progress * 100.0f);
					outMessage = line;
					return true;
				}

				case FuncAction::CancelRupture:
				{
					if (!wave->IsWaveInProgress())
					{
						outMessage = "No rupture running -- nothing to cancel";
						return false;
					}

					wave->CancelCurrentWave();
					outMessage = "Rupture cancelled";
					return true;
				}

				case FuncAction::PauseRupture:
				{
					if (!wave->IsWaveInProgress())
					{
						outMessage = "No rupture running -- nothing to pause";
						return false;
					}

					wave->PauseCurrentWave();
					outMessage = "Rupture paused";
					return true;
				}

				case FuncAction::ResumeRupture:
				{
					wave->ResumeCurrentWave();
					outMessage = "Rupture resumed";
					return true;
				}

				default:
					outMessage = "This cue does nothing";
					return false;
			}
		}
		catch (const std::exception& e)
		{
			LOG_ERROR("WorldFunc: '%s' threw: %s", FuncActionName(frame.action), e.what());
			outMessage = "The world refused that cue -- see the log";
			return false;
		}
		catch (...)
		{
			LOG_ERROR("WorldFunc: '%s' threw", FuncActionName(frame.action));
			outMessage = "The world refused that cue -- see the log";
			return false;
		}
	}
}
