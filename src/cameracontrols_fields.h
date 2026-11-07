#pragma once

#include "plugin_interface.h"
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"
#include "Chimera_structs.hpp"
#include "plugin_map.h"

#include <cstdint>
#include <iterator>

// Every member of a game class the plugin reads or writes, taken from the
// loader's map for the running game build (see plugin_map.h). Resolved once in
// OnPluginLoadHooks; used on the game thread with In(object), which is null
// when the object is null or the member did not resolve, in which case the
// feature using it stays off. Engine members (APlayerController::Pawn,
// ::PlayerCameraManager, ::CheatManager, UWorld, the character movement
// component) are not game classes and stay compiled in.

#define CC_FIELD(Class, Member) SR_FIELD(decltype(SDK::Class::Member), Class, Member, 0)

namespace CameraControlsFields
{
	// -------------------------------------------------------------------
	// Native members: no UPROPERTY, so the SDK has no name for them and the
	// generated class knows them only as padding. The offsets were measured
	// from the game's code and are this plugin's own rows until the loader's
	// map carries them.
	// -------------------------------------------------------------------

	// `AAuPlayerCameraManager::FOV` and `::AdditionalFOVOffset`. Neither is a
	// UPROPERTY, so the generated class knows the whole run of them only as
	// `Pad_28F8[0x118]` between `DefaultCameraSettings` (0x28A8) and
	// `CurrentConfig` (0x2A10).
	//
	// The offsets are measured from `UpdateViewTarget`, not guessed, and they
	// corroborate each other: that one function touches the manager's live
	// mirror of `FAuCameraConfig` in the struct's own field order --
	//
	//     0x295C YOffset        0x2968 SocketOffset (FVector)
	//     0x2960 ZOffset        0x2980 TargetOffset (FVector)
	//     0x2964 TargetArmLenght
	//     0x2998 FOV            0x299C AdditionalFOVOffset
	//
	// -- and the same function reads `CurrentConfig.InterpolationSpeed` at
	// 0x2A54, which is 0x2A10 + 0x44, matching the offset the generated header
	// gives `CurrentConfig` and the layout it gives `FAuCameraConfig`. The
	// arithmetic only closes if the run starts where we think it does.
	//
	// Each is a field request: the loader's map holds the row per game build,
	// and the built CL-127004 row below stands in when the map has none. The
	// two ULocalPlayer members have no row of their own here: the map carries
	// both on every supported build, and the offset in the request is only the
	// one this plugin's SDK was generated with.

	// `ULocalPlayer::Origin` and `::Size` -- two FVector2Ds, which are pairs of
	// doubles in UE5. Neither is a UPROPERTY, so they land inside the
	// generated class's padding: the dump has ViewportClient at 0x78,
	// Pad_80[0x38], then AspectRatioAxisConstraint at 0xB8. The engine's
	// member order across that gap is Origin, Size, LastViewLocation, and
	// 0x10 + 0x10 + 0x18 is exactly 0x38 -- the arithmetic only closes here.
	inline auto Fov = PluginField<float>{ { "AAuPlayerCameraManager::FOV",
		"AAuPlayerCameraManager", "FOV", PLUGIN_FIELD_NATIVE, sizeof(float), 0x2998 } };
	inline auto AdditionalFovOffset = PluginField<float>{ { "AAuPlayerCameraManager::AdditionalFOVOffset",
		"AAuPlayerCameraManager", "AdditionalFOVOffset", PLUGIN_FIELD_NATIVE, sizeof(float), 0x299C } };
	inline auto LocalPlayerOrigin = PluginField<SDK::FVector2D>{ { "ULocalPlayer::Origin",
		"ULocalPlayer", "Origin", PLUGIN_FIELD_NATIVE, sizeof(SDK::FVector2D), 0x80 } };
	inline auto LocalPlayerSize = PluginField<SDK::FVector2D>{ { "ULocalPlayer::Size",
		"ULocalPlayer", "Size", PLUGIN_FIELD_NATIVE, sizeof(SDK::FVector2D), 0x90 } };

	// CL-127004's rows, the build this plugin was compiled against; every other
	// build comes from the loader's map.
	inline const PluginFieldCandidate kFovRows[] = {
		{ 127004, 0x2998, sizeof(float), 0, nullptr },
	};
	inline const PluginFieldCandidate kAdditionalFovRows[] = {
		{ 127004, 0x299C, sizeof(float), 0, nullptr },
	};

	// `APlayerController::bCinematicMode` and `::bHidePawnInCinematicMode`.
	// Neither is a UPROPERTY, so the generated class knows them only as the
	// low bits of `BitPad_4D0_0 : 4`, and each is a native bitfield: the
	// loader's row holds the byte and the mask. They share their byte with the
	// reflected `bPlayerIsWaiting`, which the control probe uses to check that
	// the byte it reads is the one it means.
	inline auto PcCinematicMode = PluginBitField{ { "APlayerController::bCinematicMode",
		"APlayerController", "bCinematicMode", PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_BITFIELD, 1, -1 } };
	inline auto PcCinematicHidePlayer = PluginBitField{ { "APlayerController::bHidePawnInCinematicMode",
		"APlayerController", "bHidePawnInCinematicMode", PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_BITFIELD, 1, -1 } };
	inline auto PcIsWaiting = SR_BITFIELD(APlayerController, bPlayerIsWaiting);

	// The byte and masks on each game build CameraControls supports (the same on
	// both, measured from the PDBs). The loader's own row for the running build
	// wins when it has one.
	inline const PluginFieldCandidate kCinematicModeRows[] = {
		{ 127004, 0x4D0, 1, 0x02, nullptr },
		{ 128786, 0x4D0, 1, 0x02, nullptr },
	};
	inline const PluginFieldCandidate kCinematicHidePlayerRows[] = {
		{ 127004, 0x4D0, 1, 0x04, nullptr },
		{ 128786, 0x4D0, 1, 0x04, nullptr },
	};

	// -------------------------------------------------------------------
	// Reflected members
	// -------------------------------------------------------------------

	inline auto ContextualInputBindings = CC_FIELD(UCrHeroComponent, ContextualInputBindings);

	// What RestorePlayerInputConfigs logs about the hero component, for the
	// input diagnostics: the exclusive bindings, and the default mappings with
	// each entry's context asset and priority.
	inline auto ExclusiveInputBindings   = CC_FIELD(UCrHeroComponent, ExclusiveInputBindings);
	inline auto DefaultInputMappings     = CC_FIELD(UCrHeroComponent, DefaultInputMappings);
	inline auto DefaultMappingContext    = CC_FIELD(FInputMappingContextAndPriority, InputMapping);
	inline auto DefaultMappingPriority   = CC_FIELD(FInputMappingContextAndPriority, Priority);

	// The player character's survival attribute sets.
	inline auto HealthSet    = CC_FIELD(ACrCharacterPlayerBase, HealthAttributes);
	inline auto CaloriesSet  = CC_FIELD(ACrCharacterPlayerBase, CaloriesAttributes);
	inline auto HydrationSet = CC_FIELD(ACrCharacterPlayerBase, HydrationAttributes);
	inline auto OxygenSet    = CC_FIELD(ACrCharacterPlayerBase, OxygenAttributes);
	inline auto EnergySet    = CC_FIELD(ACrCharacterPlayerBase, EnergyAttributes);
	inline auto ShieldSet    = CC_FIELD(ACrCharacterPlayerBase, ShieldAttributes);
	inline auto ToxicitySet  = CC_FIELD(ACrCharacterPlayerBase, ToxicityAttributes);
	inline auto RadiationSet = CC_FIELD(ACrCharacterPlayerBase, RadiationAttributes);
	inline auto HeatSet      = CC_FIELD(ACrCharacterPlayerBase, HeatAttributes);
	inline auto DrainSet     = CC_FIELD(ACrCharacterPlayerBase, DrainAttributes);
	inline auto CorrosionSet = CC_FIELD(ACrCharacterPlayerBase, CorrosionAttributes);
	inline auto InfectionSet = CC_FIELD(ACrCharacterPlayerBase, InfectionAttributes);

	// Resources are held at their maximum ...
	inline auto HealthCurrent    = CC_FIELD(UCrHealthAttributeSet, CurrentHealth);
	inline auto HealthMax        = CC_FIELD(UCrHealthAttributeSet, MaxHealth);
	inline auto CaloriesCurrent  = CC_FIELD(UCrCaloriesAttributeSet, CurrentCalories);
	inline auto CaloriesMax      = CC_FIELD(UCrCaloriesAttributeSet, MaxCalories);
	inline auto HydrationCurrent = CC_FIELD(UCrHydrationAttributeSet, CurrentHydration);
	inline auto HydrationMax     = CC_FIELD(UCrHydrationAttributeSet, MaxHydration);
	inline auto OxygenCurrent    = CC_FIELD(UCrOxygenAttributeSet, CurrentOxygen);
	inline auto OxygenMax        = CC_FIELD(UCrOxygenAttributeSet, MaxOxygen);
	inline auto EnergyCurrent    = CC_FIELD(UCrEnergyAttributeSet, CurrentEnergy);
	inline auto EnergyMax        = CC_FIELD(UCrEnergyAttributeSet, MaxEnergy);
	inline auto ShieldCurrent    = CC_FIELD(UCrShieldAttributeSet, CurrentShield);
	inline auto ShieldMax        = CC_FIELD(UCrShieldAttributeSet, MaxShield);

	// ... and hazards at their minimum.
	inline auto ToxicityCurrent  = CC_FIELD(UCrToxicityAttributeSet, CurrentToxicity);
	inline auto ToxicityMin      = CC_FIELD(UCrToxicityAttributeSet, MinToxicity);
	inline auto RadiationCurrent = CC_FIELD(UCrRadiationAttributeSet, CurrentRadiation);
	inline auto RadiationMin     = CC_FIELD(UCrRadiationAttributeSet, MinRadiation);
	inline auto HeatCurrent      = CC_FIELD(UCrHeatAttributeSet, CurrentHeat);
	inline auto HeatMin          = CC_FIELD(UCrHeatAttributeSet, MinHeat);
	inline auto DrainCurrent     = CC_FIELD(UCrDrainAttributeSet, CurrentDrain);
	inline auto DrainMin         = CC_FIELD(UCrDrainAttributeSet, MinDrain);
	inline auto CorrosionCurrent = CC_FIELD(UCrCorrosionAttributeSet, CurrentCorrosion);
	inline auto CorrosionMin     = CC_FIELD(UCrCorrosionAttributeSet, MinCorrosion);
	inline auto InfectionCurrent = CC_FIELD(UCrInfectionAttributeSet, CurrentInfection);
	inline auto InfectionMin     = CC_FIELD(UCrInfectionAttributeSet, MinInfection);

	// What the control probe logs about the player character and controller.
	inline auto CharDisabledPawn   = CC_FIELD(ACrCharacterPlayerBase, bIsDisabledPawn);
	inline auto CharInteractState  = CC_FIELD(ACrCharacterPlayerBase, InteractionState);
	inline auto CharDead           = CC_FIELD(ACrCharacterPlayerBase, bDead);
	inline auto CharOnLadder       = CC_FIELD(ACrCharacterPlayerBase, bOnLadder);
	inline auto CharInitialised    = CC_FIELD(ACrCharacterPlayerBase, bCharacterSelectedAndInitialized);
	inline auto CharBuildingDrone  = CC_FIELD(ACrCharacterPlayerBase, BuildingDrone);
	inline auto CharController     = CC_FIELD(ACrCharacterPlayerBase, CrPC);

	inline auto PcMenuBlockers     = CC_FIELD(ACrPlayerControllerBase, GameMenuBlockers);
	inline auto PcInPauseMenu      = CC_FIELD(ACrPlayerControllerBase, bReplInPauseMenu);
	inline auto PcPreviousPawn     = CC_FIELD(ACrPlayerControllerBase, PreviousPawn);
	inline auto PcMainCharacter    = CC_FIELD(ACrPlayerControllerBase, PlayerMainCharacter);
	inline auto PcInteractable     = CC_FIELD(ACrPlayerControllerBase, CurrentInteractableActor);
	inline auto PcActiveInteract   = CC_FIELD(ACrPlayerControllerBase, CurrentInteractableActorWithActiveInteraction);
	inline auto PcZipline          = CC_FIELD(ACrPlayerControllerBase, CurrentZipline);
	inline auto PcBuildTarget      = CC_FIELD(ACrPlayerControllerBase, CurrentBuildingTarget);

	// The rupture settings block is copied by value out of the running wave and
	// back into StartWaveCustom, and the contextual bindings are the value of a
	// TMap that the SDK walks with the compiled element size, so each is checked
	// against the running build.
	inline auto WaveSpeed         = CC_FIELD(FCrEnviroWaveSettings, WaveSpeed);
	inline auto WaveSettingsSize  = SR_SIZE_CHECK(FCrEnviroWaveSettings);
	inline auto BindingSize       = SR_SIZE_CHECK(FCrContextualInputBinding);

	// The default mappings array is only stepped, by the size the running build
	// reports (PluginMap::ElementAt).
	inline auto MappingSize       = SR_SIZE(FInputMappingContextAndPriority);

	// The parts of the plugin that can be off while the rest runs. A member that
	// does not resolve on a game build turns off the part that reads it, and the
	// config entries it drives are greyed out. The contextual input bindings
	// stay core: without them the player's keys cannot be given back.
	inline PluginMap::Feature FovControl{ "fov", "Field of view control",
		{ "AAuPlayerCameraManager::FOV", "AAuPlayerCameraManager::AdditionalFOVOffset" },
		{ "Keybinds.FovIn", "Keybinds.FovOut" } };
	inline PluginMap::Feature ViewportMask{ "viewport", "Masking the game view beside the panels",
		{ "ULocalPlayer::Origin", "ULocalPlayer::Size" },
		{ "Editor.FitViewport" } };
	inline PluginMap::Feature VitalsLock{ "vitals", "Locking health, food and water",
		{ "ACrCharacterPlayerBase::HealthAttributes", "UCrHealthAttributeSet::CurrentHealth", "UCrHealthAttributeSet::MaxHealth",
		  "ACrCharacterPlayerBase::CaloriesAttributes", "UCrCaloriesAttributeSet::CurrentCalories", "UCrCaloriesAttributeSet::MaxCalories",
		  "ACrCharacterPlayerBase::HydrationAttributes", "UCrHydrationAttributeSet::CurrentHydration", "UCrHydrationAttributeSet::MaxHydration",
		  "ACrCharacterPlayerBase::OxygenAttributes", "UCrOxygenAttributeSet::CurrentOxygen", "UCrOxygenAttributeSet::MaxOxygen",
		  "ACrCharacterPlayerBase::EnergyAttributes", "UCrEnergyAttributeSet::CurrentEnergy", "UCrEnergyAttributeSet::MaxEnergy",
		  "ACrCharacterPlayerBase::ShieldAttributes", "UCrShieldAttributeSet::CurrentShield", "UCrShieldAttributeSet::MaxShield",
		  "ACrCharacterPlayerBase::ToxicityAttributes", "UCrToxicityAttributeSet::CurrentToxicity", "UCrToxicityAttributeSet::MinToxicity",
		  "ACrCharacterPlayerBase::RadiationAttributes", "UCrRadiationAttributeSet::CurrentRadiation", "UCrRadiationAttributeSet::MinRadiation",
		  "ACrCharacterPlayerBase::HeatAttributes", "UCrHeatAttributeSet::CurrentHeat", "UCrHeatAttributeSet::MinHeat",
		  "ACrCharacterPlayerBase::DrainAttributes", "UCrDrainAttributeSet::CurrentDrain", "UCrDrainAttributeSet::MinDrain",
		  "ACrCharacterPlayerBase::CorrosionAttributes", "UCrCorrosionAttributeSet::CurrentCorrosion", "UCrCorrosionAttributeSet::MinCorrosion",
		  "ACrCharacterPlayerBase::InfectionAttributes", "UCrInfectionAttributeSet::CurrentInfection", "UCrInfectionAttributeSet::MinInfection" },
		{ "Safety.LockVitals" } };
	inline PluginMap::Feature RupturePhase{ "rupture", "Rupture phase cues",
		{ "FCrEnviroWaveSettings::WaveSpeed", "FCrEnviroWaveSettings" }, {} };
	inline PluginMap::Feature CinematicBits{ "pcbits", "Cinematic mode bits in the input diagnostics",
		PluginMap::KeysOf(PcCinematicMode, PcCinematicHidePlayer, PcIsWaiting), {} };
	inline PluginMap::Feature ControlProbe{ "probe", "Input diagnostics",
		{ "ACrCharacterPlayerBase::bIsDisabledPawn", "ACrCharacterPlayerBase::InteractionState",
		  "ACrCharacterPlayerBase::bDead", "ACrCharacterPlayerBase::bOnLadder",
		  "ACrCharacterPlayerBase::bCharacterSelectedAndInitialized", "ACrCharacterPlayerBase::BuildingDrone",
		  "ACrCharacterPlayerBase::CrPC", "ACrPlayerControllerBase::GameMenuBlockers",
		  "ACrPlayerControllerBase::bReplInPauseMenu", "ACrPlayerControllerBase::PreviousPawn",
		  "ACrPlayerControllerBase::PlayerMainCharacter", "ACrPlayerControllerBase::CurrentInteractableActor",
		  "ACrPlayerControllerBase::CurrentInteractableActorWithActiveInteraction",
		  "ACrPlayerControllerBase::CurrentZipline", "ACrPlayerControllerBase::CurrentBuildingTarget",
		  "UCrHeroComponent::ExclusiveInputBindings", "UCrHeroComponent::DefaultInputMappings",
		  "FInputMappingContextAndPriority::InputMapping", "FInputMappingContextAndPriority::Priority",
		  "FInputMappingContextAndPriority" }, {} };

	// Call before Resolve, so a miss lands on a feature and not on the plugin.
	inline void DeclareFeatures(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		PluginMap::DeclareFeatures(scanner, self,
			FovControl, ViewportMask, VitalsLock, RupturePhase, ControlProbe, CinematicBits);
	}

	// Asks the loader for every row above. False when any did not resolve; the
	// loader has then recorded the failure and refuses the plugin.
	inline bool Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		auto native = [&](PluginFieldSpec& field, const PluginFieldCandidate* rows, size_t count)
		{
			return PluginMap::ResolveField(scanner, self, field, rows, static_cast<uint32_t>(count));
		};

		bool nativeOk = native(Fov, kFovRows, std::size(kFovRows));
		nativeOk = native(AdditionalFovOffset, kAdditionalFovRows, std::size(kAdditionalFovRows)) && nativeOk;
		nativeOk = native(LocalPlayerOrigin, nullptr, 0) && nativeOk;
		nativeOk = native(LocalPlayerSize, nullptr, 0) && nativeOk;
		nativeOk = native(PcCinematicMode, kCinematicModeRows, std::size(kCinematicModeRows)) && nativeOk;
		nativeOk = native(PcCinematicHidePlayer, kCinematicHidePlayerRows, std::size(kCinematicHidePlayerRows)) && nativeOk;

		return PluginMap::ResolveFields(scanner, self,
			ContextualInputBindings, ExclusiveInputBindings, DefaultInputMappings,
			DefaultMappingContext, DefaultMappingPriority,
			HealthSet, CaloriesSet, HydrationSet, OxygenSet, EnergySet, ShieldSet,
			ToxicitySet, RadiationSet, HeatSet, DrainSet, CorrosionSet, InfectionSet,
			HealthCurrent, HealthMax, CaloriesCurrent, CaloriesMax, HydrationCurrent, HydrationMax,
			OxygenCurrent, OxygenMax, EnergyCurrent, EnergyMax, ShieldCurrent, ShieldMax,
			ToxicityCurrent, ToxicityMin, RadiationCurrent, RadiationMin, HeatCurrent, HeatMin,
			DrainCurrent, DrainMin, CorrosionCurrent, CorrosionMin, InfectionCurrent, InfectionMin,
			CharDisabledPawn, CharInteractState, CharDead, CharOnLadder, CharInitialised,
			CharBuildingDrone, CharController,
			PcMenuBlockers, PcInPauseMenu, PcPreviousPawn, PcMainCharacter, PcInteractable,
			PcActiveInteract, PcZipline, PcBuildTarget,
			PcIsWaiting, WaveSpeed, WaveSettingsSize, BindingSize, MappingSize) && nativeOk;
	}

	// The member's value, or a fallback when the object is null or the member
	// did not resolve.
	using PluginMap::ValueOr;
}
