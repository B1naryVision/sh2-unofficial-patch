#pragma once
#include "../../core/hotkey.h"

// In-game Auto-Market threshold editor: a dismissible panel drawn over the game
// via the D3D EndScene hook. Reads/writes the live thresholds through the
// autoMarket accessors. See docs/features/auto-market.md (overlay section).

// Reads the toggle hotkey and, if enabled, registers the D3D render callback.
// Returns true if the editor is enabled (hotkey set).
bool installAutoMarketOverlay();

// Hides the editor and resets its selection (called on return to the main menu).
void autoMarketOverlayReset();

// True while the editor panel is on screen. The settings overlay checks this so
// it never opens on top of the editor (both subclass the same window).
bool autoMarketOverlayVisible();

// Live toggle binding, for the settings overlay — see stopTroopsHotkey.h for
// the contract (setter does not persist; installed() false means the whole
// auto-market feature was off at startup and a rebind needs a restart).
Hotkey autoMarketOverlayBinding();
void autoMarketOverlaySetBinding(const Hotkey &hk);
bool autoMarketOverlayInstalled();
