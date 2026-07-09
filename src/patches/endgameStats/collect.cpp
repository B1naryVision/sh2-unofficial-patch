#include "collect.h"
#include "gameOffsets.h"
#include "session.h"
#include "unitTracker.h"
#include <cstring>
#include <windows.h>

static EndgameSnapshot s_snap;
static bool s_slotAdded[PLAYER_TABLE_SLOTS];

// Converts a raw (float-free) stat into a display-ready player column and
// appends it to the snapshot. Unit counts come from the tracker; the name is
// resolved in a final pass once all players are selected.
static void addPlayer(const RawPlayerStat &raw) {
    if (s_snap.count >= kMaxPlayers) {
        return;
    }

    PlayerRow &row = s_snap.players[s_snap.count++];
    row = {};
    row.slot = raw.slot;
    row.colorIdx = raw.colorIdx;
    row.gold = (int)bitsToFloat(raw.goldBits);
    row.honor = raw.honor;
    row.troops = raw.troops;
    row.siege = raw.siege;
    row.titleIdx = raw.titleIdx;
    row.popularity = (int)bitsToFloat(raw.popBits);
    memcpy(row.income, raw.income, sizeof(row.income));
    memcpy(row.honSrc, raw.honSrc, sizeof(row.honSrc));

    if (raw.slot >= 0 && raw.slot < PLAYER_TABLE_SLOTS) {
        memcpy(row.units, unitCountsForSlot(raw.slot), sizeof(row.units));
        s_slotAdded[raw.slot] = true;
    }
}

static void addFromLive(int slot, uintptr_t playerPtr) {
    addPlayer(readRawPlayerStat(slot, playerPtr));
}

// ── Pass 1: local player via the dedicated current-player global ─────────────
// The pointer stays valid regardless of castle state. Resolve its table slot
// by pointer match; when the local player has died the game cleans their
// object out of the table, so fall back to the local-slot-index global, which
// survives death — this recovers the correct unit counts and frozen identity.
static void passLocalPlayer(uintptr_t base, uintptr_t *table) {
    uintptr_t localPtr = *(uintptr_t *)(base + LOCAL_PLAYER_RVA);

    if (!localPtr || !isValidColor(PlayerView{localPtr}.color())) {
        return;
    }

    int localSlot = -1;

    for (int i = 0; i < PLAYER_TABLE_SLOTS; ++i) {
        if (table[i] == localPtr) {
            localSlot = i;
            break;
        }
    }

    if (localSlot < 0) {
        int slotGlobal = *(int *)(base + LOCAL_SLOT_RVA);

        if (slotGlobal >= 0 && slotGlobal < PLAYER_TABLE_SLOTS) {
            localSlot = slotGlobal;
        }
    }

    if (localSlot >= 0) {
        addFromLive(localSlot, localPtr);
    }
}

// ── Pass 2: live remote players — castle state 1 ──────────────────────────────
// castle 2 = local player (always found by pass 1), castle 1 = remote player
// (human or AI), castle 0 = unused. Entries where both gold and honour are
// zero are persistent game-init template slots, not real players.
// ── Pass 3: unit-recruit fallback ─────────────────────────────────────────────
// Any slot where the spawn hook logged a recruit is definitively a real
// player, even if its castle flag was modified at game end.
static void passLiveRemotes(uintptr_t *table, bool requireRecruits) {
    for (int i = 0; i < PLAYER_TABLE_SLOTS && s_snap.count < kMaxPlayers; ++i) {
        if (s_slotAdded[i]) {
            continue;
        }

        uintptr_t playerPtr = table[i];

        if (!playerPtr) {
            continue;
        }

        PlayerView pv = {playerPtr};

        if (!isValidColor(pv.color())) {
            continue;
        }

        if (requireRecruits) {
            if (!slotHasRecruits(i)) {
                continue;
            }
        } else if (pv.castle() != 1) {
            continue;
        }

        // Skip zeroed live objects (templates in pass 2; crash/cleanup-cleared
        // entries in pass 3 — pass 4 uses the session snapshot for those).
        if (pv.goldBits() == 0 && pv.honor() == 0) {
            continue;
        }

        addFromLive(i, playerPtr);
    }
}

// ── Pass 4: session cache for eliminated/disconnected players ─────────────────
// Players who left or were defeated may have had their table entries cleaned
// up before the endgame screen fired; use the last snapshot the session
// polled. An all-zero snapshot with no recorded recruits is a template/ghost
// entry, never a real player — a real player eliminated mid-game leaves the
// table entirely, so their last snapshot retains real (non-zero) values.
static void passSessionCache() {
    for (int i = 0; i < PLAYER_TABLE_SLOTS && s_snap.count < kMaxPlayers; ++i) {
        if (s_slotAdded[i]) {
            continue;
        }

        SlotView view = sessionSlot(i);

        if (!view.seen) {
            continue;
        }

        const RawPlayerStat &raw = *view.stat;
        int color = view.stableColor ? view.stableColor : raw.colorIdx;

        if (!isValidColor(color)) {
            continue;
        }

        bool allZero = raw.goldBits == 0 && raw.honor == 0 && raw.troops == 0 && raw.siege == 0;

        if (allZero && !slotHasRecruits(i)) {
            continue;
        }

        addPlayer(raw);
    }
}

// ── Identity: frozen colour + name per slot ───────────────────────────────────
// Never re-derived from the live colour field at endgame: an eliminated
// player's colour is rewritten to their conqueror's, so two slots can read
// the same colour here. The session froze each slot's colour and name at
// first sighting, while the colour was still true. The colour-index lookup
// below is only a last resort for a slot that was never frozen, and it
// deliberately does NOT fall back to the live (corruptible) colour field.
static void resolveIdentities() {
    for (int i = 0; i < s_snap.count; ++i) {
        PlayerRow &row = s_snap.players[i];
        row.name[0] = 0;

        SlotView view = sessionSlot(row.slot);
        int color = 0;

        if (isValidColor(view.stableColor)) {
            color = view.stableColor;
            row.colorIdx = color;
        }

        if (view.stableName && view.stableName[0] != 0) {
            wcsncpy(row.name, view.stableName, kMaxNameChars - 1);
            row.name[kMaxNameChars - 1] = 0;
        } else if (color != 0) {
            sessionLoadName(color, row.name);
        }
    }
}

const EndgameSnapshot &collectEndgameStats(bool won) {
    s_snap = {};
    s_snap.won = won;
    memset(s_slotAdded, 0, sizeof(s_slotAdded));

    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uintptr_t *table = playerTable(base);

    passLocalPlayer(base, table);
    passLiveRemotes(table, false); // pass 2: castle state
    passLiveRemotes(table, true); // pass 3: recorded recruits
    passSessionCache(); // pass 4
    resolveIdentities();

    return s_snap;
}
