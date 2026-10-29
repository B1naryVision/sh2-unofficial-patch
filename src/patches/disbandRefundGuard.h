#pragma once

// The disband peasant-duplication fix, as an automatic match fix. Install
// verifies the sites and prepares the patched bytes without writing them;
// patches/matchRules.cpp switches it on whenever every player has it.
// See docs/bugs/disband-peasant-duplication.md.

// False when a site does not hold the stock bytes; the fix is then unavailable.
bool installDisbandRefundGuard();

// Simulation thread only.
void disbandRefundGuardSetEnabled(bool enabled);

bool disbandRefundGuardAvailable();
