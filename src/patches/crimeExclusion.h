#pragma once

// Keeps Wheat Farms and Chandler's Workshops out of the crime system, the way
// Firefly already keeps Gong Pits and Falconer's Posts out of it, as an automatic
// match fix. Install verifies the table without writing it;
// patches/matchRules.cpp switches it on whenever every player has it.
// See docs/bugs/stuck-worker-crime.md.

// False when the table does not hold the stock values; the fix is then unavailable.
bool installCrimeExclusion();

// Simulation thread only.
void crimeExclusionSetEnabled(bool enabled);

bool crimeExclusionAvailable();
