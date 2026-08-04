#pragma once

// Auto-Market QoL (Phase 1 vertical slice): keeps a resource's stock at or
// above a configured minimum by posting real Market buy commands through the
// game's networked command layer. Slice scope = Wood, buy-only, ini-driven.
//
// Reverse-engineering trail, offsets and MP analysis live in
// docs/features/auto-market.md. Config keys are documented in
// docs/features/configuration.md.

void installAutoMarket();

// Clears all thresholds and hides the editor. Called on return to the main menu
// so thresholds never carry between games.
void autoMarketResetThresholds();

// ── In-game editor integration ─────────────────────────────────────────────────
// The overlay reads the good list and current thresholds, and writes edits back
// into the live engine state.

int autoMarketGoodCount(); // rows shown in the editor
const char *autoMarketGoodName(int row);
int autoMarketGoodId(int row);
const char *autoMarketGoodCategory(int row); // category header text for grouping
int autoMarketBuyPrice(int goodId); // 0 if not tradeable / unknown
int autoMarketSellPrice(int goodId);

int autoMarketGetMin(int goodId);
int autoMarketGetMax(int goodId);
void autoMarketSetMin(int goodId, int value);
void autoMarketSetMax(int goodId, int value);

// Presets: named threshold sets loaded from [preset:NAME] ini sections. Applying
// one is replace-all — goods not listed are cleared to 0/0.
int autoMarketPresetCount();
const char *autoMarketPresetName(int index);
void autoMarketApplyPreset(int index);
