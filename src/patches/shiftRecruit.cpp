#include "shiftRecruit.h"
#include "../core/config.h"
#include "../core/frameTick.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

// Shift-click batch recruitment for the barracks, mercenary post and
// monastery. Configurable via [recruitment] RecruitmentShiftMultiplier in
// sh2-unofficial-patch.ini; 0 or 1 disables the feature entirely. Offsets
// and the reverse-engineering trail live in
// docs/features/shift-click-recruitment.md.

static const int MULTIPLIER_DEFAULT = 20;
static const int MULTIPLIER_MAX = 100;

// Shared UI helper that builds a recruit-command event and hands it to the
// simulation's command layer. Callee cleans the stack (ret 8); ECX is dead
// on entry (overwritten before its first read).
static const uintptr_t POST_RECRUIT_RVA = 0xef700;
// Player::canRecruit(unitTypeId) — validation only (gold, honor, weapons,
// peasants, unit cap). Writes nothing but the failure reason at +0x10e4.
static const uintptr_t CAN_RECRUIT_RVA = 0x18640;
// Returns the local player object; NULL once the Lord is dead.
static const uintptr_t GET_PLAYER_RVA = 0x11360;
// World/game singleton the player getter is called on (static instance).
static const uintptr_t WORLD_RVA = 0xd781d0;

// The five `call 0x4ef700` recruit-post sites retargeted to the stub below.
static const uintptr_t CALL_SITE_RVAS[] = {
    0x22083a, // SubPanelBarracks::process
    0x223ca9, // SubPanelEngineersGuild::process (unit 0x30 button)
    0x223dc9, // SubPanelEngineersGuild::process (engineer button, unit 0x1d)
    0x225622, // SubPanelMercPost::process
    0x228539, // SubPanelMonastery::process
};

// The siege camp builds its command events inline instead of calling the
// shared post helper, so its process() virtual is wrapped via the vtable.
static const uintptr_t SIEGE_VTABLE_SLOT_RVA = 0x5ccba4; // SubPanelSiegeCamp vtable[0]
static const uintptr_t SIEGE_PROCESS_RVA = 0x228c50; // SubPanelSiegeCamp::process
// Player::canRecruitSiege(engineTypeId) — validation-only sibling of
// canRecruit used by the siege pipeline.
static const uintptr_t CAN_RECRUIT_SIEGE_RVA = 0x18940;

// Siege pane button ids handled by SubPanelSiegeCamp::process.
static const int16_t SIEGE_SUBID_FIRST = 0xfff; // nine engine buttons ...
static const int16_t SIEGE_SUBID_ENGINES_LAST = 0x1007;
static const int16_t SIEGE_SUBID_ENGINEER = 0x1008; // ... plus the engineer button
static const uint32_t UNIT_ENGINEER = 0x1d;
static const uint16_t EVENT_CLICK = 0x66;

// Layout of the UI event the pane handlers receive (fields actually read by
// the game: type word at +0, button subid word at +2, payload dword at +8).
struct PaneEvent {
    uint16_t type;
    int16_t subId;
    uint32_t unused;
    uint32_t payload;
};

typedef void(__attribute__((stdcall)) * PostRecruitFn)(uint32_t ctx, uint32_t unitTypeId);
typedef uint8_t(__attribute__((thiscall)) * CanRecruitFn)(void *player, uint32_t unitTypeId);
typedef void *(__attribute__((thiscall)) * GetPlayerFn)(void *world);
typedef uint8_t(__attribute__((thiscall)) * PaneProcessFn)(void *pane, void *event);

static int s_multiplier = MULTIPLIER_DEFAULT;
static PaneProcessFn s_siegeProcess = nullptr;

// Live state for the settings overlay. The hooks stay in once installed; a
// multiplier of 1 makes every patched path queue exactly one unit, so turning
// the feature off never rewrites code the game might be executing.
static bool s_installed = false;
static bool s_failed = false;
static bool s_installPending = false;

// Same calling convention as the stock post function, so the three patched
// call sites need no register or stack fixups.
static void __attribute__((stdcall)) shiftRecruitPost(uint32_t ctx, uint32_t unitTypeId) {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    PostRecruitFn post = (PostRecruitFn)(base + POST_RECRUIT_RVA);

    post(ctx, unitTypeId);

    if (!(GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
        return;
    }

    GetPlayerFn getPlayer = (GetPlayerFn)(base + GET_PLAYER_RVA);
    CanRecruitFn canRecruit = (CanRecruitFn)(base + CAN_RECRUIT_RVA);
    void *world = (void *)(base + WORLD_RVA);

    // The simulation re-validates every command before executing it and
    // silently drops what the player cannot afford, so overshooting here can
    // never overdraw gold, weapons or peasants. The local canRecruit check
    // only avoids posting commands that already have no chance to succeed.
    for (int i = 1; i < s_multiplier; i++) {
        void *player = getPlayer(world);

        if (!player || !canRecruit(player, unitTypeId)) {
            break;
        }

        post(ctx, unitTypeId);
    }
}

// Re-invokes the stock handler once per extra unit: it rebuilds and posts the
// same command event each time, and the simulation validates every command
// before deducting anything (worker at RVA 0x1ab50 → canRecruitSiege), so a
// partially affordable batch stops at the engine's own checks.
static uint8_t __attribute__((thiscall)) siegeCampProcess(void *pane, void *event) {
    uint8_t handled = s_siegeProcess(pane, event);

    const PaneEvent *evt = (const PaneEvent *)event;

    if (evt->type != EVENT_CLICK || evt->subId < SIEGE_SUBID_FIRST ||
        evt->subId > SIEGE_SUBID_ENGINEER) {
        return handled;
    }

    if (!(GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
        return handled;
    }

    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    GetPlayerFn getPlayer = (GetPlayerFn)(base + GET_PLAYER_RVA);
    void *world = (void *)(base + WORLD_RVA);

    uint32_t unitTypeId = evt->payload;
    uintptr_t canRecruitRva = CAN_RECRUIT_SIEGE_RVA;

    if (evt->subId == SIEGE_SUBID_ENGINEER) {
        unitTypeId = UNIT_ENGINEER;
        canRecruitRva = CAN_RECRUIT_RVA;
    }

    CanRecruitFn canRecruit = (CanRecruitFn)(base + canRecruitRva);

    for (int i = 1; i < s_multiplier; i++) {
        void *player = getPlayer(world);

        if (!player || !canRecruit(player, unitTypeId)) {
            break;
        }

        s_siegeProcess(pane, event);
    }

    return handled;
}

static bool writeHooks() {
    bool any = false;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uintptr_t stub = (uintptr_t)reinterpret_cast<void *>(&shiftRecruitPost);

    for (uintptr_t rva : CALL_SITE_RVAS) {
        uint8_t *site = (uint8_t *)(base + rva);

        // Only retarget a site whose stock relative call still points at the
        // shared post function, so a future build that shifts this code
        // cannot be silently corrupted.
        uint8_t expected[5] = {0xe8, 0, 0, 0, 0};
        int32_t stockRel = (int32_t)((base + POST_RECRUIT_RVA) - (base + rva + 5));
        memcpy(expected + 1, &stockRel, sizeof(stockRel));

        if (memcmp(site, expected, sizeof(expected)) != 0) {
            continue;
        }

        uint8_t patched[5] = {0xe8, 0, 0, 0, 0};
        int32_t stubRel = (int32_t)(stub - (base + rva + 5));
        memcpy(patched + 1, &stubRel, sizeof(stubRel));

        DWORD oldProtect;
        VirtualProtect(site, sizeof(patched), PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy(site, patched, sizeof(patched));
        VirtualProtect(site, sizeof(patched), oldProtect, &oldProtect);
        any = true;
    }

    // Siege camp: swap SubPanelSiegeCamp's process() vtable entry for the
    // wrapper. Only patched if the slot still holds the stock handler.
    uintptr_t *slot = (uintptr_t *)(base + SIEGE_VTABLE_SLOT_RVA);

    if (*slot == base + SIEGE_PROCESS_RVA) {
        s_siegeProcess = (PaneProcessFn)(base + SIEGE_PROCESS_RVA);
        uintptr_t wrapper = (uintptr_t)reinterpret_cast<void *>(&siegeCampProcess);

        DWORD oldProtect;
        VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtect);
        *slot = wrapper;
        VirtualProtect(slot, sizeof(*slot), oldProtect, &oldProtect);
        any = true;
    }

    return any;
}

// Runs on the game thread: the retargeted call sites are live code, and the
// vtable slot is read by the pane the player may be clicking in. No float
// arithmetic here, as the tick site requires.
static void shiftRecruitTick() {
    if (!s_installPending) {
        return;
    }

    s_installPending = false;
    s_installed = writeHooks();
    s_failed = !s_installed;
}

int shiftRecruitMultiplier() { return s_multiplier; }

bool shiftRecruitFailed() { return s_failed; }

void shiftRecruitSetMultiplier(int multiplier) {
    if (multiplier < 1 || multiplier > MULTIPLIER_MAX) {
        return;
    }

    s_multiplier = multiplier;

    if (multiplier == 1 || s_installed) {
        return;
    }

    s_installPending = true;
}

void installShiftRecruit() {
    // Registered even when the feature is off, so the settings panel can switch
    // it on later: the tick is where the code rewrite happens, and the hook
    // behind it may only be installed at load time.
    registerFrameTick(&shiftRecruitTick);

    int multiplier = configInt("recruitment", "RecruitmentShiftMultiplier", MULTIPLIER_DEFAULT);

    if (multiplier < 0 || multiplier > MULTIPLIER_MAX) {
        multiplier = MULTIPLIER_DEFAULT;
    }

    // 0 and 1 both mean the stock single recruit.
    if (multiplier == 0) {
        multiplier = 1;
    }

    s_multiplier = multiplier;

    if (multiplier == 1) {
        return; // default off: the game code is left completely untouched
    }

    // Install time runs under the loader lock with no game thread yet, so the
    // rewrites are safe to do directly rather than deferring them to a frame.
    s_installed = writeHooks();
    s_failed = !s_installed;
}
