#include "replayTrace.h"
#include "choreCodec.h"
#include "spectatorGuard.h"
#include "replayRecorder.h"
#include "../core/hook.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

// Ghidra VAs below use ImageBase 0x400000. Runtime addresses are base + RVA.
static const uintptr_t NET_MGR_RVA = 0x17e6a30; // static singleton, VA 0x1be6a30
static const uintptr_t NET_TICK_OFF = 0x06;      // authoritative uint16 job tick
static const uintptr_t WORLD_RVA = 0xd781d0;
static const uintptr_t WORLD_MODE_OFF = 0x40089;
// Exe global read by NetGameMgr::post to decide whether to serialise a command
// onto the wire; 2 == networked session. Measured 0 at the main menu.
static const uintptr_t NET_MODE_GLOBAL_RVA = 0x26cd7f0;

static const uintptr_t ADMIT_SITE_RVA = 0x37efb0;       // World::vtbl[0x20]
static const uintptr_t QUEUED_EXEC_SITE_RVA = 0x367c61; // NetGameMgr tick list
static const uintptr_t DIRECT_EXEC_SITE_RVA = 0x37f018; // World direct branch

// NetGameMgr::enqueue -- the ONE point every command passes on
// its way into the lockstep job queue, from either direction:
//
//   local   World::admit -> NetGameMgr::post -> enqueue
//   remote  NetGameMgr::onPacket -> createSharedInstance -> serial -> enqueue
//
// The remote route never touches World::admit, so the three hooks above cannot
// see a peer's commands at all. This is not a misclassification -- an earlier
// note predicted remote input would arrive classified as AI or system -- it is
// a total absence, which would have made a multiplayer container look clean on
// every command counter while replaying only half the match.
//
// Only the remote side is recorded here. Local commands keep the proven
// World::admit path exactly as it is; recording them twice would double every
// admission and desynchronise the within-tick order the replay depends on.
static const uintptr_t ENQUEUE_SITE_RVA = 0x366c00;

// NetGameMgr::onPacket, [begin, end). A return address inside this body is a
// command that arrived from a peer. The end is the function's last byte plus
// one, taken from the disassembly (final `RET 0xc` at 0x36765c).
static const uint32_t ONPACKET_BEGIN_RVA = 0x367250;
static const uint32_t ONPACKET_END_RVA = 0x367660;

static const uint8_t ADMIT_STOCK[] = {0x55, 0x8b, 0xec, 0x6a, 0xff};
// Byte-identical MSVC prologue to the admission site, hence the same
// re-emission in the trampoline below.
static const uint8_t ENQUEUE_STOCK[] = {0x55, 0x8b, 0xec, 0x6a, 0xff};
static const uint8_t QUEUED_EXEC_STOCK[] = {
    0x8b, 0x08, 0x8b, 0x0c, 0xb9, 0x8b, 0x11, 0x8b, 0x42, 0x08, 0xff, 0xd0
};
static const uint8_t DIRECT_EXEC_STOCK[] = {
    0x8b, 0x4d, 0x08, 0x8b, 0x11, 0x8b, 0x42, 0x08, 0xff, 0xd0
};

static const uint32_t TRACE_RING_CAPACITY = 128;
static const uint16_t SNAPSHOT_CAPACITY = 40; // header + records = 984 bytes
static const uint32_t COMMAND_RING_CAPACITY = 256;
static const uint16_t COMMAND_SNAPSHOT_CAPACITY = 12; // 24 + 12 * 80 = 984 bytes
static const uint32_t END_JOB_VTABLE_RVA = 0x5f32b0;

// RTTI-derived concrete command classes. Entries here only identify schemas
// already recovered, never unclassified commands.
static const uintptr_t TRADE_VTABLE_RVA = 0x5c6190;
static const uintptr_t CREATE_TROOP_VTABLE_RVA = 0x5c69b8;
static const uintptr_t CREATE_SIEGE_VTABLE_RVA = 0x5c68fc;
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
static const uintptr_t CREATE_SIEGE_VTABLE2_RVA = 0x5c68f0;

// Player slot table used by LordNotControllableChore::execute (VA 0xae8bd8,
// slots 1..0x1f). The Keep pair holder is player + 0x674, and the stock getter
// RVA 0x0ede60 reads its pair vector at holder + 0x204 / +0x208. Only the
// first pair's packed handle is needed to attribute a Keep to a player.
static const uintptr_t PLAYER_TABLE_RVA = 0x6e8bd8;
static const uintptr_t PLAYER_KEEP_VECTOR_OFF = 0x674 + 0x204;
static const uint32_t PLAYER_TABLE_SLOTS = 0x20;
static const uintptr_t DISBAND_TROOPS_VTABLE_RVA = 0x5c6a40;
static const uintptr_t FORMATE_TROOPS_VTABLE_RVA = 0x5c6c10;
static const uintptr_t SET_TROOP_STANCE_VTABLE_RVA = 0x5c6bbc;
static const uintptr_t MOUNT_HORSES_VTABLE_RVA = 0x5c6bd8;
static const uintptr_t STOP_ALL_TROOPS_VTABLE_RVA = 0x5c7008;
static const uintptr_t SELECT_TROOPS_VTABLE_RVA = 0x5c6fb4;

static ReplayTraceRecord s_ring[TRACE_RING_CAPACITY];
static uint32_t s_totalRecords = 0;
static ReplayCommandRecord s_commandRing[COMMAND_RING_CAPACITY];
static uint32_t s_totalCommandRecords = 0;
static uint32_t s_installStatus = 0;
static uintptr_t s_exeBase = 0;

typedef ReplayBoundaryEntry BoundaryEntry;
#pragma pack(push, 1)
struct BoundaryHeader {
    char magic[8];
    uint32_t version, entrySize, count, epoch, dropped, hooks;
};
#pragma pack(pop)
static_assert(sizeof(BoundaryEntry) == 32, "boundary entry layout");
static_assert(sizeof(BoundaryHeader) == 32, "boundary header layout");
static const uint32_t BOUNDARY_CAPACITY = REPLAY_BOUNDARY_CAPACITY;
static BoundaryEntry s_boundaries[BOUNDARY_CAPACITY];
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
uintptr_t g_replayTraceExecEvent = 0;
uintptr_t g_replayTraceExecMethod = 0;
uintptr_t g_replayTraceExecJob = 0;
uint32_t g_replayTraceExecRoute = 0;
uintptr_t g_replayTraceEnqueueReturn = 0;
uintptr_t g_replayTraceEnqueueEvent = 0;
uintptr_t g_replayTraceEnqueueCaller = 0;
uintptr_t g_replayTraceEnqueueFrame = 0;

// Receive-path counters, reported by replayTraceNetState.
static uint32_t s_localEnqueues = 0;
static uint32_t s_remoteCommands = 0;
static uint32_t s_remoteEndJobs = 0;
static uint32_t s_remoteRejected = 0;

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


static bool readableRange(uintptr_t address, size_t length) {
    if (!address || !length || address + length < address) {
        return false;
    }

    uintptr_t cur = address;
    const uintptr_t end = address + length;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((const void *)cur, &mbi, sizeof(mbi)) != sizeof(mbi)) {
            return false;
        }
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) ||
            (mbi.Protect & PAGE_NOACCESS)) {
            return false;
        }
        uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (regionEnd <= cur) {
            return false;
        }
        cur = regionEnd;
    }
    return true;
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
    if (!readableRange(s_exeBase, sizeof(IMAGE_DOS_HEADER)) ||
        dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    const uintptr_t ntAddress = s_exeBase + (uintptr_t)dos->e_lfanew;
    if (!readableRange(ntAddress, sizeof(IMAGE_NT_HEADERS32))) {
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

static uint32_t fnv1a32(const void *data, size_t length) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; ++i) {
        hash ^= p[i];
        hash *= 16777619u;
    }
    return hash;
}

struct TypeInfo {
    uint32_t nameHash;
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
    TypeInfo result = {0, false, false};
    if (vtable < s_exeBase + 4 ||
        !readableRange(vtable - 4, sizeof(uintptr_t))) {
        return result;
    }

    // MSVC x86: vtable[-1] -> COL and COL+0xc -> TypeDescriptor;
    // TypeDescriptor+8 is the NUL-terminated mangled class name.
    uintptr_t col = *(uintptr_t *)(vtable - 4);
    if (!readableRange(col + 0x0c, sizeof(uintptr_t))) {
        return result;
    }
    uintptr_t descriptor = *(uintptr_t *)(col + 0x0c);
    if (!readableRange(descriptor + 8, 1)) {
        return result;
    }

    const char *name = (const char *)(descriptor + 8);
    if (!readableRange((uintptr_t)name, 128)) {
        return result;
    }
    size_t length = 0;
    while (length < 128 && name[length]) {
        ++length;
    }
    if (length == 128) {
        return result;
    }

    result.nameHash = fnv1a32(name, length);
    // The concrete AI Chores are named both `AIAttack...` and
    // `AIMove...`; the old `AIA` substring test missed the latter and made
    // autonomous admissions look like unknown provenance.
    result.ai = textStartsWith(name, ".?AVAI") ||
                textContains(name, "CreateAI");
    result.system = textContains(name, "EndJob") ||
                    textContains(name, "GameReady") ||
                    textContains(name, "StartGame") ||
                    textContains(name, "Peer");
    return result;
}

// These are the concrete pane process entries that already have a proven
// human-control interpretation. The parent frame is a hint only: it never
// upgrades an unknown schema into an accepted replay command.
static const uintptr_t UI_PROCESS_RVAS[] = {
    0x203990, // Base
    0x20a210, // Armoury
    0x20b150, // Blacksmith workshop
    0x20e450, // Carter post
    0x210ea0, // Granary
    0x214820, // Inn
    0x216340, // Kitchen
    0x21da50, // Market
    0x21e400, // Treasury
    0x2206d0, // Barracks
    0x223be0, // Engineers guild
    0x225490, // Mercenary post
    0x2284d0, // Monastery
    0x228c50, // Siege camp
    0x22c090, // Church
    0x22d5d0, // Constructing building
    0x22e7d0, // Troops
    0x2329c0, // Worker
    0x207150, // Keep (return Lord to keep)
    0x31e700, // Actor event / selected actor controls
};

static bool nearUiProcess(uint32_t rva) {
    if (rva == 0xffffffffu) {
        return false;
    }
    for (size_t i = 0; i < sizeof(UI_PROCESS_RVAS) / sizeof(UI_PROCESS_RVAS[0]); ++i) {
        const uint32_t start = (uint32_t)UI_PROCESS_RVAS[i];
        if (rva >= start && rva - start < 0x600) {
            return true;
        }
    }
    return false;
}

// Some autonomous routines post Chores whose RTTI name is not spelled `AI*`,
// so name-keyed classification alone leaves them at origin 0 and fail-loud
// playback stops. Classify by posting routine as well: each entry is a whole
// function body proven to be autonomous, recorded as [begin, end).
//
// `0x19e1a0 .. 0x19e390` is `FUN_0059e1a0`, a single clan routine that posts an
// `AIMoveTroopsChore` (AI-RTTI, already classified) at RVA 0x19e28d and then a
// `SetTroopStanceChore` with stance mode 2 at RVA 0x19e31f. Because one body
// posts both and the first is unambiguously autonomous, the second is too.
struct AutonomousRange {
    uint32_t begin;
    uint32_t end;
};

static const AutonomousRange KNOWN_AI_POST_RANGES[] = {
    { 0x19e1a0, 0x19e390 }, // FUN_0059e1a0: clan move + stance pass
};

// The mouse and hotkey dispatchers reach four command posters that no pane
// process encloses, so neither `nearUiProcess` frame matched them and a live
// human session left them at origin 0. Each range below is a whole function
// body, and every call path into it was walked in Ghidra against the recorded
// caller/parent pairs of a human GUI session.
//
// Both dispatchers are input code, which is what makes poster-level
// classification sound here: no autonomous routine reaches these bodies.
//
//   FUN_005f6710  the local mouse/command dispatcher -- it plays UI sounds and
//                 drives the "promotion rank screen" and "in game menu" popups.
//   FUN_006af0d0  a hotkey dispatcher that polls GetAsyncKeyState directly.
//
// The AI and system tests run before this one, so an autonomous class posted
// from one of these bodies would still be classified by its RTTI first.
static const AutonomousRange KNOWN_UI_POST_RANGES[] = {
    // The carter pane and the dispatcher branch above it, both reached only
    // from FUN_005f6710.
    { 0x1f3820, 0x1f45f2 }, // FUN_005f3820, a mouse-dispatcher branch
    { 0x20ee90, 0x20f0d2 }, // carter pane, CarterTradeChore

    // The Carter post pane process is already UI_PROCESS_RVAS entry 0x20e450,
    // but `nearUiProcess` only credits the first 0x600 bytes of a pane and this
    // one is larger: it is a switch whose jump table sits at 0x20eb38, with
    // case labels from 0x20e4df to 0x20ea49, so the body runs to about
    // 0x20eb5c. `StopCarterTradeChore` posts from 0x20eae4, which is 0x694 past
    // the entry and therefore fell outside the window. Recovered by finding the
    // stored pointer 0x5c9f5c -> 0x20e450, i.e. the pane's own vtable slot.
    //
    // Listed as an explicit body rather than by widening the 0x600 heuristic,
    // which would loosen every other pane at once. Other panes may well exceed
    // 0x600 too; that is a known limit of the heuristic, and the recorder
    // surfaces each case as origin 0 when it happens.
    { 0x20e450, 0x20eb5c }, // Carter post pane, StopCarterTradeChore

    // Panes recovered from the fourth session.
    { 0x208b90, 0x208e02 }, // estate pane, BuyVillageChore
    { 0x267ca0, 0x267ea9 }, // promotion/rank screen, ChangeRankChore

    // S2ActorHandler local troop-command posters: one contiguous family, every
    // member reached through the shared select-and-dispatch helper at
    // 0x0f30b0. Covers the SelectTroopsChore poster at 0x0f2df0 and the
    // MoveTroopsChore poster at 0x0f3ae0 that the live session surfaced. The
    // range stops at the last member proven to call that helper; anything past
    // it stays fail-loud.
    { 0x0f2df0, 0x0f3fd8 },
    { 0x1ea280, 0x1ea517 }, // place-building dispatcher, the parent frame
    { 0x1eb080, 0x1eb119 }, // delete-building dispatcher, the parent frame
    { 0x231320, 0x232446 }, // troop-move dispatcher, the parent frame

    // The two input dispatchers themselves, and the input frame above the
    // mouse one. FUN_005f6710 posts DeleteMoatChore from its own body, so the
    // caller frame lands here rather than in a poster.
    { 0x1f6710, 0x1f7590 }, // FUN_005f6710, the mouse/command dispatcher
    { 0x1f75b0, 0x1f77b3 }, // FUN_005f75b0, the frame above it
    { 0x2af0d0, 0x2af608 }, // FUN_006af0d0, the GetAsyncKeyState hotkey path
    { 0x2aff30, 0x2aff3d }, // FUN_006afef0, the frame above it

    // Posters and parent frames whose only callers are those dispatchers.
    { 0x1eb610, 0x1eb8c3 }, // AssemblyPointChore poster
    { 0x1ebe50, 0x1ec250 }, // PlaceTowerEquipmentChore poster
    { 0x1eb4a0, 0x1eb54e }, // DeleteWallsChore UI parent frame
    { 0x1ec310, 0x1ec345 }, // its one non-dispatcher caller, itself UI-only
    { 0x1ea710, 0x1ea74e }, // PlaceWallsChore UI parent frame
    { 0x1ec350, 0x1ec3a2 }, // GameGroundFeatureChore UI parent frame
    { 0x1ec3b0, 0x1ec3ea }, // GameGroundFeatureChore UI parent frame

    // Pane processes dispatched from the window proc: each recorded a parent
    // outside the executable, calls playSound, and posts one command. Same
    // species as UI_PROCESS_RVAS, listed as bodies because the recorded caller
    // is the posting site inside the pane rather than its entry.
    { 0x20fae0, 0x20fcb2 }, // workshop pane, MakeWeaponChore
    { 0x218860, 0x218a32 }, // second workshop pane, MakeWeaponChore
    { 0x206650, 0x206835 }, // gatehouse pane, OpenGateHouseChore
    { 0x2052b0, 0x2054fe }, // tower pane, burning logs and stone tipper
    { 0x208250, 0x2083d8 }, // repair pane, RepairBuildingChore
    { 0x22ad30, 0x22aeb4 }, // tunnel pane, TunnelAddEngineerChore

    // The shared recruit poster FUN_004ef700 (ret 8 at 0xef7ea), which posts
    // CreateTroopChore from 0xef7ac. Stock, its parent frame is one of the five
    // pane call sites and `nearUiProcess` credits it. The patch's
    // shiftRecruit retargets all five calls to a DLL stub, so the parent is
    // outside the exe and every recruit fell to origin 0 (merc-post Vikings
    // stopped playback). A byte scan of
    // the Steam exe finds exactly five E8 calls to it -- 0x22083a barracks,
    // 0x223ca9/0x223dc9 engineers guild, 0x225622 merc post, 0x228539
    // monastery, all pane processes -- and no stored pointer.
    { 0x0ef700, 0x0ef7ed }, // FUN_004ef700, recruit post helper

    // In-game chat send, FUN_0064d8e0: builds a ChatChore (ctor 0x5e8b30; +8
    // sender = DAT_02acd8b4, +0xc the chat box text) and admits it from
    // 0x24d9d2. Its only callers are the chat pane's event handler
    // FUN_0064e350 (vtable slot 0x9d1f3c; event 0x69 or button 0x1e76) and
    // the keyboard dispatcher FUN_005f2510 at 0x5f2f71, whose only caller is
    // the input frame FUN_005f75b0 above. A local chat was origin 0 and
    // stopped playback.
    // The lobby's ChatChore post at 0x6a2e2d is pre-epoch and not listed.
    { 0x24d8e0, 0x24da0f }, // FUN_0064d8e0, ChatChore poster
};

// Deliberately absent: the PlaceWallsChore poster at 0x1c9cd0 and the
// DeleteWallsChore poster at 0x1c9ee0. Both are also reached by routines that
// are not input code -- FUN_004564c0 posts walls under FUN_00467190, which is
// the same per-player update pass that owns the origin-3 policy and trade
// parents at 0x06720c/0x067228. Classifying those two at poster level would
// label an autonomous wall command as human. The UI parent frames above admit
// the human route without admitting the autonomous one, so a wall command from
// any other path stays origin 0 and stops playback, which is correct.

// The autonomous per-player update pass, recovered from the third live human
// session. That map had three active AI opponents, which finally exercised
// routines the earlier fixture never ran, and it showed that four command
// posters are shared between the human UI and this pass:
//
//   PlaceBuildingChore    45 admissions from the UI dispatcher, 128 from here
//   PlaceWallsChore        3 from the UI dispatcher, 24 from here
//   GameGroundFeatureChore 4 from the UI dispatcher, 213 from here
//   PlaceTowerEquipmentChore, likewise
//
// A poster is therefore never sufficient evidence of a human origin. These
// ranges are whole function bodies on the three autonomous roots -- the
// per-player pass FUN_00467190, and the economy and policy passes FUN_0045b9b0
// and FUN_0045d9b0 that already own the legacy ranges below.
static const AutonomousRange KNOWN_SYSTEM_POST_RANGES[] = {
    // FUN_00467190 and everything it reaches.
    { 0x067190, 0x067246 }, // FUN_00467190, the per-player pass root
    { 0x0670d0, 0x067183 }, // FUN_004670d0
    { 0x066e40, 0x0670cb }, // FUN_00466e40
    { 0x05f160, 0x05feaa }, // FUN_0045f160, posts CreateTroopChore
    { 0x065470, 0x0660d5 }, // FUN_00465470, posts CreateTroopChore
    { 0x0645b0, 0x064c22 }, // FUN_004645b0, posts policy and CarterTradeChore
    { 0x0619d0, 0x061c17 }, // FUN_004619d0
    { 0x05de20, 0x05e049 }, // FUN_0045de20, posts BuyVillageChore
    { 0x061f80, 0x062ab9 }, // FUN_00461f80, posts HarassClanInstructionsChore
    { 0x0564c0, 0x056f12 }, // FUN_004564c0, posts walls, ground and buildings

    // The autonomous construction tree under FUN_00457de0, which the economy
    // and policy passes call. It reaches SH2_PostPlaceBuildingChore through the
    // shared trampoline FUN_0077e9e0 -- a two-frame provenance sees only the
    // trampoline, so the trampoline itself has to be classified. That is sound
    // only because all eight of its callers were walked and none is input code.
    { 0x37e9e0, 0x37ea5b }, // FUN_0077e9e0, the shared poster trampoline
    { 0x057de0, 0x057ff4 }, // FUN_00457de0, the construction hub
    { 0x057500, 0x057639 }, // FUN_00457500
    // Added after a loaded-save test match halted playback with
    // refused=1 on 20 PlaceBuildingChore admissions. A sibling of FUN_00457500
    // under the same hub FUN_00457de0, missed only because none of the four
    // human GUI fixtures ever exercised it. Walked one frame further for the
    // same reason the trampoline was: FUN_00457de0's own callers are
    // FUN_0045b9b0 and FUN_0045d9b0, neither of which is input code.
    { 0x0578c0, 0x057bf9 }, // FUN_004578c0, posts PlaceBuildingChore
    // Freeman extended capture: FUN_00457780 posts building type 0x43 from
    // its only caller, autonomous construction hub FUN_00457de0. Parent RVA
    // 0x57891 was origin 0 and stopped playback at rel971 before this range.
    { 0x057780, 0x0578b9 }, // FUN_00457780, posts PlaceBuildingChore
    { 0x057640, 0x057779 }, // FUN_00457640
    { 0x055660, 0x0556e7 }, // FUN_00455660
    { 0x0556f0, 0x055777 }, // FUN_004556f0
    { 0x055780, 0x055864 }, // FUN_00455780
    { 0x056240, 0x05630a }, // FUN_00456240
    { 0x056310, 0x05645c }, // FUN_00456310
    { 0x054dd0, 0x054e7e }, // FUN_00454dd0
    { 0x0584e0, 0x05894a }, // FUN_004584e0
    { 0x01e4f0, 0x01e7a8 }, // FUN_0041e4f0, also posts DeleteWallsChore
};

static bool nearKnownUiPost(uint32_t rva) {
    if (rva == 0xffffffffu) {
        return false;
    }
    for (size_t i = 0;
         i < sizeof(KNOWN_UI_POST_RANGES) / sizeof(KNOWN_UI_POST_RANGES[0]);
         ++i) {
        if (rva >= KNOWN_UI_POST_RANGES[i].begin &&
            rva < KNOWN_UI_POST_RANGES[i].end) {
            return true;
        }
    }
    return false;
}

static bool nearKnownAiPost(uint32_t rva) {
    if (rva == 0xffffffffu) {
        return false;
    }
    for (size_t i = 0;
         i < sizeof(KNOWN_AI_POST_RANGES) / sizeof(KNOWN_AI_POST_RANGES[0]);
         ++i) {
        if (rva >= KNOWN_AI_POST_RANGES[i].begin &&
            rva < KNOWN_AI_POST_RANGES[i].end) {
            return true;
        }
    }
    return false;
}

// These are verified autonomous game routines, not UI process entries. The
// first is the periodic stock/gold economy pass that posts TradeChore, and the
// second is the player-policy update pass that posts policy Chores. Keep this
// as a provenance hint only: the eventual recorder still resolves and stores
// the admission route independently of the hint.
static bool nearKnownSystemPost(uint32_t rva) {
    if (rva == 0xffffffffu) {
        return false;
    }
    if ((rva >= 0x5be80 && rva <= 0x5c1fe) ||
        (rva >= 0x5e050 && rva <= 0x5ea4b)) {
        return true;
    }
    for (size_t i = 0;
         i < sizeof(KNOWN_SYSTEM_POST_RANGES) / sizeof(KNOWN_SYSTEM_POST_RANGES[0]);
         ++i) {
        if (rva >= KNOWN_SYSTEM_POST_RANGES[i].begin &&
            rva < KNOWN_SYSTEM_POST_RANGES[i].end) {
            return true;
        }
    }
    return false;
}

static uint8_t commandSchema(uint32_t vtableRva) {
    if (vtableRva == TRADE_VTABLE_RVA) return 1;
    if (vtableRva == CREATE_TROOP_VTABLE_RVA) return 2;
    if (vtableRva == SET_FOOD_VTABLE_RVA) return 5;
    if (vtableRva == CHANGE_TAX_VTABLE_RVA ||
        vtableRva == SET_RATIONS_VTABLE_RVA ||
        vtableRva == SET_CHURCH_VTABLE_RVA ||
        vtableRva == SET_INN_VTABLE_RVA) return 3;
    if (vtableRva == PLACE_BUILDING_VTABLE_RVA) return 4;
    if (vtableRva == DISBAND_TROOPS_VTABLE_RVA || vtableRva == 0x5c6a4c) return 6;
    if (vtableRva == FORMATE_TROOPS_VTABLE_RVA) return 7;
    if (vtableRva == SET_TROOP_STANCE_VTABLE_RVA || vtableRva == 0x5c6bb0) return 8;
    if (vtableRva == MOUNT_HORSES_VTABLE_RVA) return 9;
    if (vtableRva == STOP_ALL_TROOPS_VTABLE_RVA || vtableRva == 0x5c6ffc) return 10;
    if (vtableRva == SELECT_TROOPS_VTABLE_RVA || vtableRva == 0x5c6fa8) return 11;
    if (vtableRva == RETURN_LORD_VTABLE_RVA ||
        vtableRva == RETURN_LORD_VTABLE2_RVA) return 12;
    if (vtableRva == LORD_NOT_CONTROLLABLE_VTABLE_RVA ||
        vtableRva == LORD_NOT_CONTROLLABLE_VTABLE2_RVA) return 13;
    // CreateSiegeEquipmentChore is deliberately not assigned a schema yet.
    // It is visible in the census, but replay must reject it until its payload
    // fields have been recovered rather than silently recording a guess.
    (void)CREATE_SIEGE_VTABLE_RVA;
    return 0;
}

static uint16_t commandSize(uint8_t schema) {
    switch (schema) {
    case 1: return 0x1c; // TradeChore
    case 2: return 0x1c; // CreateTroopChore
    case 3: return 0x10; // policy value + player slot
    case 4: return 0x18; // PlaceBuildingChore
    case 5: return 0x14; // food id + on/off byte + player slot
    case 6: return 0x14; // selected group id (DisbandTroopsChore)
    case 7: return 0x18; // selected group id + formation descriptor
    case 8: return 0x18; // selected group id + stance mode
    case 9: return 0x18; // selected group id + mount/dismount mode
    case 10: return 0x14; // selected group id (StopAllTroopsChore)
    case 11: return 0x20; // actor-pair vector + selected group id + slot
    case 12: return 0x10; // ReturnLordChore: cached Keep* + packed Keep handle
    case 13: return 0x18; // LordNotControllableChore: handle + actor id + slot
    default: return 0;
    }
}

// Preserve the exact small object for live schema recovery without treating it
// as replay input. Both Lord commands graduated to real schemas; the remaining
// entry is CreateSiegeEquipmentChore, whose object size 0x28 is proven by its
// three stock allocators but whose field mapping is not yet recovered.
static uint16_t diagnosticSize(uint32_t vtableRva) {
    if (vtableRva == CREATE_SIEGE_VTABLE_RVA ||
        vtableRva == CREATE_SIEGE_VTABLE2_RVA) return 0x28;
    return 0;
}

// ReturnLordChore carries no player field: execute resolves only the packed
// Keep handle at +0x0c. Provenance is still recoverable because the Keep pair
// that SubPanelKeep::process copies into the Chore is the issuing player's own
// keep pair, so the slot is derived by matching that handle in the stock
// player table rather than by assuming a layout the Chore does not have.
static uint32_t playerSlotForKeepHandle(uint32_t handle) {
    if (!handle || !s_exeBase) {
        return 0xffffffffu;
    }
    for (uint32_t slot = 1; slot < PLAYER_TABLE_SLOTS; ++slot) {
        const uintptr_t entry = s_exeBase + PLAYER_TABLE_RVA + slot * 4;
        if (!readableRange(entry, 4)) {
            continue;
        }
        const uintptr_t player = *(uintptr_t *)entry;
        if (!player) {
            continue;
        }
        const uintptr_t vec = player + PLAYER_KEEP_VECTOR_OFF;
        if (!readableRange(vec, 8)) {
            continue;
        }
        const uintptr_t begin = *(uintptr_t *)vec;
        const uintptr_t end = *(uintptr_t *)(vec + 4);
        if (!begin || end < begin || end - begin < 8 || !readableRange(begin, 8)) {
            continue;
        }
        if (*(uint32_t *)(begin + 4) == handle) {
            return slot;
        }
    }
    return 0xffffffffu;
}

static uint32_t commandPlayerSlot(uintptr_t event, uint8_t schema) {
    if (!event) return 0xffffffffu;
    switch (schema) {
    case 1: return *(uint32_t *)(event + 0x18); // TradeChore player id
    case 2: return *(uint8_t *)(event + 0x09);  // CreateTroop slot
    case 3: return *(uint32_t *)(event + 0x0c); // policy player index
    case 4: return *(uint8_t *)(event + 0x13);  // placement player slot
    case 5: return *(uint32_t *)(event + 0x10); // food Chore player index
    case 6:
    case 7:
    case 8:
    case 9:
    case 10: return *(uint32_t *)(event + 0x10) >> 24; // selection id prefix
    case 11: return *(uint32_t *)(event + 0x1c); // SelectTroopsChore slot
    case 12: return playerSlotForKeepHandle(*(uint32_t *)(event + 0x0c));
    case 13: return *(uint32_t *)(event + 0x14); // explicit player argument
    default: return 0xffffffffu;
    }
}

static uint32_t canonicalPayloadHash(uintptr_t event, uint8_t schema) {
    if (!event) return 0;
    uint8_t canonical[32];
    size_t used = 0;
    switch (schema) {
    case 1: // good, amount, price, buy, sell, player
        memcpy(canonical + used, (void *)(event + 0x08), 4); used += 4;
        memcpy(canonical + used, (void *)(event + 0x0c), 4); used += 4;
        memcpy(canonical + used, (void *)(event + 0x10), 4); used += 4;
        canonical[used++] = *(uint8_t *)(event + 0x14);
        canonical[used++] = *(uint8_t *)(event + 0x15);
        memcpy(canonical + used, (void *)(event + 0x18), 4); used += 4;
        break;
    case 2: // unit id, player slot, reserved byte
        memcpy(canonical + used, (void *)(event + 0x08), 3); used += 3;
        break;
    case 3: // value, player index
        memcpy(canonical + used, (void *)(event + 0x08), 8); used += 8;
        break;
    case 4: // position, building id, rotation, player, charge fields
        memcpy(canonical + used, (void *)(event + 0x08), 12); used += 12;
        memcpy(canonical + used, (void *)(event + 0x12), 4); used += 4;
        break;
    case 5: // food id, on/off byte, player index
        memcpy(canonical + used, (void *)(event + 0x08), 4); used += 4;
        canonical[used++] = *(uint8_t *)(event + 0x0c);
        memcpy(canonical + used, (void *)(event + 0x10), 4); used += 4;
        break;
    case 6: // selected group id; the other two fields are live references
    case 10:
        memcpy(canonical + used, (void *)(event + 0x10), 4); used += 4;
        break;
    case 7: // selected group id plus formation mode/descriptor identity
    case 8: // selected group id plus stance mode
    case 9: // selected group id plus mount/dismount mode
        memcpy(canonical + used, (void *)(event + 0x10), 4); used += 4;
        memcpy(canonical + used, (void *)(event + 0x14), 4); used += 4;
        break;
    case 12: // packed Keep handle only; +0x08 is a cached pointer execute
             // recomputes from this field and is deliberately excluded
        memcpy(canonical + used, (void *)(event + 0x0c), 4); used += 4;
        break;
    case 13: // packed actor handle, selected actor id, player slot; +0x08 is
             // a cached Actor* that execute recomputes from the other two
        memcpy(canonical + used, (void *)(event + 0x0c), 4); used += 4;
        memcpy(canonical + used, (void *)(event + 0x10), 4); used += 4;
        memcpy(canonical + used, (void *)(event + 0x14), 4); used += 4;
        break;
    case 11: { // selected actor handles, group id, player slot; no pointers
        uint32_t begin = *(uint32_t *)(event + 0x08);
        uint32_t end = *(uint32_t *)(event + 0x0c);
        uint32_t groupId = *(uint32_t *)(event + 0x18);
        uint32_t playerSlot = *(uint32_t *)(event + 0x1c);
        memcpy(canonical + used, &groupId, 4); used += 4;
        memcpy(canonical + used, &playerSlot, 4); used += 4;
        // The fixed canonical buffer is intentionally bounded.  A selection
        // with more pairs still records a stable prefix and the count; raw
        // diagnostic bytes retain the native object for later recovery.
        uint32_t count = 0;
        if (end >= begin && (end - begin) % 8 == 0 &&
            end - begin <= 0x100000 && readableRange(begin, end - begin)) {
            count = (end - begin) / 8;
            memcpy(canonical + used, &count, 4); used += 4;
            uint32_t limit = count < 4 ? count : 4;
            for (uint32_t i = 0; i < limit; ++i) {
                uint32_t handle = *(uint32_t *)(begin + i * 8 + 4);
                memcpy(canonical + used, &handle, 4); used += 4;
            }
        } else {
            memcpy(canonical + used, &count, 4); used += 4;
        }
        break;
    }
    default:
        return 0;
    }
    return fnv1a32(canonical, used);
}

// One frame up from a hooked prologue: the trampoline captured the caller's
// EBP, so [EBP+4] is the caller's own return address.
static uint32_t parentCallerRvaOf(uintptr_t frame) {
    if (!readableRange(frame, 8)) {
        return 0xffffffffu;
    }
    uintptr_t parentReturn = *(uintptr_t *)(frame + 4);
    return exeRva(parentReturn);
}

static uint32_t parentCallerRva() {
    return parentCallerRvaOf(g_replayTraceAdmitFrame);
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

static void appendTraceRecord(uint8_t phase, uint8_t route, uintptr_t event,
                              uintptr_t method, uint16_t tick,
                              uint32_t simulationTick, uint16_t order) {
    uintptr_t vtable = 0;
    if (event && readableRange(event, sizeof(uintptr_t))) {
        vtable = *(uintptr_t *)event;
    }
    uint32_t vtableRva = exeRva(vtable);

    // NetGameMgr appends one EndJobChore to every successful tick. It is the
    // clock delimiter, not a human/AI action, and recording it would evict the
    // useful boundary evidence in about one second. It still contributes to
    // `order` above, and currentTick in the snapshot proves tick progress.
    if (phase == 2 && vtableRva == END_JOB_VTABLE_RVA) {
        return;
    }

    ReplayTraceRecord &r = s_ring[s_totalRecords % TRACE_RING_CAPACITY];
    r.serial = s_totalRecords + 1;
    r.simulationTick = simulationTick;
    r.engineTick = tick;
    r.order = order;
    r.phase = phase;
    r.route = route;
    r.reserved = 0;
    r.vtableRva = vtableRva;
    r.methodRva = exeRva(method);
    ++s_totalRecords;
    if (replayRecorderIsRecording()) {
        if (s_boundaryCount == BOUNDARY_CAPACITY) {
            ++s_boundaryDropped;
        } else {
            BoundaryEntry &entry = s_boundaries[s_boundaryCount++];
            entry = {};
            entry.trace = r;
            entry.eventToken = (uint32_t)event;
            entry.netCounter = *(uint8_t *)(s_exeBase + NET_MGR_RVA + 4);
            if (phase == 2 && route == 0 &&
                readableRange(g_replayTraceExecJob, 0x18)) {
                entry.scheduledJob = *(uint16_t *)(g_replayTraceExecJob + 0x14);
                entry.jobValid = 1;
            }
        }
    }
}

// `originOverride` is 0 for the ordinary World admission path, which derives
// provenance from the call site. The multiplayer receive path passes 4: a
// command that arrived over the wire has no local call site to classify, and
// its origin is known by the route it took, not by who posted it.
static void appendCommandRecord(uintptr_t event, uintptr_t method,
                                uint16_t tick, uint32_t simulationTick,
                                uint16_t order, uint32_t callerRva,
                                uint32_t parentRva, uint8_t originOverride) {
    if (!event || !readableRange(event, sizeof(uintptr_t))) {
        return;
    }
    // Without a diagnostic census nothing reads the command ring, so an admission
    // that is neither being recorded nor checked by the spectator guard costs
    // nothing beyond the clock and order bookkeeping done by the caller.
    if (!replayRecorderIsRecording() && !spectatorGuardActive()) {
        return;
    }

    uintptr_t vtable = *(uintptr_t *)event;
    const uint32_t vtableRva = exeRva(vtable);
    const uint8_t schema = commandSchema(vtableRva);
    const uint16_t size = schema ? commandSize(schema) : diagnosticSize(vtableRva);
    const TypeInfo type = typeInfo(vtable);

    ReplayCommandRecord &r = s_commandRing[
        s_totalCommandRecords % COMMAND_RING_CAPACITY];
    memset(&r, 0, sizeof(r));
    r.serial = s_totalCommandRecords + 1;
    r.simulationTick = simulationTick;
    r.engineTick = tick;
    r.order = order;
    r.vtableRva = vtableRva;
    r.methodRva = exeRva(method);
    r.callerRva = callerRva;
    r.parentCallerRva = parentRva;
    r.typeNameHash = type.nameHash;
    r.playerSlot = commandPlayerSlot(event, schema);
    r.payloadHash = canonicalPayloadHash(event, schema);
    r.payloadSize = size;
    r.schema = schema;

    // The stock codec is the authoritative payload for every registered class,
    // including the ones with no hand-recovered schema. Recording it here means
    // an unmapped Chore is still captured portably instead of being lost.
    // 4096, not 256: choreCodecSerialize discards anything longer than the
    // capacity it is given (`length <= cap` in serializeWith), so a 256-byte
    // buffer silently produced a zero-length payload for any larger command.
    // The first human match hit exactly that: one SelectTroopsChore
    // of 582 events -- a selection of more than ~61 units, whose stock payload
    // is 4 bytes per unit -- came back empty and flagged the container
    // truncated=1. 0x1000 is SERIALIZE_CAPACITY, the size the game's own send
    // path uses, so nothing the engine can transmit overflows it now. Static
    // rather than stack: this hook runs on the simulation thread deep inside
    // game code, and the buffer is copied out before the hook returns.
    static uint8_t stock[0x1000];
    uint32_t stockIndex = 0xffffffffu;
    const size_t stockLength =
        choreCodecSerialize(reinterpret_cast<void *>(event), stock,
                            sizeof(stock), &stockIndex);
    r.stockClassIndex = stockIndex <= 0xfffeu ? (uint16_t)stockIndex : 0xffffu;
    r.stockLength = (uint16_t)stockLength;
    r.stockHash = stockLength ? fnv1a32(stock, stockLength) : 0;

    if (originOverride) {
        r.originHint = originOverride;
    } else if (type.ai || nearKnownAiPost(callerRva) || nearKnownAiPost(parentRva)) {
        r.originHint = 2;
    } else if (type.system || nearKnownSystemPost(callerRva) ||
               nearKnownSystemPost(parentRva)) {
        r.originHint = 3;
    } else if (nearUiProcess(callerRva) || nearUiProcess(parentRva) ||
               nearKnownUiPost(callerRva) || nearKnownUiPost(parentRva)) {
        r.originHint = 1;
    } else {
        r.originHint = 0;
    }

    // Proof half of the spectator contract. If a UI-origin command is admitted
    // while playback is armed, live local input reached the simulation and the
    // window-message guard did not stop it -- which is exactly what would
    // happen if the game read a device directly. Count it here, at the only
    // place that sees every admission.
    if (r.originHint == 1 && spectatorGuardActive()) {
        spectatorGuardNoteLocalCommand();
    }

    // Raw bytes are bounded to the fixed diagnostic area and captured only
    // for known small layouts. Unknown classes remain visible by identity and
    // call path but are not allowed to smuggle an assumed payload into replay.
    const size_t rawLength = size < sizeof(r.raw) ? size : sizeof(r.raw);
    if (size && readableRange(event, rawLength)) {
        memcpy(r.raw, (const void *)event, rawLength);
        r.rawHash = fnv1a32(r.raw, rawLength);
    }
    ++s_totalCommandRecords;

    // The append-only replay log shares this hook rather than adding a second
    // one: two hooks on the same site could disagree about tick or order, and
    // the ordering within a tick is exactly what replay has to reproduce.
    ReplayEventRecord logged = {};
    logged.simulationTick = simulationTick;
    logged.engineTick = tick;
    logged.order = order;
    logged.classIndex = r.stockClassIndex;
    logged.payloadLength = r.stockLength;
    logged.vtableRva = vtableRva;
    logged.callerRva = callerRva;
    logged.parentCallerRva = parentRva;
    logged.playerSlot = r.playerSlot;
    logged.originHint = r.originHint;
    logged.schema = schema;
    if (callerIsForeign(callerRva)) {
        logged.flags |= REPLAY_FLAG_FOREIGN_CALLER;
    }
    replayRecorderNote(logged, stockLength ? stock : nullptr);
}

static void appendRecord(uint8_t phase, uint8_t route, uintptr_t event,
                         uintptr_t method) {
    uint16_t tick = currentTick();
    uint32_t simulationTick = liftTick(tick);
    uint16_t order = nextOrder(phase, tick);
    appendTraceRecord(phase, route, event, method, tick, simulationTick, order);
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
    appendTraceRecord(1, mode, event, method, tick, simulationTick, order);
    appendCommandRecord(event, method, tick, simulationTick, order,
                        exeRva(g_replayTraceAdmitCaller), parentCallerRva(), 0);
}

// The multiplayer receive path.
//
// Fires on every enqueue, local and remote, and records only the remote ones.
// Provenance is the return address, which is the same mechanism the World
// admission hook already uses: a caller inside NetGameMgr::onPacket is a
// command that came off the wire. Local enqueues are counted and dropped,
// because World::admit already recorded them one frame earlier.
//
// EndJobChore is skipped for the same reason the execution trace skips it: it
// is the lockstep tick delimiter, generated independently by every peer, and
// replaying one would inject a second clock into the playback.
extern "C" void replayTraceCaptureEnqueue() {
    const uintptr_t event = g_replayTraceEnqueueEvent;
    const uint32_t callerRva = exeRva(g_replayTraceEnqueueCaller);
    if (callerRva < ONPACKET_BEGIN_RVA || callerRva >= ONPACKET_END_RVA) {
        ++s_localEnqueues;
        return;
    }
    if (!event || !readableRange(event, sizeof(uintptr_t))) {
        ++s_remoteRejected;
        return;
    }

    const uintptr_t vtable = *(uintptr_t *)event;
    if (exeRva(vtable) == END_JOB_VTABLE_RVA) {
        ++s_remoteEndJobs;
        return;
    }

    uintptr_t method = 0;
    if (vtable && readableRange(vtable + 8, sizeof(uintptr_t))) {
        method = *(uintptr_t *)(vtable + 8);
    }

    const uint16_t tick = currentTick();
    const uint32_t simulationTick = liftTick(tick);
    // Phase 1's order counter, shared with local admissions on purpose: within
    // a tick the two sources interleave, and replay has to reproduce that one
    // combined order rather than two independent ones.
    const uint16_t order = nextOrder(1, tick);
    // Route byte 4 marks the receive path in the trace ring, distinct from the
    // World mode bytes the admission path records.
    appendTraceRecord(1, 4, event, method, tick, simulationTick, order);
    appendCommandRecord(event, method, tick, simulationTick, order, callerRva,
                        parentCallerRvaOf(g_replayTraceEnqueueFrame), 4);
    ++s_remoteCommands;
}

extern "C" void replayTraceCaptureExecution() {
    appendRecord(2, (uint8_t)g_replayTraceExecRoute,
                 g_replayTraceExecEvent, g_replayTraceExecMethod);
}

__declspec(naked) static void admissionHook() {
    __asm__ volatile(
        "movl (%esp), %eax\n\t"
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
        // Re-emit the exact five-byte prologue.
        "pushl %ebp\n\t"
        "movl %esp, %ebp\n\t"
        "pushl $-1\n\t"
        "jmp *_g_replayTraceAdmitReturn\n\t");
}

// Same shape as admissionHook, against a byte-identical MSVC prologue. At
// entry ESP holds the return address and ESP+4 the Ptr<Chore> argument; ECX
// holds the queue object and is left untouched for the callee.
__declspec(naked) static void enqueueHook() {
    __asm__ volatile(
        "movl (%esp), %eax\n\t"
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
        "jmp *_g_replayTraceQueuedExecReturn\n\t");
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
        "jmp *_g_replayTraceDirectExecReturn\n\t");
}

void installReplayTrace() {
    s_exeBase = (uintptr_t)GetModuleHandleA(NULL);
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

bool replayTraceSessionIsNetworked() {
    if (!s_exeBase) {
        return false;
    }
    const uintptr_t netModeGlobal = s_exeBase + NET_MODE_GLOBAL_RVA;
    if (!readableRange(netModeGlobal, 4)) {
        return false;
    }
    return *(volatile uint32_t *)netModeGlobal == 2;
}

void replayTraceRecordingReset() {
    s_boundaryCount = s_boundaryDropped = 0;
}

void replayTraceBoundaryView(const ReplayBoundaryEntry **entries, uint32_t *count,
                             uint32_t *dropped, uint32_t *hooks) {
    *entries = s_boundaries;
    *count = s_boundaryCount;
    *dropped = s_boundaryDropped;
    *hooks = s_installStatus;
}

uint32_t replayTraceRecordingSave(const char *path) {
    if (!path || !*path || replayRecorderIsRecording()) return 0;
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    BoundaryHeader header = {};
    memcpy(header.magic, "SH2RBND", 8);
    header.version = 1;
    header.entrySize = sizeof(BoundaryEntry);
    header.count = s_boundaryCount;
    header.epoch = replayRecorderDataEpochTick();
    header.dropped = s_boundaryDropped;
    header.hooks = s_installStatus;
    DWORD written = 0;
    const uint32_t bytes = s_boundaryCount * sizeof(BoundaryEntry);
    bool ok = WriteFile(file, &header, sizeof(header), &written, NULL) && written == sizeof(header);
    ok = ok && (!bytes || (WriteFile(file, s_boundaries, bytes, &written, NULL) && written == bytes));
    CloseHandle(file);
    return ok ? sizeof(header) + bytes : 0;
}

size_t replayTraceNetState(void *out, size_t cap) {
    if (!out || cap < sizeof(ReplayNetStateRecord) || !s_exeBase) {
        return 0;
    }

    ReplayNetStateRecord r;
    memset(&r, 0, sizeof(r));
    r.magic = 0x3154454e; // "NET1"
    r.version = 1;
    r.engineTick = currentTick();
    // Deliberately does NOT call liftTick: this is a read-only probe and the
    // lifted clock must advance only from the hooks that see every tick.
    r.simulationTick = s_simulationTick;
    r.installStatus = s_installStatus;
    r.localEnqueues = s_localEnqueues;
    r.remoteCommands = s_remoteCommands;
    r.remoteEndJobs = s_remoteEndJobs;
    r.remoteRejected = s_remoteRejected;

    const uintptr_t netModeGlobal = s_exeBase + NET_MODE_GLOBAL_RVA;
    if (readableRange(netModeGlobal, 4)) {
        r.netModeGlobal = *(volatile uint32_t *)netModeGlobal;
    }

    const uintptr_t mgr = s_exeBase + NET_MGR_RVA;
    if (readableRange(mgr, 0x74)) {
        r.sessionMode = *(volatile uint8_t *)(mgr + 0x48);
        r.syncTick = *(volatile uint16_t *)(mgr + 0x44);
        r.liveJobCount = *(volatile uint32_t *)(mgr + 0x20);
        const uintptr_t classBegin = *(uintptr_t *)(mgr + 0x5c);
        const uintptr_t classEnd = *(uintptr_t *)(mgr + 0x60);
        if (classEnd >= classBegin) {
            r.classCount = (uint32_t)((classEnd - classBegin) / 4);
        }
        const uintptr_t peerBegin = *(uintptr_t *)(mgr + 0x6c);
        const uintptr_t peerEnd = *(uintptr_t *)(mgr + 0x70);
        if (peerEnd >= peerBegin) {
            r.peerCount = (uint32_t)((peerEnd - peerBegin) / 4);
        }
    }

    memcpy(out, &r, sizeof(r));
    return sizeof(r);
}

size_t replayTraceSnapshot(void *out, size_t cap) {
    const size_t need = sizeof(ReplayTraceSnapshotHeader) +
                        sizeof(ReplayTraceRecord) * SNAPSHOT_CAPACITY;
    if (!out || cap < need) {
        return 0;
    }

    ReplayTraceSnapshotHeader h;
    h.magic = 0x31435254;
    h.version = 2;
    h.currentEngineTick = s_exeBase ? currentTick() : 0;
    h.totalRecords = s_totalRecords;
    h.count = (uint16_t)(s_totalRecords < SNAPSHOT_CAPACITY
                             ? s_totalRecords : SNAPSHOT_CAPACITY);
    h.capacity = SNAPSHOT_CAPACITY;
    h.installStatus = s_installStatus;
    h.currentSimulationTick = s_exeBase ? liftTick(h.currentEngineTick) : 0;
    memcpy(out, &h, sizeof(h));

    ReplayTraceRecord *dst = (ReplayTraceRecord *)((uint8_t *)out + sizeof(h));
    uint32_t first = s_totalRecords - h.count;
    for (uint16_t i = 0; i < h.count; ++i) {
        dst[i] = s_ring[(first + i) % TRACE_RING_CAPACITY];
    }
    return sizeof(h) + sizeof(ReplayTraceRecord) * h.count;
}

static size_t replayCommandSnapshotInternal(void *out, size_t cap,
                                             uint32_t filterVtableRva) {
    const size_t need = sizeof(ReplayCommandSnapshotHeader) +
                        sizeof(ReplayCommandRecord) * COMMAND_SNAPSHOT_CAPACITY;
    if (!out || cap < need) {
        return 0;
    }

    ReplayCommandSnapshotHeader h;
    h.magic = 0x31444d43;
    h.version = 2;
    h.currentEngineTick = s_exeBase ? currentTick() : 0;
    h.totalRecords = s_totalCommandRecords;
    uint32_t retained = s_totalCommandRecords < COMMAND_RING_CAPACITY
                            ? s_totalCommandRecords
                            : COMMAND_RING_CAPACITY;
    uint32_t first = s_totalCommandRecords - retained;
    uint32_t matches = 0;
    if (filterVtableRva != 0xffffffffu) {
        for (uint32_t i = 0; i < retained; ++i) {
            const ReplayCommandRecord &record =
                s_commandRing[(first + i) % COMMAND_RING_CAPACITY];
            if (record.vtableRva == filterVtableRva) {
                ++matches;
            }
        }
    }
    uint32_t selected = filterVtableRva == 0xffffffffu
                            ? (retained < COMMAND_SNAPSHOT_CAPACITY
                                   ? retained : COMMAND_SNAPSHOT_CAPACITY)
                            : (matches < COMMAND_SNAPSHOT_CAPACITY
                                   ? matches : COMMAND_SNAPSHOT_CAPACITY);
    h.count = static_cast<uint16_t>(selected);
    h.capacity = COMMAND_SNAPSHOT_CAPACITY;
    h.installStatus = s_installStatus & 0x1;
    h.currentSimulationTick = s_exeBase ? liftTick(h.currentEngineTick) : 0;
    memcpy(out, &h, sizeof(h));

    ReplayCommandRecord *dst = (ReplayCommandRecord *)
        ((uint8_t *)out + sizeof(h));
    if (filterVtableRva == 0xffffffffu) {
        uint32_t recentFirst = s_totalCommandRecords - h.count;
        for (uint16_t i = 0; i < h.count; ++i) {
            dst[i] = s_commandRing[(recentFirst + i) % COMMAND_RING_CAPACITY];
        }
    } else {
        uint32_t skip = matches > h.count ? matches - h.count : 0;
        uint16_t outIndex = 0;
        for (uint32_t i = 0; i < retained; ++i) {
            const ReplayCommandRecord &record =
                s_commandRing[(first + i) % COMMAND_RING_CAPACITY];
            if (record.vtableRva != filterVtableRva) {
                continue;
            }
            if (skip != 0) {
                --skip;
                continue;
            }
            dst[outIndex++] = record;
        }
    }
    return sizeof(h) + sizeof(ReplayCommandRecord) * h.count;
}

size_t replayCommandSnapshot(void *out, size_t cap) {
    return replayCommandSnapshotInternal(out, cap, 0xffffffffu);
}

size_t replayCommandSnapshotForVtable(void *out, size_t cap,
                                      uint32_t vtableRva) {
    return replayCommandSnapshotInternal(out, cap, vtableRva);
}

uint8_t replayTraceClassifyCaller(uint32_t callerRva, uint32_t parentCallerRva) {
    if (nearKnownAiPost(callerRva) || nearKnownAiPost(parentCallerRva)) {
        return 2;
    }
    if (nearKnownSystemPost(callerRva) || nearKnownSystemPost(parentCallerRva)) {
        return 3;
    }
    if (nearUiProcess(callerRva) || nearUiProcess(parentCallerRva) ||
        nearKnownUiPost(callerRva) || nearKnownUiPost(parentCallerRva)) {
        return 1;
    }
    return 0;
}

uint16_t replayTraceCurrentEngineTick() {
    return s_exeBase ? currentTick() : 0;
}

uint32_t replayTraceCurrentSimulationTick() {
    return s_exeBase ? liftTick(currentTick()) : 0;
}
