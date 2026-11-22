#include "replayKeyframes.h"

#include "keyframeSerial.h"

#include "../core/exeBase.h"
#include "../core/observerContext.h"
#include "../core/readableMemory.h"
#include "../patches/endgameStats/killTracker.h"
#include "../patches/endgameStats/session.h"
#include "../patches/endgameStats/unitTracker.h"
#include "../patches/matchRules.h"
#include "prngGuard.h"
#include "replayCommon.h"
#include "replayFormat.h"
#include "replayPlayer.h"
#include "replaySession.h"
#include "replayViewer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>
#include <windows.h>

// One keyframe per this much match time. A rewind then skips at most this far
// from the keyframe it resumes at: a few seconds even in a late-game battle,
// where the simulation runs 5-8x real time.
static const uint32_t KEYFRAME_SECONDS = 60;
static const uint32_t KEYFRAME_MIN_JOBS = 50;
// About 1 MB of disk each; 150 is two and a half hours of match.
static const uint32_t KEYFRAMES_MAX = 150;

// RandomIter #0's cursor (Game at RVA 0x6e8bcc, +0x40078). A load draws it
// (Hovel vt+0xc), so the keyframe's own value is put back at the epoch, as the
// checkpoint's is.
static const uintptr_t GAME_PTR_RVA = 0x6e8bcc;
static const uintptr_t ITER0_CURSOR_OFF = 0x40078;

// A TunnelHole's "send the first engineer" flag (byte +0x114) is set by its
// ctor (0x4c8820) and never saved, so a load re-arms it: the first update then
// replaces slot 0's engineer and raises the wanted count (+0x118) by one, and a
// tunnel that wanted five recruits into slot -1, over its own second vtable
// (docs/features/replay-seeking.md). Each tunnel's live flag is kept with the
// keyframe and written back after the load.
static const uintptr_t BUILDING_MGR_RVA = 0x6e5f40;
static const uintptr_t TUNNEL_VTABLE_RVA = 0x5a847c;
static const uint32_t TUNNEL_BYTES = 0x160;
static const uint32_t BUILDING_ID_OFF = 0x9c; // serial << 16 | slot
static const uint32_t TUNNEL_FIRST_ENGINEER_OFF = 0x114;

struct TunnelFlag {
    uint32_t slot;
    uint32_t id;
    uint8_t firstEngineer;
};

// Each actor is in four intrusive lists, node {owner, next, back-link} at
// +0x64 (spatial grid cell, which the node also keeps at +0xc), +0x74 (tile),
// +0x80 (16x16-tile block) and +0x8c (region). Every position update moves the
// actor to the front of its lists, and the actor speed routine (0x7875f0)
// slows it by 0.8 per actor ahead of it on its tile. A load leaves every node
// unlinked and relinks an actor only when it next moves, so a standing actor is
// missing from its tile and the order differs. Each keyframe keeps every chain
// by its head slot (tables and grid cells keep their addresses across a load)
// and rebuilds them straight after the load. Words per chain: {list, head slot,
// count, then slot and vtable per member, head first}.
static const uintptr_t ACTOR_MGR_RVA = 0x6e8728;
static const uint32_t ACTOR_LIST_OFF[4] = {0x64, 0x74, 0x80, 0x8c};
static const uint32_t ACTOR_LISTS = 4;
static const uint32_t GRID_LIST = 0;
static const uint32_t GRID_CELL_OFF = 0xc;
static const uint32_t ACTOR_NODE_BYTES = 0x98;
static const uintptr_t SPATIAL_GRID_RVA = 0x6e5440;
static const uintptr_t GRID_MARK_RVA = 0x387810; // (cell, 0): marks the cell occupied

// A Mother's ambient sound timer (+0x2f8) is not saved and a load resets it to
// 0x4b0; when it runs out she draws RandomIter #1 inside the simulation update
// (0x51d859), which shifts any later draw of that update.
static const uintptr_t MOTHER_VTABLE_RVA = 0x5b22a4;
static const uint32_t MOTHER_SOUND_TIMER_OFF = 0x2f8;

// A movement cluster (GlowWorm::Cluster, vtable 0x9f4bcc, 0xc8 bytes; each
// clan's vector of them at +0x14/+0x18) keeps its members' formation spots in
// 16-byte records {actor cache, handle id, x, y} (vector +0x18/+0x1c/+0x20)
// and a flag byte at +0x90, neither of which its serializer writes. A load
// leaves the records empty and a moving cluster rebuilds them from scratch,
// so its path progress (+0x8c) and its members' paths come out on other
// ticks. Each keyframe keeps them per clan slot and cluster index, with the
// member handles to check against, and writes them back after the load into
// a vector from the game's own operator new. Words per cluster: {clan slot,
// index, member count, member ids..., record count, (id, x, y)..., flag}.
static const uintptr_t CLAN_MGR_RVA = 0xc7937c;
static const uint32_t CLAN_CLUSTERS_OFF = 0x14;
static const uintptr_t CLUSTER_VTABLE_RVA = 0x5f4bcc;
static const uint32_t CLUSTER_BYTES = 0xc8;
static const uint32_t CLUSTER_MEMBERS_OFF = 0x8; // {cache, id} records
static const uint32_t CLUSTER_RECORDS_OFF = 0x18; // {cache, id, x, y} records
static const uint32_t CLUSTER_FLAG_OFF = 0x90;
static const uint32_t CLUSTER_RECORDS_MAX = 4096;
static const uintptr_t GAME_NEW_RVA = 0x40fbb6; // operator new, cdecl
static const uintptr_t GAME_DELETE_RVA = 0x40fb8c; // operator delete, cdecl
static const uintptr_t ACTOR_RESOLVE_RVA = 0x369360;
static const uintptr_t ACTOR_CLASS_RVA = 0x1d71d98;

typedef void *(__cdecl *GameNewFn)(uint32_t bytes);
typedef void(__cdecl *GameDeleteFn)(void *p);
typedef uintptr_t(__attribute__((thiscall)) * ActorResolveFn)(
    void *mgr, uintptr_t actorClass, uint32_t serial, uint32_t id
);

struct ActorWord {
    uint32_t slot;
    uint32_t value;
};

typedef void(__attribute__((thiscall)) * GridMarkFn)(void *grid, uintptr_t cell, uint32_t zero);

struct Keyframe {
    uint32_t job;
    uint32_t run; // the world it was taken from (replayKeyframesBeginRun)
    bool verified; // a run resumed from it has matched the recording for a while
    wchar_t path[MAX_PATH];
    ReplayPlayerPosition player;
    ReplayPrngPosition prng;
    MatchRulesReplayPosition rules;
    uint32_t iter0;
    bool rawPath; // how the file holds path progress (keyframeSerial.h)
    // In the sidecar's own section format, for the player's validating loaders;
    // empty when there was nothing to capture.
    std::vector<uint8_t> tile;
    std::vector<uint8_t> fly;
    std::vector<ReplayMonasteryClock> clocks;
    std::vector<uint8_t> units;
    std::vector<uint8_t> kills;
    std::vector<TunnelFlag> tunnels;
    std::vector<uint32_t> chains;
    std::vector<ActorWord> soundTimers;
    std::vector<uint32_t> clusters;
};

static std::vector<Keyframe> s_keyframes; // ascending by job
static uint32_t s_run = 0;
static wchar_t s_replayPath[MAX_PATH] = L""; // the replay they belong to
static wchar_t s_root[MAX_PATH] = L""; // %TEMP%\sh2up-keyframes
static wchar_t s_dir[MAX_PATH] = L""; // this process's folder in it
static ReplayMonasteryClock s_clockScratch[REPLAY_CLOCK_MAX];
// On: a restore the save cannot reproduce is caught by the seeker's check
// against the recording and redone from an older keyframe or the replay's own
// save (replaySeeker.cpp, verifyFrame).
static bool s_enabled = true;

void replayKeyframesSetEnabled(bool enabled) { s_enabled = enabled; }

bool replayKeyframesEnabled() { return s_enabled; }

#ifdef DEBUG
static uint32_t s_dbgCaptures = 0, s_dbgJob = 0, s_dbgSaveUs = 0, s_dbgBytes = 0;
static uint32_t s_dbgHandles = 0;

void replayKeyframesDebugLast(
    uint32_t *captures, uint32_t *job, uint32_t *saveUs, uint32_t *bytes, uint32_t *handles
) {
    *captures = s_dbgCaptures;
    *job = s_dbgJob;
    *saveUs = s_dbgSaveUs;
    *bytes = s_dbgBytes;
    *handles = s_dbgHandles;
}
#endif

static uint32_t readIter0() {
    const uintptr_t game = *(const volatile uintptr_t *)(exeBase() + GAME_PTR_RVA);

    if (!game) {
        return 0xffffffffu;
    }

    return *(const volatile uint16_t *)(game + ITER0_CURSOR_OFF);
}

static bool buildingSlots(const uintptr_t **slots, uint32_t *count) {
    const uintptr_t header = exeBase() + BUILDING_MGR_RVA + 8;

    if (!observerReadable(header, 8)) {
        return false;
    }

    const uintptr_t begin = *(const uint32_t *)header, end = *(const uint32_t *)(header + 4);

    if (end < begin || (end - begin) % 4 || (end - begin) / 4 > 65536) {
        return false;
    }

    *count = (uint32_t)((end - begin) / 4);
    *slots = (const uintptr_t *)begin;
    return !*count || observerReadable(begin, *count * 4);
}

static uintptr_t tunnelAt(const uintptr_t *slots, uint32_t i) {
    const uintptr_t b = slots[i];

    if (!b || !observerReadable(b, TUNNEL_BYTES) ||
        *(const uint32_t *)b != exeBase() + TUNNEL_VTABLE_RVA) {
        return 0;
    }

    return b;
}

static void saveTunnels(std::vector<TunnelFlag> &out) {
    const uintptr_t *slots = nullptr;
    uint32_t count = 0;
    out.clear();

    if (!buildingSlots(&slots, &count)) {
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const uintptr_t t = tunnelAt(slots, i);

        if (t) {
            out.push_back(
                {i, *(const uint32_t *)(t + BUILDING_ID_OFF),
                 *(const uint8_t *)(t + TUNNEL_FIRST_ENGINEER_OFF)}
            );
        }
    }
}

// A tunnel the load did not bring back in the recorded slot with the recorded
// id is left alone.
static void restoreTunnels(const std::vector<TunnelFlag> &in) {
    const uintptr_t *slots = nullptr;
    uint32_t count = 0;

    if (in.empty() || !buildingSlots(&slots, &count)) {
        return;
    }

    for (const TunnelFlag &f : in) {
        const uintptr_t t = f.slot < count ? tunnelAt(slots, f.slot) : 0;

        if (t && *(const uint32_t *)(t + BUILDING_ID_OFF) == f.id) {
            *(uint8_t *)(t + TUNNEL_FIRST_ENGINEER_OFF) = f.firstEngineer;
        }
    }
}

static inline uintptr_t &word(uintptr_t address) { return *reinterpret_cast<uintptr_t *>(address); }

static bool actorSlots(const uintptr_t **slots, uint32_t *count) {
    const uintptr_t header = exeBase() + ACTOR_MGR_RVA + 8;

    if (!observerReadable(header, 8)) {
        return false;
    }

    const uintptr_t begin = *(const uint32_t *)header, end = *(const uint32_t *)(header + 4);

    if (end < begin || (end - begin) % 4 || (end - begin) / 4 > 65536) {
        return false;
    }

    *count = (uint32_t)((end - begin) / 4);
    *slots = (const uintptr_t *)begin;
    return !*count || observerReadable(begin, *count * 4);
}

static uintptr_t actorAt(const uintptr_t *slots, uint32_t i) {
    const uintptr_t a = slots[i];

    if (!a || !observerReadable(a, ACTOR_NODE_BYTES)) {
        return 0;
    }

    return a;
}

struct ChainNode {
    uintptr_t node;
    uint32_t slot;
};

static bool nodeBefore(const ChainNode &a, const ChainNode &b) { return a.node < b.node; }

static const ChainNode *findChainNode(const std::vector<ChainNode> &nodes, uintptr_t node) {
    size_t lo = 0, hi = nodes.size();

    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;

        if (nodes[mid].node < node) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (lo < nodes.size() && nodes[lo].node == node) {
        return &nodes[lo];
    }

    return nullptr;
}

#ifdef DEBUG
static uint32_t s_dbgChainsSaved = 0, s_dbgChainsForeign = 0;
static uint32_t s_dbgChainsRestored = 0, s_dbgChainsSkipped = 0;
#endif

// A chain holding a node that is not an actor's is left out; its actors then
// stay as the load leaves them.
static void saveChains(std::vector<uint32_t> &out) {
    const uintptr_t *slots = nullptr;
    uint32_t count = 0;
    out.clear();
#ifdef DEBUG
    s_dbgChainsSaved = 0;
    s_dbgChainsForeign = 0;
#endif

    if (!actorSlots(&slots, &count)) {
        return;
    }

    std::vector<ChainNode> nodes;
    std::vector<uint32_t> members;

    for (uint32_t list = 0; list < ACTOR_LISTS; ++list) {
        const uint32_t off = ACTOR_LIST_OFF[list];
        nodes.clear();

        for (uint32_t i = 0; i < count; ++i) {
            const uintptr_t a = actorAt(slots, i);

            if (a) {
                nodes.push_back({a + off, i});
            }
        }

        std::sort(nodes.begin(), nodes.end(), nodeBefore);

        for (const ChainNode &n : nodes) {
            const uintptr_t back = word(n.node + 8);

            // Not linked, or behind another actor: its back-link is that
            // actor's `next` field.
            if (!back || findChainNode(nodes, back - 4)) {
                continue;
            }

            members.clear();
            bool foreign = false;

            for (uintptr_t node = n.node; node && members.size() <= nodes.size() * 2;
                 node = word(node + 4)) {
                const ChainNode *m = findChainNode(nodes, node);

                if (!m) {
                    foreign = true;
                    break;
                }

                members.push_back(m->slot);
                members.push_back(*(const uint32_t *)slots[m->slot]);
            }

            if (foreign || members.size() > nodes.size() * 2) {
#ifdef DEBUG
                ++s_dbgChainsForeign;
#endif
                continue;
            }

            out.push_back(list);
            out.push_back((uint32_t)back);
            out.push_back((uint32_t)(members.size() / 2));
            out.insert(out.end(), members.begin(), members.end());
#ifdef DEBUG
            ++s_dbgChainsSaved;
#endif
        }
    }
}

static void unlinkNode(uintptr_t node) {
    const uintptr_t next = word(node + 4), back = word(node + 8);

    if (!back) {
        return;
    }

    if (next) {
        word(next + 8) = back;
    }

    word(back) = next;
    word(node + 4) = 0;
    word(node + 8) = 0;
}

// Everything the load linked is taken out first; a chain whose head slot is
// still occupied after that (a node that is not an actor's), or whose members
// did not come back in their slots, is skipped.
static void restoreChains(const std::vector<uint32_t> &in) {
    const uintptr_t *slots = nullptr;
    uint32_t count = 0;
#ifdef DEBUG
    s_dbgChainsRestored = 0;
    s_dbgChainsSkipped = 0;
#endif

    if (in.empty() || !actorSlots(&slots, &count)) {
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const uintptr_t a = actorAt(slots, i);

        if (!a) {
            continue;
        }

        for (uint32_t list = 0; list < ACTOR_LISTS; ++list) {
            unlinkNode(a + ACTOR_LIST_OFF[list]);
        }

        word(a + ACTOR_LIST_OFF[GRID_LIST] + GRID_CELL_OFF) = 0;
    }

    const GridMarkFn mark = (GridMarkFn)(exeBase() + GRID_MARK_RVA);
    void *const grid = (void *)(exeBase() + SPATIAL_GRID_RVA);
    std::vector<uintptr_t> nodes;
    size_t at = 0;

    while (at + 3 <= in.size()) {
        const uint32_t list = in[at], n = in[at + 2];
        const uintptr_t head = in[at + 1];

        if (list >= ACTOR_LISTS || !n || at + 3 + 2 * (size_t)n > in.size()) {
            break;
        }

        const uint32_t *member = &in[at + 3];
        at += 3 + 2 * (size_t)n;
        const uint32_t off = ACTOR_LIST_OFF[list];
        bool ok = observerReadable(head, 4) && word(head) == 0;
        nodes.clear();

        for (uint32_t i = 0; i < n && ok; ++i) {
            const uintptr_t a = member[2 * i] < count ? actorAt(slots, member[2 * i]) : 0;
            ok = a && *(const uint32_t *)a == member[2 * i + 1] && word(a + off + 8) == 0;

            if (ok) {
                nodes.push_back(a + off);
            }
        }

        if (!ok) {
#ifdef DEBUG
            ++s_dbgChainsSkipped;
#endif
            continue;
        }

        word(head) = nodes[0];
        word(nodes[0] + 8) = head;

        for (uint32_t i = 0; i < n; ++i) {
            word(nodes[i] + 4) = i + 1 < n ? nodes[i + 1] : 0;

            if (i + 1 < n) {
                word(nodes[i + 1] + 8) = nodes[i] + 4;
            }

            if (list == GRID_LIST) {
                word(nodes[i] + GRID_CELL_OFF) = head;
            }
        }

        if (list == GRID_LIST) {
            mark(grid, head, 0);
        }

#ifdef DEBUG
        ++s_dbgChainsRestored;
#endif
    }
}

static void saveSoundTimers(std::vector<ActorWord> &out) {
    const uintptr_t *slots = nullptr;
    uint32_t count = 0;
    const uint32_t vt = (uint32_t)(exeBase() + MOTHER_VTABLE_RVA);
    out.clear();

    if (!actorSlots(&slots, &count)) {
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const uintptr_t a = slots[i];

        if (a && observerReadable(a, MOTHER_SOUND_TIMER_OFF + 4) && *(const uint32_t *)a == vt) {
            out.push_back({i, *(const uint32_t *)(a + MOTHER_SOUND_TIMER_OFF)});
        }
    }
}

static void restoreSoundTimers(const std::vector<ActorWord> &in) {
    const uintptr_t *slots = nullptr;
    uint32_t count = 0;
    const uint32_t vt = (uint32_t)(exeBase() + MOTHER_VTABLE_RVA);

    if (in.empty() || !actorSlots(&slots, &count)) {
        return;
    }

    for (const ActorWord &w : in) {
        const uintptr_t a = w.slot < count ? slots[w.slot] : 0;

        if (a && observerReadable(a, MOTHER_SOUND_TIMER_OFF + 4) && *(const uint32_t *)a == vt) {
            *(uint32_t *)(a + MOTHER_SOUND_TIMER_OFF) = w.value;
        }
    }
}

static bool vectorView(uintptr_t at, uint32_t stride, uintptr_t *begin, uint32_t *count) {
    if (!observerReadable(at, 12)) {
        return false;
    }

    const uintptr_t first = word(at), last = word(at + 4);

    if (last < first || (last - first) % stride || (last - first) / stride > CLUSTER_RECORDS_MAX ||
        (last > first && !observerReadable(first, last - first))) {
        return false;
    }

    *begin = first;
    *count = (uint32_t)((last - first) / stride);
    return true;
}

static bool clanSlots(const uintptr_t **slots, uint32_t *count) {
    const uintptr_t header = exeBase() + CLAN_MGR_RVA + 8;

    if (!observerReadable(header, 8)) {
        return false;
    }

    const uintptr_t begin = word(header), end = word(header + 4);

    if (end < begin || (end - begin) % 4 || (end - begin) / 4 > 65536) {
        return false;
    }

    *count = (uint32_t)((end - begin) / 4);
    *slots = (const uintptr_t *)begin;
    return !*count || observerReadable(begin, *count * 4);
}

static uintptr_t clusterAt(uintptr_t clan, uint32_t index, uint32_t *clusterCount) {
    uintptr_t begin = 0;
    uint32_t n = 0;
    *clusterCount = 0;

    if (!clan || !vectorView(clan + CLAN_CLUSTERS_OFF, 4, &begin, &n)) {
        return 0;
    }

    *clusterCount = n;
    const uintptr_t c = index < n ? word(begin + 4 * index) : 0;

    if (!c || !observerReadable(c, CLUSTER_BYTES) || word(c) != exeBase() + CLUSTER_VTABLE_RVA) {
        return 0;
    }

    return c;
}

#ifdef DEBUG
static uint32_t s_dbgClustersSaved = 0, s_dbgClustersRestored = 0, s_dbgClustersSkipped = 0;
#endif

static void saveClusters(std::vector<uint32_t> &out) {
    const uintptr_t *slots = nullptr;
    uint32_t count = 0;
    out.clear();
#ifdef DEBUG
    s_dbgClustersSaved = 0;
#endif

    if (!clanSlots(&slots, &count)) {
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const uintptr_t clan = slots[i];

        if (!clan || !observerReadable(clan, CLAN_CLUSTERS_OFF + 8)) {
            continue;
        }

        uint32_t clusters = 0;
        clusterAt(clan, 0, &clusters);

        for (uint32_t j = 0; j < clusters; ++j) {
            uint32_t n = 0;
            const uintptr_t c = clusterAt(clan, j, &n);
            uintptr_t members = 0, records = 0;
            uint32_t memberCount = 0, recordCount = 0;

            if (!c || !vectorView(c + CLUSTER_MEMBERS_OFF, 8, &members, &memberCount) ||
                !vectorView(c + CLUSTER_RECORDS_OFF, 16, &records, &recordCount)) {
                continue;
            }

            out.push_back(i);
            out.push_back(j);
            out.push_back(memberCount);

            for (uint32_t m = 0; m < memberCount; ++m) {
                out.push_back(word(members + 8 * m + 4));
            }

            out.push_back(recordCount);

            for (uint32_t r = 0; r < recordCount; ++r) {
                out.push_back(word(records + 16 * r + 4));
                out.push_back(word(records + 16 * r + 8));
                out.push_back(word(records + 16 * r + 12));
            }

            out.push_back(*(const uint8_t *)(c + CLUSTER_FLAG_OFF));
#ifdef DEBUG
            ++s_dbgClustersSaved;
#endif
        }
    }
}

// A cluster whose members did not come back as they were is left as the load
// made it.
static void restoreClusters(const std::vector<uint32_t> &in) {
    const uintptr_t *slots = nullptr;
    uint32_t count = 0;
#ifdef DEBUG
    s_dbgClustersRestored = 0;
    s_dbgClustersSkipped = 0;
#endif

    if (in.empty() || !clanSlots(&slots, &count)) {
        return;
    }

    const uintptr_t base = exeBase();
    const GameNewFn gameNew = (GameNewFn)(base + GAME_NEW_RVA);
    const GameDeleteFn gameDelete = (GameDeleteFn)(base + GAME_DELETE_RVA);
    const ActorResolveFn resolve = (ActorResolveFn)(base + ACTOR_RESOLVE_RVA);
    void *const actorMgr = (void *)(base + ACTOR_MGR_RVA);
    size_t at = 0;

    while (at + 4 <= in.size()) {
        const uint32_t clanSlot = in[at], index = in[at + 1], memberCount = in[at + 2];

        if (memberCount > CLUSTER_RECORDS_MAX || at + 4 + memberCount > in.size()) {
            break;
        }

        const uint32_t *memberIds = &in[at + 3];
        const uint32_t recordCount = in[at + 3 + memberCount];
        const size_t recordsAt = at + 4 + memberCount;

        if (recordCount > CLUSTER_RECORDS_MAX ||
            recordsAt + 3 * (size_t)recordCount + 1 > in.size()) {
            break;
        }

        const uint32_t flag = in[recordsAt + 3 * (size_t)recordCount];
        at = recordsAt + 3 * (size_t)recordCount + 1;
        uint32_t n = 0;
        const uintptr_t c = clanSlot < count ? clusterAt(slots[clanSlot], index, &n) : 0;
        uintptr_t members = 0, records = 0;
        uint32_t nowMembers = 0, nowRecords = 0;
        bool ok = c && vectorView(c + CLUSTER_MEMBERS_OFF, 8, &members, &nowMembers) &&
                  vectorView(c + CLUSTER_RECORDS_OFF, 16, &records, &nowRecords) &&
                  nowMembers == memberCount;

        for (uint32_t m = 0; m < memberCount && ok; ++m) {
            ok = word(members + 8 * m + 4) == memberIds[m];
        }

        if (!ok) {
#ifdef DEBUG
            ++s_dbgClustersSkipped;
#endif
            continue;
        }

        const uintptr_t vec = c + CLUSTER_RECORDS_OFF;
        const uint32_t capacity = word(vec) ? (uint32_t)((word(vec + 8) - word(vec)) / 16) : 0;

        if (recordCount > capacity) {
            void *fresh = gameNew(recordCount * 16);

            if (!fresh) {
                continue;
            }

            if (word(vec)) {
                gameDelete((void *)word(vec));
            }

            word(vec) = (uintptr_t)fresh;
            word(vec + 8) = (uintptr_t)fresh + recordCount * 16;
        }

        const uintptr_t first = word(vec);

        for (uint32_t r = 0; r < recordCount; ++r) {
            const uint32_t id = in[recordsAt + 3 * r];
            const uintptr_t rec = first + 16 * r;
            word(rec) = id ? resolve(actorMgr, base + ACTOR_CLASS_RVA, id >> 16, id) : 0;
            word(rec + 4) = id;
            word(rec + 8) = in[recordsAt + 3 * r + 1];
            word(rec + 12) = in[recordsAt + 3 * r + 2];
        }

        word(vec + 4) = first + recordCount * 16;
        *(uint8_t *)(c + CLUSTER_FLAG_OFF) = (uint8_t)flag;
#ifdef DEBUG
        ++s_dbgClustersRestored;
#endif
    }
}

#ifdef DEBUG
void replayKeyframesDebugClusters(uint32_t *saved, uint32_t *restored, uint32_t *skipped) {
    *saved = s_dbgClustersSaved;
    *restored = s_dbgClustersRestored;
    *skipped = s_dbgClustersSkipped;
}

void replayKeyframesDebugChains(
    uint32_t *saved, uint32_t *foreign, uint32_t *restored, uint32_t *skipped
) {
    *saved = s_dbgChainsSaved;
    *foreign = s_dbgChainsForeign;
    *restored = s_dbgChainsRestored;
    *skipped = s_dbgChainsSkipped;
}
#endif

static void writeIter0(uint32_t cursor) {
    const uintptr_t game = *(const volatile uintptr_t *)(exeBase() + GAME_PTR_RVA);

    if (game && cursor <= 0xffff) {
        *(volatile uint16_t *)(game + ITER0_CURSOR_OFF) = (uint16_t)cursor;
    }
}

static void deleteFolder(const wchar_t *dir) {
    wchar_t pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%ls\\*", dir);
    pattern[MAX_PATH - 1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);

    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                wchar_t file[MAX_PATH];
                _snwprintf(file, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
                file[MAX_PATH - 1] = 0;
                DeleteFileW(file);
            }
        } while (FindNextFileW(find, &fd));

        FindClose(find);
    }

    RemoveDirectoryW(dir);
}

void replayKeyframesForget() {
    for (const Keyframe &kf : s_keyframes) {
        DeleteFileW(kf.path);
    }

    s_keyframes.clear();
    s_replayPath[0] = 0;

    if (s_dir[0]) {
        RemoveDirectoryW(s_dir);
    }
}

void replayKeyframesBeginRun() { ++s_run; }

void replayKeyframesDiscard(uint32_t job, bool thisRunFromJob) {
    for (size_t at = s_keyframes.size(); at-- > 0;) {
        const Keyframe &kf = s_keyframes[at];
        bool drop = kf.job == job;

        if (thisRunFromJob) {
            drop = kf.run == s_run && kf.job >= job;
        }

        if (drop) {
            DeleteFileW(kf.path);
            s_keyframes.erase(s_keyframes.begin() + at);
        }
    }
}

void replayKeyframesOnLoad(const wchar_t *path) {
    if (!path || wcscmp(path, s_replayPath) != 0) {
        replayKeyframesForget();
    }
}

static bool ensureFolder() {
    if (!s_dir[0]) {
        return false;
    }

    CreateDirectoryW(s_root, NULL);
    CreateDirectoryW(s_dir, NULL);
    const DWORD attrs = GetFileAttributesW(s_dir);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static uint32_t intervalJobs() {
    uint32_t end = 0, msPerJob = 0;

    if (!replaySessionTimeline(&end, &msPerJob) || !msPerJob) {
        return 0;
    }

    const uint32_t jobs = KEYFRAME_SECONDS * 1000 / msPerJob;
    return jobs < KEYFRAME_MIN_JOBS ? KEYFRAME_MIN_JOBS : jobs;
}

static bool haveWindow(uint32_t window, uint32_t interval) {
    for (const Keyframe &kf : s_keyframes) {
        if (kf.job / interval == window) {
            return true;
        }
    }

    return false;
}

static void sectionBytes(
    std::vector<uint8_t> &out, uint32_t magic, uint32_t count, const void *payload,
    size_t payloadBytes
) {
    const uint32_t header[3] = {magic, 1, count};
    out.resize(sizeof(header) + payloadBytes);
    memcpy(out.data(), header, sizeof(header));

    if (payloadBytes) {
        memcpy(out.data() + sizeof(header), payload, payloadBytes);
    }
}

// Everything is read before the save and the save changes none of it: SaveGame
// draws neither random cursor and runs no update (phase 0 measurements).
static bool capture(uint32_t job) {
    Keyframe kf;
    kf.job = job;
    kf.run = s_run;
    kf.verified = false;

    if (!replayPlayerSavePosition(&kf.player) || kf.player.job != job ||
        !prngGuardSavePosition(&kf.prng)) {
        return false;
    }

    const uint32_t clocks = prngGuardSaveClocks(s_clockScratch, REPLAY_CLOCK_MAX);

    if (clocks == 0xffffffffu) {
        return false;
    }

    kf.clocks.assign(s_clockScratch, s_clockScratch + clocks);
    matchRulesReplaySave(&kf.rules);
    kf.iter0 = readIter0();
    replayCaptureTileOrder();
    const uint32_t *words = nullptr;
    uint32_t wordCount = 0, lists = 0;

    if (replayTileOrderView(&words, &wordCount, &lists) && lists) {
        sectionBytes(kf.tile, REPLAY_TILE_MAGIC, lists, words, wordCount * 4);
    }

    replayCaptureFlyState();
    const ReplayFlyField *fly = nullptr;
    uint32_t flyCount = 0;

    if (replayFlyStateView(&fly, &flyCount) && flyCount) {
        sectionBytes(kf.fly, REPLAY_FLY_MAGIC, flyCount, fly, flyCount * sizeof(ReplayFlyField));
    }

    kf.units.resize(unitCountsBytes());
    unitCountsSave(kf.units.data());
    kf.kills.resize(killCountsBytes());
    killCountsSave(kf.kills.data());
    saveTunnels(kf.tunnels);
    saveChains(kf.chains);
    saveSoundTimers(kf.soundTimers);
    saveClusters(kf.clusters);

    if (!ensureFolder()) {
        return false;
    }

    _snwprintf(kf.path, MAX_PATH, L"%ls\\kf-%u.sh2", s_dir, job);
    kf.path[MAX_PATH - 1] = 0;
    bool saved = false;
#ifdef DEBUG
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
#endif

    uint32_t handles = 0;

    if (!keyframeSaveBegin(&kf.rawPath)) {
        return false;
    }

    {
        // SaveGame computes floats, and the frame tick is a mid-function site.
        ObserverFpScope fp;
        // Mode 2: no state announcement, as the recorder's networked save.
        saved = replayCheckpointSave(kf.path, 2);
    }

    // Puts back every handle cache the save refreshed. False: the cache log
    // overflowed and some dead handles were zeroed as a stock save does.
    if (!keyframeSaveEnd(&handles)) {
        saved = false;
    }

#ifdef DEBUG
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    s_dbgJob = job;
    s_dbgHandles = handles;
    s_dbgSaveUs = (uint32_t)((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart);
    s_dbgBytes = GetFileAttributesExW(kf.path, GetFileExInfoStandard, &fa) ? fa.nFileSizeLow : 0;
    ++s_dbgCaptures;
#endif

    // Never seen to move, but a keyframe must not change the playback it is
    // taken from.
    if (readIter0() != kf.iter0) {
        writeIter0(kf.iter0);
    }

    if (!saved) {
        DeleteFileW(kf.path);
        return false;
    }

    size_t at = 0;

    while (at < s_keyframes.size() && s_keyframes[at].job < job) {
        ++at;
    }

    s_keyframes.insert(s_keyframes.begin() + at, std::move(kf));
    return true;
}

#ifdef DEBUG
bool replayKeyframesCaptureNow(uint32_t *keyframeJob) {
    uint32_t job = 0;
    const wchar_t *replay = replaySessionPlaybackPath();

    if (!replayPlayerPlaybackJob(&job) || !replay || replayNetQueuedCommands() != 0 ||
        replayPendingPlacements() != 0 || replayScreenPopupOn()) {
        return false;
    }

    if (wcscmp(replay, s_replayPath) != 0) {
        replayKeyframesForget();
        wcsncpy(s_replayPath, replay, MAX_PATH - 1);
        s_replayPath[MAX_PATH - 1] = 0;
    }

    for (const Keyframe &kf : s_keyframes) {
        if (kf.job == job) {
            *keyframeJob = job;
            return true;
        }
    }

    *keyframeJob = job;
    return capture(job);
}
#endif

bool replayKeyframesFrame() {
    uint32_t job = 0;

    if (!s_enabled || !replayPlayerPlaybackJob(&job) || replayViewerEnded() ||
        s_keyframes.size() >= KEYFRAMES_MAX) {
        return false;
    }

    const wchar_t *replay = replaySessionPlaybackPath();
    const uint32_t interval = intervalJobs();

    // The first minute has the replay's own checkpoint.
    if (!replay || !interval || job / interval == 0 || haveWindow(job / interval, interval)) {
        return false;
    }

    if (replayNetQueuedCommands() != 0 || replayPendingPlacements() != 0 || replayScreenPopupOn()) {
        return false;
    }

    if (wcscmp(replay, s_replayPath) != 0) {
        replayKeyframesForget();
        wcsncpy(s_replayPath, replay, MAX_PATH - 1);
        s_replayPath[MAX_PATH - 1] = 0;
    }

    return capture(job);
}

bool replayKeyframeAtOrBefore(uint32_t job, uint32_t margin, uint32_t *keyframeJob) {
    const wchar_t *replay = replaySessionPlaybackPath();

    if (!replay || wcscmp(replay, s_replayPath) != 0) {
        return false;
    }

    bool found = false;

    for (const Keyframe &kf : s_keyframes) {
        if (kf.job > job) {
            break;
        }

        if (kf.verified || kf.job + margin <= job) {
            *keyframeJob = kf.job;
            found = true;
        }
    }

    return found;
}

void replayKeyframesMarkVerified(uint32_t job) {
    for (Keyframe &kf : s_keyframes) {
        if (kf.job == job) {
            kf.verified = true;
        }
    }
}

bool replayKeyframeRestore(uint32_t keyframeJob, bool *loaded) {
    *loaded = false;
    size_t at = 0;

    while (at < s_keyframes.size() && s_keyframes[at].job != keyframeJob) {
        ++at;
    }

    if (at == s_keyframes.size() || replayScreenPopupOn()) {
        return false;
    }

    const Keyframe &kf = s_keyframes[at];

    if (GetFileAttributesW(kf.path) == INVALID_FILE_ATTRIBUTES) {
        s_keyframes.erase(s_keyframes.begin() + at);
        return false;
    }

    // Applied by the player at the epoch, as the checkpoint's are.
    replayPlayerClearTileOrder();
    replayPlayerClearFlyState();

    if (!kf.tile.empty()) {
        replayPlayerLoadTileOrderBytes(kf.tile.data(), (uint32_t)kf.tile.size());
    }

    if (!kf.fly.empty()) {
        replayPlayerLoadFlyStateBytes(kf.fly.data(), (uint32_t)kf.fly.size());
    }

    replayPlayerSetIter0AtSave(kf.iter0);
    replaySessionExpectKeyframe(kf.path, &kf.rules);
    bool ok = false;

    {
        // LoadGame computes floats, and the frame tick is a mid-function site.
        ObserverFpScope fp;
        // A keyframe file holds path progress as raw float bits.
        keyframeLoadBegin(kf.rawPath);
        ok = replayLoadSave(kf.path);
        keyframeLoadEnd();
    }

    *loaded = true;

    if (!ok) {
        replaySessionExpectKeyframe(nullptr, nullptr);
        return false;
    }

    // Before any update: the first tunnel update would act on the flag, and the
    // first move on the lists.
    restoreTunnels(kf.tunnels);
    restoreChains(kf.chains);
    restoreSoundTimers(kf.soundTimers);
    restoreClusters(kf.clusters);

    // Straight after the load, before this frame's update loop can run.
    if (!replayPlayerStartAt(kf.player)) {
        return false;
    }

    prngGuardRestorePosition(kf.prng);
    prngGuardRestoreClocks(kf.clocks.data(), (uint32_t)kf.clocks.size());

    if (kf.units.size() == unitCountsBytes()) {
        unitCountsRestore(kf.units.data());
    }

    if (kf.kills.size() == killCountsBytes()) {
        killCountsRestore(kf.kills.data());
    }

    sessionRebindSlots();
    return true;
}

// A folder named for a process id that no longer runs. An id the system will
// not open (another user's process, say) counts as running.
static bool processGone(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);

    if (!h) {
        return GetLastError() == ERROR_INVALID_PARAMETER;
    }

    const bool gone = WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
    CloseHandle(h);
    return gone;
}

void installReplayKeyframes() {
    installKeyframeSerial();
    wchar_t temp[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, temp);

    if (!n || n + 40 >= MAX_PATH) {
        return;
    }

    _snwprintf(s_root, MAX_PATH, L"%lssh2up-keyframes", temp);
    s_root[MAX_PATH - 1] = 0;
    const DWORD self = GetCurrentProcessId();
    _snwprintf(s_dir, MAX_PATH, L"%ls\\%lu", s_root, (unsigned long)self);
    s_dir[MAX_PATH - 1] = 0;
    wchar_t pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%ls\\*", s_root);
    pattern[MAX_PATH - 1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);

    if (find == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        wchar_t *endp = nullptr;
        const unsigned long pid = wcstoul(fd.cFileName, &endp, 10);

        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !pid || *endp || pid == self ||
            !processGone((DWORD)pid)) {
            continue;
        }

        wchar_t dir[MAX_PATH];
        _snwprintf(dir, MAX_PATH, L"%ls\\%ls", s_root, fd.cFileName);
        dir[MAX_PATH - 1] = 0;
        deleteFolder(dir);
    } while (FindNextFileW(find, &fd));

    FindClose(find);
}
