#pragma once
#include "units.h"
#include <cstdint>

// Tracks kills per player table slot, by victim unit type, via lethal damage,
// accepted ballista launches and committed pit falls. Repeat notifications
// are deduplicated. Read by the replay panel (replay/replayViewer.cpp).

// Kill rows: the kUnits types in kUnits order, then one row for enemy lords.
// The lord is not a recruitable unit, so it has no kUnits entry. Its unit def
// type is 0x20 (RTTI .?AVLord@Stronghold2@@, vtable RVA 0x5bf51c, the only Lord
// class); 0x12, once taken for the lord, is the campfire Peasant (vtable RVA
// 0x5b293c) — verified live.
inline constexpr uint32_t kLordUnitId = 0x20;
inline constexpr int kKillLordRow = kUnitCount;
inline constexpr int kKillRowCount = kUnitCount + 1;

// Kill row for a victim unit type id, -1 when that type is not tallied.
inline int killRowFromUnitId(uint32_t typeId) {
    return typeId == kLordUnitId ? kKillLordRow : unitIndexFromId(typeId);
}

inline const char *killRowName(int row) {
    return row == kKillLordRow ? "Lord" : kUnits[row].name;
}

void installKillTracker();

// kKillRowCount entries, indexed by kill row: kills BY this slot's player
// (any of its units, the lord included), BY victim type. slot must be 0..31.
const uint32_t *killCountsForSlot(int slot);

void resetKillCounts();
void resetKillCountsForSlot(int slot);
