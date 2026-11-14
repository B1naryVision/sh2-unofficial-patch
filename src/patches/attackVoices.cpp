#include "attackVoices.h"
#include "../core/config.h"
#include "../core/frameTick.h"
#include "../replay/spectatorGuard.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

// Units answer a move order with a spoken line, but the stock game never plays
// the attack lines it loads for every soldier type. This plays one when the
// local player orders an attack, from the same click-time UI path, with the
// same speaker the move voice picks. Sound only. Offsets and the
// reverse-engineering trail live in docs/features/attack-voices.md.

// S2ActorHandler's attack posters: thiscall (slot, target, target handle),
// ret 0xc. Each has exactly one caller, in the troop-order dispatcher.
static const uintptr_t ATTACK_ACTOR_POST_RVA = 0xf38a0;
static const uintptr_t ATTACK_BUILDING_POST_RVA = 0xf39c0;
static const uintptr_t ATTACK_ACTOR_SITE_RVA = 0x2316a1;
static const uintptr_t ATTACK_BUILDING_SITE_RVA = 0x2319ac;
static const uintptr_t ACTOR_HANDLER_RVA = 0xdb8cb8;

// The move poster's speaker choice: thiscall (out {Actor *, handle}), ret 4.
static const uintptr_t PICK_SPEAKER_RVA = 0xefac0;
// Handle<CompositeSoundInfo> from a name: thiscall (this = handle, name), ret 4.
static const uintptr_t HANDLE_FROM_NAME_RVA = 0x2e410;
// SoundUtil::playStream(StreamTypes, Handle, unsigned, bool), cdecl.
static const uintptr_t PLAY_STREAM_IAT_RVA = 0x5008a4;

// The soldier move-voice virtual (vtable +0x1dc): Soldier's, shared by its 19
// voiced subclasses, and Knight's override. Only these classes' class objects
// carry the voice-handle table.
static const uintptr_t VT_MOVE_VOICE = 0x1dc;
static const uintptr_t SOLDIER_VOICE_RVA = 0xf5360;
static const uintptr_t KNIGHT_VOICE_RVA = 0x1042f0;

static const uintptr_t ACTOR_CLASS = 0x174;
static const uintptr_t ACTOR_MAX_HEALTH = 0x48;
static const uintptr_t CLASS_MOVE_VOICE = 0x124;

static const int STREAM_SPEECH = 1;

static const uint8_t SEH_PROLOGUE[5] = {0x55, 0x8b, 0xec, 0x6a, 0xff};
static const uint8_t PICK_SPEAKER_STOCK[9] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x2c, 0x83, 0xc8, 0xff};
static const uint8_t SOLDIER_VOICE_STOCK[7] = {0x55, 0x8b, 0xec, 0x53, 0x8b, 0x5d, 0x08};
static const uint8_t KNIGHT_VOICE_STOCK[6] = {0x55, 0x8b, 0xec, 0x56, 0x8b, 0xf1};

// Keyed by the unit's move line, which identifies its voice set. Every name
// here is a row of fx/sh2_speech.csv; the exe's own ATTACK_STRONGER/WEAKER
// names are not, for archers, crossbowmen, horse archers and the knight's
// STRONGER, so those use the recordings that exist.
struct VoiceSet {
    const char *move;
    const char *stronger;
    const char *weaker;
};

static const VoiceSet VOICE_SETS[] = {
    {"FXT_PEASANT_MOVE", "FXT_PEASANT_ATTACK_STRONGER", "FXT_PEASANT_ATTACK_WEAKER"},
    {"FXT_ASSASSIN_MOVE", "FXT_ASSASSIN_ATTACK_STRONGER", "FXT_ASSASSIN_ATTACK_WEAKER"},
    {"FXT_ENGINEER_MOVE", "FXT_ENGINEER_ATTACK_STRONGER", "FXT_ENGINEER_ATTACK_WEAKER"},
    {"FXT_LADDER_MOVE", "FXT_LADDER_ATTACK_STRONGER", "FXT_LADDER_ATTACK_WEAKER"},
    {"FXT_MACEMEN_MOVE", "FXT_MACEMEN_ATTACK_STRONGER", "FXT_MACEMEN_ATTACK_WEAKER"},
    {"FXT_MONK_MOVE", "FXT_MONK_ATTACK_STRONGER", "FXT_MONK_ATTACK_WEAKER"},
    {"FXT_PIKEMEN_MOVE", "FXT_PIKEMEN_ATTACK_STRONGER", "FXT_PIKEMEN_ATTACK_WEAKER"},
    {"FXT_SPEARMEN_MOVE", "FXT_SPEARMEN_ATTACK_STRONGER", "FXT_SPEARMEN_ATTACK_WEAKER"},
    {"FXT_SWORDSMEN_MOVE", "FXT_SWORDSMEN_ATTACK_STRONGER", "FXT_SWORDSMEN_ATTACK_WEAKER"},
    {"FXT_WARRIOR_MONK_MOVE", "FXT_WARRIOR_MONK_ATTACK_STRONGER", "FXT_WARRIOR_MONK_ATTACK_WEAKER"},
    {"FXT_AXETHROWER_MOVE", "FXT_AXETHROWER_ATTACK_STRONGER", "FXT_AXETHROWER_ATTACK_WEAKER"},
    {"FXT_BERSERKER_MOVE", "FXT_BERSERKER_ATTACK_STRONGER", "FXT_BERSERKER_ATTACK_WEAKER"},
    {"FXT_BOAT_MOVE", "FXT_BOAT_ATTACK_STRONGER", "FXT_BOAT_ATTACK_WEAKER"},
    {"FXT_OUTLAW_MOVE", "FXT_OUTLAW_ATTACK_STRONGER", "FXT_OUTLAW_ATTACK_WEAKER"},
    {"FXT_CAVALRY_MOVE", "FXT_CAVALRY_ATTACK_STRONGER", "FXT_CAVALRY_ATTACK_WEAKER"},
    {"FXT_THIEF_MOVE", "FXT_THIEF_ATTACK_STRONGER", "FXT_THIEF_ATTACK_WEAKER"},
    {"FXT_KNIGHT_MOVE", "FXT_KNIGHT_ATTACK_WEAKER", "FXT_KNIGHT_ATTACK_WEAKER"},
    {"FXT_ARCHER_MOVE", "FXT_ARCHER_ATTACK_MISSLE", "FXT_ARCHER_ATTACK_MISSLE"},
    {"FXT_CROSSBOWMEN_MOVE", "FXT_CROSSBOWMEN_ATTACK_MISSLE", "FXT_CROSSBOWMEN_ATTACK_MISSLE"},
    {"FXT_HORSEARCHER_MOVE", "FXT_HORSEARCHER_ATTACK_MISSLE", "FXT_HORSEARCHER_ATTACK_MISSLE"},
};

static const int VOICE_SET_COUNT = sizeof(VOICE_SETS) / sizeof(VOICE_SETS[0]);

struct ResolvedSet {
    uint32_t move;
    uint32_t stronger;
    uint32_t weaker;
};

struct ActorRef {
    const uint8_t *actor;
    uint32_t handle;
};

typedef void(__attribute__((thiscall)) * AttackPostFn)(
    void *handler, int slot, void *target, uint32_t targetHandle
);
typedef ActorRef *(__attribute__((thiscall)) * PickSpeakerFn)(void *handler, ActorRef *out);
typedef uint32_t *(__attribute__((thiscall)) * HandleFromNameFn)(
    uint32_t *handle, const char *name
);
typedef bool (*PlayStreamFn)(int streamType, uint32_t handle, unsigned count, bool flag);

static uintptr_t s_base = 0;
static AttackPostFn s_postActor = nullptr;
static AttackPostFn s_postBuilding = nullptr;
static PickSpeakerFn s_pickSpeaker = nullptr;
static HandleFromNameFn s_handleFromName = nullptr;
static ResolvedSet s_sets[VOICE_SET_COUNT];
static bool s_resolved = false;

// Live state for the settings panel. Once in, the hooks stay in; switching the
// feature off only silences the voice.
static bool s_enabled = false;
static bool s_installed = false;
static bool s_failed = false;
static bool s_installPending = false;

// Deferred to the first attack order: the sound resources are loaded by then,
// and they are not while DllMain runs.
static void resolveVoiceSets() {
    for (int i = 0; i < VOICE_SET_COUNT; ++i) {
        s_handleFromName(&s_sets[i].move, VOICE_SETS[i].move);
        s_handleFromName(&s_sets[i].stronger, VOICE_SETS[i].stronger);
        s_handleFromName(&s_sets[i].weaker, VOICE_SETS[i].weaker);
    }

    s_resolved = true;
}

static const ResolvedSet *voiceSetOf(const uint8_t *speaker) {
    uintptr_t vtable = *(const uintptr_t *)speaker;
    uintptr_t voice = *(const uintptr_t *)(vtable + VT_MOVE_VOICE);

    if (voice != s_base + SOLDIER_VOICE_RVA && voice != s_base + KNIGHT_VOICE_RVA) {
        return nullptr;
    }

    const uint8_t *unitClass = *(const uint8_t *const *)(speaker + ACTOR_CLASS);

    if (!unitClass) {
        return nullptr;
    }

    uint32_t move = *(const uint32_t *)(unitClass + CLASS_MOVE_VOICE);

    if (!s_resolved) {
        resolveVoiceSets();
    }

    for (int i = 0; i < VOICE_SET_COUNT; ++i) {
        if (s_sets[i].move == move) {
            return &s_sets[i];
        }
    }

    return nullptr;
}

// Maximum health, compared as raw bits: both are positive floats, whose bit
// patterns order the same way as their values.
static bool targetIsStronger(const uint8_t *target, const uint8_t *speaker) {
    int32_t targetHealth = 0;
    int32_t speakerHealth = 0;

    if (!target) {
        return false;
    }

    memcpy(&targetHealth, target + ACTOR_MAX_HEALTH, sizeof(targetHealth));
    memcpy(&speakerHealth, speaker + ACTOR_MAX_HEALTH, sizeof(speakerHealth));

    return targetHealth > speakerHealth;
}

static void playAttackVoice(void *handler, const uint8_t *targetActor) {
    ActorRef speaker = {nullptr, 0};

    // A replay refuses the click's command, so the unit should not answer it.
    if (!s_enabled || spectatorGuardActive()) {
        return;
    }

    s_pickSpeaker(handler, &speaker);

    if (!speaker.actor) {
        return;
    }

    const ResolvedSet *set = voiceSetOf(speaker.actor);

    if (!set) {
        return;
    }

    PlayStreamFn playStream = *(PlayStreamFn *)(s_base + PLAY_STREAM_IAT_RVA);

    if (!playStream) {
        return;
    }

    uint32_t line = set->weaker;

    if (targetIsStronger(targetActor, speaker.actor)) {
        line = set->stronger;
    }

    // The arguments the stock move voice passes.
    playStream(STREAM_SPEECH, line, 1, false);
}

static void __attribute__((thiscall))
attackActorPost(void *handler, int slot, void *target, uint32_t targetHandle) {
    s_postActor(handler, slot, target, targetHandle);
    playAttackVoice(handler, (const uint8_t *)target);
}

// A building cannot fight back, so it always gets the "weaker" line.
static void __attribute__((thiscall))
attackBuildingPost(void *handler, int slot, void *target, uint32_t targetHandle) {
    s_postBuilding(handler, slot, target, targetHandle);
    playAttackVoice(handler, nullptr);
}

// mov ecx, <S2ActorHandler> / call <poster>. The handler's address is
// base-relocated, so the operand is rebuilt rather than compared to disk bytes.
static bool siteIsStock(uintptr_t site, uintptr_t posterRva) {
    const uint8_t *movEcx = (const uint8_t *)(site - 5);
    uint32_t operand = 0;
    int32_t rel = 0;

    if (movEcx[0] != 0xb9) {
        return false;
    }

    memcpy(&operand, movEcx + 1, sizeof(operand));

    if (operand != (uint32_t)(s_base + ACTOR_HANDLER_RVA)) {
        return false;
    }

    if (*(const uint8_t *)site != 0xe8) {
        return false;
    }

    memcpy(&rel, (const void *)(site + 1), sizeof(rel));

    return site + 5 + rel == s_base + posterRva;
}

static bool bytesAre(uintptr_t rva, const uint8_t *expected, size_t length) {
    return memcmp((const void *)(s_base + rva), expected, length) == 0;
}

static bool retargetCall(uintptr_t site, void *stub) {
    int32_t rel = (int32_t)((uintptr_t)stub - (site + 5));
    DWORD oldProtect = 0;

    if (!VirtualProtect((void *)(site + 1), sizeof(rel), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    memcpy((void *)(site + 1), &rel, sizeof(rel));
    VirtualProtect((void *)(site + 1), sizeof(rel), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (void *)site, 5);
    return true;
}

static bool writeHooks() {
    s_base = (uintptr_t)GetModuleHandleA(NULL);

    uintptr_t actorSite = s_base + ATTACK_ACTOR_SITE_RVA;
    uintptr_t buildingSite = s_base + ATTACK_BUILDING_SITE_RVA;

    // All-or-nothing: every function the stubs call must be the expected one.
    if (!siteIsStock(actorSite, ATTACK_ACTOR_POST_RVA) ||
        !siteIsStock(buildingSite, ATTACK_BUILDING_POST_RVA)) {
        return false;
    }

    if (!bytesAre(ATTACK_ACTOR_POST_RVA, SEH_PROLOGUE, sizeof(SEH_PROLOGUE)) ||
        !bytesAre(ATTACK_BUILDING_POST_RVA, SEH_PROLOGUE, sizeof(SEH_PROLOGUE)) ||
        !bytesAre(HANDLE_FROM_NAME_RVA, SEH_PROLOGUE, sizeof(SEH_PROLOGUE)) ||
        !bytesAre(PICK_SPEAKER_RVA, PICK_SPEAKER_STOCK, sizeof(PICK_SPEAKER_STOCK)) ||
        !bytesAre(SOLDIER_VOICE_RVA, SOLDIER_VOICE_STOCK, sizeof(SOLDIER_VOICE_STOCK)) ||
        !bytesAre(KNIGHT_VOICE_RVA, KNIGHT_VOICE_STOCK, sizeof(KNIGHT_VOICE_STOCK))) {
        return false;
    }

    s_postActor = (AttackPostFn)(s_base + ATTACK_ACTOR_POST_RVA);
    s_postBuilding = (AttackPostFn)(s_base + ATTACK_BUILDING_POST_RVA);
    s_pickSpeaker = (PickSpeakerFn)(s_base + PICK_SPEAKER_RVA);
    s_handleFromName = (HandleFromNameFn)(s_base + HANDLE_FROM_NAME_RVA);

    if (!retargetCall(actorSite, (void *)attackActorPost)) {
        return false;
    }

    return retargetCall(buildingSite, (void *)attackBuildingPost);
}

// Runs on the game thread, between frames: the troop-order dispatcher that owns
// both call sites is not running then.
static void attackVoicesTick() {
    if (!s_installPending) {
        return;
    }

    s_installPending = false;
    s_installed = writeHooks();
    s_failed = !s_installed;
}

bool attackVoicesEnabled() { return s_enabled; }

bool attackVoicesFailed() { return s_failed; }

void attackVoicesSetEnabled(bool enabled) {
    s_enabled = enabled;

    if (!enabled || s_installed) {
        return;
    }

    s_installPending = true;
}

void installAttackVoices() {
    // Registered even when the feature is off, so the settings panel can switch
    // it on later.
    registerFrameTick(&attackVoicesTick);

    if (configInt("sound", "AttackVoices", 1) == 0) {
        return; // vanilla: the game code is left completely untouched
    }

    s_enabled = true;

    // Install time runs under the loader lock with no game thread yet.
    s_installed = writeHooks();
    s_failed = !s_installed;
}
