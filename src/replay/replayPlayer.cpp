#include "replayPlayer.h"

#include "../core/exeBase.h"
#include "../core/frameTick.h"
#include "choreCodec.h"
#include "replayCommon.h"
#include "replayFormat.h"
#include "replayRecorder.h"
#include "replayTrace.h"
#include "spectatorGuard.h"

#include <cstring>
#include <windows.h>

// The recorder's arena size: a log larger than this is refused.
static const uint32_t PLAYBACK_CAPACITY = REPLAY_LOG_CAPACITY;

enum PlayerState : uint16_t {
    PLAYER_IDLE = 0,
    PLAYER_LOADED = 1,
    PLAYER_WAITING_EPOCH = 2,
    PLAYER_PLAYING = 3,
    PLAYER_DONE = 4,
};

static uint8_t *s_log = nullptr; // PLAYBACK_CAPACITY, allocated by LoadBytes
static uint32_t s_logBytes = 0;
static uint32_t s_recordEpoch = 0;
static uint32_t s_playbackEpoch = 0;
static uint32_t s_cursorOffset = 0;
static uint16_t s_state = PLAYER_IDLE;
static bool s_multiplayerTiming = false;
static uint8_t s_multiplayerPhase = 0;
static bool s_installed = false;
// Keep MoveTroopsChore's World+5 scatter checks stock. Forcing their MP branch
// regressed playback actor movement and box geometry (A/B tested): the stock
// path matched all 677 compared actor fingerprints, the override first
// diverged at relative tick 218.
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
    __asm__ volatile("pushfl\n\t"
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
    __asm__ volatile("cmpl $0, _g_replayMpRulesApplied\n\t"
                     "jne 1f\n\t"
                     "cmpb $0, 0x4029a(%ecx)\n\t"
                     "jmp *_g_replayPacerReturn\n\t"
                     "1: cmpl %ecx, %ecx\n\t" // ZF=1: the stock jnz falls through to Game Speed
                     "jmp *_g_replayPacerReturn\n\t");
}

static void clearMultiplayerRules() {
    // Never clear native rules if the player has entered a real MP session.
    if (g_replayMpRulesApplied && !replayTraceSessionIsNetworked()) {
        *(uint8_t *)(exeBase() + 0xdb846a) = 0;
    }

    g_replayMpRulesApplied = 0;
    g_replayMpRulesRequested = 0;
}

// NetGameMgr::tick skips the job drain on four of every five simulation
// updates in MP. Preserve that stock byte-counter/modulo path offline without
// changing the network mode (which would enable sends and peer waits).
static const uintptr_t CADENCE_RVA = 0x3679cc;
__declspec(naked) static void cadenceHook() {
    __asm__ volatile("cmpl $0, _g_replayMpCadence\n\t"
                     "jne 1f\n\t"
                     "pushl %eax\n\t"
                     "movl _g_replayNetMode, %eax\n\t"
                     "cmpl $2, (%eax)\n\t"
                     "popl %eax\n\t"
                     "jmp *_g_replayCadenceReturn\n\t"
                     "1: cmpl $1, _g_replayMpCadence\n\t"
                     "jmp *_g_replayCadenceReturn\n\t");
}

// The pump site; see replayPlayer.h.
static const uintptr_t PUMP_RVA = 0x367a03;
static const uintptr_t PUMP_TARGET_RVA = 0x3676c0;
static const uint8_t PUMP_STOCK[] = {0xe8, 0xb8, 0xfc, 0xff, 0xff};

// Trampoline state needs external linkage: MinGW GAS cannot resolve local
// static symbols named from naked inline assembly.
uintptr_t g_replayPlayerPumpReturn = 0;
uintptr_t g_replayPlayerPumpTarget = 0;

static bool isAutonomous(const ReplayEventRecord &e) {
    return e.originHint == 2 || e.originHint == 3;
}

// A sidecar carries the origin the recording build decided on. Re-run the
// caller-route classifier over origin-0 events only: a recorded nonzero origin
// is never overridden, so this can rescue a route the recorder did not know
// but cannot reinterpret one it classified.
static uint8_t effectiveOrigin(const ReplayEventRecord &e) {
    if (e.originHint != 0 || (e.flags & REPLAY_FLAG_FOREIGN_CALLER)) {
        return e.originHint;
    }

    return replayTraceClassifyCaller(e.callerRva, e.parentCallerRva);
}

bool replayPlayerLoadBytes(const void *data, uint32_t bytes) {
    replayPlayerUnload();
    ReplayLogFileHeader header;

    if (!replayValidateCommandLog(
            data, bytes, choreCodecClassCount(), PLAYBACK_CAPACITY, &header
        )) {
        return false;
    }

    if (header.byteCount) {
        s_log = (uint8_t *)VirtualAlloc(
            NULL, header.byteCount, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
        );

        if (!s_log) {
            return false;
        }

        memcpy(s_log, (const uint8_t *)data + sizeof(header), header.byteCount);
    }

    s_logBytes = header.byteCount;
    s_recordEpoch = header.epochTick;
    s_cursorOffset = 0;
    s_playbackEpoch = 0;
    s_state = PLAYER_LOADED;
    return true;
}

bool replayPlayerPlaybackJob(uint32_t *relativeJob) {
    if ((s_state != PLAYER_PLAYING && s_state != PLAYER_DONE) || !s_playbackEpoch) {
        return false;
    }

    const uint32_t now = replayTraceCurrentSimulationTick();
    *relativeJob = now >= s_playbackEpoch ? now - s_playbackEpoch : 0;
    return true;
}

uint32_t replayPlayerPlaybackEpoch() {
    return (s_state == PLAYER_PLAYING || s_state == PLAYER_DONE) ? s_playbackEpoch : 0;
}

bool replayPlayerArmed() {
    return s_state == PLAYER_WAITING_EPOCH || s_state == PLAYER_PLAYING || s_state == PLAYER_DONE;
}

static void resetInjectedTracking();

uint32_t replayPlayerControl(uint32_t mode, uint32_t multiplayerPhase) {
    if (mode == 0) {
        clearMultiplayerRules();
        s_multiplayerTiming = false;
        g_replayMpCadence = 0;

        // DONE too: a finished playback that is stopped must not still read
        // as armed when the next match (or the next replay) starts.
        if (replayPlayerArmed()) {
            s_state = PLAYER_LOADED;
        }

        s_playbackEpoch = 0;
        spectatorGuardSetActive(0);
        return s_state;
    }

    if (s_state != PLAYER_LOADED || !s_installed) {
        return s_state;
    }

    // MP timing is only armed offline. Its byte-counter phase belongs to the
    // checkpoint: the uint8 rollover shortens one job interval every 256
    // updates, so preserving only the modulo-five remainder fails.
    if (mode == 2 && (replayTraceSessionIsNetworked() || multiplayerPhase > 255)) {
        return s_state;
    }

    s_multiplayerTiming = mode == 2;
    g_replayMpRulesRequested = s_multiplayerTiming ? 1u : 0u;
    s_multiplayerPhase = (uint8_t)multiplayerPhase;
    g_replayMpCadence = 0;
    s_cursorOffset = 0;
    s_playbackEpoch = 0;
    resetInjectedTracking();
    // The spectator guard comes up on the first in-match frame (the epoch
    // transition below), not here: this runs inside the load, and before the
    // match exists there is no simulation state for input to corrupt.
    s_state = PLAYER_WAITING_EPOCH;
    return s_state;
}

// --- Occupancy-list order restore (see replayPlayer.h) ----------------------
// Intrusive head-inserted nodes: tile list at actor+0x74, region list at
// actor+0x80, each {owner, next node, back-link to the pointer that holds this
// node}. The list head lives in a table slot reached through the head member's
// back-link, so no table formula is needed here.
static const uintptr_t TILE_ACTOR_MGR_RVA = 0x6e8728;
static uint32_t s_tileOrder[REPLAY_TILE_MAX_WORDS];
static uint32_t s_tileOrderLists = 0;

bool replayPlayerLoadTileOrderBytes(const void *data, uint32_t bytes) {
    replayPlayerClearTileOrder();
    uint32_t lists = 0;
    const uint32_t words = replayValidateTileOrder(data, bytes, &lists);

    if (words == REPLAY_FORMAT_REFUSED) {
        return false;
    }

    memcpy(s_tileOrder, (const uint8_t *)data + 12, words * 4);
    s_tileOrderLists = lists;
    return true;
}

void replayPlayerClearTileOrder() { s_tileOrderLists = 0; }

static inline uintptr_t &word(uintptr_t address) { return *reinterpret_cast<uintptr_t *>(address); }

static void applyTileOrder() {
    if (!s_tileOrderLists) {
        return;
    }

    const uintptr_t base = exeBase();
    const uintptr_t begin = word(base + TILE_ACTOR_MGR_RVA + 8);
    const uintptr_t end = word(base + TILE_ACTOR_MGR_RVA + 12);
    const uint32_t slotCount = end > begin ? (uint32_t)((end - begin) / 4) : 0;
    uint32_t at = 0;

    for (uint32_t list = 0; list < s_tileOrderLists; ++list) {
        const uint32_t off = s_tileOrder[at] ? 0x80 : 0x74;
        const uint32_t n = s_tileOrder[at + 1];
        const uint32_t *slots = s_tileOrder + at + 2;
        at += 2 + n;
        uintptr_t obj[REPLAY_TILE_MAX_MEMBERS];
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
    }

    s_tileOrderLists = 0; // once per load
}

// --- Projectile damage (FLYS) ------------------------------------------------
static ReplayFlyField s_flyFields[REPLAY_FLY_MAX];
static uint32_t s_flyFieldCount = 0; // pending until the epoch applies them

bool replayPlayerLoadFlyStateBytes(const void *data, uint32_t bytes) {
    replayPlayerClearFlyState();
    const uint32_t count = replayValidateFlyState(data, bytes);

    if (count == REPLAY_FORMAT_REFUSED) {
        return false;
    }

    memcpy(s_flyFields, (const uint8_t *)data + 12, count * sizeof(ReplayFlyField));
    s_flyFieldCount = count;
    return true;
}

void replayPlayerClearFlyState() { s_flyFieldCount = 0; }

// A projectile the load did not bring back in the recorded slot, with the
// recorded serial and class, is left alone.
static void applyFlyState() {
    const uintptr_t *slots = nullptr;
    uint32_t count = 0;
    const bool haveSlots = s_flyFieldCount && replayFlySlots(&slots, &count);
    const uintptr_t base = exeBase();

    for (uint32_t i = 0; i < s_flyFieldCount; ++i) {
        const ReplayFlyField &e = s_flyFields[i];
        const uintptr_t fly = haveSlots && e.slot < count ? slots[e.slot] : 0;

        if (!fly || word(fly) != base + e.vtableRva ||
            *(const uint16_t *)(fly + 0x7a) != e.serial) {
            continue;
        }

        *(uint32_t *)(fly + e.offset) = e.value;
    }

    s_flyFieldCount = 0; // once per load
}

// --- RandomIter #0 at the checkpoint -----------------------------------------
// The stock load re-runs Hovel vt+0xc (FUN_00480b80, vtable VA 0x9a00ac) for
// every Hovel, and each call draws iterator 0 (`rand % 6` into +0xec). The live
// game drew those when the Hovels were placed and SaveGame draws nothing, so a
// load came up one draw ahead per Hovel (one recording: 240, cursor 10 -> 250
// before job 0). The cursor the recorder read right after its save is the
// checkpoint's own value; restore it once, before the first update.
static uint32_t s_iter0AtSave = 0xffffffffu;
static const uintptr_t ITER0_GAME_PTR_RVA = 0x6e8bcc;
static const uintptr_t ITER0_CURSOR_OFF = 0x40078;

void replayPlayerSetIter0AtSave(uint32_t cursor) {
    s_iter0AtSave = cursor <= 0xffff ? cursor : 0xffffffffu;
}

static void applyIter0AtSave() {
    if (s_iter0AtSave > 0xffff) {
        return;
    }

    const uintptr_t game = *(const uintptr_t *)(exeBase() + ITER0_GAME_PTR_RVA);

    if (game) {
        *(uint16_t *)(game + ITER0_CURSOR_OFF) = (uint16_t)s_iter0AtSave;
    }

    s_iter0AtSave = 0xffffffffu; // once per load
}

// --- Intra-drain execution order (EXOR) ---------------------------------------
// Injection happens at the pump, after that job's autonomous admissions, so
// injected commands always execute last in their drain. Where the recording
// executed one BEFORE an autonomous command, permute that job's queue vector
// just before it drains. Identity of injected objects comes from
// choreCodecAdmit. Every position's vtable is checked; any mismatch leaves the
// queue stock.
static const uintptr_t EXEC_NET_MGR_RVA = 0x17e6a30;
static uint32_t *s_execOrder = nullptr; // validated words, allocated by LoadExecOrderBytes
static uint32_t s_execOrderJobs = 0;
struct InjectedObject {
    uintptr_t object;
    uint32_t execRel;
};
static const uint32_t INJECTED_TRACK = 1024;
static InjectedObject s_injectedObjects[INJECTED_TRACK];
static uint32_t s_injectedNext = 0;

bool replayPlayerLoadExecOrderBytes(const void *data, uint32_t bytes) {
    replayPlayerClearExecOrder();
    uint32_t jobs = 0;
    const uint32_t words = replayValidateExecOrder(data, bytes, &jobs);

    if (words == REPLAY_FORMAT_REFUSED) {
        return false;
    }

    if (!words) {
        return true;
    }

    s_execOrder =
        (uint32_t *)VirtualAlloc(NULL, words * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (!s_execOrder) {
        return false;
    }

    memcpy(s_execOrder, (const uint8_t *)data + 12, words * 4);
    s_execOrderJobs = jobs;
    return true;
}

void replayPlayerClearExecOrder() {
    s_execOrderJobs = 0;

    if (s_execOrder) {
        VirtualFree(s_execOrder, 0, MEM_RELEASE);
        s_execOrder = nullptr;
    }
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

    const uintptr_t base = exeBase();
    const uintptr_t mgr = base + EXEC_NET_MGR_RVA;
    const uintptr_t storageBegin = word(mgr + 0x10), storageEnd = word(mgr + 0x14);
    uintptr_t cursor = word(mgr + 0x18);
    const uint32_t count = (uint32_t)word(mgr + 0x20);

    if (storageEnd < storageBegin || (storageEnd - storageBegin) % 0x18 != 0 ||
        count > (storageEnd - storageBegin) / 0x18) {
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

        // EndJobChore markers (vtable RVA 0x5f32b0) are queued with the job but
        // are not recorded as command executions: keep them where they are and
        // permute only the command slots.
        uintptr_t injected[REPLAY_EXOR_MAX_ITEMS], other[REPLAY_EXOR_MAX_ITEMS];
        uint32_t slots[REPLAY_EXOR_MAX_ITEMS];
        uint32_t ni = 0, no = 0, ns = 0;
        bool ok = size <= REPLAY_EXOR_MAX_ITEMS;

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
        uintptr_t out[REPLAY_EXOR_MAX_ITEMS];
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
            return;
        }

        for (uint32_t k = 0; k < n; ++k) {
            vecBegin[slots[k]] = out[k];
        }

        return;
    }
}

void replayPlayerUnload() {
    replayPlayerControl(0);
    s_state = PLAYER_IDLE;
    s_logBytes = 0;
    s_cursorOffset = 0;

    if (s_log) {
        VirtualFree(s_log, 0, MEM_RELEASE);
        s_log = nullptr;
    }
}

// The epoch transition, on the first in-match frame after the load. The
// session's own frame-tick predicate (World+4) decides when the match started,
// so the recording and the playback agree on it.
static void advanceEpochState() {
    if (s_state != PLAYER_WAITING_EPOCH || !replayInGame()) {
        return;
    }

    // Armed offline, but the player could then have entered a real
    // multiplayer lobby. Never inject a replay into that match.
    if (replayTraceSessionIsNetworked()) {
        clearMultiplayerRules();
        s_state = PLAYER_LOADED;
        return;
    }

    s_playbackEpoch = replayTraceCurrentSimulationTick();

    if (s_multiplayerTiming) {
        *(uint8_t *)(exeBase() + 0x17e6a34) = s_multiplayerPhase;
        g_replayMpCadence = 1;
    }

    // Before the first simulation update of the match can move anyone.
    applyTileOrder();
    applyFlyState();
    applyIter0AtSave();
    spectatorGuardSetActive(1);
    s_state = PLAYER_PLAYING;
}

static void replayPlayerFrameTick() {
    if (!replayInGame()) {
        g_replayMpCadence = 0;

        if (s_state != PLAYER_WAITING_EPOCH) {
            clearMultiplayerRules();
        }
    }

    advanceEpochState();
}

// Drains every scheduled event whose target tick has arrived, once per job.
static void replayPlayerSimulationTick() {
    if (s_state != PLAYER_PLAYING) {
        return;
    }

    const uint32_t now = replayTraceCurrentSimulationTick();
    applyExecOrder(now - s_playbackEpoch);

    // The log was validated at load: every record and payload fits, ticks do
    // not go backwards and none precedes the record epoch.
    while (s_cursorOffset < s_logBytes) {
        ReplayEventRecord event;
        memcpy(&event, s_log + s_cursorOffset, sizeof(event));
        const uint32_t target = s_playbackEpoch + (event.simulationTick - s_recordEpoch);

        if (target > now) {
            return;
        }

        const uint8_t *payload = s_log + s_cursorOffset + sizeof(event);
        event.originHint = effectiveOrigin(event);

        if (replayEventRequiresInjection(event)) {
            void *admitted = nullptr;

            // Only command classes, never the lobby/session ones: a sidecar is
            // shared input, and nothing a player records produces those.
            if (event.classIndex != 0xffffu && event.payloadLength &&
                choreCodecIsInjectable(event.classIndex) &&
                choreCodecAdmit(event.classIndex, payload, event.payloadLength, &admitted)) {
                // Injected at pump `target`, executed one job later (the
                // retimer scheduled it at its live execution job - 1).
                s_injectedObjects[s_injectedNext % INJECTED_TRACK] = {
                    (uintptr_t)admitted, target - s_playbackEpoch + 1
                };
                ++s_injectedNext;
            }
        } else if (!isAutonomous(event)) {
            // Unclassified native route. Stop the schedule: continuing would
            // either duplicate a command the simulation also produces, or drop
            // one it does not, and both corrupt the replay silently.
            s_state = PLAYER_DONE;
            return;
        }

        s_cursorOffset += sizeof(event) + event.payloadLength;
    }

    s_state = PLAYER_DONE;
}

extern "C" void replayPlayerPump() { replayPlayerSimulationTick(); }

// Calls the stock callee first, with the registers exactly as the game set
// them, then pumps the schedule and resumes at the site + 5.
// `pushal`/`popal` preserve the callee's return value across the pump.
__declspec(naked) static void replayPlayerPumpHook() {
    __asm__ volatile("call *_g_replayPlayerPumpTarget\n\t"
                     "pushal\n\t"
                     "pushfl\n\t"
                     "call _replayPlayerPump\n\t"
                     "popfl\n\t"
                     "popal\n\t"
                     "jmp *_g_replayPlayerPumpReturn\n\t");
}

bool replayPlayerInstalled() { return s_installed; }

void installReplayPlayer() {
    const uintptr_t base = exeBase();

    if (!base) {
        return;
    }

    // Verify all four sites before modifying any. Rebuild the absolute cmp
    // operand under ASLR; acquire every page region before writing any jump.
    uint8_t cadenceStock[] = {0x83, 0x3d, 0, 0, 0, 0, 0x02};
    g_replayNetMode = base + 0x26cd7f0;
    memcpy(cadenceStock + 2, &g_replayNetMode, 4);
    uint8_t *cadence = (uint8_t *)(base + CADENCE_RVA);
    uint8_t *site = (uint8_t *)(base + PUMP_RVA);
    uint8_t *rules = (uint8_t *)(base + LOAD_RULES_RVA);
    uint8_t *pacer = (uint8_t *)(base + PACER_RULES_RVA);

    if (memcmp(cadence, cadenceStock, sizeof(cadenceStock)) != 0 ||
        memcmp(site, PUMP_STOCK, sizeof(PUMP_STOCK)) != 0 ||
        memcmp(rules, LOAD_RULES_STOCK, sizeof(LOAD_RULES_STOCK)) != 0 ||
        memcmp(pacer, PACER_RULES_STOCK, sizeof(PACER_RULES_STOCK)) != 0) {
        return;
    }

    // The stock bytes encode a RELATIVE call, so the displacement is checked to
    // resolve to the expected callee before the site is trusted.
    int32_t displacement = 0;
    memcpy(&displacement, site + 1, 4);
    const uintptr_t callee = (uintptr_t)site + sizeof(PUMP_STOCK) + (intptr_t)displacement;

    if (callee != base + PUMP_TARGET_RVA) {
        return;
    }

    g_replayPlayerPumpTarget = callee;
    g_replayPlayerPumpReturn = (uintptr_t)site + sizeof(PUMP_STOCK);
    g_replayCadenceReturn = (uintptr_t)cadence + sizeof(cadenceStock);
    g_replayRulesReturn = (uintptr_t)rules + sizeof(LOAD_RULES_STOCK);
    g_replayPacerReturn = (uintptr_t)pacer + sizeof(PACER_RULES_STOCK);
    const size_t span = (site + sizeof(PUMP_STOCK)) - cadence;
    DWORD oldProtect = 0;

    if (!VirtualProtect(cadence, span, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return;
    }

    DWORD rulesProtect = 0;

    if (!VirtualProtect(rules, sizeof(LOAD_RULES_STOCK), PAGE_EXECUTE_READWRITE, &rulesProtect)) {
        DWORD ignored = 0;
        VirtualProtect(cadence, span, oldProtect, &ignored);
        return;
    }

    DWORD pacerProtect = 0;

    if (!VirtualProtect(pacer, sizeof(PACER_RULES_STOCK), PAGE_EXECUTE_READWRITE, &pacerProtect)) {
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
    writeJump(site, (uintptr_t)replayPlayerPumpHook, sizeof(PUMP_STOCK));
    writeJump(rules, (uintptr_t)loadRulesHook, sizeof(LOAD_RULES_STOCK));
    writeJump(pacer, (uintptr_t)pacerRulesHook, sizeof(PACER_RULES_STOCK));
    DWORD ignored = 0;
    VirtualProtect(cadence, span, oldProtect, &ignored);
    VirtualProtect(rules, sizeof(LOAD_RULES_STOCK), rulesProtect, &ignored);
    VirtualProtect(pacer, sizeof(PACER_RULES_STOCK), pacerProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), cadence, span);
    FlushInstructionCache(GetCurrentProcess(), rules, sizeof(LOAD_RULES_STOCK));
    FlushInstructionCache(GetCurrentProcess(), pacer, sizeof(PACER_RULES_STOCK));
    s_installed = true;

    if (!registerFrameTick(replayPlayerFrameTick)) {
        // Without the frame tick the epoch is never sampled: refuse playback.
        s_installed = false;
    }
}
