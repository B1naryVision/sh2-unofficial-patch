#pragma once
#include <cstdint>

// Recruitable unit types tracked by the spawn hook, stored densely so the
// per-player count arrays are kUnitCount entries instead of a sparse 256-slot
// table. Unit type IDs confirmed in x32dbg — see docs/features/endgame-stats.md.

struct UnitDef {
    uint8_t id; // game unit type ID (EDI at the spawn hook site)
    const char *name;
};

inline constexpr UnitDef kUnits[] = {
    {0x13, "Archer"},
    {0x1B, "Swordsman"},
    {0x1C, "Spearman"},
    {0x1D, "Ladderman"},
    {0x30, "Engineer"},
    {0x31, "Peasant"},
    {0x32, "Maceman"},
    {0x33, "Pikeman"},
    {0x34, "Crossbowman"},
    {0x47, "Knight"},
    {0x48, "Assassin"},
    {0x49, "Outlaw"},
    {0x4A, "Horse Archer"},
    {0x4B, "Berserker"},
    {0x4C, "Pictish Boat Warrior"},
    {0x4D, "Light Cavalry"},
    {0x4E, "Axe Thrower"},
    {0x4F, "Thief"},
    {0x5D, "Monk"},
    {0x5E, "Warrior Monk"},
    // Siege equipment — the same spawn hook covers these (the CMP at RVA
    // 0x0EE3BE executes for all unit types).
    {0x42, "Trebuchet"},
    {0x43, "Fire Ballista"},
    {0x44, "Catapult"},
    {0x58, "Siege Tower (small)"},
    {0x59, "Siege Tower (large)"},
    {0x5A, "Battering Ram"},
    {0x5B, "Cat"},
    {0x66, "Mantlet"},
    {0x67, "Burning Cart"},
};

inline constexpr int kUnitCount = (int)(sizeof(kUnits) / sizeof(kUnits[0]));

struct UnitIndexMap {
    int8_t map[256];
};

// Game unit type ID → dense index into kUnits, -1 for untracked types.
inline constexpr UnitIndexMap kUnitIndex = [] {
    UnitIndexMap m = {};

    for (int i = 0; i < 256; ++i) {
        m.map[i] = -1;
    }

    for (int i = 0; i < kUnitCount; ++i) {
        m.map[kUnits[i].id] = (int8_t)i;
    }

    return m;
}();

inline int unitIndexFromId(uint32_t typeId) {
    if (typeId >= 256) {
        return -1;
    }

    return kUnitIndex.map[typeId];
}
