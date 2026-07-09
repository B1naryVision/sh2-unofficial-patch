#include "debugDump.h"

#ifndef DEBUG

void dumpEndgameDebug(const EndgameSnapshot &) {}

#else

#include "gameOffsets.h"
#include "session.h"
#include "unitTracker.h"
#include <fstream>
#include <iomanip>
#include <string>
#include <windows.h>

// ISteamFriends::GetPersonaName() — the local player's Steam display name.
// NOTE: this is the Steam name, which differs from the in-game (Firefly
// Online) name, so it cannot identify a player in the name array. Kept for
// the debug log only.
static const char *getSteamPersonaName() {
    HMODULE steamApi = GetModuleHandleA("steam_api.dll");

    if (!steamApi) {
        return NULL;
    }

    typedef void *(__cdecl * SteamFriends_fn)();
    auto pSteamFriends = (SteamFriends_fn)(void *)GetProcAddress(steamApi, "SteamFriends");

    if (!pSteamFriends) {
        return NULL;
    }

    void *friends = pSteamFriends();

    if (!friends) {
        return NULL;
    }

    void **vtable = *(void ***)friends;
    typedef const char *(__attribute__((thiscall)) * GetPersonaName_fn)(void *);
    auto getName = (GetPersonaName_fn)vtable[0];

    return getName(friends);
}

// Converts a null-terminated wide string to UTF-8 for the debug log.
static std::string wideToUtf8(const wchar_t *s) {
    int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);

    if (len <= 1) {
        return "";
    }

    std::string out(len - 1, '\0'); // exclude null terminator
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), len, NULL, NULL);
    return out;
}

void dumpEndgameDebug(const EndgameSnapshot &snap) {
    std::ofstream f("endgame_debug.txt");

    if (!f.is_open()) {
        return;
    }

    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uintptr_t *table = playerTable(base);

    const EndgameCounters &counters = endgameCounters();
    f << "=== Session counters ===\n";
    f << "  pollTicks=" << counters.pollTicks << " pollsSkippedIdle=" << counters.pollsSkippedIdle
      << " snapshots=" << counters.snapshots << " identityFreezes=" << counters.identityFreezes
      << "\n";
    f << "  slotReallocs=" << counters.slotReallocs << " recruitEvents=" << counters.recruitEvents
      << " sessionStarts=" << counters.sessionStarts << " sessionResets=" << counters.sessionResets
      << " inGame=" << sessionInGame() << "\n";

    const char *steamName = getSteamPersonaName();
    f << "=== Steam persona name ===\n";
    f << "  " << (steamName ? steamName : "(unavailable)") << "\n";

    // Local player identity: the slot-index global and the pointer global.
    int localSlotGlobal = *(int *)(base + LOCAL_SLOT_RVA);
    uintptr_t localPtr = *(uintptr_t *)(base + LOCAL_PLAYER_RVA);
    int localSlotResolved = -1;

    for (int i = 0; i < PLAYER_TABLE_SLOTS; ++i) {
        if (table[i] == localPtr) {
            localSlotResolved = i;
            break;
        }
    }

    f << "=== Local player ===\n";
    f << "  slotGlobal=" << localSlotGlobal << " ptr=0x" << std::hex << localPtr << std::dec
      << " resolvedSlot=" << localSlotResolved << "\n";

    f << "=== Player name array (raw) ===\n";

    for (int i = 0; i < NAME_ARRAY_COUNT; ++i) {
        uintptr_t recordBase = base + NAME_ARRAY_RVA + (uintptr_t)i * NAME_ARRAY_STRIDE;
        const uint8_t *rec = (const uint8_t *)recordBase;
        uintptr_t ptr = *(uintptr_t *)(recordBase + NAME_ARRAY_PTR_OFF);

        f << "  record[" << i << "]: bytes=";

        for (int b = 0; b < (int)NAME_ARRAY_STRIDE; ++b) {
            f << std::hex << std::setw(2) << std::setfill('0') << (int)rec[b] << " ";
        }

        f << std::dec;

        if (ptr == 0) {
            f << " (end of array)\n";
            break;
        }

        wchar_t name[kMaxNameChars];

        if (sessionLoadName(i + 1, name)) {
            f << " name=\"" << wideToUtf8(name) << "\"";
        } else {
            f << " ptr=0x" << std::hex << ptr << std::dec << " (not readable)";
        }

        f << "\n";
    }

    f << "=== Endgame slot dump ===\n";

    for (int i = 0; i < PLAYER_TABLE_SLOTS; ++i) {
        uintptr_t playerPtr = table[i];
        SlotView view = sessionSlot(i);

        f << "slot " << i << ": table=" << (playerPtr ? "set" : "null");

        if (playerPtr) {
            RawPlayerStat live = readRawPlayerStat(i, playerPtr);
            f << " live[color=" << live.colorIdx << " castle=" << PlayerView{playerPtr}.castle()
              << " gold=" << (int)bitsToFloat(live.goldBits) << " honor=" << live.honor
              << " troops=" << live.troops << " siege=" << live.siege << "]";
        }

        f << " cache[seen=" << view.seen << " objPtr=0x" << std::hex << view.objPtr << std::dec
          << " stableColor=" << view.stableColor << " stableName=\"" << wideToUtf8(view.stableName)
          << "\"";

        if (view.seen) {
            f << " color=" << view.stat->colorIdx
              << " gold=" << (int)bitsToFloat(view.stat->goldBits) << " honor=" << view.stat->honor
              << " troops=" << view.stat->troops << " siege=" << view.stat->siege;
        }

        f << "] hasUnits=" << slotHasRecruits(i) << "\n";
    }

    f << "=== Selected players (count=" << snap.count << ") ===\n";

    for (int p = 0; p < snap.count; ++p) {
        const PlayerRow &row = snap.players[p];
        uintptr_t playerPtr = 0;

        if (row.slot >= 0 && row.slot < PLAYER_TABLE_SLOTS) {
            playerPtr = table[row.slot];
        }

        int castle = -1;

        if (playerPtr) {
            castle = PlayerView{playerPtr}.castle();
        }

        bool isLocal = (row.slot == localSlotResolved) ||
                       (localSlotResolved < 0 && row.slot == localSlotGlobal);

        f << "  stat[" << p << "]: slot=" << row.slot << " color=" << row.colorIdx
          << " castle=" << castle << " gold=" << row.gold << " honor=" << row.honor << " name=\""
          << wideToUtf8(row.name) << "\"" << (isLocal ? " <-- LOCAL" : "");

        // Per-type unit counts, so ground-truth ("I made the monks") can be
        // matched against the slot and the assigned name.
        f << " units=[";
        bool first = true;

        for (int u = 0; u < kUnitCount; ++u) {
            if (row.units[u]) {
                if (!first) {
                    f << ", ";
                }

                f << kUnits[u].name << ":" << row.units[u];
                first = false;
            }
        }

        f << "]\n";
    }
}

#endif // DEBUG
