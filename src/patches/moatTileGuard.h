#pragma once

// Stops a soldier's fill-in-moat script from reading the tile-flag table with
// the "no tile" value, as an automatic match fix. Install verifies the site
// without writing it; patches/matchRules.cpp switches it on whenever every
// player has it. See docs/bugs/outlaw-moat-crash.md.

// False when the site does not hold the stock bytes; the fix is then unavailable.
bool installMoatTileGuard();

// Simulation thread only.
void moatTileGuardSetEnabled(bool enabled);

bool moatTileGuardAvailable();
