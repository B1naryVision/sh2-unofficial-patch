#include "unitTracker.h"
#include "../../core/hook.h"
#include "gameOffsets.h"
#include "session.h"
#include <cstring>
#include <windows.h>

// Hook site RVA 0x0EE3BE: `cmp byte ptr [ecx+0x179], 0` (7 bytes:
// 80 B9 79 01 00 00 00). This CMP is NOT conditional — it executes for both
// regular units and siege equipment, so one hook covers everything. Found via
// watchpoint on player+0xD8C (army count) + call-stack analysis; EDI = unit
// type ID and ESI = player_base + 0x674 at this site.
static const uintptr_t SPAWN_HOOK_RVA = 0x0EE3BE;

// Per-player, per-unit recruit count [table_slot][dense unit index].
static uint32_t s_unitsMade[PLAYER_TABLE_SLOTS][kUnitCount] = {};

// Return-address globals: must NOT be static (GAS cannot resolve local statics).
uintptr_t g_spawnUnitType = 0; // EDI at hook site
uintptr_t g_spawnPlayerArmy = 0; // ESI at hook site (= player_base + 0x674)
uintptr_t g_spawnReturn = 0;

// Runs on the game thread for every unit spawn (a hot path — it fires far more
// often than just on recruit, so the unit-type filter comes first). Must stay
// float-free: the hook site is mid-function (see core/frameTick.h).
extern "C" void unitSpawnLogic() {
    int unitIdx = unitIndexFromId((uint32_t)g_spawnUnitType);

    if (unitIdx < 0) {
        return;
    }

    uintptr_t playerBase = g_spawnPlayerArmy - ARMY_SUBOBJ_OFF;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uintptr_t *table = playerTable(base);

    for (int i = 0; i < PLAYER_TABLE_SLOTS; ++i) {
        if (table[i] == playerBase) {
            // Recruitment is definitive proof of a live game and a real
            // player: the session uses it to start tracking and to freeze
            // this slot's identity early. It must run BEFORE the count is
            // recorded — if this recruit is what reveals a recycled slot,
            // the session clears the slot's stale unit counts, and this
            // recruit belongs to the new occupant.
            sessionOnRecruit(i, playerBase);
            s_unitsMade[i][unitIdx]++;
            break;
        }
    }
}

// installHook writes a 5-byte JMP + 2 NOPs over the 7-byte CMP. The tick runs
// first (registers preserved), then the original CMP is re-emitted — its flags
// result is consumed right after the resume point, so it must be the last
// instruction before the jump back.
__declspec(naked) static void unitSpawnHook() {
    __asm__ volatile("pushal\n\t"
                     "movl %edi, _g_spawnUnitType\n\t"
                     "movl %esi, _g_spawnPlayerArmy\n\t"
                     "call _unitSpawnLogic\n\t"
                     "popal\n\t"
                     "cmpb $0, 0x179(%ecx)\n\t"
                     "jmp *_g_spawnReturn\n\t");
}

const uint32_t *unitCountsForSlot(int slot) { return s_unitsMade[slot]; }

bool slotHasRecruits(int slot) {
    if (slot < 0 || slot >= PLAYER_TABLE_SLOTS) {
        return false;
    }

    for (int i = 0; i < kUnitCount; ++i) {
        if (s_unitsMade[slot][i]) {
            return true;
        }
    }

    return false;
}

void resetUnitCounts() { memset(s_unitsMade, 0, sizeof(s_unitsMade)); }

void resetUnitCountsForSlot(int slot) {
    if (slot >= 0 && slot < PLAYER_TABLE_SLOTS) {
        memset(s_unitsMade[slot], 0, sizeof(s_unitsMade[slot]));
    }
}

void installUnitTracker() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uintptr_t site = base + SPAWN_HOOK_RVA;
    g_spawnReturn = site + 7;
    installHook((void *)site, reinterpret_cast<void *>(unitSpawnHook), 7);
}
