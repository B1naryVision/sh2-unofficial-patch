#pragma once
#include "units.h"
#include <cstdint>

// Tracks cumulative units recruited per player table slot via the unit spawn
// hook at RVA 0x0EE3BE. See docs/features/endgame-stats.md.

void installUnitTracker();

// kUnitCount entries, indexed like kUnits. slot must be 0..31.
const uint32_t *unitCountsForSlot(int slot);

bool slotHasRecruits(int slot);

void resetUnitCounts();
void resetUnitCountsForSlot(int slot);
