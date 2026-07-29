#include "hud_visibility.h"
#include "plugin_helpers.h"

#include "Engine_classes.hpp"
#include "UMG_classes.hpp"

namespace CameraControls::Hud
{
	namespace
	{
		// --- How the game itself does it -------------------------------------
		//
		// StarRupture binds a "cinematic mode" key (F1 by default) to
		// UCrInputNativeCinematicMode. Its InputPressed does precisely one
		// thing -- flip AHUD::bShowHUD on the local player's HUD actor -- and
		// nothing else. So that flag, and not any widget of ours, is what the
		// game watches to put its interface away.
		//
		// Doing the same thing is both the supported path and the reversible
		// one: the game has to cope with it being toggled at any moment,
		// because its own key does.
		//
		// The CommonUI layout collapse below is kept as a second line of
		// defence. It targets the widget tree directly, so between the two of
		// them anything the game puts on screen is covered whether it is
		// routed through the HUD actor or not. Both are logged, so the log
		// says which one actually did the work.

		constexpr const char* kLayoutClassName = "WBP_OverallUILayout_C";

		// Buffer for the object-walker lookup. 32 is generous: there is one
		// live layout per local player.
		constexpr int kMaxMatches = 32;

		SDK::AHUD* g_hud             = nullptr;
		bool       g_previousShowHud = true;
		bool       g_hudFlagTaken    = false;

		SDK::UUserWidget*     g_layout   = nullptr;
		SDK::ESlateVisibility g_previous = SDK::ESlateVisibility::Visible;

		bool g_hidden = false;

		SDK::UWorld* GetWorld()
		{
			try { return SDK::UWorld::GetWorld(); }
			catch (...) { return nullptr; }
		}

		SDK::AHUD* FindHud()
		{
			try
			{
				SDK::UWorld* world = GetWorld();
				if (!world)
					return nullptr;

				SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
				return pc ? pc->GetHUD() : nullptr;
			}
			catch (...)
			{
				return nullptr;
			}
		}

		SDK::UUserWidget* FindLayoutWidget()
		{
			auto* hooks = GetHooks();
			if (!hooks || !hooks->ObjectWalker || !hooks->ObjectWalker->IsReady())
				return nullptr;

			PluginObjectInfo matches[kMaxMatches] = {};
			const int found = hooks->ObjectWalker->FindObjectsByClassNameInto(
				kLayoutClassName, PluginObjectLookup_InstanceOnly, matches, kMaxMatches);

			if (found <= 0)
			{
				LOG_DEBUG("Hud: no live '%s' instance found -- relying on bShowHUD alone",
				          kLayoutClassName);
				return nullptr;
			}

			// The walk returns instances in GObjects order; the first live one
			// is the local player's layout in a single-viewport client.
			LOG_DEBUG("Hud: found %d '%s' instance(s), using %p",
			          found, kLayoutClassName, matches[0].object);
			return static_cast<SDK::UUserWidget*>(matches[0].object);
		}
	}

	bool IsHidden() { return g_hidden; }

	void ForgetWorldState()
	{
		g_hud          = nullptr;
		g_hudFlagTaken = false;
		g_layout       = nullptr;
		g_hidden       = false;
	}

	bool Hide()
	{
		if (g_hidden)
			return true;

		bool anything = false;

		// --- The game's own switch ---------------------------------------
		try
		{
			g_hud = FindHud();
			if (g_hud)
			{
				g_previousShowHud = g_hud->bShowHUD != 0;
				g_hud->bShowHUD   = 0;
				g_hudFlagTaken    = true;
				anything          = true;

				LOG_DEBUG("Hud: bShowHUD %d -> 0 on %p", g_previousShowHud ? 1 : 0,
				          static_cast<void*>(g_hud));
			}
			else
			{
				LOG_WARN("Hud: no HUD actor on the local controller");
			}
		}
		catch (...)
		{
			LOG_WARN("Hud: clearing bShowHUD threw");
			g_hud          = nullptr;
			g_hudFlagTaken = false;
		}

		// --- Anything the HUD actor does not own --------------------------
		try
		{
			g_layout = FindLayoutWidget();
			if (g_layout)
			{
				g_previous = g_layout->GetVisibility();
				g_layout->SetVisibility(SDK::ESlateVisibility::Collapsed);
				anything = true;

				LOG_DEBUG("Hud: layout visibility %d -> Collapsed",
				          static_cast<int>(g_previous));
			}
		}
		catch (...)
		{
			LOG_WARN("Hud: collapsing the layout widget threw");
			g_layout = nullptr;
		}

		g_hidden = anything;

		if (anything)
			LOG_INFO("Hud: game UI hidden (bShowHUD=%d layout=%d)",
			         g_hudFlagTaken ? 1 : 0, g_layout ? 1 : 0);
		else
			LOG_WARN("Hud: nothing to hide -- the game UI will stay on screen");

		return anything;
	}

	void Show()
	{
		if (!g_hidden)
			return;

		try
		{
			if (g_hud && g_hudFlagTaken)
			{
				// Put back what was there, not a hard "true" -- the player may
				// have had the HUD off with the game's own key before the
				// editor was ever opened, and coming out of a shot should not
				// silently turn it back on.
				g_hud->bShowHUD = g_previousShowHud ? 1 : 0;
			}
		}
		catch (...)
		{
			LOG_WARN("Hud: restoring bShowHUD threw");
		}

		try
		{
			if (g_layout)
			{
				// Restore whatever it was, rather than forcing Visible -- the
				// layout is normally SelfHitTestInvisible and forcing Visible
				// would start eating clicks meant for the world.
				g_layout->SetVisibility(g_previous);
			}
		}
		catch (...)
		{
			LOG_WARN("Hud: restoring the layout widget threw -- the game UI may need a "
			         "reload to come back");
		}

		ForgetWorldState();
		LOG_INFO("Hud: game UI restored");
	}
}
