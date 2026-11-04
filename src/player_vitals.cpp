#include "player_vitals.h"
#include "plugin_helpers.h"
#include "cameracontrols_fields.h"

#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"

namespace Fields = CameraControlsFields;

namespace CameraControls::Vitals
{
	namespace
	{
		bool g_active   = false;
		bool g_reported = false;   // edge-trigger, so Pin() never spams

		SDK::ACrCharacterPlayerBase* GetLocalCharacter()
		{
			try
			{
				SDK::UWorld* world = PluginSdkRuntime::GetWorld();
				if (!world) return nullptr;

				SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
				if (!pc || !pc->Pawn) return nullptr;

				return static_cast<SDK::ACrCharacterPlayerBase*>(pc->Pawn);
			}
			catch (...)
			{
				return nullptr;
			}
		}

		// Both halves matter. CurrentValue is what the game reads, but gameplay
		// effects recompute it from BaseValue, so writing only the current value
		// gets undone the moment the next survival tick lands.
		void Set(SDK::FGameplayAttributeData& attribute, float value)
		{
			attribute.BaseValue    = value;
			attribute.CurrentValue = value;
		}

		// Pins `current` to `target`, but only when it has actually drifted --
		// writing an attribute that is already correct just churns replication.
		void PinTo(SDK::FGameplayAttributeData& current, const SDK::FGameplayAttributeData& target)
		{
			if (current.CurrentValue != target.CurrentValue ||
			    current.BaseValue    != target.CurrentValue)
			{
				Set(current, target.CurrentValue);
			}
		}
	}

	bool IsActive() { return g_active; }

	void Stop()
	{
		// Nothing to put back -- see the header. This only stops the flag claiming
		// the attributes are being held when the tick has stopped holding them.
		g_active   = false;
		g_reported = false;
	}

	void ForgetWorldState()
	{
		g_active   = false;
		g_reported = false;
	}

	void Pin()
	{
		SDK::ACrCharacterPlayerBase* character = GetLocalCharacter();
		if (!character)
		{
			g_active = false;
			return;
		}

		try
		{
			bool touchedAnything = false;

			// --- Resources: hold at maximum ----------------------------------
			// Health, and the three the player actually watches -- food, water,
			// air -- plus shield and suit energy, which are just as capable of
			// ending a long take.
			auto pinToMax = [&](const auto& setField, const auto& currentField, const auto& boundField)
			{
				auto* set     = Fields::ValueOr(setField, character);
				auto* current = currentField.In(set);
				auto* bound   = boundField.In(set);
				if (!current || !bound) return;
				PinTo(*current, *bound);
				touchedAnything = true;
			};

			pinToMax(Fields::HealthSet, Fields::HealthCurrent, Fields::HealthMax);
			pinToMax(Fields::CaloriesSet, Fields::CaloriesCurrent, Fields::CaloriesMax);
			pinToMax(Fields::HydrationSet, Fields::HydrationCurrent, Fields::HydrationMax);
			pinToMax(Fields::OxygenSet, Fields::OxygenCurrent, Fields::OxygenMax);
			pinToMax(Fields::EnergySet, Fields::EnergyCurrent, Fields::EnergyMax);
			pinToMax(Fields::ShieldSet, Fields::ShieldCurrent, Fields::ShieldMax);

			// --- Hazards: hold at minimum -------------------------------------
			// These count *up* to kill you, so their floor is the safe end.
			auto pinToMin = [&](const auto& setField, const auto& currentField, const auto& boundField)
			{
				auto* set     = Fields::ValueOr(setField, character);
				auto* current = currentField.In(set);
				auto* bound   = boundField.In(set);
				if (!current || !bound) return;
				PinTo(*current, *bound);
				touchedAnything = true;
			};

			pinToMin(Fields::ToxicitySet, Fields::ToxicityCurrent, Fields::ToxicityMin);
			pinToMin(Fields::RadiationSet, Fields::RadiationCurrent, Fields::RadiationMin);
			pinToMin(Fields::HeatSet, Fields::HeatCurrent, Fields::HeatMin);
			pinToMin(Fields::DrainSet, Fields::DrainCurrent, Fields::DrainMin);
			pinToMin(Fields::CorrosionSet, Fields::CorrosionCurrent, Fields::CorrosionMin);
			pinToMin(Fields::InfectionSet, Fields::InfectionCurrent, Fields::InfectionMin);

			// Temperature is deliberately left alone. Unlike the others it is a
			// band, not a bar -- its minimum means "freezing", which is every bit
			// as lethal as its maximum, so there is no single safe value to pin
			// it to without guessing at the comfortable range.

			if (touchedAnything != g_reported)
			{
				g_reported = touchedAnything;
				if (touchedAnything)
				{
					LOG_DEBUG("Vitals: locking survival attributes "
					          "(health=%d calories=%d hydration=%d oxygen=%d energy=%d shield=%d)",
					          Fields::ValueOr(Fields::HealthSet, character)    ? 1 : 0,
					          Fields::ValueOr(Fields::CaloriesSet, character)  ? 1 : 0,
					          Fields::ValueOr(Fields::HydrationSet, character) ? 1 : 0,
					          Fields::ValueOr(Fields::OxygenSet, character)    ? 1 : 0,
					          Fields::ValueOr(Fields::EnergySet, character)    ? 1 : 0,
					          Fields::ValueOr(Fields::ShieldSet, character)    ? 1 : 0);
				}
				else
				{
					LOG_WARN("Vitals: the character has no attribute sets -- nothing to lock");
				}
			}

			g_active = touchedAnything;
		}
		catch (...)
		{
			LOG_WARN("Vitals: Pin threw -- attribute locking disabled for this session");
			g_active = false;
		}
	}
}
