#pragma once
#include "snapshot.h"

// Writes endgame_debug.txt (session counters, per-slot live/cache state, name
// array contents, and the final selected players) so a missing or mislabelled
// player can be diagnosed against the raw data that fed each detection pass.
// No-op in release builds.
void dumpEndgameDebug(const EndgameSnapshot &snap);
