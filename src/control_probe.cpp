#include "control_probe.h"

#include "camera_rig.h"
#include "death_guard.h"
#include "editor_state.h"
#include "player_safeguard.h"
#include "player_vitals.h"
#include "plugin_helpers.h"
#include "viewport_fit.h"

#include "Chimera_classes.hpp"
#include "EnhancedInput_classes.hpp"
#include "Engine_classes.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

namespace CameraControls::Probe
{
	namespace
	{
		// A snapshot names about fifty values and the whole point is to be able to
		// read them next to each other, so they go out in grouped lines rather than
		// one line per field -- a fifty-line burst per probe would push the diffs
		// off the top of anyone's log viewer.
		//
		// 700 rather than something rounder because the loader's logger formats
		// through two 1024-byte buffers in series, each adding a prefix. Overrun
		// there truncates silently, which for a diagnostic would mean losing
		// exactly the last field on the longest line.
		constexpr int kLineBudget = 700;

		Snapshot g_baseline;    // before the editor touched anything
		Snapshot g_running;     // what the last Step() saw

		// How many times the editor has been opened since the plugin loaded.
		//
		// Only there to make a pasted log unambiguous. A dump taken before the
		// editor has ever run and one taken after it closed are the two halves of
		// the comparison that matters, and they are otherwise near-identical --
		// same mode, no token, nothing engaged. Zero versus one says which is
		// which without having to trust the order they were pasted in.
		int g_editorSessions = 0;

		// Written from whichever thread acquires or releases the token (game thread
		// in practice, render thread on an unload) and read by a capture on the game
		// thread. Plain atomics rather than the state lock: the probe must be
		// callable from inside the tick, which already holds it.
		std::atomic<void*> g_inputToken{ nullptr };
		std::atomic<bool>  g_inputPassthrough{ false };

		// -------------------------------------------------------------------
		// Value formatting
		//
		// Everything becomes a string, so one diff implementation covers pointers,
		// enums, floats and names alike. Values are formatted to be *stable*: a
		// float that wobbles in its last decimal place would show up as a change
		// every tick of the watch and bury the one that matters.
		// -------------------------------------------------------------------
		std::string Str(const char* format, ...)
		{
			char buffer[256] = {};
			va_list args;
			va_start(args, format);
			vsnprintf(buffer, sizeof(buffer), format, args);
			va_end(args);
			return std::string(buffer);
		}

		std::string Bl(bool value) { return value ? "yes" : "no"; }

		std::string Ptr(const void* p)
		{
			return p ? Str("%p", p) : std::string("null");
		}

		// Objects are reported as class'name'(address). All three matter: the
		// address says whether it is the *same* object as last time, the class says
		// whether the game swapped it for a different kind of thing, and the name
		// is what makes a log line readable at all.
		std::string Obj(SDK::UObject* object)
		{
			if (!object)
				return "null";

			try
			{
				const std::string cls  = object->Class ? object->Class->GetName() : std::string("?");
				const std::string name = object->GetName();
				return Str("%s'%s'(%p)", cls.c_str(), name.c_str(), object);
			}
			catch (...)
			{
				return Str("unreadable(%p)", object);
			}
		}

		std::string Vec(const SDK::FVector& v)
		{
			return Str("%.0f,%.0f,%.0f", v.X, v.Y, v.Z);
		}

		std::string Rotator(const SDK::FRotator& r)
		{
			return Str("p%.1f y%.1f r%.1f", r.Pitch, r.Yaw, r.Roll);
		}

		const char* MovementModeName(SDK::EMovementMode mode)
		{
			switch (mode)
			{
				case SDK::EMovementMode::MOVE_None:      return "None";
				case SDK::EMovementMode::MOVE_Walking:   return "Walking";
				case SDK::EMovementMode::MOVE_NavWalking:return "NavWalking";
				case SDK::EMovementMode::MOVE_Falling:   return "Falling";
				case SDK::EMovementMode::MOVE_Swimming:  return "Swimming";
				case SDK::EMovementMode::MOVE_Flying:    return "Flying";
				case SDK::EMovementMode::MOVE_Custom:    return "Custom";
				default:                                 return "?";
			}
		}

		const char* ModeName(Mode mode)
		{
			switch (mode)
			{
				case Mode::Off:      return "Off";
				case Mode::Editor:   return "Editor";
				case Mode::Playback: return "Playback";
				default:             return "?";
			}
		}

		void Add(Snapshot& snapshot, const char* group, const char* name, std::string value)
		{
			snapshot.fields.push_back(Field{ group, name, std::move(value) });
		}

		const std::string* Find(const Snapshot& snapshot, const char* group, const char* name)
		{
			for (const Field& field : snapshot.fields)
			{
				// Pointer comparison first: both sides come from the same literals
				// in the same translation unit, so it almost always settles it.
				if ((field.group == group || std::strcmp(field.group, group) == 0) &&
				    (field.name == name || std::strcmp(field.name, name) == 0))
					return &field.value;
			}
			return nullptr;
		}

		SDK::UWorld* GetWorld()
		{
			try { return SDK::UWorld::GetWorld(); }
			catch (...) { return nullptr; }
		}

		// -------------------------------------------------------------------
		// Capture, one group at a time
		// -------------------------------------------------------------------

		// What the plugin believes about itself. First group on purpose: if this
		// disagrees with the engine groups below, the bug is ours and there is no
		// need to read any further.
		void CapturePlugin(const State& state, Snapshot& snapshot)
		{
			void* token = g_inputToken.load(std::memory_order_relaxed);

			Add(snapshot, "plugin", "mode",         ModeName(state.mode));
			Add(snapshot, "plugin", "editorRuns",   Str("%d", g_editorSessions));
			Add(snapshot, "plugin", "inputToken",   Ptr(token));
			Add(snapshot, "plugin", "tokenKind",
			    token ? (g_inputPassthrough.load(std::memory_order_relaxed)
			                 ? "passthrough" : "exclusive")
			          : "none");
			Add(snapshot, "plugin", "rigActive",    Bl(Rig::IsActive()));
			Add(snapshot, "plugin", "safeguard",    Bl(Safeguard::IsEngaged()));
			Add(snapshot, "plugin", "vitalsPinned", Bl(Vitals::IsActive()));
			Add(snapshot, "plugin", "guardBounds",  Bl(DeathGuard::BoundsSuppressed()));
			Add(snapshot, "plugin", "guardDamage",  Bl(DeathGuard::DamageBlocked()));
			Add(snapshot, "plugin", "guardCheat",   Bl(DeathGuard::GameImmortal()));
			Add(snapshot, "plugin", "viewportFit",  Bl(ViewportFit::IsActive()));
		}

		void CaptureWorld(SDK::UWorld* world, Snapshot& snapshot)
		{
			if (!world)
			{
				Add(snapshot, "world", "name", "none");
				return;
			}

			try
			{
				Add(snapshot, "world", "name", world->GetName());
			}
			catch (...)
			{
				Add(snapshot, "world", "name", "unreadable");
			}

			// A paused world stops the character responding to input while leaving
			// every flag we restore looking perfectly correct, so it is worth one
			// line even though nothing here pauses anything.
			try
			{
				Add(snapshot, "world", "paused",
				    Bl(SDK::UGameplayStatics::IsGamePaused(world)));
			}
			catch (...)
			{
				Add(snapshot, "world", "paused", "unreadable");
			}
		}

		void CaptureControllerBits(SDK::APlayerController* pc, Snapshot& snapshot);

		// The controller is where UE actually decides whether input reaches the
		// pawn, so this is the group most likely to hold the answer.
		void CaptureController(SDK::APlayerController* pc, Snapshot& snapshot)
		{
			if (!pc)
			{
				Add(snapshot, "pc", "ptr", "null");
				return;
			}

			Add(snapshot, "pc", "ptr", Obj(pc));

			// The two that decide it outright. UE's own UI modes, CommonUI's input
			// config and the game's cutscene code all work by incrementing these
			// counters, and every one of them is invisible in any other field.
			try { Add(snapshot, "pc", "moveIgnored", Bl(pc->IsMoveInputIgnored())); }
			catch (...) { Add(snapshot, "pc", "moveIgnored", "unreadable"); }

			try { Add(snapshot, "pc", "lookIgnored", Bl(pc->IsLookInputIgnored())); }
			catch (...) { Add(snapshot, "pc", "lookIgnored", "unreadable"); }

			try { Add(snapshot, "pc", "state", pc->StateName.ToString()); }
			catch (...) { Add(snapshot, "pc", "state", "unreadable"); }

			Add(snapshot, "pc", "pawn",     Obj(pc->Pawn));
			Add(snapshot, "pc", "ackPawn",  Obj(pc->AcknowledgedPawn));
			Add(snapshot, "pc", "character",Obj(pc->Character));

			// The view target is ours while the editor is open and has to be the
			// pawn again afterwards. Read from the controller rather than from our
			// own record of what we set, which is the whole point.
			try { Add(snapshot, "pc", "viewTarget", Obj(pc->GetViewTarget())); }
			catch (...) { Add(snapshot, "pc", "viewTarget", "unreadable"); }

			Add(snapshot, "pc", "playerInput",  Ptr(pc->PlayerInput));
			Add(snapshot, "pc", "cheatManager", Ptr(pc->CheatManager));
			Add(snapshot, "pc", "camManager",   Ptr(pc->PlayerCameraManager));
			Add(snapshot, "pc", "player",       Ptr(pc->Player));

			Add(snapshot, "pc", "showCursor",   Bl(pc->bShowMouseCursor != 0));
			Add(snapshot, "pc", "clickEvents",  Bl(pc->bEnableClickEvents != 0));
			Add(snapshot, "pc", "autoCamera",   Bl(pc->bAutoManageActiveCameraTarget));
			Add(snapshot, "pc", "isWaiting",    Bl(pc->bPlayerIsWaiting != 0));
			Add(snapshot, "pc", "streamSource", Bl(pc->bEnableStreamingSource != 0));

			try { Add(snapshot, "pc", "controlRot", Rotator(pc->GetControlRotation())); }
			catch (...) { Add(snapshot, "pc", "controlRot", "unreadable"); }

			// The *class* of the input object, not just its address. This is what
			// says whether the game is on Enhanced Input (UEnhancedPlayerInput) or
			// legacy UPlayerInput, and therefore which of the two pipelines a
			// missing keypress could be getting lost in. Cheap, and it was the
			// obvious omission from the first version of this probe: everything
			// checked was on the legacy path.
			try
			{
				Add(snapshot, "pc", "inputClass",
				    pc->PlayerInput && pc->PlayerInput->Class
				        ? pc->PlayerInput->Class->GetName() : std::string("null"));
			}
			catch (...) { Add(snapshot, "pc", "inputClass", "unreadable"); }

			// AHUD::bShowHUD, which is what the game's own F1 cinematic mode
			// toggles and what hud_visibility restores. Read from the HUD actor
			// rather than from our record of what we set it to.
			try
			{
				Add(snapshot, "pc", "showHUD",
				    pc->MyHUD ? Bl(pc->MyHUD->bShowHUD) : std::string("no HUD"));
			}
			catch (...) { Add(snapshot, "pc", "showHUD", "unreadable"); }

			CaptureControllerBits(pc, snapshot);
		}

		// -------------------------------------------------------------------
		// Cinematic mode -- the second module to read a UObject by raw offset
		//
		// `APlayerController::bCinematicMode` is not a UPROPERTY, so the generated
		// header only knows its byte as `BitPad_4D0_0 : 4` sitting below the
		// exposed `bPlayerIsWaiting : 1` at bit 4. The bit assignments come from
		// the dump rather than from UE's declaration order:
		// `APlayerController::SetCinematicMode` (0x144FB5E40) reads `[this+4D0h]`,
		// does `and edx, ~2` then `or edx, bInCinematicMode*2`, and separately sets
		// `4` when `bInCinematicMode && bHidePlayer`. So bit 1 is bCinematicMode
		// and bit 2 is its hide-the-pawn flag, for this build, measured.
		//
		// Bits 0 and 3 stay unnamed. Nothing in the dump pinned them, and labelling
		// a bit with a guess is how a diagnostic starts lying -- "bit3 no -> yes"
		// in a diff is enough to go and find out which one it is.
		//
		// The read validates itself before it reports: bit 4 of the byte must agree
		// with the generated `bPlayerIsWaiting` accessor, which sits in the same
		// byte. If they disagree the offset is wrong on this build and we say so
		// rather than printing plausible-looking rubbish.
		//
		// Worth knowing but NOT the smoking gun for the current bug: UE routes
		// cinematic mode's input suppression through SetIgnoreMoveInput /
		// SetIgnoreLookInput, so `pc.moveIgnored` and `pc.lookIgnored` already
		// cover that path -- and they read clean. These bits are here to make that
		// reasoning checkable rather than assumed.
		// -------------------------------------------------------------------
		void CaptureControllerBits(SDK::APlayerController* pc, Snapshot& snapshot)
		{
			constexpr int kBitsOffset      = 0x4D0;
			constexpr int kWaitingBitIndex = 4;

			try
			{
				const uint8_t byte =
					*(reinterpret_cast<const uint8_t*>(pc) + kBitsOffset);

				const bool rawWaiting    = (byte & (1u << kWaitingBitIndex)) != 0;
				const bool headerWaiting = pc->bPlayerIsWaiting != 0;

				if (rawWaiting != headerWaiting)
				{
					// The one check that makes this trustworthy, and it failed.
					Add(snapshot, "pcbits", "offset",
					    Str("0x%X REJECTED (bit%d=%d but bPlayerIsWaiting=%d)",
					        kBitsOffset, kWaitingBitIndex, rawWaiting ? 1 : 0,
					        headerWaiting ? 1 : 0));
					return;
				}

				Add(snapshot, "pcbits", "raw",           Str("0x%02X", byte));
				Add(snapshot, "pcbits", "cinematicMode", Bl((byte & 0x02) != 0));
				Add(snapshot, "pcbits", "cinemaHidePlayer", Bl((byte & 0x04) != 0));
				Add(snapshot, "pcbits", "bit0",          Bl((byte & 0x01) != 0));
				Add(snapshot, "pcbits", "bit3",          Bl((byte & 0x08) != 0));
			}
			catch (...)
			{
				Add(snapshot, "pcbits", "raw", "unreadable");
			}
		}

		// StarRupture's own controller state. `GameMenuBlockers` is the one to
		// watch: the game gates menu and gameplay input on that list being empty,
		// and anything that pushed onto it and did not pop is indistinguishable
		// from our own bug right up until you can see the count.
		void CaptureGameController(SDK::APlayerController* pc, Snapshot& snapshot)
		{
			if (!pc)
				return;

			try
			{
				if (!pc->IsA(SDK::ACrPlayerControllerBase::StaticClass()))
				{
					Add(snapshot, "crpc", "ptr", "not a CrPlayerControllerBase");
					return;
				}

				auto* crpc = static_cast<SDK::ACrPlayerControllerBase*>(pc);

				Add(snapshot, "crpc", "menuBlockers",  Str("%d", crpc->GameMenuBlockers.Num()));
				Add(snapshot, "crpc", "inPauseMenu",   Bl(crpc->bReplInPauseMenu));
				Add(snapshot, "crpc", "previousPawn",  Ptr(crpc->PreviousPawn));
				Add(snapshot, "crpc", "mainCharacter", Ptr(crpc->PlayerMainCharacter));
				Add(snapshot, "crpc", "interactable",  Obj(crpc->CurrentInteractableActor));
				Add(snapshot, "crpc", "activeInteract",Ptr(crpc->CurrentInteractableActorWithActiveInteraction));
				Add(snapshot, "crpc", "zipline",       Ptr(crpc->CurrentZipline));
				Add(snapshot, "crpc", "buildTarget",   Ptr(crpc->CurrentBuildingTarget));
			}
			catch (...)
			{
				Add(snapshot, "crpc", "ptr", "unreadable");
			}
		}

		void CapturePawn(SDK::APawn* pawn, Snapshot& snapshot)
		{
			if (!pawn)
			{
				Add(snapshot, "pawn", "ptr", "null");
				return;
			}

			Add(snapshot, "pawn", "ptr", Obj(pawn));

			try
			{
				Add(snapshot, "pawn", "controller", Ptr(pawn->Controller));

				// A pawn with no input component receives nothing, whatever the
				// controller thinks. Cheap to check and one of the few ways a
				// character can go inert with every other flag looking right.
				Add(snapshot, "pawn", "inputComp", Ptr(pawn->InputComponent));

				Add(snapshot, "pawn", "location",   Vec(pawn->K2_GetActorLocation()));
				Add(snapshot, "pawn", "rotation",   Rotator(pawn->K2_GetActorRotation()));
				Add(snapshot, "pawn", "velocity",   Vec(pawn->GetVelocity()));
				Add(snapshot, "pawn", "hidden",     Bl(pawn->bHidden != 0));
				Add(snapshot, "pawn", "collision",  Bl(pawn->bActorEnableCollision != 0));
				Add(snapshot, "pawn", "canDamage",  Bl(pawn->bCanBeDamaged != 0));
				Add(snapshot, "pawn", "tickEnabled",Bl(pawn->IsActorTickEnabled()));
				Add(snapshot, "pawn", "ctrlRotYaw", Bl(pawn->bUseControllerRotationYaw != 0));

				// MEASURED AND FOUND USELESS -- kept only so the next person does
				// not add them again expecting what they promise.
				//
				// The theory was that these split the problem: UE accumulates
				// movement input into the pawn and clears it when the movement
				// component consumes it, so zero with a key held should have meant
				// "input never arrived". It does not. Both read 0,0,0 with W held
				// while walking worked perfectly, because this probe samples from
				// the engine tick, which runs after the input has already been
				// consumed for the frame. Same for `velocity`.
				//
				// So they cannot tell "input never arrived" from "input arrived and
				// was consumed normally", which is the only distinction anyone
				// would read them for. Anything concluded from a zero here is
				// unsupported.
				Add(snapshot, "pawn", "pendingInput", Vec(pawn->GetPendingMovementInputVector()));
				Add(snapshot, "pawn", "lastInput",    Vec(pawn->GetLastMovementInputVector()));
			}
			catch (...)
			{
				Add(snapshot, "pawn", "readError", "yes");
			}
		}

		// -------------------------------------------------------------------
		// Enhanced Input
		//
		// What a working/broken pair actually showed, so none of it gets chased
		// again:
		//
		//   keysThisTick        0 in both. Transient and cleared before our tick
		//                       samples it, exactly as feared. No signal.
		//   appliedContexts     0 in both, *including while walking worked*. This
		//                       game does not leave anything in that map, so a zero
		//                       here means nothing at all.
		//   actionMappings      73 at editor-open, 11 in both later dumps -- so it
		//                       changes on its own and 11 is a perfectly working
		//                       value. Not a signal either.
		//
		// The group is kept because the counts are cheap and a *future* difference
		// here would still be meaningful. But nothing in it discriminated between
		// control working and control not working, and the whole engine-side half of
		// this probe found the two states byte-identical.
		//
		// That null result is the useful part: if nothing gameplay-side differs,
		// nothing gameplay-side is broken, and the fault is in how the *host* routes
		// input into the engine -- Slate focus and viewport capture, which no field
		// reachable from the generated SDK exposes. See ExitEditor's
		// "restore game input mode" step.
		// -------------------------------------------------------------------
		void CaptureEnhancedInput(SDK::APlayerController* pc, Snapshot& snapshot)
		{
			if (!pc || !pc->PlayerInput)
			{
				Add(snapshot, "ei", "input", "null");
				return;
			}

			try
			{
				if (!pc->PlayerInput->IsA(SDK::UEnhancedPlayerInput::StaticClass()))
				{
					Add(snapshot, "ei", "input", "not UEnhancedPlayerInput");
					return;
				}

				auto* input = static_cast<SDK::UEnhancedPlayerInput*>(pc->PlayerInput);

				// Counts first: they are stable, cheap, and a zero in the wrong
				// place is the whole answer.
				const int contexts = input->AppliedInputContexts.Num();
				Add(snapshot, "ei", "appliedContexts", Str("%d", contexts));
				Add(snapshot, "ei", "actionMappings",  Str("%d", input->EnhancedActionMappings.Num()));
				Add(snapshot, "ei", "actionInstances", Str("%d", input->ActionInstanceData.Num()));
				Add(snapshot, "ei", "injectedThisTick",Str("%d", input->InputsInjectedThisTick.Num()));

				// Which contexts, by name. "How many" says whether input can map to
				// anything at all; "which" says whether the *gameplay* one is the
				// one that went missing, and those are different bugs.
				std::string names;
				for (int i = 0; i < input->AppliedInputContexts.Num() && names.size() < 220; ++i)
				{
					if (!input->AppliedInputContexts.IsValidIndex(i))
						continue;

					const auto& pair = input->AppliedInputContexts[i];
					SDK::UInputMappingContext* context = pair.Key();
					if (!context)
						continue;

					if (!names.empty()) names += ",";
					names += context->GetName();
					names += Str("(p%d)", pair.Value());
				}
				Add(snapshot, "ei", "contexts", names.empty() ? "none" : names);

				// The decisive one. Sparse map, so IsValidIndex before every read.
				const int pressed = input->KeysPressedThisTick.Num();
				Add(snapshot, "ei", "keysThisTick", Str("%d", pressed));

				std::string keys;
				for (int i = 0; i < pressed && keys.size() < 180; ++i)
				{
					if (!input->KeysPressedThisTick.IsValidIndex(i))
						continue;

					if (!keys.empty()) keys += ",";
					keys += input->KeysPressedThisTick[i].Key().KeyName.ToString();
				}
				Add(snapshot, "ei", "keyNames", keys.empty() ? "none" : keys);
			}
			catch (...)
			{
				Add(snapshot, "ei", "input", "unreadable");
			}
		}

		// Logs every live action mapping as Action/Key, in batches so no line
		// overruns the logger's buffer.
		//
		// Only ever called on a *change* in the mapping count, never per tick: with
		// 73 mappings this is 73 name lookups and several log lines, which is fine
		// once per transition and completely unacceptable at 60Hz.
		void DumpActionMappings(const State& state)
		{
			try
			{
				SDK::UWorld* world = GetWorld();
				if (!world)
					return;

				SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
				if (!pc || !pc->PlayerInput ||
				    !pc->PlayerInput->IsA(SDK::UEnhancedPlayerInput::StaticClass()))
					return;

				auto* input = static_cast<SDK::UEnhancedPlayerInput*>(pc->PlayerInput);
				const auto& mappings = input->EnhancedActionMappings;
				const int count = mappings.Num();

				char line[512];
				int  used  = 0;
				int  batch = 0;

				auto flush = [&]()
				{
					if (used <= 0) return;
					LOG_TRACE("InputMappings: live set [%d] %s", batch, line);
					++batch;
					used    = 0;
					line[0] = '\0';
				};

				for (int i = 0; i < count; ++i)
				{
					std::string action = "?";
					std::string key    = "?";

					try
					{
						const auto& mapping = mappings[i];
						if (mapping.Action)
							action = mapping.Action->GetName();
						key = mapping.Key.KeyName.ToString();
					}
					catch (...)
					{
						action = "unreadable";
					}

					const int remaining = static_cast<int>(sizeof(line)) - used;
					const int written = snprintf(line + used, remaining, "%s%s/%s",
					                             used > 0 ? " " : "",
					                             action.c_str(), key.c_str());

					if (written < 0 || written >= remaining)
					{
						line[used] = '\0';
						flush();
						used = snprintf(line, sizeof(line), "%s/%s",
						                action.c_str(), key.c_str());
						continue;
					}

					used += written;
				}

				flush();

				if (count == 0)
					LOG_TRACE("InputMappings: live set is EMPTY (mode %s)",
					          state.mode == Mode::Off ? "Off" : "active");

				// Duplicate Action/Key pairs, called out explicitly.
				//
				// Re-adding a mapping context that is already applied can leave the
				// same action bound to the same key twice, and a doubled `LookMouse`
				// on `Mouse2D` means the frame's mouse delta is consumed twice --
				// which the player experiences as mouse sensitivity being wrong after
				// the input set is repaired, with nothing else obviously broken.
				//
				// O(n^2) over ~73 entries, on a path that only runs when the count
				// changes. Cheap enough, and it answers a question that is otherwise
				// guesswork.
				int duplicates = 0;
				char dupList[256] = {};
				int  dupUsed = 0;

				for (int i = 0; i < count; ++i)
				{
					for (int j = i + 1; j < count; ++j)
					{
						try
						{
							const auto& a = mappings[i];
							const auto& b = mappings[j];

							if (a.Action != b.Action ||
							    a.Key.KeyName != b.Key.KeyName)
								continue;

							++duplicates;

							const int remaining = static_cast<int>(sizeof(dupList)) - dupUsed;
							if (remaining <= 1)
								continue;

							const std::string name =
								a.Action ? a.Action->GetName() : std::string("?");
							const int written = snprintf(dupList + dupUsed, remaining, "%s%s/%s",
							                             dupUsed > 0 ? " " : "",
							                             name.c_str(),
							                             a.Key.KeyName.ToString().c_str());
							if (written > 0 && written < remaining)
								dupUsed += written;
						}
						catch (...) {}
					}
				}

				if (duplicates > 0)
					LOG_WARN("InputMappings: %d DUPLICATE action/key pair(s) -- %s. A doubled "
					         "look or move binding applies its input twice.",
					         duplicates, dupList);
			}
			catch (...)
			{
				LOG_TRACE("InputMappings: could not enumerate the live set");
			}
		}

		// Does the game window actually have OS keyboard focus, and who owns the
		// cursor? Not engine state at all, which is exactly why it is worth having:
		// if the loader's UI left focus or the cursor clip somewhere else, UE
		// receives nothing and every gameplay flag stays perfectly healthy. This
		// probe would otherwise report a fully controllable character and be right
		// about all of it.
		void CaptureOsInput(Snapshot& snapshot)
		{
			const HWND foreground = ::GetForegroundWindow();
			const HWND focus      = ::GetFocus();

			DWORD foregroundPid = 0;
			if (foreground)
				::GetWindowThreadProcessId(foreground, &foregroundPid);

			Add(snapshot, "os", "weAreForeground",
			    Bl(foregroundPid != 0 && foregroundPid == ::GetCurrentProcessId()));
			Add(snapshot, "os", "focusWnd", Ptr(focus));

			RECT clip{};
			if (::GetClipCursor(&clip))
				Add(snapshot, "os", "cursorClip",
				    Str("%ld,%ld %ldx%ld", clip.left, clip.top,
				        clip.right - clip.left, clip.bottom - clip.top));
			else
				Add(snapshot, "os", "cursorClip", "unreadable");

			// Negative means hidden. UE hides the cursor for gameplay and shows it
			// for menus, so this is a cheap read on which of the two the game
			// believes it is in, independent of anything it told us.
			CURSORINFO info{};
			info.cbSize = sizeof(info);
			if (::GetCursorInfo(&info))
				Add(snapshot, "os", "cursorShowing", Bl(info.flags != 0));
			else
				Add(snapshot, "os", "cursorShowing", "unreadable");
		}

		void CaptureMovement(SDK::APawn* pawn, Snapshot& snapshot)
		{
			SDK::UCharacterMovementComponent* movement = nullptr;

			try
			{
				if (pawn && pawn->IsA(SDK::ACharacter::StaticClass()))
					movement = static_cast<SDK::ACharacter*>(pawn)->CharacterMovement;
			}
			catch (...) {}

			if (!movement)
			{
				Add(snapshot, "move", "ptr", "null");
				return;
			}

			Add(snapshot, "move", "ptr", Ptr(movement));

			try
			{
				// The mode is the field this plugin is most likely to have broken:
				// the safeguard parks the body in MOVE_None and a body left there
				// is exactly "my character will not respond".
				Add(snapshot, "move", "mode",       MovementModeName(movement->MovementMode));
				Add(snapshot, "move", "customMode", Str("%d", static_cast<int>(movement->CustomMovementMode)));
				Add(snapshot, "move", "gravity",    Str("%.2f", movement->GravityScale));
				Add(snapshot, "move", "velocity",   Vec(movement->Velocity));
				Add(snapshot, "move", "maxWalk",    Str("%.0f", movement->MaxWalkSpeed));
				Add(snapshot, "move", "landMode",   MovementModeName(movement->DefaultLandMovementMode));
				Add(snapshot, "move", "groundMode", MovementModeName(movement->GroundMovementMode));
				Add(snapshot, "move", "updatedComp",Ptr(movement->UpdatedComponent));
				Add(snapshot, "move", "deferUpdate",Bl(movement->bDeferUpdateMoveComponent != 0));
				Add(snapshot, "move", "noCtrlPhys", Bl(movement->bRunPhysicsWithNoController != 0));
			}
			catch (...)
			{
				Add(snapshot, "move", "readError", "yes");
			}
		}

		// The game's own idea of what the character is currently doing.
		// `bIsDisabledPawn` and `InteractionState` are the two that would explain
		// the symptom perfectly, and neither is something the plugin sets -- which
		// is exactly why they are worth watching.
		void CaptureGameCharacter(SDK::APawn* pawn, Snapshot& snapshot)
		{
			if (!pawn)
				return;

			try
			{
				if (!pawn->IsA(SDK::ACrCharacterPlayerBase::StaticClass()))
				{
					Add(snapshot, "crchar", "ptr", "not a CrCharacterPlayerBase");
					return;
				}

				auto* character = static_cast<SDK::ACrCharacterPlayerBase*>(pawn);

				Add(snapshot, "crchar", "disabledPawn", Bl(character->bIsDisabledPawn));
				Add(snapshot, "crchar", "interactState",
				    Str("%d", static_cast<int>(character->InteractionState)));
				Add(snapshot, "crchar", "dead",         Bl(character->bDead));
				Add(snapshot, "crchar", "onLadder",     Bl(character->bOnLadder));
				Add(snapshot, "crchar", "initialised",  Bl(character->bCharacterSelectedAndInitialized));
				Add(snapshot, "crchar", "buildingDrone",Ptr(character->BuildingDrone));
				Add(snapshot, "crchar", "crpc",         Ptr(character->CrPC));
			}
			catch (...)
			{
				Add(snapshot, "crchar", "ptr", "unreadable");
			}
		}

		// -------------------------------------------------------------------
		// Emitting
		// -------------------------------------------------------------------

		// Groups the fields into a handful of lines. Fields arrive grouped, so
		// this only has to break on the group changing and on the line filling up.
		void Emit(const char* label, const Snapshot& snapshot)
		{
			char line[kLineBudget];
			int  used  = 0;
			const char* currentGroup = nullptr;

			auto flush = [&]()
			{
				if (used > 0)
					LOG_TRACE("Probe[%s] %s", label, line);
				used    = 0;
				line[0] = '\0';
			};

			for (const Field& field : snapshot.fields)
			{
				const bool newGroup = currentGroup == nullptr ||
				                      std::strcmp(currentGroup, field.group) != 0;

				// One line per group, split again if a group runs long. Keeping a
				// group together is what makes the log readable: every value that
				// describes the pawn sits on the pawn's line.
				if (newGroup)
				{
					flush();
					currentGroup = field.group;
					used = snprintf(line, sizeof(line), "%s:", field.group);
				}

				const int remaining = static_cast<int>(sizeof(line)) - used;
				const int written   = snprintf(line + used, remaining, " %s=%s",
				                               field.name, field.value.c_str());

				if (written < 0 || written >= remaining)
				{
					// Did not fit -- start a continuation line for the same group
					// rather than truncating a value.
					line[used] = '\0';
					flush();
					used = snprintf(line, sizeof(line), "%s(cont):", field.group);
					used += snprintf(line + used, sizeof(line) - used, " %s=%s",
					                 field.name, field.value.c_str());
					continue;
				}

				used += written;
			}

			flush();
		}
	}

	void NoteInputToken(void* token, bool passthrough)
	{
		g_inputToken.store(token, std::memory_order_relaxed);
		g_inputPassthrough.store(passthrough, std::memory_order_relaxed);
	}

	int ActionMappingCount()
	{
		try
		{
			SDK::UWorld* world = GetWorld();
			if (!world)
				return -1;

			SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
			if (!pc || !pc->PlayerInput ||
			    !pc->PlayerInput->IsA(SDK::UEnhancedPlayerInput::StaticClass()))
				return -1;

			return static_cast<SDK::UEnhancedPlayerInput*>(pc->PlayerInput)
			           ->EnhancedActionMappings.Num();
		}
		catch (...)
		{
			return -1;
		}
	}

	void Forget()
	{
		g_baseline = Snapshot{};
		g_running  = Snapshot{};

		// g_editorSessions deliberately survives. It counts what happened this
		// process, and a world reload is exactly the sort of thing worth still
		// being able to see the editor ran before.
	}

	void Capture(const State& state, Snapshot& out)
	{
		out.fields.clear();
		out.fields.reserve(64);
		out.valid = false;

		CapturePlugin(state, out);

		SDK::UWorld* world = GetWorld();
		CaptureWorld(world, out);

		SDK::APlayerController* pc = nullptr;
		try
		{
			if (world)
				pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
		}
		catch (...) {}

		CaptureController(pc, out);
		CaptureEnhancedInput(pc, out);
		CaptureGameController(pc, out);

		SDK::APawn* pawn = nullptr;
		try { pawn = pc ? pc->Pawn : nullptr; }
		catch (...) {}

		CapturePawn(pawn, out);
		CaptureMovement(pawn, out);
		CaptureGameCharacter(pawn, out);
		CaptureOsInput(out);

		out.valid = world != nullptr;
	}

	void Log(const char* label, const Snapshot& snapshot)
	{
		if (snapshot.fields.empty())
		{
			LOG_TRACE("Probe[%s] nothing captured", label);
			return;
		}

		Emit(label, snapshot);
	}

	bool LogDiff(const char* label, const Snapshot& before, const Snapshot& after)
	{
		Snapshot changes;

		for (const Field& field : after.fields)
		{
			const std::string* previous = Find(before, field.group, field.name);

			if (!previous)
			{
				// A field that only exists on one side is itself the news: the
				// pawn changed class, the controller went away, the character
				// stopped being a CrCharacterPlayerBase.
				Add(changes, field.group, field.name, Str("(new) %s", field.value.c_str()));
				continue;
			}

			if (*previous != field.value)
				Add(changes, field.group, field.name,
				    Str("%s -> %s", previous->c_str(), field.value.c_str()));
		}

		for (const Field& field : before.fields)
		{
			if (!Find(after, field.group, field.name))
				Add(changes, field.group, field.name, Str("%s -> (gone)", field.value.c_str()));
		}

		if (changes.fields.empty())
		{
			LOG_TRACE("Probe[%s] no observable change", label);
			return false;
		}

		Emit(label, changes);
		return true;
	}

	void CaptureBaseline(const State& state)
	{
		// Counted before the snapshot, so the baseline itself reads as run 1 rather
		// than as the pre-editor state it is about to stop being.
		++g_editorSessions;

		Capture(state, g_baseline);
		g_running = g_baseline;

		LOG_TRACE("Probe: baseline before the editor touches anything");
		Log("baseline", g_baseline);
	}

	void Step(const char* what, const State& state)
	{
		Snapshot now;
		Capture(state, now);

		char label[64] = {};
		snprintf(label, sizeof(label), "step %s", what);

		LogDiff(label, g_running, now);
		g_running = std::move(now);
	}

	void FinishExit(const State& state)
	{
		Snapshot now;
		Capture(state, now);

		LOG_TRACE("Probe: teardown finished -- full state, then what it cost against the baseline");
		Log("exited", now);

		// The diff that answers the question. Anything still different from the
		// baseline is either something we changed and did not put back, or
		// something the game changed while we had the camera -- and either way it
		// is the shortlist of suspects, arrived at by elimination rather than by
		// guessing which subsystem to blame.
		if (g_baseline.fields.empty())
			LOG_TRACE("Probe[exited-vs-baseline] no baseline was captured");
		else
			LogDiff("exited-vs-baseline", g_baseline, now);

		g_running = std::move(now);
	}

	void WatchInputMappings(const State& state)
	{
		// -1 so the first tick after load reports the starting values rather than
		// silently adopting them: "what it was before anything happened" is the
		// number every later change has to be read against.
		static int s_mappings = -1;
		static int s_instances = -1;

		int mappings  = -1;
		int instances = -1;

		try
		{
			SDK::UWorld* world = GetWorld();
			if (!world)
				return;

			SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
			if (!pc || !pc->PlayerInput ||
			    !pc->PlayerInput->IsA(SDK::UEnhancedPlayerInput::StaticClass()))
				return;

			auto* input = static_cast<SDK::UEnhancedPlayerInput*>(pc->PlayerInput);
			mappings  = input->EnhancedActionMappings.Num();
			instances = input->ActionInstanceData.Num();
		}
		catch (...)
		{
			return;   // per-tick path: say nothing, try again next tick
		}

		if (mappings == s_mappings && instances == s_instances)
			return;

		const int wasMappings  = s_mappings;
		const int wasInstances = s_instances;
		s_mappings  = mappings;
		s_instances = instances;

		if (wasMappings < 0)
		{
			LOG_TRACE("InputMappings: starting values -- mappings=%d instances=%d",
			          mappings, instances);
			return;
		}

		// A drop is the interesting direction and is called out as such, because a
		// large one is the whole bug: fewer mappings means keys that used to reach
		// an action no longer map to anything.
		const char* direction = mappings < wasMappings ? "DROPPED" : "changed";

		LOG_TRACE("InputMappings: %s -- mappings %d -> %d, instances %d -> %d (mode %s)",
		          direction, wasMappings, mappings, wasInstances, instances,
		          state.mode == Mode::Off      ? "Off"
		        : state.mode == Mode::Editor   ? "Editor"
		                                       : "Playback");

		// Then name what survived, which is the question the counts cannot answer.
		//
		// The game builds its input from `UCrHeroComponent`'s *exclusive* and
		// *contextual* configs, keyed by gameplay tag -- the tags in this build are
		// Base, BasePlayerAlive, Building, Deconstruct, Drone, Incapacitated,
		// OnlyMovement, QuickUse and Zipline (from the binary). An exclusive config
		// replaces the whole mapping set rather than adding to it, so binding one is
		// exactly how 73 mappings become 11.
		//
		// `BindExclusiveMapping` is reachable only from its exec thunk, i.e. it is
		// called from Blueprint, which is why no native caller exists to find. So
		// the surviving mappings have to be identified by what they *are*: a set of
		// drone-movement actions names the Drone config, a menu set names another,
		// and the tag it belongs to says what the game thinks is going on.
		DumpActionMappings(state);
	}

	void DumpNow(const State& state)
	{
		Snapshot now;
		Capture(state, now);

		// Numbered so two dumps can be talked about without ambiguity, which
		// matters because the intended use is a pair: one with a movement key held
		// and one without.
		static int s_dumpNumber = 0;
		++s_dumpNumber;

		char label[32] = {};
		snprintf(label, sizeof(label), "dump %d", s_dumpNumber);

		LOG_TRACE("Probe: on-demand dump %d requested", s_dumpNumber);
		Log(label, now);

		// The diff against whatever was last seen, which for a second keypress is
		// the first dump. Everything a held key should have moved -- the pawn's
		// pending input above all -- shows up here as a one-line answer instead of
		// two full snapshots to compare by eye.
		if (!g_running.fields.empty())
		{
			char diffLabel[48] = {};
			snprintf(diffLabel, sizeof(diffLabel), "dump %d vs previous", s_dumpNumber);
			LogDiff(diffLabel, g_running, now);
		}

		if (!g_baseline.fields.empty())
		{
			char baseLabel[48] = {};
			snprintf(baseLabel, sizeof(baseLabel), "dump %d vs baseline", s_dumpNumber);
			LogDiff(baseLabel, g_baseline, now);
		}

		g_running = std::move(now);
	}

}
