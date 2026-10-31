#include "crimeExclusion.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

// Clears the crime flag of two buildings in the game's crime-eligibility table,
// so their workers can no longer be picked to turn criminal. A fix that changes
// the simulation, so it is switched on per match by patches/matchRules.cpp only
// when every player has it. Offsets and the reverse-engineering trail live in
// docs/bugs/stuck-worker-crime.md.

// The table the crime routine (0x41ec30) walks: 12-byte records of
// {building type, other flag, can turn to crime}, ended by a type of 0.
static const uintptr_t CRIME_TABLE_RVA = 0x68d4a8;
static const uintptr_t RECORD_SIZE = 12;
static const uintptr_t CRIME_FLAG_OFF = 8;

struct CrimeRecord {
    uint32_t type;
    uint32_t other;
    uint32_t crime;
};

static const int WHEAT_FARM_INDEX = 10;
static const int CHANDLER_WORKSHOP_INDEX = 20;
static const int GONG_PIT_INDEX = 13;
static const int FALCONERS_POST_INDEX = 14;

// What the stock table holds at the four records we look at. The Gong Pit and
// Falconer's Post are Firefly's own exclusions, checked only to fingerprint the
// table; the other two are the ones we switch.
static const CrimeRecord WHEAT_FARM_STOCK = {0x03, 0, 1};
static const CrimeRecord CHANDLER_WORKSHOP_STOCK = {0x48, 0, 1};
static const CrimeRecord GONG_PIT_STOCK = {0x45, 0, 0};
static const CrimeRecord FALCONERS_POST_STOCK = {0x42, 0, 0};

// The crime routine's read of the flag: `cmp dword [eax + table+8], 0`. Its
// operand is an absolute address the loader relocates, so it is compared against
// one rebuilt from the base, never against the disk bytes.
static const uintptr_t CRIME_READER_RVA = 0x1ec83;
static const uint8_t CMP_DISP32_EAX[2] = {0x83, 0xb8};

static bool s_ready = false;
static bool s_enabled = false;

static uint8_t *recordAt(uintptr_t base, int index) {
    return (uint8_t *)(base + CRIME_TABLE_RVA + index * RECORD_SIZE);
}

static bool recordIs(uintptr_t base, int index, const CrimeRecord &expected) {
    return memcmp(recordAt(base, index), &expected, sizeof(expected)) == 0;
}

static bool readerIsStock(uintptr_t base) {
    const uint8_t *site = (const uint8_t *)(base + CRIME_READER_RVA);
    uint32_t operand = 0;

    if (memcmp(site, CMP_DISP32_EAX, sizeof(CMP_DISP32_EAX)) != 0) {
        return false;
    }

    memcpy(&operand, site + 2, sizeof(operand));

    return operand == (uint32_t)(base + CRIME_TABLE_RVA + CRIME_FLAG_OFF) && site[6] == 0x00;
}

bool installCrimeExclusion() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);

    // All-or-nothing: the record indices are only meaningful in this exact table.
    if (!recordIs(base, WHEAT_FARM_INDEX, WHEAT_FARM_STOCK) ||
        !recordIs(base, CHANDLER_WORKSHOP_INDEX, CHANDLER_WORKSHOP_STOCK) ||
        !recordIs(base, GONG_PIT_INDEX, GONG_PIT_STOCK) ||
        !recordIs(base, FALCONERS_POST_INDEX, FALCONERS_POST_STOCK)) {
        return false;
    }

    if (!readerIsStock(base)) {
        return false;
    }

    s_ready = true;

    return true;
}

static void writeCrimeFlag(uintptr_t base, int index, uint32_t value) {
    uint8_t *flag = recordAt(base, index) + CRIME_FLAG_OFF;
    DWORD oldProtect = 0;

    if (!VirtualProtect(flag, sizeof(value), PAGE_READWRITE, &oldProtect)) {
        return;
    }

    memcpy(flag, &value, sizeof(value));
    VirtualProtect(flag, sizeof(value), oldProtect, &oldProtect);
}

// Called on the simulation thread only, so no crime check is half-way through
// the table while it changes.
void crimeExclusionSetEnabled(bool enabled) {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uint32_t value = 1;

    if (!s_ready || enabled == s_enabled) {
        return;
    }

    if (enabled) {
        value = 0;
    }

    writeCrimeFlag(base, WHEAT_FARM_INDEX, value);
    writeCrimeFlag(base, CHANDLER_WORKSHOP_INDEX, value);
    s_enabled = enabled;
}

bool crimeExclusionAvailable() { return s_ready; }
