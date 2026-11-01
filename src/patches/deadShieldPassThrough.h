#pragma once

// "Dead shields let shots through" match option: a Mantlet or Cat stops catching
// projectiles the moment its health runs out, instead of at the end of its death
// animation. Install verifies the site without writing it;
// patches/matchRules.cpp switches it on per match. See
// docs/features/dead-shield-pass-through.md.

// False when the site does not hold the stock bytes; the option is then unavailable.
bool installDeadShieldPassThrough();

// Simulation thread only.
void deadShieldPassThroughSetEnabled(bool enabled);

bool deadShieldPassThroughAvailable();
