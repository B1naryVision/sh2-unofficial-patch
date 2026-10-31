#include "replayTrace.h"
#include "../core/exeBase.h"
#include "../core/hook.h"
#include "../core/readableMemory.h"
#include "choreCodec.h"
#include "replayOriginTables.h"
#include "replayRecorder.h"
#include "replayViewer.h"
#include "spectatorGuard.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

// Ghidra VAs below use ImageBase 0x400000. Runtime addresses are base + RVA.
static const uintptr_t NET_MGR_RVA = 0x17e6a30; // static singleton, VA 0x1be6a30
static const uintptr_t NET_TICK_OFF = 0x06; // authoritative uint16 job tick
static const uintptr_t WORLD_RVA = 0xd781d0;
static const uintptr_t WORLD_MODE_OFF = 0x40089;
// Exe global read by NetGameMgr::post to decide whether to serialise a command
// onto the wire; 2 == networked session. Measured 0 at the main menu.
static const uintptr_t NET_MODE_GLOBAL_RVA = 0x26cd7f0;

static const uintptr_t ADMIT_SITE_RVA = 0x37efb0; // World::vtbl[0x20]
static const uintptr_t QUEUED_EXEC_SITE_RVA = 0x367c61; // NetGameMgr tick list
static const uintptr_t DIRECT_EXEC_SITE_RVA = 0x37f018; // World direct branch

// NetGameMgr::enqueue -- the ONE point every command passes on its way into
// the lockstep job queue, from either direction:
//
//   local   World::admit -> NetGameMgr::post -> enqueue
//   remote  NetGameMgr::onPacket -> createSharedInstance -> serial -> enqueue
//
// The remote route never touches World::admit, so the admission hook cannot
// see a peer's commands at all. Only the remote side is recorded here: local
// commands keep the World::admit path, and recording them twice would double
// every admission and desynchronise the within-tick order.
static const uintptr_t ENQUEUE_SITE_RVA = 0x366c00;

// NetGameMgr::onPacket, [begin, end). A return address inside this body is a
// command that arrived from a peer. The end is the function's last byte plus
// one (final `RET 0xc` at 0x36765c).
static const uint32_t ONPACKET_BEGIN_RVA = 0x367250;
static const uint32_t ONPACKET_END_RVA = 0x367660;

static const uint8_t ADMIT_STOCK[] = {0x55, 0x8b, 0xec, 0x6a, 0xff};
// Byte-identical MSVC prologue to the admission site, hence the same
// re-emission in the trampoline below.
static const uint8_t ENQUEUE_STOCK[] = {0x55, 0x8b, 0xec, 0x6a, 0xff};
static const uint8_t QUEUED_EXEC_STOCK[] = {0x8b, 0x08, 0x8b, 0x0c, 0xb9, 0x8b,
                                            0x11, 0x8b, 0x42, 0x08, 0xff, 0xd0};
static const uint8_t DIRECT_EXEC_STOCK[] = {0x8b, 0x4d, 0x08, 0x8b, 0x11,
                                            0x8b, 0x42, 0x08, 0xff, 0xd0};

static const uint32_t END_JOB_VTABLE_RVA = 0x5f32b0;

// RTTI-derived concrete command classes with a recovered schema, for the
// player slot the log records.
static const uintptr_t TRADE_VTABLE_RVA = 0x5c6190;
static const uintptr_t CREATE_TROOP_VTABLE_RVA = 0x5c69b8;
static const uintptr_t PLACE_BUILDING_VTABLE_RVA = 0x5c5ab4;
static const uintptr_t CHANGE_TAX_VTABLE_RVA = 0x5c5cb0;
static const uintptr_t SET_RATIONS_VTABLE_RVA = 0x5c60a8;
static const uintptr_t SET_CHURCH_VTABLE_RVA = 0x5c6054;
static const uintptr_t SET_FOOD_VTABLE_RVA = 0x5c608c;
static const uintptr_t SET_INN_VTABLE_RVA = 0x5c6070;
static const uintptr_t RETURN_LORD_VTABLE_RVA = 0x5c621c;
static const uintptr_t RETURN_LORD_VTABLE2_RVA = 0x5c6210;
static const uintptr_t LORD_NOT_CONTROLLABLE_VTABLE_RVA = 0x5c6bf4;
static const uintptr_t LORD_NOT_CONTROLLABLE_VTABLE2_RVA = 0x5c6be8;
static const uintptr_t DISBAND_TROOPS_VTABLE_RVA = 0x5c6a40;
static const uintptr_t FORMATE_TROOPS_VTABLE_RVA = 0x5c6c10;
static const uintptr_t SET_TROOP_STANCE_VTABLE_RVA = 0x5c6bbc;
static const uintptr_t MOUNT_HORSES_VTABLE_RVA = 0x5c6bd8;
static const uintptr_t STOP_ALL_TROOPS_VTABLE_RVA = 0x5c7008;
static const uintptr_t SELECT_TROOPS_VTABLE_RVA = 0x5c6fb4;

// Player slot table used by LordNotControllableChore::execute (VA 0xae8bd8,
// slots 1..0x1f). The Keep pair holder is player + 0x674, and the stock getter
// RVA 0x0ede60 reads its pair vector at holder + 0x204 / +0x208. Only the
// first pair's packed handle is needed to attribute a Keep to a player.
static const uintptr_t PLAYER_TABLE_RVA = 0x6e8bd8;
static const uintptr_t PLAYER_KEEP_VECTOR_OFF = 0x674 + 0x204;
static const uint32_t PLAYER_TABLE_SLOTS = 0x20;

static uint32_t s_installStatus = 0;
static uintptr_t s_exeBase = 0;

static ReplayBoundaryEntry *s_boundaries = nullptr;
static uint32_t s_boundaryCount = 0, s_boundaryDropped = 0;

static uint16_t s_lastAdmissionTick = 0;
static uint16_t s_lastExecutionTick = 0;
static uint16_t s_admissionOrder = 0;
static uint16_t s_executionOrder = 0;
static bool s_haveAdmissionTick = false;
static bool s_haveExecutionTick = false;
static bool s_haveClock = false;
static uint16_t s_lastEngineTick = 0;
static uint32_t s_simulationTick = 0;

// Trampoline state must have external linkage: MinGW GAS cannot resolve local
// static symbols referenced by naked inline assembly.
uintptr_t g_replayTraceAdmitReturn = 0;
uintptr_t g_replayTraceQueuedExecReturn = 0;
uintptr_t g_replayTraceDirectExecReturn = 0;
uintptr_t g_replayTraceAdmitEvent = 0;
uintptr_t g_replayTraceAdmitCaller = 0;
uintptr_t g_replayTraceAdmitFrame = 0;
// Set by the admission capture to refuse this admission (admissionHook).
uint32_t g_replayTraceDropAdmission = 0;
uintptr_t g_replayTraceExecEvent = 0;
uintptr_t g_replayTraceExecMethod = 0;
uintptr_t g_replayTraceExecJob = 0;
uint32_t g_replayTraceExecRoute = 0;
uintptr_t g_replayTraceEnqueueReturn = 0;
uintptr_t g_replayTraceEnqueueEvent = 0;
uintptr_t g_replayTraceEnqueueCaller = 0;
uintptr_t g_replayTraceEnqueueFrame = 0;

static uint16_t currentTick() {
    return *(volatile uint16_t *)(s_exeBase + NET_MGR_RVA + NET_TICK_OFF);
}

static uint32_t liftTick(uint16_t engineTick) {
    if (!s_haveClock) {
        s_haveClock = true;
        s_lastEngineTick = engineTick;
        s_simulationTick = engineTick;
        return s_simulationTick;
    }

    // Unsigned-16 subtraction is the protocol's own modulo-0x10000 distance.
    // This hook observes every successful tick through EndJobChore, so the
    // forward distance is never ambiguous even across the wrap.
    uint16_t delta = (uint16_t)(engineTick - s_lastEngineTick);
    s_simulationTick += delta;
    s_lastEngineTick = engineTick;
    return s_simulationTick;
}

static uint32_t exeRva(uintptr_t address) {
    return address >= s_exeBase ? (uint32_t)(address - s_exeBase) : 0xffffffffu;
}

// Our DLL loads well ABOVE the executable, so a caller inside it still produces
// a large positive "RVA" rather than the 0xffffffff sentinel. Distinguishing an
// injected command from one the game originated therefore needs the real image
// size, read once from the PE headers.
static uint32_t s_exeImageSize = 0;

static uint32_t exeImageSize() {
    if (s_exeImageSize || !s_exeBase) {
        return s_exeImageSize;
    }

    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)s_exeBase;

    if (!observerReadable(s_exeBase, sizeof(IMAGE_DOS_HEADER)) ||
        dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }

    const uintptr_t ntAddress = s_exeBase + (uintptr_t)dos->e_lfanew;

    if (!observerReadable(ntAddress, sizeof(IMAGE_NT_HEADERS32))) {
        return 0;
    }

    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)ntAddress;

    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }

    s_exeImageSize = nt->OptionalHeader.SizeOfImage;
    return s_exeImageSize;
}

static bool callerIsForeign(uint32_t callerRva) {
    if (callerRva == 0xffffffffu) {
        return true;
    }

    const uint32_t size = exeImageSize();
    return size != 0 && callerRva >= size;
}

struct TypeInfo {
    bool ai;
    bool system;
};

static bool textContains(const char *text, const char *needle) {
    if (!text || !needle || !*needle) {
        return false;
    }

    for (const char *p = text; *p; ++p) {
        const char *a = p;
        const char *b = needle;

        while (*a && *b && *a == *b) {
            ++a;
            ++b;
        }

        if (!*b) {
            return true;
        }
    }

    return false;
}

static bool textStartsWith(const char *text, const char *prefix) {
    if (!text || !prefix || !*prefix) {
        return false;
    }

    while (*prefix) {
        if (*text++ != *prefix++) {
            return false;
        }
    }

    return true;
}

static TypeInfo typeInfo(uintptr_t vtable) {
    TypeInfo result = {false, false};

    if (vtable < s_exeBase + 4 || !observerReadable(vtable - 4, sizeof(uintptr_t))) {
        return result;
    }

    // MSVC x86: vtable[-1] -> COL and COL+0xc -> TypeDescriptor;
    // TypeDescriptor+8 is the NUL-terminated mangled class name.
    uintptr_t col = *(uintptr_t *)(vtable - 4);

    if (!observerReadable(col + 0x0c, sizeof(uintptr_t))) {
        return result;
    }

    uintptr_t descriptor = *(uintptr_t *)(col + 0x0c);
    const char *name = (const char *)(descriptor + 8);

    if (!observerReadable((uintptr_t)name, 128)) {
        return result;
    }

    size_t length = 0;

    while (length < 128 && name[length]) {
        ++length;
    }

    if (length == 128) {
        return result;
    }

    // The concrete AI Chores are named both `AIAttack...` and `AIMove...`.
    result.ai = textStartsWith(name, ".?AVAI") || textContains(name, "CreateAI");
    result.system = textContains(name, "EndJob") || textContains(name, "GameReady") ||
                    textContains(name, "StartGame") || textContains(name, "Peer");
    return result;
}

static bool inRanges(const ReplayRoutineRange *ranges, size_t count, uint32_t rva) {
    if (rva == 0xffffffffu) {
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        if (rva >= ranges[i].begin && rva < ranges[i].end) {
            return true;
        }
    }

    return false;
}

static bool nearUiProcess(uint32_t rva) {
    if (rva == 0xffffffffu) {
        return false;
    }

    for (const uint32_t start : REPLAY_UI_PROCESS_RVAS) {
        if (rva >= start && rva - start < REPLAY_UI_PROCESS_SPAN) {
            return true;
        }
    }

    return false;
}

static bool nearKnownUiPost(uint32_t rva) {
    return inRanges(
        REPLAY_UI_POST_RANGES, sizeof(REPLAY_UI_POST_RANGES) / sizeof(REPLAY_UI_POST_RANGES[0]), rva
    );
}

static bool nearKnownAiPost(uint32_t rva) {
    return inRanges(
        REPLAY_AI_POST_RANGES, sizeof(REPLAY_AI_POST_RANGES) / sizeof(REPLAY_AI_POST_RANGES[0]), rva
    );
}

static bool nearKnownSystemPost(uint32_t rva) {
    return inRanges(
        REPLAY_SYSTEM_POST_RANGES,
        sizeof(REPLAY_SYSTEM_POST_RANGES) / sizeof(REPLAY_SYSTEM_POST_RANGES[0]), rva
    );
}

static uint8_t commandSchema(uint32_t vtableRva) {
    if (vtableRva == TRADE_VTABLE_RVA) {
        return 1;
    }

    if (vtableRva == CREATE_TROOP_VTABLE_RVA) {
        return 2;
    }

    if (vtableRva == SET_FOOD_VTABLE_RVA) {
        return 5;
    }

    if (vtableRva == CHANGE_TAX_VTABLE_RVA || vtableRva == SET_RATIONS_VTABLE_RVA ||
        vtableRva == SET_CHURCH_VTABLE_RVA || vtableRva == SET_INN_VTABLE_RVA) {
        return 3;
    }

    if (vtableRva == PLACE_BUILDING_VTABLE_RVA) {
        return 4;
    }

    if (vtableRva == DISBAND_TROOPS_VTABLE_RVA || vtableRva == 0x5c6a4c) {
        return 6;
    }

    if (vtableRva == FORMATE_TROOPS_VTABLE_RVA) {
        return 7;
    }

    if (vtableRva == SET_TROOP_STANCE_VTABLE_RVA || vtableRva == 0x5c6bb0) {
        return 8;
    }

    if (vtableRva == MOUNT_HORSES_VTABLE_RVA) {
        return 9;
    }

    if (vtableRva == STOP_ALL_TROOPS_VTABLE_RVA || vtableRva == 0x5c6ffc) {
        return 10;
    }

    if (vtableRva == SELECT_TROOPS_VTABLE_RVA || vtableRva == 0x5c6fa8) {
        return 11;
    }

    if (vtableRva == RETURN_LORD_VTABLE_RVA || vtableRva == RETURN_LORD_VTABLE2_RVA) {
        return 12;
    }

    if (vtableRva == LORD_NOT_CONTROLLABLE_VTABLE_RVA ||
        vtableRva == LORD_NOT_CONTROLLABLE_VTABLE2_RVA) {
        return 13;
    }

    return 0;
}

// ReturnLordChore carries no player field: execute resolves only the packed
// Keep handle at +0x0c. The Keep pair that SubPanelKeep::process copies into
// the Chore is the issuing player's own, so the slot is derived by matching
// that handle in the stock player table.
static uint32_t playerSlotForKeepHandle(uint32_t handle) {
    if (!handle || !s_exeBase) {
        return 0xffffffffu;
    }

    for (uint32_t slot = 1; slot < PLAYER_TABLE_SLOTS; ++slot) {
        const uintptr_t entry = s_exeBase + PLAYER_TABLE_RVA + slot * 4;

        if (!observerReadable(entry, 4)) {
            continue;
        }

        const uintptr_t player = *(uintptr_t *)entry;

        if (!player) {
            continue;
        }

        const uintptr_t vec = player + PLAYER_KEEP_VECTOR_OFF;

        if (!observerReadable(vec, 8)) {
            continue;
        }

        const uintptr_t begin = *(uintptr_t *)vec;
        const uintptr_t end = *(uintptr_t *)(vec + 4);

        if (!begin || end < begin || end - begin < 8 || !observerReadable(begin, 8)) {
            continue;
        }

        if (*(uint32_t *)(begin + 4) == handle) {
            return slot;
        }
    }

    return 0xffffffffu;
}

static uint32_t commandPlayerSlot(uintptr_t event, uint8_t schema) {
    if (!event) {
        return 0xffffffffu;
    }

    switch (schema) {
    case 1:
        return *(uint32_t *)(event + 0x18); // TradeChore player id
    case 2:
        return *(uint8_t *)(event + 0x09); // CreateTroop slot
    case 3:
        return *(uint32_t *)(event + 0x0c); // policy player index
    case 4:
        return *(uint8_t *)(event + 0x13); // placement player slot
    case 5:
        return *(uint32_t *)(event + 0x10); // food Chore player index
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
        return *(uint32_t *)(event + 0x10) >> 24; // selection id prefix
    case 11:
        return *(uint32_t *)(event + 0x1c); // SelectTroopsChore slot
    case 12:
        return playerSlotForKeepHandle(*(uint32_t *)(event + 0x0c));
    case 13:
        return *(uint32_t *)(event + 0x14); // explicit player argument
    default:
        return 0xffffffffu;
    }
}

// One frame up from a hooked prologue: the trampoline captured the caller's
// EBP, so [EBP+4] is the caller's own return address.
static uint32_t parentCallerRvaOf(uintptr_t frame) {
    if (!observerReadable(frame, 8)) {
        return 0xffffffffu;
    }

    uintptr_t parentReturn = *(uintptr_t *)(frame + 4);
    return exeRva(parentReturn);
}

static uint16_t nextOrder(uint8_t phase, uint16_t tick) {
    if (phase == 1) {
        if (!s_haveAdmissionTick || tick != s_lastAdmissionTick) {
            s_lastAdmissionTick = tick;
            s_admissionOrder = 0;
            s_haveAdmissionTick = true;
        }

        return s_admissionOrder++;
    }

    if (!s_haveExecutionTick || tick != s_lastExecutionTick) {
        s_lastExecutionTick = tick;
        s_executionOrder = 0;
        s_haveExecutionTick = true;
    }

    return s_executionOrder++;
}

static void appendBoundary(
    uint8_t phase, uint8_t route, uintptr_t event, uintptr_t method, uint16_t tick,
    uint32_t simulationTick, uint16_t order
) {
    if (!replayRecorderIsRecording() || !s_boundaries) {
        return;
    }

    uintptr_t vtable = 0;

    if (event && observerReadable(event, sizeof(uintptr_t))) {
        vtable = *(uintptr_t *)event;
    }

    const uint32_t vtableRva = exeRva(vtable);

    // NetGameMgr appends one EndJobChore to every successful tick. It is the
    // clock delimiter, not an action. It still contributes to `order` above.
    if (phase == 2 && vtableRva == END_JOB_VTABLE_RVA) {
        return;
    }

    if (s_boundaryCount == REPLAY_BOUNDARY_CAPACITY) {
        ++s_boundaryDropped;
        return;
    }

    ReplayBoundaryEntry &entry = s_boundaries[s_boundaryCount];
    entry = {};
    entry.trace.serial = s_boundaryCount + 1;
    entry.trace.simulationTick = simulationTick;
    entry.trace.engineTick = tick;
    entry.trace.order = order;
    entry.trace.phase = phase;
    entry.trace.route = route;
    entry.trace.vtableRva = vtableRva;
    entry.trace.methodRva = exeRva(method);
    entry.eventToken = (uint32_t)event;
    entry.netCounter = *(uint8_t *)(s_exeBase + NET_MGR_RVA + 4);

    if (phase == 2 && route == 0 && observerReadable(g_replayTraceExecJob, 0x18)) {
        entry.scheduledJob = *(uint16_t *)(g_replayTraceExecJob + 0x14);
        entry.jobValid = 1;
    }

    // Published after the entry, for the crash filter's reader.
    __atomic_store_n(&s_boundaryCount, s_boundaryCount + 1, __ATOMIC_RELEASE);
}

static uint8_t classifyOrigin(const TypeInfo &type, uint32_t callerRva, uint32_t parentRva) {
    if (type.ai || nearKnownAiPost(callerRva) || nearKnownAiPost(parentRva)) {
        return 2;
    }

    if (type.system || nearKnownSystemPost(callerRva) || nearKnownSystemPost(parentRva)) {
        return 3;
    }

    if (nearUiProcess(callerRva) || nearUiProcess(parentRva) || nearKnownUiPost(callerRva) ||
        nearKnownUiPost(parentRva)) {
        return 1;
    }

    return 0;
}

// `originOverride` is 0 for the ordinary World admission path, which derives
// provenance from the call site. The multiplayer receive path passes 4: a
// command that arrived over the wire has no local call site to classify.
static void appendCommandRecord(
    uintptr_t event, uint16_t tick, uint32_t simulationTick, uint16_t order, uint32_t callerRva,
    uint32_t parentRva, uint8_t originOverride
) {
    // Only a recording or the spectator guard needs any of this.
    if (!replayRecorderIsRecording() && !spectatorGuardActive()) {
        return;
    }

    if (!event || !observerReadable(event, sizeof(uintptr_t))) {
        return;
    }

    const uintptr_t vtable = *(uintptr_t *)event;
    const uint32_t vtableRva = exeRva(vtable);
    const uint8_t schema = commandSchema(vtableRva);
    uint8_t origin = originOverride;

    if (!origin) {
        origin = classifyOrigin(typeInfo(vtable), callerRva, parentRva);
    }

    // Backstop of the spectator contract. A UI-origin command reaching
    // admission while playback is armed means live local input got past the
    // window-message guard (a device read directly, say). It is refused here,
    // at the only place that sees every admission, rather than let it change
    // the replay. The in-game menu is the exception, as in the window guard:
    // its clicks are let through so the viewer can leave.
    if (origin == 1 && spectatorGuardActive() && !originOverride && !replayViewerMenuOpen()) {
        g_replayTraceDropAdmission = 1;
        return;
    }

    if (!replayRecorderIsRecording()) {
        return;
    }

    // 0x1000 is the stock send path's capacity, so nothing the engine can
    // transmit overflows it; choreCodecSerialize returns nothing for a payload
    // larger than the buffer it is given. Static rather than stack: this hook
    // runs on the simulation thread deep inside game code, and the buffer is
    // copied out before the hook returns.
    static uint8_t stock[0x1000];
    uint32_t stockIndex = 0xffffffffu;
    const size_t stockLength =
        choreCodecSerialize(reinterpret_cast<void *>(event), stock, sizeof(stock), &stockIndex);

    ReplayEventRecord logged = {};
    logged.simulationTick = simulationTick;
    logged.engineTick = tick;
    logged.order = order;
    logged.classIndex = stockIndex <= 0xfffeu ? (uint16_t)stockIndex : 0xffffu;
    logged.payloadLength = (uint16_t)stockLength;
    logged.vtableRva = vtableRva;
    logged.callerRva = callerRva;
    logged.parentCallerRva = parentRva;
    logged.playerSlot = commandPlayerSlot(event, schema);
    logged.originHint = origin;
    logged.schema = schema;

    if (callerIsForeign(callerRva)) {
        logged.flags |= REPLAY_FLAG_FOREIGN_CALLER;
    }

    replayRecorderNote(logged, stockLength ? stock : nullptr);
}

extern "C" void replayTraceCaptureAdmission() {
    uintptr_t event = g_replayTraceAdmitEvent;
    uintptr_t method = 0;

    if (event) {
        uintptr_t vtable = *(uintptr_t *)event;

        if (vtable) {
            method = *(uintptr_t *)(vtable + 8);
        }
    }

    uint8_t mode = *(volatile uint8_t *)(s_exeBase + WORLD_RVA + WORLD_MODE_OFF);
    uint16_t tick = currentTick();
    uint32_t simulationTick = liftTick(tick);
    uint16_t order = nextOrder(1, tick);
    appendBoundary(1, mode, event, method, tick, simulationTick, order);
    appendCommandRecord(
        event, tick, simulationTick, order, exeRva(g_replayTraceAdmitCaller),
        parentCallerRvaOf(g_replayTraceAdmitFrame), 0
    );
}

// The multiplayer receive path.
//
// Fires on every enqueue, local and remote, and records only the remote ones.
// A caller inside NetGameMgr::onPacket is a command that came off the wire.
// Local enqueues are dropped, because World::admit already recorded them.
//
// EndJobChore is skipped too: it is the lockstep tick delimiter, generated
// independently by every peer, and replaying one would inject a second clock.
extern "C" void replayTraceCaptureEnqueue() {
    if (!replayRecorderIsRecording()) {
        return;
    }

    const uintptr_t event = g_replayTraceEnqueueEvent;
    const uint32_t callerRva = exeRva(g_replayTraceEnqueueCaller);

    if (callerRva < ONPACKET_BEGIN_RVA || callerRva >= ONPACKET_END_RVA) {
        return;
    }

    if (!event || !observerReadable(event, sizeof(uintptr_t))) {
        return;
    }

    const uintptr_t vtable = *(uintptr_t *)event;

    if (exeRva(vtable) == END_JOB_VTABLE_RVA) {
        return;
    }

    uintptr_t method = 0;

    if (vtable && observerReadable(vtable + 8, sizeof(uintptr_t))) {
        method = *(uintptr_t *)(vtable + 8);
    }

    const uint16_t tick = currentTick();
    const uint32_t simulationTick = liftTick(tick);
    // Phase 1's order counter, shared with local admissions on purpose: within
    // a tick the two sources interleave, and replay has to reproduce that one
    // combined order rather than two independent ones.
    const uint16_t order = nextOrder(1, tick);
    // Route byte 4 marks the receive path, distinct from the World mode bytes
    // the admission path records.
    appendBoundary(1, 4, event, method, tick, simulationTick, order);
    appendCommandRecord(
        event, tick, simulationTick, order, callerRva, parentCallerRvaOf(g_replayTraceEnqueueFrame),
        4
    );
}

extern "C" void replayTraceCaptureExecution() {
    const uint16_t tick = currentTick();
    const uint32_t simulationTick = liftTick(tick);
    const uint16_t order = nextOrder(2, tick);
    appendBoundary(
        2, (uint8_t)g_replayTraceExecRoute, g_replayTraceExecEvent, g_replayTraceExecMethod, tick,
        simulationTick, order
    );
}

__declspec(naked) static void admissionHook() {
    __asm__ volatile("movl (%esp), %eax\n\t"
                     "movl %eax, _g_replayTraceAdmitCaller\n\t"
                     "movl %ebp, %eax\n\t"
                     "movl %eax, _g_replayTraceAdmitFrame\n\t"
                     "movl 4(%esp), %eax\n\t"
                     "movl %eax, _g_replayTraceAdmitEvent\n\t"
                     "pushal\n\t"
                     "pushfl\n\t"
                     "call _replayTraceCaptureAdmission\n\t"
                     "popfl\n\t"
                     "popal\n\t"
                     // Flags are dead at a function entry.
                     "cmpl $0, _g_replayTraceDropAdmission\n\t"
                     "jne 1f\n\t"
                     // Re-emit the exact five-byte prologue.
                     "pushl %ebp\n\t"
                     "movl %esp, %ebp\n\t"
                     "pushl $-1\n\t"
                     "jmp *_g_replayTraceAdmitReturn\n\t"
                     // Refused: do what the stock epilogue does with its by-value handle
                     // (VA 0x77f022..0x77f057) -- drop the one reference, destroy through
                     // the deleter at [obj-8+4] on the last one -- and `ret 4`. ESI holds
                     // ESP so the deleter's own stack cleanup cannot matter.
                     "1:\n\t"
                     "movl $0, _g_replayTraceDropAdmission\n\t"
                     "pushl %esi\n\t"
                     "movl %esp, %esi\n\t"
                     "movl 8(%esp), %eax\n\t"
                     "testl %eax, %eax\n\t"
                     "jz 2f\n\t"
                     "leal -8(%eax), %edx\n\t"
                     "movl $-1, %ecx\n\t"
                     "lock xaddl %ecx, (%edx)\n\t"
                     "cmpl $1, %ecx\n\t"
                     "jne 2f\n\t"
                     "movl 4(%edx), %ecx\n\t"
                     "movl (%ecx), %eax\n\t"
                     "addl $8, %edx\n\t"
                     "pushl %edx\n\t"
                     "call *(%eax)\n\t"
                     "2:\n\t"
                     "movl %esi, %esp\n\t"
                     "popl %esi\n\t"
                     "ret $4\n\t");
}

// Same shape as admissionHook, against a byte-identical MSVC prologue. At
// entry ESP holds the return address and ESP+4 the Ptr<Chore> argument; ECX
// holds the queue object and is left untouched for the callee.
__declspec(naked) static void enqueueHook() {
    __asm__ volatile("movl (%esp), %eax\n\t"
                     "movl %eax, _g_replayTraceEnqueueCaller\n\t"
                     "movl %ebp, %eax\n\t"
                     "movl %eax, _g_replayTraceEnqueueFrame\n\t"
                     "movl 4(%esp), %eax\n\t"
                     "movl %eax, _g_replayTraceEnqueueEvent\n\t"
                     "pushal\n\t"
                     "pushfl\n\t"
                     "call _replayTraceCaptureEnqueue\n\t"
                     "popfl\n\t"
                     "popal\n\t"
                     // Re-emit the exact five-byte prologue.
                     "pushl %ebp\n\t"
                     "movl %esp, %ebp\n\t"
                     "pushl $-1\n\t"
                     "jmp *_g_replayTraceEnqueueReturn\n\t");
}

__declspec(naked) static void queuedExecutionHook() {
    __asm__ volatile(
        // Re-emit the list lookup and vtable slot-2 load.
        "movl %eax, _g_replayTraceExecJob\n\t"
        "movl (%eax), %ecx\n\t"
        "movl (%ecx,%edi,4), %ecx\n\t"
        "movl (%ecx), %edx\n\t"
        "movl 8(%edx), %eax\n\t"
        "movl %ecx, _g_replayTraceExecEvent\n\t"
        "movl %eax, _g_replayTraceExecMethod\n\t"
        "movl $0, _g_replayTraceExecRoute\n\t"
        "pushal\n\t"
        "pushfl\n\t"
        "call _replayTraceCaptureExecution\n\t"
        "popfl\n\t"
        "popal\n\t"
        "call *%eax\n\t"
        "jmp *_g_replayTraceQueuedExecReturn\n\t"
    );
}

__declspec(naked) static void directExecutionHook() {
    __asm__ volatile(
        // Re-emit World::dispatch's direct-execution branch.
        "movl 8(%ebp), %ecx\n\t"
        "movl (%ecx), %edx\n\t"
        "movl 8(%edx), %eax\n\t"
        "movl %ecx, _g_replayTraceExecEvent\n\t"
        "movl %eax, _g_replayTraceExecMethod\n\t"
        "movl $1, _g_replayTraceExecRoute\n\t"
        "pushal\n\t"
        "pushfl\n\t"
        "call _replayTraceCaptureExecution\n\t"
        "popfl\n\t"
        "popal\n\t"
        "call *%eax\n\t"
        "jmp *_g_replayTraceDirectExecReturn\n\t"
    );
}

void installReplayTrace() {
    s_exeBase = exeBase();
    uint8_t *admit = (uint8_t *)(s_exeBase + ADMIT_SITE_RVA);
    uint8_t *queued = (uint8_t *)(s_exeBase + QUEUED_EXEC_SITE_RVA);
    uint8_t *direct = (uint8_t *)(s_exeBase + DIRECT_EXEC_SITE_RVA);
    uint8_t *enqueue = (uint8_t *)(s_exeBase + ENQUEUE_SITE_RVA);

    // All-or-nothing: an unknown executable must retain all four stock sites.
    if (memcmp(admit, ADMIT_STOCK, sizeof(ADMIT_STOCK)) != 0 ||
        memcmp(queued, QUEUED_EXEC_STOCK, sizeof(QUEUED_EXEC_STOCK)) != 0 ||
        memcmp(direct, DIRECT_EXEC_STOCK, sizeof(DIRECT_EXEC_STOCK)) != 0 ||
        memcmp(enqueue, ENQUEUE_STOCK, sizeof(ENQUEUE_STOCK)) != 0) {
        return;
    }

    g_replayTraceAdmitReturn = (uintptr_t)admit + sizeof(ADMIT_STOCK);
    g_replayTraceQueuedExecReturn = (uintptr_t)queued + sizeof(QUEUED_EXEC_STOCK);
    g_replayTraceDirectExecReturn = (uintptr_t)direct + sizeof(DIRECT_EXEC_STOCK);
    g_replayTraceEnqueueReturn = (uintptr_t)enqueue + sizeof(ENQUEUE_STOCK);

    installHook(admit, reinterpret_cast<void *>(admissionHook), sizeof(ADMIT_STOCK));
    installHook(queued, reinterpret_cast<void *>(queuedExecutionHook), sizeof(QUEUED_EXEC_STOCK));
    installHook(direct, reinterpret_cast<void *>(directExecutionHook), sizeof(DIRECT_EXEC_STOCK));
    installHook(enqueue, reinterpret_cast<void *>(enqueueHook), sizeof(ENQUEUE_STOCK));
    s_installStatus = 0xf;
}

bool replayTraceInstalled() { return s_installStatus == 0xf; }

bool replayTraceSessionIsNetworked() {
    if (!s_exeBase) {
        return false;
    }

    const uintptr_t netModeGlobal = s_exeBase + NET_MODE_GLOBAL_RVA;
    return *(volatile uint32_t *)netModeGlobal == 2;
}

bool replayTraceAcquire() {
    if (!s_boundaries) {
        s_boundaries = (ReplayBoundaryEntry *)VirtualAlloc(
            NULL, REPLAY_BOUNDARY_CAPACITY * sizeof(ReplayBoundaryEntry), MEM_COMMIT | MEM_RESERVE,
            PAGE_READWRITE
        );
    }

    return s_boundaries != nullptr;
}

void replayTraceRelease() {
    replayTraceRecordingReset();

    if (s_boundaries) {
        VirtualFree(s_boundaries, 0, MEM_RELEASE);
        s_boundaries = nullptr;
    }
}

void replayTraceRecordingReset() {
    __atomic_store_n(&s_boundaryCount, 0u, __ATOMIC_RELEASE);
    s_boundaryDropped = 0;
}

void replayTraceBoundaryView(
    const ReplayBoundaryEntry **entries, uint32_t *count, uint32_t *dropped, uint32_t *hooks
) {
    *entries = s_boundaries;
    *count = __atomic_load_n(&s_boundaryCount, __ATOMIC_ACQUIRE);
    *dropped = s_boundaryDropped;
    *hooks = s_installStatus;
}

uint8_t replayTraceClassifyCaller(uint32_t callerRva, uint32_t parentCallerRva) {
    const TypeInfo none = {false, false};
    return classifyOrigin(none, callerRva, parentCallerRva);
}

uint32_t replayTraceCurrentSimulationTick() { return s_exeBase ? liftTick(currentTick()) : 0; }
