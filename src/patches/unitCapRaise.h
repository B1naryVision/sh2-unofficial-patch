#pragma once

// "Unit cap 550" match option: every player's unit cap is 550 whatever the
// player count. Install verifies the sites without writing them;
// patches/matchRules.cpp switches it on per match. See
// docs/features/unit-cap-raise.md.

// False when a site does not hold the stock bytes; the option is then unavailable.
bool installUnitCapRaise();

// Simulation thread only.
void unitCapRaiseSetEnabled(bool enabled);
