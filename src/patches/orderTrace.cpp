#include "orderTrace.h"

#ifndef DEBUG

void installOrderTrace() {}

#else

#include "../core/hook.h"
#include "../core/readableMemory.h"
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <windows.h>

// Diagnostic for ranged units ignoring move orders in combat. Logs every move
// order a troop receives together with its action stack, then every change to
// that unit's stack for a while afterwards, to order_trace.txt next to the exe.
// Read-only: the game sees the same calls with the same arguments. The action
// stack model and the open question are in docs/bugs/ranged-move-orders.md.

// Soldier::moveTo is vtable slot +0x98. The formation pass calls it once per
// unit per player move order.
static const uintptr_t MOVE_TO_SLOT = 0x98;

// The two ways a ranged unit is handed a target: setTarget (+0x19c, which for
// ranged classes only stores it at +0x194/+0x198 for think to act on) and
// slot +0x22c, which stores it and starts shooting.
static const uintptr_t SET_TARGET_SLOT = 0x19c;
static const uintptr_t ENGAGE_SLOT = 0x22c;

struct TrackedClass {
    const char *name;
    uintptr_t vtableRva;
    uintptr_t moveToRva;
    uintptr_t setTargetRva; // 0: not wrapped
    uintptr_t engageRva; // 0: not wrapped
};

// Four ranged classes and four melee ones to compare them with.
static const TrackedClass CLASSES[] = {
    {"Archer", 0x5b0d3c, 0x113400, 0xfda60, 0x112f80},
    {"Crossbowman", 0x5ab784, 0xf7270, 0xfda60, 0xfda90},
    {"AxeThrower", 0x5ae9ec, 0xf7270, 0xfda60, 0x109c80},
    {"HorseArcher", 0x5af464, 0x10d040, 0xfda60, 0},
    {"Swordsman", 0x5adf74, 0xf7270, 0, 0},
    {"Pikeman", 0x5ad5ac, 0xf7270, 0, 0},
    {"Spearman", 0x5ada84, 0xf7270, 0, 0},
    {"Maceman", 0x5acc34, 0xf7270, 0, 0},
};

static const int CLASS_COUNT = sizeof(CLASSES) / sizeof(CLASSES[0]);
static const int ARCHER = 0;

typedef void(__attribute__((thiscall)) * MoveToFn)(void *, uint32_t, uint32_t, uint32_t);
typedef void(__attribute__((thiscall)) * TargetFn)(void *, uint32_t, uint32_t);

static uintptr_t s_vtables[CLASS_COUNT];
static MoveToFn s_moveTo[CLASS_COUNT];
static TargetFn s_setTarget[CLASS_COUNT];
static TargetFn s_engage[CLASS_COUNT];

// Action stack hooks. All three re-emit position-independent bytes.
// clearActions: push ebx / mov ebx,ecx / cmp byte [ebx+0x139],0
static const uintptr_t CLEAR_RVA = 0x389f70;
static const uint8_t CLEAR_STOCK[10] = {0x53, 0x8b, 0xd9, 0x80, 0xbb, 0x39, 0x01, 0x00, 0x00, 0x00};

// pushAction(action, parentStep, flags): push ebp / mov ebp,esp / push esi /
// mov esi,ecx / cmp byte [esi+0x139],0
static const uintptr_t PUSH_RVA = 0x38a020;
static const uint8_t PUSH_STOCK[13] = {0x55, 0x8b, 0xec, 0x56, 0x8b, 0xf1, 0x80,
                                       0xbe, 0x39, 0x01, 0x00, 0x00, 0x00};

// pushInterrupt(action, flags), past its SEH prologue so the frame is set up:
// mov esi,ecx / cmp byte [esi+0x139],0
static const uintptr_t INTERRUPT_SITE_RVA = 0x38a0e6;
static const uint8_t INTERRUPT_STOCK[9] = {0x8b, 0xf1, 0x80, 0xbe, 0x39, 0x01, 0x00, 0x00, 0x00};

// Action classes, by vtable.
static const uintptr_t SCRIPT_ACTION_VT_RVA = 0x5f47b0;
static const uintptr_t INTERRUPT_ACTION_VT_RVA = 0x5f46fc;
static const uintptr_t GOTO_LOCATION_VT_RVA = 0x5f4738;
static const uintptr_t EVASIVE_ACTION_VT_RVA = 0x5f4800;

static const uintptr_t GAME_TICK_RVA = 0x6e8bd4;
static const uintptr_t GAME_PAUSED_BYTE_RVA = 0xd781d5; // S2Game+5, moveTo's first bypass

static const uintptr_t ACTOR_TICK = 0x44;
static const uintptr_t ACTOR_PATH = 0xb4;
static const uintptr_t ACTOR_STACK_BEGIN = 0x158;
static const uintptr_t ACTOR_STACK_END = 0x15c;
static const uintptr_t ACTOR_MOVE_BYPASS = 0x17b;
static const uintptr_t ACTOR_ORDERED = 0x300;
static const uintptr_t ACTOR_TARGET_HANDLE = 0x198;
static const uintptr_t ARCHER_SHOT_HANDLE = 0x444;
static const uintptr_t ACTION_STEP = 0x08;
static const uintptr_t ACTION_FLAGS = 0x18;
static const uintptr_t ACTION_ID = 0x24;

static const uint32_t MOVE_ACCEPTED_BIT = 0x80;
static const uint32_t INHERIT_BIT = 0x80000000;
static const size_t MAX_STACK_LISTED = 8;

// The player's own commands, by call site (recipe A: each e8 is retargeted).
// The right-click dispatch (0x631320) asks S2ActorHandler for an enemy under
// the cursor; a hit becomes an attack order, a miss a move.
struct CallSite {
    uintptr_t siteRva;
    uintptr_t targetRva;
};

static const CallSite PICK_ENEMY_CALL = {0x2313f2, 0xf19f0};
static const CallSite ATTACK_ORDER_CALL = {0x2316a1, 0xf38a0};
static const CallSite STOP_BUTTON_CALL = {0x22e9bb, 0xf3140};

typedef bool(__attribute__((thiscall)) * PickEnemyFn)(
    void *, uint32_t, int32_t, int32_t, void *, uint32_t *, uint32_t *
);
typedef void(__attribute__((thiscall)) * AttackOrderFn)(void *, uint32_t, uint32_t, uint32_t);
typedef void(__attribute__((thiscall)) * StopSelectedFn)(void *, uint32_t);

static PickEnemyFn s_pickEnemy;
static AttackOrderFn s_attackOrder;
static StopSelectedFn s_stopSelected;

// Units whose stack changes are logged: those given a move order within the
// last TRACE_WINDOW_TICKS ticks.
static const uint32_t TRACE_WINDOW_TICKS = 400;
static const int MAX_RECENT = 128;

struct RecentOrder {
    const void *actor;
    uint32_t tick;
};

static RecentOrder s_recent[MAX_RECENT];
static int s_recentNext = 0;

static uintptr_t s_base = 0;
static FILE *s_log = NULL;

extern "C" {
uintptr_t g_orderTraceClearResume = 0;
uintptr_t g_orderTracePushResume = 0;
uintptr_t g_orderTraceInterruptResume = 0;
void orderTraceOnClear(const uint8_t *actor, uintptr_t ret, uintptr_t callerFrame);
void orderTraceOnPush(
    const uint8_t *actor, const uint8_t *action, uint32_t flags, uintptr_t ret, int viaInterrupt
);
void orderTraceClearHook();
void orderTracePushHook();
void orderTraceInterruptHook();
}

static void logLine(const char *fmt, ...) {
    char line[512];
    va_list args;

    if (!s_log) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    fputs(line, s_log);
    fputc('\n', s_log);
    fflush(s_log);
}

static uint32_t readU32(const uint8_t *p, uintptr_t off) {
    uint32_t v = 0;
    memcpy(&v, p + off, sizeof(v));

    return v;
}

static uint32_t gameTick() { return *(const uint32_t *)(s_base + GAME_TICK_RVA); }

static int classOf(const uint8_t *actor) {
    uintptr_t vt = readU32(actor, 0);

    for (int i = 0; i < CLASS_COUNT; ++i) {
        if (s_vtables[i] == vt) {
            return i;
        }
    }

    return -1;
}

static size_t stackDepth(const uint8_t *actor) {
    uint32_t begin = readU32(actor, ACTOR_STACK_BEGIN);
    uint32_t end = readU32(actor, ACTOR_STACK_END);

    if (end < begin) {
        return 0;
    }

    return (end - begin) / 4;
}

static const uint8_t *actionAt(const uint8_t *actor, size_t index) {
    const uint8_t *begin = (const uint8_t *)(uintptr_t)readU32(actor, ACTOR_STACK_BEGIN);

    return (const uint8_t *)(uintptr_t)readU32(begin, index * 4);
}

// The same walk as the game's own 0x787190: OR the flags from the top down,
// stopping after the first action that does not inherit.
static uint32_t inputMask(const uint8_t *actor) {
    size_t depth = stackDepth(actor);
    uint32_t mask = 0;

    for (size_t i = depth; i > 0; --i) {
        uint32_t flags = readU32(actionAt(actor, i - 1), ACTION_FLAGS);
        mask |= flags;

        if (!(flags & INHERIT_BIT)) {
            break;
        }
    }

    return mask;
}

static void describeAction(const uint8_t *action, char *out, size_t cap) {
    uintptr_t vt = readU32(action, 0);
    uint32_t flags = readU32(action, ACTION_FLAGS);

    if (vt == s_base + SCRIPT_ACTION_VT_RVA) {
        snprintf(
            out, cap, "S%x.%u/%x", readU32(action, ACTION_ID), readU32(action, ACTION_STEP), flags
        );
    } else if (vt == s_base + INTERRUPT_ACTION_VT_RVA) {
        snprintf(out, cap, "Int/%x", flags);
    } else if (vt == s_base + GOTO_LOCATION_VT_RVA) {
        snprintf(out, cap, "Goto/%x", flags);
    } else if (vt == s_base + EVASIVE_ACTION_VT_RVA) {
        snprintf(out, cap, "Evade/%x", flags);
    } else {
        snprintf(out, cap, "?%x/%x", (uint32_t)(vt - s_base), flags);
    }
}

// Bottom to top, e.g. "[S0.1/800000c3 Int/80000000 S1.2/81] mask=81".
static void describeStack(const uint8_t *actor, char *out, size_t cap) {
    size_t depth = stackDepth(actor);
    size_t used = 0;

    used += snprintf(out, cap, "[");

    for (size_t i = 0; i < depth && i < MAX_STACK_LISTED && used < cap; ++i) {
        char one[48];
        describeAction(actionAt(actor, i), one, sizeof(one));
        used += snprintf(out + used, cap - used, "%s%s", i ? " " : "", one);
    }

    if (depth > MAX_STACK_LISTED && used < cap) {
        used += snprintf(out + used, cap - used, " +%u", (unsigned)(depth - MAX_STACK_LISTED));
    }

    if (used < cap) {
        snprintf(out + used, cap - used, "] mask=%x", inputMask(actor));
    }
}

// The target handle think compares against; for Archers also the one it is
// already shooting at, since a mismatch is what makes think start an attack.
static void describeUnit(const uint8_t *actor, int cls, char *out, size_t cap) {
    size_t used = snprintf(
        out, cap, "t%u path=%d ordered=%d tgt=%x", readU32(actor, ACTOR_TICK),
        readU32(actor, ACTOR_PATH) != 0, actor[ACTOR_ORDERED], readU32(actor, ACTOR_TARGET_HANDLE)
    );

    if (cls == ARCHER && used < cap) {
        snprintf(out + used, cap - used, " shot=%x", readU32(actor, ARCHER_SHOT_HANDLE));
    }
}

static void rememberOrder(const void *actor, uint32_t tick) {
    for (int i = 0; i < MAX_RECENT; ++i) {
        if (s_recent[i].actor == actor) {
            s_recent[i].tick = tick;
            return;
        }
    }

    s_recent[s_recentNext].actor = actor;
    s_recent[s_recentNext].tick = tick;
    s_recentNext = (s_recentNext + 1) % MAX_RECENT;
}

static bool orderedRecently(const void *actor, uint32_t tick) {
    for (int i = 0; i < MAX_RECENT; ++i) {
        if (s_recent[i].actor == actor && tick - s_recent[i].tick <= TRACE_WINDOW_TICKS) {
            return true;
        }
    }

    return false;
}

static void __attribute__((thiscall))
moveToWrapper(uint8_t *actor, uint32_t x, uint32_t y, uint32_t layer) {
    int cls = classOf(actor);
    char before[384];
    char after[384];
    char unit[96];
    uint32_t tick = gameTick();
    const char *gate = "SHUT";
    const char *outcome = "ACCEPTED";

    if (cls < 0) {
        return;
    }

    if (inputMask(actor) & MOVE_ACCEPTED_BIT) {
        gate = "open";
    }

    describeStack(actor, before, sizeof(before));
    describeUnit(actor, cls, unit, sizeof(unit));
    uint8_t bypass = *(const uint8_t *)(s_base + GAME_PAUSED_BYTE_RVA) | actor[ACTOR_MOVE_BYPASS];
    logLine(
        "%u MOVE %s %p gate=%s bypass=%d %s", tick, CLASSES[cls].name, (void *)actor, gate, bypass,
        unit
    );
    logLine("      before %s", before);

    // Remembered first, so the clear and push of an accepted order are logged.
    rememberOrder(actor, tick);
    s_moveTo[cls](actor, x, y, layer);
    describeStack(actor, after, sizeof(after));

    if (strcmp(before, after) == 0) {
        outcome = "UNCHANGED";
    }

    logLine("      after  %s -> %s", after, outcome);
}

// callerFrame is ebp at clearActions' entry, i.e. the caller's frame when the
// caller keeps one (startAttack does); its return address names who called it.
extern "C" void orderTraceOnClear(const uint8_t *actor, uintptr_t ret, uintptr_t callerFrame) {
    int cls = classOf(actor);
    char stack[384];
    char unit[96];
    uint32_t tick = gameTick();
    uint32_t via = 0;

    if (cls < 0 || !orderedRecently(actor, tick)) {
        return;
    }

    if (observerReadable(callerFrame + 4, sizeof(via))) {
        via = *(const uint32_t *)(callerFrame + 4) - (uint32_t)s_base;
    }

    describeStack(actor, stack, sizeof(stack));
    describeUnit(actor, cls, unit, sizeof(unit));
    logLine(
        "%u CLEAR %s %p by %x via %x %s was %s", tick, CLASSES[cls].name, (const void *)actor,
        (uint32_t)(ret - s_base), via, unit, stack
    );
}

static void
logTargetCall(const char *what, const uint8_t *actor, int cls, uint32_t handle, uintptr_t ret) {
    char stack[384];
    char unit[96];
    uint32_t tick = gameTick();

    if (!orderedRecently(actor, tick)) {
        return;
    }

    describeStack(actor, stack, sizeof(stack));
    describeUnit(actor, cls, unit, sizeof(unit));
    logLine(
        "%u %s %s %p to %x by %x %s on %s", tick, what, CLASSES[cls].name, (const void *)actor,
        handle, (uint32_t)(ret - s_base), unit, stack
    );
}

static void __attribute__((thiscall))
setTargetWrapper(uint8_t *actor, uint32_t obj, uint32_t handle) {
    int cls = classOf(actor);
    uintptr_t ret = (uintptr_t)__builtin_return_address(0);

    if (cls < 0) {
        return;
    }

    logTargetCall("SETTARGET", actor, cls, handle, ret);
    s_setTarget[cls](actor, obj, handle);
}

static void __attribute__((thiscall)) engageWrapper(uint8_t *actor, uint32_t obj, uint32_t handle) {
    int cls = classOf(actor);
    uintptr_t ret = (uintptr_t)__builtin_return_address(0);

    if (cls < 0) {
        return;
    }

    logTargetCall("ENGAGE", actor, cls, handle, ret);
    s_engage[cls](actor, obj, handle);
}

// MSVC RTTI from a vtable: complete-object locator at vt-4, type descriptor at
// COL+0xc, mangled name at TD+8 (".?AVArcher@Stronghold2@@").
static const char *rttiName(uintptr_t vt) {
    uintptr_t col = 0;
    uintptr_t td = 0;

    if (!observerReadable(vt - 4, 4)) {
        return "?";
    }

    col = *(const uint32_t *)(vt - 4);

    if (!observerReadable(col + 0xc, 4)) {
        return "?";
    }

    td = *(const uint32_t *)(col + 0xc);

    if (!observerReadable(td + 8, 32)) {
        return "?";
    }

    return (const char *)(td + 8);
}

static const char *objectName(uintptr_t obj) {
    if (!obj || !observerReadable(obj, 4)) {
        return "none";
    }

    return rttiName(*(const uint32_t *)obj);
}

// The right-click's enemy-under-cursor test. A hit here is what turns the click
// into an attack order (if the enemy is also in front of the ground).
static bool __attribute__((thiscall)) pickEnemyWrapper(
    void *handler, uint32_t slot, int32_t x, int32_t y, void *point, uint32_t *picked,
    uint32_t *dist
) {
    bool hit = s_pickEnemy(handler, slot, x, y, point, picked, dist);

    if (hit) {
        logLine(
            "%u CLICK mouse=%d,%d enemy %s %x dist=%x", gameTick(), x, y, objectName(picked[0]),
            picked[1], *dist
        );
    } else {
        logLine("%u CLICK mouse=%d,%d no enemy", gameTick(), x, y);
    }

    return hit;
}

static void __attribute__((thiscall))
attackOrderWrapper(void *handler, uint32_t slot, uint32_t obj, uint32_t handle) {
    logLine("%u ATTACKORDER target %s %x", gameTick(), objectName(obj), handle);
    s_attackOrder(handler, slot, obj, handle);
}

// Only the troop panel's Stop button; a Stop without this line is the hotkey.
static void __attribute__((thiscall)) stopButtonWrapper(void *handler, uint32_t slot) {
    logLine("%u STOPBUTTON", gameTick());
    s_stopSelected(handler, slot);
}

extern "C" void orderTraceOnPush(
    const uint8_t *actor, const uint8_t *action, uint32_t flags, uintptr_t ret, int viaInterrupt
) {
    int cls = classOf(actor);
    char pushed[48];
    char stack[384];
    uint32_t tick = gameTick();

    if (cls < 0 || !orderedRecently(actor, tick)) {
        return;
    }

    describeAction(action, pushed, sizeof(pushed));
    describeStack(actor, stack, sizeof(stack));
    logLine(
        "%u %s %s %p %s flags=%x by %x onto %s", tick, viaInterrupt ? "PUSHINT" : "PUSH",
        CLASSES[cls].name, (const void *)actor, pushed, flags, (uint32_t)(ret - s_base), stack
    );
}

// Each stub reports, then re-emits the bytes it replaced and resumes. pushal +
// pushfl put the caller's return address at esp+36.
__declspec(naked) void orderTraceClearHook() {
    __asm__ volatile("pushal\n\t"
                     "pushfl\n\t"
                     "pushl %ebp\n\t"
                     "pushl 40(%esp)\n\t"
                     "pushl %ecx\n\t"
                     "call _orderTraceOnClear\n\t"
                     "addl $12, %esp\n\t"
                     "popfl\n\t"
                     "popal\n\t"
                     "pushl %ebx\n\t"
                     "movl %ecx, %ebx\n\t"
                     "cmpb $0, 0x139(%ebx)\n\t"
                     "jmp *_g_orderTraceClearResume\n\t");
}

// Stack after pushal + pushfl: ret 36, action 40, parentStep 44, flags 48.
__declspec(naked) void orderTracePushHook() {
    __asm__ volatile("pushal\n\t"
                     "pushfl\n\t"
                     "pushl $0\n\t"
                     "pushl 40(%esp)\n\t"
                     "pushl 56(%esp)\n\t"
                     "pushl 52(%esp)\n\t"
                     "pushl %ecx\n\t"
                     "call _orderTraceOnPush\n\t"
                     "addl $20, %esp\n\t"
                     "popfl\n\t"
                     "popal\n\t"
                     "pushl %ebp\n\t"
                     "movl %esp, %ebp\n\t"
                     "pushl %esi\n\t"
                     "movl %ecx, %esi\n\t"
                     "cmpb $0, 0x139(%esi)\n\t"
                     "jmp *_g_orderTracePushResume\n\t");
}

// Inside pushInterrupt the frame is set up: ret [ebp+4], action [ebp+8],
// flags [ebp+0xc], and ecx still holds the actor.
__declspec(naked) void orderTraceInterruptHook() {
    __asm__ volatile("pushal\n\t"
                     "pushfl\n\t"
                     "pushl $1\n\t"
                     "pushl 4(%ebp)\n\t"
                     "pushl 12(%ebp)\n\t"
                     "pushl 8(%ebp)\n\t"
                     "pushl %ecx\n\t"
                     "call _orderTraceOnPush\n\t"
                     "addl $20, %esp\n\t"
                     "popfl\n\t"
                     "popal\n\t"
                     "movl %ecx, %esi\n\t"
                     "cmpb $0, 0x139(%esi)\n\t"
                     "jmp *_g_orderTraceInterruptResume\n\t");
}

static bool writeSlot(uintptr_t slot, uintptr_t value) {
    DWORD oldProtect = 0;

    if (!VirtualProtect((void *)slot, sizeof(value), PAGE_READWRITE, &oldProtect)) {
        return false;
    }

    memcpy((void *)slot, &value, sizeof(value));
    VirtualProtect((void *)slot, sizeof(value), oldProtect, &oldProtect);

    return true;
}

// A class whose entry has no RVA for this slot is not wrapped there.
static bool callIsStock(const CallSite &call) {
    const uint8_t *site = (const uint8_t *)(s_base + call.siteRva);
    int32_t rel = 0;

    if (site[0] != 0xe8) {
        return false;
    }

    memcpy(&rel, site + 1, sizeof(rel));

    return (uintptr_t)(site + 5) + rel == s_base + call.targetRva;
}

static void retargetCall(const CallSite &call, void *fn) {
    uint8_t *site = (uint8_t *)(s_base + call.siteRva);
    int32_t rel = (int32_t)((uintptr_t)fn - (uintptr_t)(site + 5));
    DWORD oldProtect = 0;

    if (!VirtualProtect(site + 1, sizeof(rel), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return;
    }

    memcpy(site + 1, &rel, sizeof(rel));
    VirtualProtect(site + 1, sizeof(rel), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
}

static bool slotIsStock(int cls, uintptr_t slotOffset, uintptr_t expectedRva) {
    if (!expectedRva) {
        return true;
    }

    return *(const uintptr_t *)(s_base + CLASSES[cls].vtableRva + slotOffset) ==
           s_base + expectedRva;
}

static void openLog() {
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = NULL;

    if (len == 0 || len >= MAX_PATH) {
        return;
    }

    slash = strrchr(path, '\\');

    if (!slash || (size_t)(slash - path) + sizeof("\\order_trace.txt") > MAX_PATH) {
        return;
    }

    strcpy(slash + 1, "order_trace.txt");
    s_log = fopen(path, "w");
}

void installOrderTrace() {
    s_base = (uintptr_t)GetModuleHandleA(NULL);

    // All-or-nothing: every vtable slot and every hook site must be stock.
    for (int i = 0; i < CLASS_COUNT; ++i) {
        uintptr_t slot = s_base + CLASSES[i].vtableRva + MOVE_TO_SLOT;

        if (*(const uintptr_t *)slot != s_base + CLASSES[i].moveToRva) {
            return;
        }

        if (!slotIsStock(i, SET_TARGET_SLOT, CLASSES[i].setTargetRva) ||
            !slotIsStock(i, ENGAGE_SLOT, CLASSES[i].engageRva)) {
            return;
        }
    }

    if (!callIsStock(PICK_ENEMY_CALL) || !callIsStock(ATTACK_ORDER_CALL) ||
        !callIsStock(STOP_BUTTON_CALL)) {
        return;
    }

    if (memcmp((void *)(s_base + CLEAR_RVA), CLEAR_STOCK, sizeof(CLEAR_STOCK)) != 0 ||
        memcmp((void *)(s_base + PUSH_RVA), PUSH_STOCK, sizeof(PUSH_STOCK)) != 0 ||
        memcmp((void *)(s_base + INTERRUPT_SITE_RVA), INTERRUPT_STOCK, sizeof(INTERRUPT_STOCK)) !=
            0) {
        return;
    }

    openLog();

    if (!s_log) {
        return;
    }

    logLine("order trace: tick EVENT class actor ... (see docs/bugs/ranged-move-orders.md)");

    for (int i = 0; i < CLASS_COUNT; ++i) {
        uintptr_t slot = s_base + CLASSES[i].vtableRva + MOVE_TO_SLOT;
        s_vtables[i] = s_base + CLASSES[i].vtableRva;
        s_moveTo[i] = (MoveToFn)(*(const uintptr_t *)slot);
        writeSlot(slot, (uintptr_t)(void *)moveToWrapper);

        if (CLASSES[i].setTargetRva) {
            slot = s_base + CLASSES[i].vtableRva + SET_TARGET_SLOT;
            s_setTarget[i] = (TargetFn)(*(const uintptr_t *)slot);
            writeSlot(slot, (uintptr_t)(void *)setTargetWrapper);
        }

        if (CLASSES[i].engageRva) {
            slot = s_base + CLASSES[i].vtableRva + ENGAGE_SLOT;
            s_engage[i] = (TargetFn)(*(const uintptr_t *)slot);
            writeSlot(slot, (uintptr_t)(void *)engageWrapper);
        }
    }

    s_pickEnemy = (PickEnemyFn)(s_base + PICK_ENEMY_CALL.targetRva);
    s_attackOrder = (AttackOrderFn)(s_base + ATTACK_ORDER_CALL.targetRva);
    s_stopSelected = (StopSelectedFn)(s_base + STOP_BUTTON_CALL.targetRva);
    retargetCall(PICK_ENEMY_CALL, (void *)pickEnemyWrapper);
    retargetCall(ATTACK_ORDER_CALL, (void *)attackOrderWrapper);
    retargetCall(STOP_BUTTON_CALL, (void *)stopButtonWrapper);

    g_orderTraceClearResume = s_base + CLEAR_RVA + sizeof(CLEAR_STOCK);
    g_orderTracePushResume = s_base + PUSH_RVA + sizeof(PUSH_STOCK);
    g_orderTraceInterruptResume = s_base + INTERRUPT_SITE_RVA + sizeof(INTERRUPT_STOCK);
    installHook((void *)(s_base + CLEAR_RVA), (void *)orderTraceClearHook, sizeof(CLEAR_STOCK));
    installHook((void *)(s_base + PUSH_RVA), (void *)orderTracePushHook, sizeof(PUSH_STOCK));
    installHook(
        (void *)(s_base + INTERRUPT_SITE_RVA), (void *)orderTraceInterruptHook,
        sizeof(INTERRUPT_STOCK)
    );
}

#endif
