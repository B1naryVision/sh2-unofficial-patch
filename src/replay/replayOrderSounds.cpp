#include "replayOrderSounds.h"

#include "../core/exeBase.h"
#include "../core/hook.h"

#include <cstdint>
#include <cstring>

// FUN_005993d0, thiscall on the clan, one bool argument (move 1, attack 0),
// `ret 4`. Its prologue `push ebp; mov ebp,esp; sub esp,0x48` is position
// independent and no branch lands inside it, so the hook re-emits it as is.
static const uintptr_t GROUP_SOUND_RVA = 0x1993d0;
static const uint8_t GROUP_SOUND_STOCK[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x48};

// Not static: GAS names them from the naked hook.
extern "C" {
volatile uint32_t g_replayOrderSoundsMuted = 0;
uintptr_t g_replayOrderSoundsResume = 0;
void replayOrderSoundsHook();
}

// Muted: return as the stock function would, popping its argument. The flags it
// leaves are dead at a function entry.
__declspec(naked) void replayOrderSoundsHook() {
    __asm__ volatile("cmpl $0, _g_replayOrderSoundsMuted\n\t"
                     "jne 1f\n\t"
                     "pushl %ebp\n\t"
                     "movl %esp, %ebp\n\t"
                     "subl $0x48, %esp\n\t"
                     "jmp *_g_replayOrderSoundsResume\n\t"
                     "1:\n\t"
                     "ret $4\n\t");
}

static bool s_installed = false;

void replayOrderSoundsSetMuted(bool muted) {
    uint32_t value = 0;

    if (muted && s_installed) {
        value = 1;
    }

    g_replayOrderSoundsMuted = value;
}

void installReplayOrderSounds() {
    uint8_t *site = (uint8_t *)(exeBase() + GROUP_SOUND_RVA);

    if (memcmp(site, GROUP_SOUND_STOCK, sizeof(GROUP_SOUND_STOCK)) != 0) {
        return; // unknown build: order sounds play in replays as before
    }

    g_replayOrderSoundsResume = (uintptr_t)site + sizeof(GROUP_SOUND_STOCK);
    installHook(site, (void *)replayOrderSoundsHook, sizeof(GROUP_SOUND_STOCK));
    s_installed = true;
}
