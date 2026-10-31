#pragma once

// "No market" match option: while on, no Market can be placed, no market trade
// executes (human or AI), and the auto-market stands down. Switched on and off per
// match by patches/matchRules.cpp. See docs/features/no-market.md.

// Swaps the two command slots; false when either does not hold the stock value.
bool installNoMarket();

// Simulation thread only.
void noMarketSetEnabled(bool enabled);
