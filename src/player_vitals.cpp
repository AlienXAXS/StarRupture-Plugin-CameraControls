#include "player_vitals.h"
#include "plugin_helpers.h"

#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"

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
				SDK::UWorld* world = SDK::UWorld::GetWorld();
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
			auto pinToMax = [&](auto* set, auto memberCurrent, auto memberMax)
			{
				if (!set) return;
				PinTo(set->*memberCurrent, set->*memberMax);
				touchedAnything = true;
			};

			pinToMax(character->HealthAttributes,    &SDK::UCrHealthAttributeSet::CurrentHealth,       &SDK::UCrHealthAttributeSet::MaxHealth);
			pinToMax(character->CaloriesAttributes,  &SDK::UCrCaloriesAttributeSet::CurrentCalories,   &SDK::UCrCaloriesAttributeSet::MaxCalories);
			pinToMax(character->HydrationAttributes, &SDK::UCrHydrationAttributeSet::CurrentHydration, &SDK::UCrHydrationAttributeSet::MaxHydration);
			pinToMax(character->OxygenAttributes,    &SDK::UCrOxygenAttributeSet::CurrentOxygen,       &SDK::UCrOxygenAttributeSet::MaxOxygen);
			pinToMax(character->EnergyAttributes,    &SDK::UCrEnergyAttributeSet::CurrentEnergy,       &SDK::UCrEnergyAttributeSet::MaxEnergy);
			pinToMax(character->ShieldAttributes,    &SDK::UCrShieldAttributeSet::CurrentShield,       &SDK::UCrShieldAttributeSet::MaxShield);

			// --- Hazards: hold at minimum -------------------------------------
			// These count *up* to kill you, so their floor is the safe end.
			auto pinToMin = [&](auto* set, auto memberCurrent, auto memberMin)
			{
				if (!set) return;
				PinTo(set->*memberCurrent, set->*memberMin);
				touchedAnything = true;
			};

			pinToMin(character->ToxicityAttributes,  &SDK::UCrToxicityAttributeSet::CurrentToxicity,   &SDK::UCrToxicityAttributeSet::MinToxicity);
			pinToMin(character->RadiationAttributes, &SDK::UCrRadiationAttributeSet::CurrentRadiation, &SDK::UCrRadiationAttributeSet::MinRadiation);
			pinToMin(character->HeatAttributes,      &SDK::UCrHeatAttributeSet::CurrentHeat,           &SDK::UCrHeatAttributeSet::MinHeat);
			pinToMin(character->DrainAttributes,     &SDK::UCrDrainAttributeSet::CurrentDrain,         &SDK::UCrDrainAttributeSet::MinDrain);
			pinToMin(character->CorrosionAttributes, &SDK::UCrCorrosionAttributeSet::CurrentCorrosion, &SDK::UCrCorrosionAttributeSet::MinCorrosion);
			pinToMin(character->InfectionAttributes, &SDK::UCrInfectionAttributeSet::CurrentInfection, &SDK::UCrInfectionAttributeSet::MinInfection);

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
					          character->HealthAttributes    ? 1 : 0,
					          character->CaloriesAttributes  ? 1 : 0,
					          character->HydrationAttributes ? 1 : 0,
					          character->OxygenAttributes    ? 1 : 0,
					          character->EnergyAttributes    ? 1 : 0,
					          character->ShieldAttributes    ? 1 : 0);
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
