#pragma once
#include "snapshot.h"

// Renders a finished EndgameSnapshot as a panel drawn over the game through
// the D3D EndScene hook. Knows nothing about game memory — presentation only.

// Registers the render callback. Call from an install function (before the
// device is created), like the other overlays.
void installStatsOverlay();

// Sizes the panel to the snapshot and shows it. Does float work, so it must be
// called from a function-prologue hook or install path, never from a
// frame-tick/render callback — see docs/features/endgame-stats.md.
void showStatsOverlay(const EndgameSnapshot &snap);

void closeStatsOverlay();
