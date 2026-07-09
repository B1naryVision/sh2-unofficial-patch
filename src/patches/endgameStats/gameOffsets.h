#pragma once
#include "snapshot.h"
#include <cstdint>

// Game memory layout used by the endgame-stats feature. All values are RVAs
// (module_base + RVA) or offsets from the player object pointer, confirmed in
// x32dbg — evidence tables in docs/features/endgame-stats.md.

// Player table: array of 4-byte player object pointers, 32 slots (0–31).
// Entries past slot 31 are unrelated .data values — never dereference them.
inline constexpr uintptr_t PLAYER_TABLE_RVA = 0x6E8BD8;
inline constexpr int PLAYER_TABLE_SLOTS = 32;

// Local player: slot index (int32) and dedicated object pointer. The pointer
// stays valid for the current human player regardless of castle state; the
// slot index survives even after death cleans the object out of the table.
inline constexpr uintptr_t LOCAL_SLOT_RVA = 0x6E8C5C;
inline constexpr uintptr_t LOCAL_PLAYER_RVA = 0x6E8C60;

// Per-session player name array, indexed by (colour - 1): 8 records, stride
// 0x1C, +0x10 = heap pointer to a UTF-16LE null-terminated name string. A
// colour with no current owner holds a stale/freed pointer — see
// docs/features/endgame-stats.md "Player Name Mapping".
inline constexpr uintptr_t NAME_ARRAY_RVA = 0xDB89B0;
inline constexpr uintptr_t NAME_ARRAY_STRIDE = 0x1C;
inline constexpr uintptr_t NAME_ARRAY_PTR_OFF = 0x10;
inline constexpr int NAME_ARRAY_COUNT = 8;

// Offset from the army sub-object pointer (ESI at the spawn hook site) back
// to the player object base.
inline constexpr uintptr_t ARMY_SUBOBJ_OFF = 0x674;

// ── Player object field offsets ───────────────────────────────────────────────
inline constexpr uintptr_t OFF_COLOR = 0x4; // 1=Red…10=Gray; corrupts near endgame
inline constexpr uintptr_t OFF_HONOR = 0x1C;
inline constexpr uintptr_t OFF_TROOPS = 0xD8C;
inline constexpr uintptr_t OFF_SIEGE = 0xD90;
inline constexpr uintptr_t OFF_TITLE = 0xF58;
inline constexpr uintptr_t OFF_CASTLE = 0x10F8; // 2 = standing, 1 = destroyed, 0 = unused
inline constexpr uintptr_t OFF_GOLD = 0x1010; // float
inline constexpr uintptr_t OFF_POPULARITY = 0x1028; // float
inline constexpr uintptr_t OFF_INC_TRADE = 0x1554;
inline constexpr uintptr_t OFF_INC_TAX_CASTLE = 0x1558;
inline constexpr uintptr_t OFF_INC_TAX_ESTATES = 0x1628;
inline constexpr uintptr_t OFF_HON_BASE = 0x1714; // 8×int, order matches RawPlayerStat::honSrc

// Typed accessor over a live player object pointer.
struct PlayerView {
    uintptr_t p;

    template <typename T> T at(uintptr_t off) const {
        return *reinterpret_cast<const T *>(p + off);
    }

    int color() const { return at<int>(OFF_COLOR); }
    int castle() const { return at<int>(OFF_CASTLE); }
    uint32_t goldBits() const { return at<uint32_t>(OFF_GOLD); }
    int honor() const { return at<int>(OFF_HONOR); }
};

inline uintptr_t *playerTable(uintptr_t base) { return (uintptr_t *)(base + PLAYER_TABLE_RVA); }

inline bool isValidColor(int color) { return color >= 1 && color <= 10; }

// Reads every tracked field of a live player object into a RawPlayerStat.
// Float-free (raw bits only), so it is safe on hook paths — see snapshot.h.
inline RawPlayerStat readRawPlayerStat(int slot, uintptr_t playerPtr) {
    PlayerView pv = {playerPtr};
    RawPlayerStat stat = {};

    stat.slot = slot;
    stat.colorIdx = pv.color();
    stat.goldBits = pv.goldBits();
    stat.honor = pv.honor();
    stat.troops = pv.at<int>(OFF_TROOPS);
    stat.siege = pv.at<int>(OFF_SIEGE);
    stat.titleIdx = pv.at<int>(OFF_TITLE);
    stat.popBits = pv.at<uint32_t>(OFF_POPULARITY);
    stat.income[0] = pv.at<int>(OFF_INC_TRADE);
    stat.income[1] = pv.at<int>(OFF_INC_TAX_CASTLE);
    stat.income[2] = pv.at<int>(OFF_INC_TAX_ESTATES);

    for (int src = 0; src < 8; ++src) {
        stat.honSrc[src] = pv.at<int>(OFF_HON_BASE + (uintptr_t)src * 4);
    }

    return stat;
}
