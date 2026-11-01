#pragma once

// Keeps ranged troops on a move order while they wait to start walking, as an
// automatic match fix. Install verifies the sites without writing them;
// patches/matchRules.cpp switches it on whenever every player has it.
// See docs/bugs/ranged-move-orders.md.

// False when a site does not hold the stock bytes; the fix is then unavailable.
bool installRangedWalkGuard();

// Simulation thread only.
void rangedWalkGuardSetEnabled(bool enabled);

bool rangedWalkGuardAvailable();
