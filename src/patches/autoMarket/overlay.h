#pragma once

// In-game Auto-Market threshold editor: a dismissible panel drawn over the game
// via the D3D EndScene hook. Reads/writes the live thresholds through the
// autoMarket accessors. See docs/features/auto-market.md (overlay section).

// Reads the toggle hotkey and, if enabled, registers the D3D render callback.
// Returns true if the editor is enabled (hotkey set).
bool installAutoMarketOverlay();

// Hides the editor and resets its selection (called on return to the main menu).
void autoMarketOverlayReset();
