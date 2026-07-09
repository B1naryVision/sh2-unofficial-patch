#pragma once
#include "units.h"
#include <cstdint>

inline constexpr int kMaxPlayers = 8;
inline constexpr int kMaxNameChars = 24;

// Player fields as read from the live player object, float-free: gold and
// popularity are stored as raw float bits because this struct is filled on
// hook paths (frame tick, spawn hook) where x87 arithmetic is not safe — see
// core/frameTick.h. Convert with bitsToFloat() on a safe path only.
struct RawPlayerStat {
    int slot;
    int colorIdx;
    uint32_t goldBits;
    int honor;
    int troops;
    int siege;
    int titleIdx;
    uint32_t popBits;
    int income[3]; // trade, castle tax, estate tax
    int honSrc[8]; // feasting, dancing, monastery, jousting, church, granary, statues, crime
};

inline float bitsToFloat(uint32_t bits) {
    float f;
    __builtin_memcpy(&f, &bits, sizeof(f));
    return f;
}

// One display-ready player column, assembled by the collector at endgame.
struct PlayerRow {
    int slot;
    int colorIdx;
    int gold;
    int honor;
    int troops;
    int siege;
    int titleIdx;
    int popularity;
    int income[3];
    int honSrc[8];
    uint32_t units[kUnitCount];
    wchar_t name[kMaxNameChars]; // empty if unavailable (overlay falls back to colour label)
};

struct EndgameSnapshot {
    PlayerRow players[kMaxPlayers];
    int count;
    bool won;
};
