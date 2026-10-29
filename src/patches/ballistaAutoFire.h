#pragma once

// Field ballista auto-fire, as a match rule. Install verifies every site and
// prepares the patched bytes without writing any; the rule is switched on and off
// per match by patches/matchRules.cpp. See docs/features/ballista-auto-fire.md.

// False when a site does not hold the stock bytes; the rule is then unavailable.
bool installBallistaAutoFire();

// Simulation thread only.
void ballistaAutoFireSetEnabled(bool enabled);

bool ballistaAutoFireAvailable();
