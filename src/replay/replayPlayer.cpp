#include "replayPlayer.h"

#include "choreCodec.h"
#include "replayRecorder.h"
#include "replayTrace.h"
#include "spectatorGuard.h"
#include "replayCommon.h"
#include "../core/frameTick.h"
#include "../core/hook.h"

#include <cstring>
#include <windows.h>

// The recorder's arena size: a container larger than this is rejected
// outright by the header check below.
static const uint32_t PLAYBACK_CAPACITY = REPLAY_LOG_CAPACITY;

enum PlayerState : uint16_t {
    PLAYER_IDLE = 0,
    PLAYER_LOADED = 1,
    PLAYER_WAITING_EPOCH = 2,
    PLAYER_PLAYING = 3,
    PLAYER_DONE = 4,
    // A COLD start (New Skirmish / Kingmaker) has admissions BEFORE the
    // in-match epoch, and one of them -- StartGameChore -- is the command that
    // creates the match. There is no save to load and no lobby to click, so
    // playback has to issue those itself, from the main menu, before any
    // simulation tick exists. Loaded-save containers never enter this state:
    // they have no pre-epoch events, so the first event already sits at or
    // after the record epoch.
    PLAYER_PREEPOCH = 5,
};

static uint8_t s_log[PLAYBACK_CAPACITY];
static uint32_t s_logBytes = 0;
static uint32_t s_logEvents = 0;
static uint32_t s_recordEpoch = 0;
static uint32_t s_playbackEpoch = 0;
static uint32_t s_cursorOffset = 0;
static uint32_t s_cursorEvent = 0;
static uint16_t s_state = PLAYER_IDLE;
static bool s_multiplayerTiming = false;
static uint8_t s_multiplayerPhase = 0;
static bool s_cadenceHookInstalled = false;
// Keep MoveTroopsChore's World+5 scatter checks stock. Forcing their MP branch
// regressed Freeman playback actor movement and box geometry (A/B tested):
// the stock path matched all 677 compared actor fingerprints, the override
// first diverged at relative tick 218.
uint32_t g_replayMpCadence = 0;
uintptr_t g_replayNetMode = 0;
uintptr_t g_replayCadenceReturn = 0;
uint32_t g_replayMpRulesRequested = 0;
uint32_t g_replayMpRulesApplied = 0;
uintptr_t g_replayRulesReturn = 0;

// Save loading derives World+0x4029a from transport netMode. Offline loading
// otherwise enables SP-only producers (first observed: chickens), even when
// job cadence is correct. Preserve the native MP rules while transport stays
// offline. AL/registers/flags retain their stock values after the assignment.
static const uintptr_t LOAD_RULES_RVA = 0x2aec5;
static const uint8_t LOAD_RULES_STOCK[] = {0x88, 0x86, 0x9a, 0x02, 0x04, 0x00};
__declspec(naked) static void loadRulesHook() {
    __asm__ volatile(
        "pushfl\n\t"
        "cmpl $0, _g_replayMpRulesRequested\n\t"
        "je 1f\n\t"
        "testb %al, %al\n\t"
        "jne 1f\n\t"
        "movb $1, 0x4029a(%esi)\n\t"
        "movl $1, _g_replayMpRulesApplied\n\t"
        "jmp 2f\n\t"
        "1: movb %al, 0x4029a(%esi)\n\t"
        "2: popfl\n\t"
        "jmp *_g_replayRulesReturn\n\t");
}

// Frame pacer FUN_00420e30 (RVA 0x20e30) returns simulation updates per frame.
// Its first test `cmp byte [ecx+0x4029a],0 / jnz` forces the fixed MP rate
// (45, or 40 with World+5) whenever the MP rules byte is set; otherwise it uses
// the Game Speed setting World+0xce348. Live MP is capped at 45 but runs slower
// under load, so the simulation does not depend on that rate. While replay has
// applied the MP rules offline, report the byte as clear to the PACER ONLY so
// playback follows Game Speed; every other reader of 0x4029a (chicken producer
// etc.) still sees the MP rules. Stock compare otherwise.
uintptr_t g_replayPacerReturn = 0;
static const uintptr_t PACER_RULES_RVA = 0x20e4e;
static const uint8_t PACER_RULES_STOCK[] = {0x80, 0xb9, 0x9a, 0x02, 0x04, 0x00, 0x00};
__declspec(naked) static void pacerRulesHook() {
    __asm__ volatile(
        "cmpl $0, _g_replayMpRulesApplied\n\t"
        "jne 1f\n\t"
        "cmpb $0, 0x4029a(%ecx)\n\t"
        "jmp *_g_replayPacerReturn\n\t"
        "1: cmpl %ecx, %ecx\n\t" // ZF=1: the stock jnz falls through to Game Speed
        "jmp *_g_replayPacerReturn\n\t");
}

static void clearMultiplayerRules() {
    // Never clear native rules if the player has entered a real MP session.
    if (g_replayMpRulesApplied && !replayTraceSessionIsNetworked()) {
        const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
        *(uint8_t *)(base + 0xdb846a) = 0;
    }
    g_replayMpRulesApplied = 0;
    g_replayMpRulesRequested = 0;
}

// NetGameMgr::tick skips the job drain on four of every five simulation
// updates in MP. Preserve that stock byte-counter/modulo path offline without
// changing the network mode (which would enable sends and peer waits).
static const uintptr_t CADENCE_RVA = 0x3679cc;
__declspec(naked) static void cadenceHook() {
    __asm__ volatile(
        "cmpl $0, _g_replayMpCadence\n\t"
        "jne 1f\n\t"
        "pushl %eax\n\t"
        "movl _g_replayNetMode, %eax\n\t"
        "cmpl $2, (%eax)\n\t"
        "popl %eax\n\t"
        "jmp *_g_replayCadenceReturn\n\t"
        "1: cmpl $1, _g_replayMpCadence\n\t"
        "jmp *_g_replayCadenceReturn\n\t");
}

static uint32_t s_injected = 0;
static uint32_t s_injectFailures = 0;
static uint32_t s_skippedAutonomous = 0;
static uint32_t s_refusedUnclassified = 0;
static uint32_t s_lateEvents = 0;
static uint32_t s_maxLateness = 0;
static uint32_t s_firstTargetTick = 0;
static uint32_t s_lastTargetTick = 0;
static uint32_t s_reclassified = 0;
static uint32_t s_simTickPumps = 0;
static uint32_t s_tickHookInstalled = 0;
static uint32_t s_preEpochInjected = 0;
static uint32_t s_preEpochSkipped = 0;
static uint32_t s_preEpochFailures = 0;
// The shared startup guard in simPauseDisable clears the empirically located
// pause flag once, then leaves stock pause behavior alone so a mid-match `p`
// remains effective.


// The `call FUN_007676c0` inside FUN_00767950, RVA 0x367a03, stock
// `e8 b8 fc ff ff`. Hooking the call instruction itself is exact: the pump runs
// the instant that callee returns, before anything else in the tick.
//
// Why here rather than the tick-counter bump (RVA 0x367da5, the previous site).
// The bump is the LAST instruction of the tick window, so it is after
// FUN_00767950 has already drained tick X's command queue -- an injected
// command therefore waits and is executed as tick X+1. That site reproduced the
// recorded tick and the recorded within-tick order and was still a tick late in
// EXECUTION, which a test match caught: the iterator-0 streams are identical through
// tick 104, and then a ~420-draw batch -- the pathfinding for a 300-unit
// MoveTroops -- runs at tick 105 in the reference and at tick 106 in playback.
//
// FUN_007676c0 is called early, well before the drain at 0x767b29/0x767c6b.
// The tick's autonomous admissions also precede the drain, which is why the
// bump site got the ORDER right; pumping here keeps that order and still lands
// in the queue the SAME tick's drain consumes.
static const uintptr_t TICK_BUMP_RVA = 0x367a03;
static const uintptr_t TICK_BUMP_TARGET_RVA = 0x3676c0;
static const uint8_t TICK_BUMP_STOCK[] = {0xe8, 0xb8, 0xfc, 0xff, 0xff};

// Trampoline state needs external linkage: MinGW GAS cannot resolve local
// static symbols named from naked inline assembly.
uintptr_t g_replayPlayerBumpReturn = 0;
uintptr_t g_replayPlayerBumpTarget = 0;

// Inject local UI, remote-peer, and previous codec input. Origin 4 describes
// network provenance: it also includes the host's autonomous village commands.
// Those still need injection offline because the originating peer is absent.
// Local AI/system work must be re-derived. Native origin 0 is unclassified;
// stop rather than silently duplicate or discard simulation work.
static bool requiresInjection(const ReplayEventRecord &e) {
    if (e.flags & REPLAY_FLAG_FOREIGN_CALLER) {
        return true;
    }
    return e.originHint == 1 || e.originHint == 4;
}

static bool isAutonomous(const ReplayEventRecord &e) {
    return e.originHint == 2 || e.originHint == 3;
}

// A container carries the origin the recording build decided on. When a later
// build learns that a posting routine is autonomous, an older container would
// still stop playback on a route we now understand. Re-run the caller-route
// classifier over origin-0 events only: a recorded nonzero origin is never
// overridden, so this can rescue an unknown route but cannot reinterpret a
// route the recorder already classified.
static uint8_t effectiveOrigin(const ReplayEventRecord &e) {
    if (e.originHint != 0) {
        return e.originHint;
    }
    if (e.flags & REPLAY_FLAG_FOREIGN_CALLER) {
        return 0;
    }
    const uint8_t rescued =
        replayTraceClassifyCaller(e.callerRva, e.parentCallerRva);
    if (rescued) {
        ++s_reclassified;
    }
    return rescued;
}

static void resetCounters() {
    s_cursorOffset = 0;
    s_cursorEvent = 0;
    s_injected = 0;
    s_injectFailures = 0;
    s_skippedAutonomous = 0;
    s_refusedUnclassified = 0;
    s_lateEvents = 0;
    s_maxLateness = 0;
    s_firstTargetTick = 0;
    s_lastTargetTick = 0;
    s_reclassified = 0;
    s_simTickPumps = 0;
    s_playbackEpoch = 0;
    s_preEpochInjected = 0;
    s_preEpochSkipped = 0;
    s_preEpochFailures = 0;
}

uint32_t replayPlayerLoad(const char *path) {
    if (!path || !*path) {
        return 0;
    }
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }

    ReplayLogFileHeader header = {};
    DWORD got = 0;
    bool ok = ReadFile(file, &header, sizeof(header), &got, NULL) &&
              got == sizeof(header) &&
              memcmp(header.magic, "SH2RCMD1", 8) == 0 &&
              header.version == 2 &&
              header.recordSize == sizeof(ReplayEventRecord) &&
              header.droppedEvents == 0 &&
              header.byteCount <= PLAYBACK_CAPACITY;

    // A container recorded against a different command-class table cannot be
    // replayed: the class index is a position in that table, so a mismatch
    // would rebuild the wrong command from the right bytes.
    if (ok && header.classCount != choreCodecClassCount()) {
        ok = false;
    }

    if (ok && header.byteCount) {
        ok = ReadFile(file, s_log, header.byteCount, &got, NULL) &&
             got == header.byteCount;
    }
    CloseHandle(file);
    if (!ok) {
        return 0;
    }

    s_logBytes = header.byteCount;
    s_logEvents = header.eventCount;
    s_recordEpoch = header.epochTick;
    resetCounters();
    s_state = PLAYER_LOADED;
    return s_logEvents;
}

uint32_t replayPlayerLoadBytes(const void *data, uint32_t bytes) {
    ReplayLogFileHeader header = {};
    if (!data || bytes < sizeof(header)) {
        return 0;
    }
    memcpy(&header, data, sizeof(header));
    const bool ok = memcmp(header.magic, "SH2RCMD1", 8) == 0 && header.version == 2 &&
                    header.recordSize == sizeof(ReplayEventRecord) &&
                    header.droppedEvents == 0 && header.byteCount <= PLAYBACK_CAPACITY &&
                    bytes - sizeof(header) >= header.byteCount &&
                    header.classCount == choreCodecClassCount();
    if (!ok) {
        return 0;
    }
    memcpy(s_log, (const uint8_t *)data + sizeof(header), header.byteCount);
    s_logBytes = header.byteCount;
    s_logEvents = header.eventCount;
    s_recordEpoch = header.epochTick;
    resetCounters();
    s_state = PLAYER_LOADED;
    return s_logEvents ? s_logEvents : 1;
}

bool replayPlayerPlaybackJob(uint32_t *relativeJob) {
    if ((s_state != PLAYER_PLAYING && s_state != PLAYER_DONE) || !s_playbackEpoch) {
        return false;
    }
    const uint32_t now = replayTraceCurrentSimulationTick();
    *relativeJob = now >= s_playbackEpoch ? now - s_playbackEpoch : 0;
    return true;
}

bool replayPlayerArmed() {
    return s_state == PLAYER_WAITING_EPOCH || s_state == PLAYER_PLAYING ||
           s_state == PLAYER_DONE || s_state == PLAYER_PREEPOCH;
}

static void resetInjectedTracking();

uint32_t replayPlayerControl(uint32_t mode, uint32_t multiplayerPhase, bool fromLoad) {
    if (mode == 0) {
        clearMultiplayerRules();
        s_multiplayerTiming = false;
        g_replayMpCadence = 0;
        // DONE too: a finished playback that is stopped must not still read
        // as armed when the next match (or the next replay) starts.
        if (s_state == PLAYER_WAITING_EPOCH || s_state == PLAYER_PLAYING ||
            s_state == PLAYER_DONE || s_state == PLAYER_PREEPOCH) {
            s_state = PLAYER_LOADED;
        }
        s_playbackEpoch = 0;
        spectatorGuardSetActive(0);
        return s_state;
    }
    if (s_state != PLAYER_LOADED) {
        return s_state;
    }
    // MP timing is only armed offline at the menu. Its byte-counter phase
    // belongs to the checkpoint: the uint8 rollover shortens one job interval
    // every 256 updates, so preserving only the modulo-five remainder fails.
    // `fromLoad` is the replay session's LoadGame hook: it arms at the entry of
    // the stock load itself, which is the checkpoint boundary by construction,
    // so a match that is about to be replaced does not disqualify it.
    if (mode == 2 && (!s_cadenceHookInstalled || (replayInGame() && !fromLoad) ||
                     replayTraceSessionIsNetworked() || multiplayerPhase > 255)) {
        return s_state;
    }
    s_multiplayerTiming = mode == 2;
    g_replayMpRulesRequested = s_multiplayerTiming ? 1u : 0u;
    s_multiplayerPhase = (uint8_t)multiplayerPhase;
    g_replayMpCadence = 0;
    resetCounters();
    resetInjectedTracking();
    // The guard is NOT activated here. It is activated on the first in-match
    // frame, in the epoch transition below.
    //
    // Arming the guard here made the correct recording procedure
    // impossible. The reference container now starts at relative tick 47
    // because the recorder is armed at the MENU, before the save loads, so the
    // playback process has to be armed at the menu too or its first hundreds
    // of ticks of events fire late in a burst. But the guard swallows input to
    // the game window, including the very clicks needed to load the save.
    //
    // Activating on the epoch transition instead loses nothing: before the
    // match exists there is no simulation state for a click to corrupt, and the
    // guard comes up in the same frame batch that observes the first in-match
    // frame, so there is no unguarded in-match window.
    // A cold container's first event precedes the record epoch. Detecting it
    // from the data rather than from a flag keeps loaded-save containers on
    // exactly the path they have always taken.
    s_state = PLAYER_WAITING_EPOCH;
    if (s_logBytes >= sizeof(ReplayEventRecord)) {
        ReplayEventRecord first;
        memcpy(&first, s_log, sizeof(first));
        if (first.simulationTick < s_recordEpoch) {
            s_state = PLAYER_PREEPOCH;
        }
    }
    return s_state;
}

// --- Occupancy-list order restore (see replayPlayer.h) ----------------------
// Intrusive head-inserted nodes: tile list at actor+0x74, region list at
// actor+0x80, each {owner, next node, back-link to the pointer that holds this
// node}. The list head lives in a table slot reached through the head member's
// back-link, so no table formula is needed here.
static const uintptr_t TILE_ACTOR_MGR_RVA = 0x6e8728;
static const uint32_t TILE_ORDER_MAX_WORDS = 65536;
static const uint32_t TILE_ORDER_MAX_MEMBERS = 64;
static uint32_t s_tileOrder[TILE_ORDER_MAX_WORDS];
static uint32_t s_tileOrderWords = 0;
static uint32_t s_tileOrderLists = 0;
static uint32_t s_tileApplied = 0;
static uint32_t s_tileSkipped = 0;

static uint32_t finishTileOrder(uint32_t lists, uint32_t bytes);

uint32_t replayPlayerLoadTileOrder(const char *path) {
    s_tileOrderWords = s_tileOrderLists = 0;
    s_tileApplied = s_tileSkipped = 0;
    if (!path || !*path) {
        return 0;
    }
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return TILE_ORDER_REFUSED;
    }
    uint32_t header[3] = {};
    DWORD got = 0;
    bool ok = ReadFile(file, header, sizeof(header), &got, NULL) &&
              got == sizeof(header) && header[0] == 0x44524F54u && header[1] == 1;
    DWORD bytes = 0;
    if (ok) {
        ok = ReadFile(file, s_tileOrder, sizeof(s_tileOrder), &bytes, NULL) &&
             bytes % 4 == 0 && bytes < sizeof(s_tileOrder);
    }
    CloseHandle(file);
    if (!ok) {
        return TILE_ORDER_REFUSED;
    }
    return finishTileOrder(header[2], bytes);
}

uint32_t replayPlayerLoadTileOrderBytes(const void *data, uint32_t bytes) {
    s_tileOrderWords = s_tileOrderLists = 0;
    s_tileApplied = s_tileSkipped = 0;
    uint32_t header[3] = {};
    if (!data || bytes < sizeof(header)) {
        return TILE_ORDER_REFUSED;
    }
    memcpy(header, data, sizeof(header));
    bytes -= sizeof(header);
    if (header[0] != 0x44524F54u || header[1] != 1 || bytes % 4 != 0 ||
        bytes >= sizeof(s_tileOrder)) {
        return TILE_ORDER_REFUSED;
    }
    memcpy(s_tileOrder, (const uint8_t *)data + sizeof(header), bytes);
    return finishTileOrder(header[2], bytes);
}

static uint32_t finishTileOrder(uint32_t lists, uint32_t bytes) {
    const uint32_t words = bytes / 4;
    uint32_t at = 0;
    uint32_t header[3] = {0, 0, lists};
    for (uint32_t list = 0; list < header[2]; ++list) {
        if (at + 2 > words || s_tileOrder[at] > 1 || s_tileOrder[at + 1] < 2 ||
            s_tileOrder[at + 1] > TILE_ORDER_MAX_MEMBERS ||
            at + 2 + s_tileOrder[at + 1] > words) {
            return TILE_ORDER_REFUSED;
        }
        at += 2 + s_tileOrder[at + 1];
    }
    s_tileOrderWords = at;
    s_tileOrderLists = header[2];
    return s_tileOrderLists;
}

void replayPlayerTileOrderResult(uint32_t *applied, uint32_t *skipped) {
    *applied = s_tileApplied;
    *skipped = s_tileSkipped;
}

static inline uintptr_t &word(uintptr_t address) {
    return *reinterpret_cast<uintptr_t *>(address);
}

static void applyTileOrder() {
    if (!s_tileOrderLists) {
        return;
    }
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    const uintptr_t begin = word(base + TILE_ACTOR_MGR_RVA + 8);
    const uintptr_t end = word(base + TILE_ACTOR_MGR_RVA + 12);
    const uint32_t slotCount = end > begin ? (uint32_t)((end - begin) / 4) : 0;
    uint32_t at = 0;
    for (uint32_t list = 0; list < s_tileOrderLists; ++list) {
        const uint32_t off = s_tileOrder[at] ? 0x80 : 0x74;
        const uint32_t n = s_tileOrder[at + 1];
        const uint32_t *slots = s_tileOrder + at + 2;
        at += 2 + n;
        uintptr_t obj[TILE_ORDER_MAX_MEMBERS];
        bool ok = true;
        for (uint32_t i = 0; i < n && ok; ++i) {
            ok = slots[i] < slotCount && (obj[i] = word(begin + 4 * slots[i])) != 0;
        }
        // Exactly one member's back-link must point outside the member set:
        // that is the table slot holding the list head.
        uintptr_t head = 0;
        for (uint32_t i = 0; i < n && ok; ++i) {
            const uintptr_t back = word(obj[i] + off + 8);
            bool internal = false;
            for (uint32_t j = 0; j < n; ++j) {
                internal = internal || back == obj[j] + off + 4;
            }
            if (!internal) {
                ok = head == 0 && back != 0;
                head = back;
            }
        }
        ok = ok && head != 0;
        // The live chain from that head must be exactly these members, then 0.
        uint32_t seen = 0;
        for (uintptr_t node = ok ? word(head) : 0; ok && node; node = word(node + 4)) {
            bool member = false;
            for (uint32_t j = 0; j < n; ++j) {
                member = member || node == obj[j] + off;
            }
            ok = member && ++seen <= n;
        }
        if (!ok || seen != n) {
            ++s_tileSkipped;
            continue;
        }
        word(head) = obj[0] + off;
        word(obj[0] + off + 8) = head;
        for (uint32_t i = 0; i < n; ++i) {
            word(obj[i] + off + 4) = i + 1 < n ? obj[i + 1] + off : 0;
            if (i + 1 < n) {
                word(obj[i + 1] + off + 8) = obj[i] + off + 4;
            }
        }
        ++s_tileApplied;
    }
}

// --- Projectile damage (FLYS, replayCommon.h) --------------------------------
static ReplayFlyField s_flyFields[REPLAY_FLY_MAX];
static uint32_t s_flyFieldCount = 0; // pending until the epoch applies them
static uint32_t s_flyLoaded = 0;
static uint32_t s_flyApplied = 0;
static uint32_t s_flySkipped = 0;

uint32_t replayPlayerLoadFlyStateBytes(const void *data, uint32_t bytes) {
    s_flyFieldCount = s_flyLoaded = s_flyApplied = s_flySkipped = 0;
    uint32_t header[3] = {};
    if (!data || bytes < sizeof(header)) {
        return FLY_STATE_REFUSED;
    }
    memcpy(header, data, sizeof(header));
    if (header[0] != REPLAY_FLY_MAGIC || header[1] != 1 || header[2] > REPLAY_FLY_MAX ||
        bytes - sizeof(header) != header[2] * sizeof(ReplayFlyField)) {
        return FLY_STATE_REFUSED;
    }
    memcpy(s_flyFields, (const uint8_t *)data + sizeof(header), header[2] * sizeof(ReplayFlyField));
    for (uint32_t i = 0; i < header[2]; ++i) {
        if (!replayFlyFieldKnown(s_flyFields[i].vtableRva, s_flyFields[i].offset)) {
            return FLY_STATE_REFUSED;
        }
    }
    s_flyFieldCount = s_flyLoaded = header[2];
    return s_flyFieldCount;
}

void replayPlayerClearFlyState() { s_flyFieldCount = s_flyLoaded = s_flyApplied = s_flySkipped = 0; }

void replayPlayerFlyStateResult(uint32_t *entries, uint32_t *applied, uint32_t *skipped) {
    *entries = s_flyLoaded;
    *applied = s_flyApplied;
    *skipped = s_flySkipped;
}

// A projectile the load did not bring back in the recorded slot, with the
// recorded serial and class, is left alone and counted.
static void applyFlyState() {
    const uintptr_t *slots = nullptr;
    uint32_t count = 0;
    const bool haveSlots = s_flyFieldCount && replayFlySlots(&slots, &count);
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    for (uint32_t i = 0; i < s_flyFieldCount; ++i) {
        const ReplayFlyField &e = s_flyFields[i];
        const uintptr_t fly = haveSlots && e.slot < count ? slots[e.slot] : 0;
        if (!fly || word(fly) != base + e.vtableRva ||
            *(const uint16_t *)(fly + 0x7a) != e.serial) {
            ++s_flySkipped;
            continue;
        }
        *(uint32_t *)(fly + e.offset) = e.value;
        ++s_flyApplied;
    }
    s_flyFieldCount = 0; // once per load
}

// --- RandomIter #0 at the checkpoint -----------------------------------------
// The stock load re-runs Hovel vt+0xc (FUN_00480b80, vtable VA 0x9a00ac) for
// every Hovel, and each call draws iterator 0 (`rand % 6` into +0xec). The live
// game drew those when the Hovels were placed and SaveGame draws nothing, so a
// load came up one draw ahead per Hovel (one recording: 240, cursor
// 10 -> 250 before job 0). The cursor the recorder read right after its save
// is the checkpoint's own value; restore it once, before the first update.
static uint32_t s_iter0AtSave = 0xffffffffu;
static uint32_t s_iter0Loaded = 0xffffffffu;
static uint32_t s_iter0Written = 0xffffffffu;
static const uintptr_t ITER0_GAME_PTR_RVA = 0x6e8bcc;
static const uintptr_t ITER0_CURSOR_OFF = 0x40078;

void replayPlayerSetIter0AtSave(uint32_t cursor) {
    s_iter0AtSave = cursor <= 0xffff ? cursor : 0xffffffffu;
    s_iter0Loaded = s_iter0Written = 0xffffffffu;
}

void replayPlayerIter0Result(uint32_t *loaded, uint32_t *written) {
    *loaded = s_iter0Loaded;
    *written = s_iter0Written;
}

static void applyIter0AtSave() {
    if (s_iter0AtSave > 0xffff) {
        return;
    }
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    const uintptr_t game = *(const uintptr_t *)(base + ITER0_GAME_PTR_RVA);
    if (game) {
        uint16_t *cursor = (uint16_t *)(game + ITER0_CURSOR_OFF);
        s_iter0Loaded = *cursor;
        *cursor = (uint16_t)s_iter0AtSave;
        s_iter0Written = s_iter0AtSave;
    }
    s_iter0AtSave = 0xffffffffu; // once per load
}

// --- Intra-drain execution order (execorder.bin) --------------------------
// Injection happens at the job-boundary pump, after that job's autonomous
// admissions, so injected commands always execute last in their drain. Where
// the reference executed one BEFORE an autonomous command, permute that job's
// queue vector just before it drains. Identity of injected objects comes from
// choreCodecAdmit (the codec retains a reference, so pointers are never
// reused). Every position's vtable is checked; any mismatch leaves it stock.
static const uintptr_t EXEC_NET_MGR_RVA = 0x17e6a30;
static const uint32_t EXEC_ORDER_MAX_WORDS = 262144; // = the retimer's MAX_PLAN_WORDS
static const uint32_t EXEC_ORDER_MAX_ITEMS = 256;
static uint32_t s_execOrder[EXEC_ORDER_MAX_WORDS];
static uint32_t s_execOrderJobs = 0;
static uint32_t s_execApplied = 0;
static uint32_t s_execSkipped = 0;
static uint32_t s_execLastReason = 0; // 1 mgr, 2 size, 3 class, 4 not queued
static uint32_t s_execLastDetail = 0; // (size << 16) | n, or queued job count
struct InjectedObject { uintptr_t object; uint32_t execRel; };
static const uint32_t INJECTED_TRACK = 1024;
static InjectedObject s_injectedObjects[INJECTED_TRACK];
static uint32_t s_injectedNext = 0;

static uint32_t finishExecOrder(uint32_t jobs, uint32_t bytes);

uint32_t replayPlayerLoadExecOrder(const char *path) {
    s_execOrderJobs = 0;
    s_execApplied = s_execSkipped = 0;
    if (!path || !*path) {
        return 0;
    }
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return EXEC_ORDER_REFUSED;
    }
    uint32_t header[3] = {};
    DWORD got = 0, bytes = 0;
    bool ok = ReadFile(file, header, sizeof(header), &got, NULL) &&
              got == sizeof(header) && header[0] == 0x524F5845u && header[1] == 1;
    if (ok) {
        ok = ReadFile(file, s_execOrder, sizeof(s_execOrder), &bytes, NULL) &&
             bytes % 4 == 0 && bytes < sizeof(s_execOrder);
    }
    CloseHandle(file);
    if (!ok) {
        return EXEC_ORDER_REFUSED;
    }
    return finishExecOrder(header[2], bytes);
}

uint32_t replayPlayerLoadExecOrderBytes(const void *data, uint32_t bytes) {
    s_execOrderJobs = 0;
    s_execApplied = s_execSkipped = 0;
    uint32_t header[3] = {};
    if (!data || bytes < sizeof(header)) {
        return EXEC_ORDER_REFUSED;
    }
    memcpy(header, data, sizeof(header));
    bytes -= sizeof(header);
    if (header[0] != 0x524F5845u || header[1] != 1 || bytes % 4 != 0 ||
        bytes >= sizeof(s_execOrder)) {
        return EXEC_ORDER_REFUSED;
    }
    memcpy(s_execOrder, (const uint8_t *)data + sizeof(header), bytes);
    return finishExecOrder(header[2], bytes);
}

static uint32_t finishExecOrder(uint32_t jobs, uint32_t bytes) {
    const uint32_t words = bytes / 4;
    uint32_t at = 0;
    uint32_t header[3] = {0, 0, jobs};
    for (uint32_t job = 0; job < header[2]; ++job) {
        if (at + 2 > words || s_execOrder[at + 1] == 0 ||
            s_execOrder[at + 1] > EXEC_ORDER_MAX_ITEMS ||
            at + 2 + s_execOrder[at + 1] > words) {
            return EXEC_ORDER_REFUSED;
        }
        at += 2 + s_execOrder[at + 1];
    }
    s_execOrderJobs = header[2];
    return s_execOrderJobs;
}

void replayPlayerExecOrderResult(uint32_t *applied, uint32_t *skipped) {
    *applied = s_execApplied;
    // Diagnostics ride the high bits: reason << 28 | (detail & 0xfffff) << 8.
    *skipped = s_execSkipped | (s_execLastReason << 28) |
               ((s_execLastDetail & 0xfffffu) << 8);
}

static void resetInjectedTracking() {
    memset(s_injectedObjects, 0, sizeof(s_injectedObjects));
    s_injectedNext = 0;
}

static bool isInjectedFor(uintptr_t object, uint32_t rel) {
    for (uint32_t i = 0; i < INJECTED_TRACK; ++i) {
        if (s_injectedObjects[i].object == object && s_injectedObjects[i].execRel == rel) {
            return true;
        }
    }
    return false;
}

static void applyExecOrder(uint32_t rel) {
    if (!s_execOrderJobs) {
        return;
    }
    const uint32_t *plan = nullptr;
    uint32_t n = 0;
    for (uint32_t at = 0, job = 0; job < s_execOrderJobs; ++job) {
        if (s_execOrder[at] == rel) {
            n = s_execOrder[at + 1];
            plan = s_execOrder + at + 2;
            break;
        }
        at += 2 + s_execOrder[at + 1];
    }
    if (!plan) {
        return;
    }
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    const uintptr_t mgr = base + EXEC_NET_MGR_RVA;
    const uintptr_t storageBegin = word(mgr + 0x10), storageEnd = word(mgr + 0x14);
    uintptr_t cursor = word(mgr + 0x18);
    const uint32_t count = (uint32_t)word(mgr + 0x20);
    if (storageEnd < storageBegin || (storageEnd - storageBegin) % 0x18 != 0 ||
        count > (storageEnd - storageBegin) / 0x18) {
        ++s_execSkipped;
        s_execLastReason = 1;
        return;
    }
    for (uint32_t j = 0; j < count; ++j, cursor += 0x18) {
        if (cursor == storageEnd) {
            cursor = storageBegin;
        }
        uintptr_t *vecBegin = reinterpret_cast<uintptr_t *>(word(cursor));
        uintptr_t *vecEnd = reinterpret_cast<uintptr_t *>(word(cursor + 4));
        const uint32_t size = (uint32_t)(vecEnd - vecBegin);
        bool hasOurs = false;
        for (uint32_t k = 0; k < size && !hasOurs; ++k) {
            hasOurs = isInjectedFor(vecBegin[k], rel);
        }
        if (!hasOurs) {
            continue;
        }
        // EndJobChore markers (vtable RVA 0x5f32b0, see replayTrace.cpp) are
        // queued with the job but are not recorded as command executions: keep
        // them where they are and permute only the command slots.
        uintptr_t injected[EXEC_ORDER_MAX_ITEMS], other[EXEC_ORDER_MAX_ITEMS];
        uint32_t slots[EXEC_ORDER_MAX_ITEMS];
        uint32_t ni = 0, no = 0, ns = 0;
        bool ok = size <= EXEC_ORDER_MAX_ITEMS;
        for (uint32_t k = 0; k < size && ok; ++k) {
            if (vecBegin[k] && word(vecBegin[k]) - base == 0x5f32b0u) {
                continue;
            }
            slots[ns++] = k;
            if (isInjectedFor(vecBegin[k], rel)) {
                injected[ni++] = vecBegin[k];
            } else {
                other[no++] = vecBegin[k];
            }
        }
        ok = ok && ns == n;
        uintptr_t out[EXEC_ORDER_MAX_ITEMS];
        uint32_t ii = 0, oi = 0;
        for (uint32_t k = 0; k < n && ok; ++k) {
            const bool wantInjected = (plan[k] & 0x80000000u) != 0;
            if (wantInjected ? ii >= ni : oi >= no) {
                ok = false;
                break;
            }
            const uintptr_t object = wantInjected ? injected[ii++] : other[oi++];
            ok = object && word(object) - base == (plan[k] & 0x7fffffffu);
            out[k] = object;
        }
        ok = ok && ii == ni && oi == no;
        if (!ok) {
            ++s_execSkipped;
            s_execLastReason = ns == n ? 3 : 2;
            s_execLastDetail = (size << 16) | n;
            return;
        }
        for (uint32_t k = 0; k < n; ++k) {
            vecBegin[slots[k]] = out[k];
        }
        ++s_execApplied;
        return;
    }
    ++s_execSkipped; // planned drain, but our injected object was not queued
    s_execLastReason = 4;
    s_execLastDetail = count;
}

// The epoch transition is run from both callbacks. The frame tick is what arms
// the spectator guard promptly on the first in-match frame; the simulation tick
// runs it too so that the first drained tick can never find the player still
// waiting and report its own events late.
static void advanceEpochState() {
    if (s_state == PLAYER_WAITING_EPOCH) {
        // The recorder samples its epoch on the same predicate in the recording
        // process, so both sides agree on what "the match started" means.
        const uint32_t epoch = replayRecorderEpochTick();
        if (!epoch) {
            return;
        }
        // Arming happened at the menu, but the player could then enter a
        // real multiplayer lobby. Never inject offline replay into that match.
        if (s_multiplayerTiming && replayTraceSessionIsNetworked()) {
            clearMultiplayerRules();
            s_state = PLAYER_LOADED;
            return;
        }
        s_playbackEpoch = epoch;
        if (s_multiplayerTiming && !replayTraceSessionIsNetworked()) {
            const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
            *(uint8_t *)(base + 0x17e6a34) = s_multiplayerPhase;
            g_replayMpCadence = 1;
        }
        // The match is live as of this frame, so the session becomes
        // non-authoritative as of this frame. See replayPlayerControl.
        // Before the first simulation update of the match can move anyone.
        applyTileOrder();
        applyFlyState();
        applyIter0AtSave();
        spectatorGuardSetActive(1);
        s_state = PLAYER_PLAYING;
    }
}

// One pre-epoch event per frame, from the main menu, before any simulation
// tick exists. Pacing matters: StartGameChore loads a map, which takes seconds
// of wall clock, and the events behind it must not be issued into a process
// that is still building the world. One per frame also keeps each admission in
// its own message-pump cycle, which is how the lobby issued them.
//
// Only the two LOBBY classes are injected. Everything else recorded before the
// epoch -- the AI clan creation and the AI's opening builds -- is the
// simulation's own work and is re-derived once the match exists, exactly as
// in-match autonomous commands are. Anything that is neither is a route nobody
// has classified, and playback stops rather than guess: injecting a command the
// game also produces, or dropping one it does not, both corrupt the comparison
// silently.
static void advancePreEpochState() {
    if (s_state != PLAYER_PREEPOCH) {
        return;
    }
    if (s_cursorOffset + sizeof(ReplayEventRecord) > s_logBytes) {
        s_state = PLAYER_WAITING_EPOCH;
        return;
    }
    ReplayEventRecord event;
    memcpy(&event, s_log + s_cursorOffset, sizeof(event));
    if (s_cursorOffset + sizeof(event) + event.payloadLength > s_logBytes) {
        s_state = PLAYER_DONE;
        return;
    }
    if (event.simulationTick >= s_recordEpoch) {
        // The lobby phase is over. The epoch transition takes it from here and
        // schedules the rest of the container relative to the epoch, so the
        // in-match path is untouched by any of this.
        s_state = PLAYER_WAITING_EPOCH;
        return;
    }

    const uint8_t *payload = s_log + s_cursorOffset + sizeof(event);
    event.originHint = effectiveOrigin(event);
    if (event.classIndex != 0xffffu && choreCodecIsLobbyClass(event.classIndex)) {
        if (event.payloadLength &&
            choreCodecAdmit(event.classIndex, payload, event.payloadLength)) {
            ++s_preEpochInjected;
        } else {
            ++s_preEpochFailures;
        }
    } else if (isAutonomous(event) || requiresInjection(event)) {
        // Human-originated lobby input that is not one of the two setup classes
        // is counted as autonomous here rather than injected: before the match
        // exists there is nothing for it to act on.
        ++s_preEpochSkipped;
    } else {
        ++s_refusedUnclassified;
        s_state = PLAYER_DONE;
        return;
    }

    s_cursorOffset += sizeof(event) + event.payloadLength;
    ++s_cursorEvent;
}

static void replayPlayerFrameTick() {
    if (!replayInGame()) {
        g_replayMpCadence = 0;
        if (s_state != PLAYER_WAITING_EPOCH) clearMultiplayerRules();
    }
    advancePreEpochState();
    advanceEpochState();
}

void replayPlayerSimulationTick() {
    advanceEpochState();
    if (s_state != PLAYER_PLAYING) {
        return;
    }
    ++s_simTickPumps;

    const uint32_t now = replayTraceCurrentSimulationTick();
    applyExecOrder(now - s_playbackEpoch);

    while (s_cursorOffset + sizeof(ReplayEventRecord) <= s_logBytes) {
        ReplayEventRecord event;
        memcpy(&event, s_log + s_cursorOffset, sizeof(event));
        if (s_cursorOffset + sizeof(event) + event.payloadLength > s_logBytes) {
            // Truncated tail: stop rather than replay a partial command.
            s_state = PLAYER_DONE;
            return;
        }

        // Relative scheduling. Recorded ticks before the record epoch would
        // underflow, so they are clamped to the epoch itself and reported as
        // late rather than wrapping into the far future.
        const uint32_t delta = event.simulationTick > s_recordEpoch
                                   ? event.simulationTick - s_recordEpoch
                                   : 0;
        const uint32_t target = s_playbackEpoch + delta;
        if (target > now) {
            return;
        }

        if (!s_cursorEvent) {
            s_firstTargetTick = target;
        }
        s_lastTargetTick = target;
        const uint32_t lateness = now - target;
        if (lateness) {
            ++s_lateEvents;
            if (lateness > s_maxLateness) {
                s_maxLateness = lateness;
            }
        }

        const uint8_t *payload = s_log + s_cursorOffset + sizeof(event);
        // Rescue before dispatch so the human/autonomous split below sees the
        // classification this build actually believes.
        event.originHint = effectiveOrigin(event);
        if (requiresInjection(event)) {
            void *admitted = nullptr;
            if (event.classIndex != 0xffffu && event.payloadLength &&
                choreCodecAdmit(event.classIndex, payload, event.payloadLength,
                                &admitted)) {
                ++s_injected;
                // Injected at pump `target`, executed one job later.
                s_injectedObjects[s_injectedNext % INJECTED_TRACK] = {
                    (uintptr_t)admitted, target - s_playbackEpoch + 1};
                ++s_injectedNext;
            } else {
                ++s_injectFailures;
            }
        } else if (isAutonomous(event)) {
            ++s_skippedAutonomous;
        } else {
            // Unclassified native route. Stop the schedule: continuing would
            // either duplicate a command the simulation also produces, or drop
            // one it does not, and both corrupt the comparison silently.
            ++s_refusedUnclassified;
            s_state = PLAYER_DONE;
            return;
        }

        s_cursorOffset += sizeof(event) + event.payloadLength;
        ++s_cursorEvent;
    }

    s_state = PLAYER_DONE;
}

extern "C" void replayPlayerTickBump() {
    replayPlayerSimulationTick();
}

// Calls the stock callee first, with the registers exactly as the game set
// them, then pumps the schedule and resumes at the site + 5.
// `pushal`/`popal` preserve the callee's return value across the pump.
__declspec(naked) static void replayPlayerBumpHook() {
    __asm__ volatile(
        "call *_g_replayPlayerBumpTarget\n\t"
        "pushal\n\t"
        "pushfl\n\t"
        "call _replayPlayerTickBump\n\t"
        "popfl\n\t"
        "popal\n\t"
        "jmp *_g_replayPlayerBumpReturn\n\t");
}

void installReplayPlayer() {
    registerFrameTick(replayPlayerFrameTick);

    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    if (!base) {
        return;
    }
    // Verify all three sites before modifying any. Rebuild the absolute cmp
    // operand under ASLR; acquire both page regions before writing any jump.
    uint8_t cadenceStock[] = {0x83, 0x3d, 0, 0, 0, 0, 0x02};
    g_replayNetMode = base + 0x26cd7f0;
    memcpy(cadenceStock + 2, &g_replayNetMode, 4);
    uint8_t *cadence = (uint8_t *)(base + CADENCE_RVA);
    if (memcmp(cadence, cadenceStock, sizeof(cadenceStock)) != 0) {
        return;
    }
    uint8_t *site = (uint8_t *)(base + TICK_BUMP_RVA);
    if (memcmp(site, TICK_BUMP_STOCK, sizeof(TICK_BUMP_STOCK)) != 0) {
        return;
    }
    uint8_t *rules = (uint8_t *)(base + LOAD_RULES_RVA);
    if (memcmp(rules, LOAD_RULES_STOCK, sizeof(LOAD_RULES_STOCK)) != 0) {
        return;
    }
    uint8_t *pacer = (uint8_t *)(base + PACER_RULES_RVA);
    if (memcmp(pacer, PACER_RULES_STOCK, sizeof(PACER_RULES_STOCK)) != 0) {
        return;
    }
    // The stock bytes encode a RELATIVE call, so the displacement is checked to
    // resolve to the expected callee before the site is trusted.
    int32_t displacement = 0;
    memcpy(&displacement, site + 1, 4);
    const uintptr_t callee =
        (uintptr_t)site + sizeof(TICK_BUMP_STOCK) + (intptr_t)displacement;
    if (callee != base + TICK_BUMP_TARGET_RVA) {
        return;
    }
    g_replayPlayerBumpTarget = callee;
    g_replayPlayerBumpReturn = (uintptr_t)site + sizeof(TICK_BUMP_STOCK);
    g_replayCadenceReturn = (uintptr_t)cadence + sizeof(cadenceStock);
    g_replayRulesReturn = (uintptr_t)rules + sizeof(LOAD_RULES_STOCK);
    g_replayPacerReturn = (uintptr_t)pacer + sizeof(PACER_RULES_STOCK);
    const size_t span = (site + sizeof(TICK_BUMP_STOCK)) - cadence;
    DWORD oldProtect = 0;
    if (!VirtualProtect(cadence, span, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return;
    }
    DWORD rulesProtect = 0;
    if (!VirtualProtect(rules, sizeof(LOAD_RULES_STOCK), PAGE_EXECUTE_READWRITE,
                        &rulesProtect)) {
        DWORD ignored = 0;
        VirtualProtect(cadence, span, oldProtect, &ignored);
        return;
    }
    DWORD pacerProtect = 0;
    if (!VirtualProtect(pacer, sizeof(PACER_RULES_STOCK), PAGE_EXECUTE_READWRITE,
                        &pacerProtect)) {
        DWORD ignored = 0;
        VirtualProtect(cadence, span, oldProtect, &ignored);
        VirtualProtect(rules, sizeof(LOAD_RULES_STOCK), rulesProtect, &ignored);
        return;
    }
    const auto writeJump = [](uint8_t *at, uintptr_t target, size_t size) {
        const int32_t delta = (int32_t)(target - (uintptr_t)at - 5);
        at[0] = 0xe9;
        memcpy(at + 1, &delta, 4);
        memset(at + 5, 0x90, size - 5);
    };
    writeJump(cadence, (uintptr_t)cadenceHook, sizeof(cadenceStock));
    writeJump(site, (uintptr_t)replayPlayerBumpHook, sizeof(TICK_BUMP_STOCK));
    writeJump(rules, (uintptr_t)loadRulesHook, sizeof(LOAD_RULES_STOCK));
    writeJump(pacer, (uintptr_t)pacerRulesHook, sizeof(PACER_RULES_STOCK));
    DWORD ignored = 0;
    VirtualProtect(cadence, span, oldProtect, &ignored);
    VirtualProtect(rules, sizeof(LOAD_RULES_STOCK), rulesProtect, &ignored);
    VirtualProtect(pacer, sizeof(PACER_RULES_STOCK), pacerProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), cadence, span);
    FlushInstructionCache(GetCurrentProcess(), rules, sizeof(LOAD_RULES_STOCK));
    FlushInstructionCache(GetCurrentProcess(), pacer, sizeof(PACER_RULES_STOCK));
    s_cadenceHookInstalled = true;
    s_tickHookInstalled = 1;
}

size_t replayPlayerStatus(void *out, size_t cap) {
    if (!out || cap < sizeof(ReplayPlayerStatus)) {
        return 0;
    }
    ReplayPlayerStatus status = {};
    status.magic = 0x31535052u; // "RPS1"
    status.version = 5; // +preEpoch{Injected,Skipped,Failures}, +pauseClears
    status.state = s_state;
    status.loadedEvents = s_logEvents;
    status.loadedBytes = s_logBytes;
    status.recordEpoch = s_recordEpoch;
    status.playbackEpoch = s_playbackEpoch;
    status.preEpochInjected = s_preEpochInjected;
    status.preEpochSkipped = s_preEpochSkipped;
    status.preEpochFailures = s_preEpochFailures;
    status.currentTick = replayTraceCurrentSimulationTick();
    status.cursorEvent = s_cursorEvent;
    status.injected = s_injected;
    status.injectFailures = s_injectFailures;
    status.skippedAutonomous = s_skippedAutonomous;
    status.refusedUnclassified = s_refusedUnclassified;
    status.lateEvents = s_lateEvents;
    status.maxLateness = s_maxLateness;
    status.firstTargetTick = s_firstTargetTick;
    status.lastTargetTick = s_lastTargetTick;
    status.reclassified = s_reclassified;
    status.localCommands = spectatorGuardLocalCommands();
    status.blockedMessages = spectatorGuardBlockedMessages();
    status.guardHooked = spectatorGuardHooked() ? 1 : 0;
    status.blockedPolls = spectatorGuardBlockedPolls();
    status.pollGuardHooked = spectatorGuardPollThunks();
    status.simTickPumps = s_tickHookInstalled ? s_simTickPumps : 0;
    memcpy(out, &status, sizeof(status));
    return sizeof(status);
}
