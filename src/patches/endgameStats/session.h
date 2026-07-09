#pragma once
#include "snapshot.h"
#include <cstdint>

// Tracks per-slot player state over the course of one game session, entirely
// on the game thread (polled via the shared frame-tick dispatcher).
//
// Lifecycle: the session is Idle until the first unit recruit proves a game
// is actually running (sessionOnRecruit), and is reset back to Idle when the
// endgame screen closes (sessionReset). While Idle, nothing is polled — this
// is what stops menu-time polling from freezing identities off a dead game's
// stale player table, which was the main source of wrong names/stats.

// Diagnostic counters, dumped into endgame_debug.txt by debug builds.
struct EndgameCounters {
    uint32_t pollTicks; // polls that actually ran (in-game, past throttle)
    uint32_t pollsSkippedIdle; // frame ticks ignored because no game is active
    uint32_t snapshots; // slot snapshots taken (poll + recruit paths)
    uint32_t identityFreezes; // stableColor/stableName captures
    uint32_t slotReallocs; // slot object pointer changed → per-slot state cleared
    uint32_t recruitEvents; // tracked-unit recruits seen by the spawn hook
    uint32_t sessionStarts; // Idle → Active transitions
    uint32_t sessionResets; // Active/Idle → full reset (endgame screen closed)
};

// Read-only view of one slot's tracked state, for the collector and debug dump.
struct SlotView {
    bool seen; // at least one snapshot exists
    uintptr_t objPtr; // player object pointer the state was captured from
    int stableColor; // colour frozen at first sighting; 0 = never frozen
    const wchar_t *stableName; // name frozen with the colour; empty if none
    const RawPlayerStat *stat; // last snapshot (valid only if seen)
};

void installSession(); // registers the poll with the frame-tick dispatcher
void sessionReset(); // full reset to Idle; call when the endgame screen closes
void sessionOnRecruit(int slot, uintptr_t playerBase); // from the unit tracker

bool sessionInGame();
SlotView sessionSlot(int slot);

// Reads the name for a colour (1–10) from the game's name array into `out`
// (at least kMaxNameChars wchars). Returns false if no readable name.
bool sessionLoadName(int color, wchar_t *out);

const EndgameCounters &endgameCounters();
