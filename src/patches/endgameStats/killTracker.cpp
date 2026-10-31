#include "killTracker.h"
#include "../../core/hook.h"
#include "../../core/observerContext.h"
#include "../../core/readableMemory.h"
#include "gameOffsets.h"
#include <cstring>
#include <windows.h>

// Hook site: the lethal branch of the shared actor damage sink, RVA 0x386A40
// `Actor::applyFinalDamage(this, float damage, int code)` — the one function
// ordinary damage paths use (melee sweep, projectile impact, the lord's
// direct slot). Ballista launch and pit fall deaths bypass this sink and
// have separate observation sites below. Its death branch runs when damage
// brings HP to <= 0:
//
//   0x386A7F  8b 45 0c     mov eax, [ebp+0xc]     ; damage code   <- hook
//   0x386A82  d9 59 4c     fstp dword [ecx+0x4c]  ; HP = st0, POP <- hook
//   0x386A85  89 41 54     mov [ecx+0x54], eax    ; last damage code
//   0x386A88  b0 01        mov al, 1
//
// At the hook: ECX = the dying actor, [ebp+8] = damage bits, [ebp+0xc] =
// damage code, and the caller chain still holds the attack context — the
// melee sweep keeps its attacker at [its_ebp-0xc], and the whole caller
// region sits inside the captured stack window. The sink's `this` is a
// tile-list wrapper, not the registry actor; the real combatants are the live
// actor pointers sitting in those caller frames.
//
// Attribution (validated live — tallies reconciled exactly with observed survivors): every
// stack dword that names a live registry actor is resolved to (owner, unit
// type, HP, state); the victim is the resolved actor already at HP <= 0, and
// the killer is the first still-healthy actor owned by a different player.
// The victim's unit type gives the tally's row, the killer's owner slot its
// column.
//
// The site is x87-live (st0 holds the value the re-emitted FSTP stores), so
// capture runs inside a complete x87/SSE save/reset/restore boundary.
// The stock instruction is FSTP
// (modrm /3, `d9 59 4c`): it stores st0 into HP AND pops it, leaving the
// function's x87 stack balanced — the entry push has exactly one pop on
// every exit path (the others use `fstp %st(0)` at 0x386A61/0x386A90). The
// re-emission must be `fstps`, NOT `fsts`: skipping the pop leaks one x87
// register per kill, and once the 8-slot stack fills every later float op on
// the sim thread yields QNaN indefinite with unordered compares (verified
// empirically) — silently diverging this machine's simulation from its
// peers' and desyncing multiplayer lobbies. This hook must never disturb any
// state the game can observe.
static const uintptr_t LETHAL_BRANCH_RVA = 0x386A7F;
static const uint8_t LETHAL_BRANCH_STOCK[6] = {0x8b, 0x45, 0x0c, 0xd9, 0x59, 0x4c};

// ---------------------------------------------------------------------------
// GROUND-FIRE ATTRIBUTION (adapted from an earlier kill trace
// v7; live-validated). Fire damage has no attacker actor:
// `Fire::tick` (RVA 0x1bff00) walks the per-tile object lists and calls
// `obj->vt+0x2c(damage, 0x10000008)` — the code reaches the sink's
// [ebp+0xc] unchanged, and the game's own burn-death reaction (0xe6e60)
// keys on `[actor+0x54] == 0x10000008`. Ownership comes from tracking who
// ignited the tile:
//
//   Fire::addGroundFire (RVA 0x1bfac0) is the ONLY writer of fire records,
//   called from exactly two sites inside Fire::igniteArea (RVA 0x1bfde0,
//   rets 0x1bfec7 / 0x1bfede). At entry, esp points at that return address;
//   igniteArea's frame is a CONSTANT delta away on both sites (prologue
//   0x34 + 3 callee saves + 4 arg pushes + ret = 0x54), so:
//     [esp+0x58] = igniteArea's own return address = the PRODUCER
//     [esp+0x18] = igniteArea's saved ESI
//   Producers: BurningCart methods 0x18cc90..0x18cd46 (burn-death drop) and
//   0x18d360..0x18da6f (arrival chore drop) keep the cart actor in ESI
//   across their `call igniteArea`, so the saved ESI IS the cart; its
//   [+0x58] is the owner. Fire::tick's spread (0x1bff00..0x1c01f4) lights
//   one orthogonal neighbour of the record being ticked, so a spread child
//   inherits the owner found on its 8 neighbours. Everything else (burning
//   buildings, pitch, …) is origin-unknown and stays unattributed.
//
// A 0x10000008 death is credited to the fire owner of the VICTIM'S tile
// (actor +0x30 packedTile >> 8) when the stock diplomacy predicate
// 0x219e0(a,b) (stdcall, ret 8: same owner -> 1, relation
// [rel + b*4 + 0x28] <= 2 -> 1) says the victim is NOT friendly to the
// fire owner — friendly-fire deaths are never credited as kills. The predicate
// is reproduced with guarded reads, without calling game code. The hook
// preserves x87/SSE state. The normal candidate-scan killer is SKIPPED for
// fire deaths — Fire::tick's frames carry tile-list neighbours that would
// otherwise be mis-attributed.
// ---------------------------------------------------------------------------

static const uintptr_t ADD_GROUND_FIRE_RVA = 0x1bfac0;
static const uint8_t ADD_GROUND_FIRE_STOCK[6] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x20};
static const uintptr_t IGNITE_RET1_A = 0x1bfec7;
static const uintptr_t IGNITE_RET1_B = 0x1bfede;
static const uintptr_t CART1_BEGIN = 0x18cc90, CART1_END = 0x18cd46;
static const uintptr_t CART2_BEGIN = 0x18d360, CART2_END = 0x18da6f;
static const uintptr_t FIRE_TICK_BEGIN = 0x1bff00, FIRE_TICK_END = 0x1c01f4;
static const uint32_t FIRE_DAMAGE_CODE = 0x10000008;

// When a lord dies, every unit its player owns dies with it through this same
// sink with damage code 0x1a (verified live: the lord's lethal
// blow was code 0x4000000d, then 16 hits on its own player's actors, all code
// 0x1a, in the same millisecond). Those deaths are never credited — the kill
// was the lord. Without this, a healthy enemy that happens to sit in the
// cascade's caller frames would be credited.
static const uint32_t LORD_CASCADE_DAMAGE_CODE = 0x1a;

// tileIndex (A*256+B) -> 0xff never ignited, 0xfe origin unknown, else the
// igniting cart's owner slot. 0 is a REAL owner slot, so the map starts as
// 0xff (never zeroed): spread inheritance would otherwise adopt slot 0 from
// every untouched neighbour tile. Re-igniting a tile rewrites its entry.
// Neighbour inference remains heuristic; the entire map
// is cleared at a session boundary to avoid cross-match ownership reuse.
static uint8_t s_fireOwner[65536];

// Actor field offsets (the game's actor layout).
static const uintptr_t OFF_ACTOR_OWNER = 0x58; // player table slot (0xff = none)
static const uintptr_t OFF_ACTOR_HP = 0x4c; // float; read as raw bits below
static const uintptr_t OFF_ACTOR_STATE = 0x20; // 3 = dying
static const uintptr_t OFF_ACTOR_DEF = 0x174; // unit def -> +0x34 type id
static const uintptr_t OFF_DEF_UNITTYPE = 0x34;

// Actor registry singleton: +8 holds the std::vector of live actor pointers.
static const uintptr_t ACTOR_MGR_RVA = 0x6E8728;

static const uint32_t STACK_WORDS = 96; // dwords captured from hook-entry esp
static const uint32_t MAX_CANDIDATES = 12;

// Per-killer, per-victim-type tally [table slot][kill row]. Kills whose
// victim has no kill row (game-owned and non-combat actors) are not tallied —
// the display is per tracked type, so the matrix stays self-consistent with
// what the overlay shows. The lord has its own row (kKillLordRow).
static uint32_t s_killsByType[PLAYER_TABLE_SLOTS][kKillRowCount] = {};

struct KillCandidate {
    uint32_t ptr;
    uint32_t owner;
    uint32_t unitType;
    uint32_t hpBits; // [ptr+0x4c] raw float bits
    uint32_t state;
};

// The lethal branch re-fires for further damage on an already-dead wrapper,
// so each victim actor must be tallied once only. The ring remembers the last
// RING_CAPACITY victims by pointer and generation handle; scanning it deduplicates a repeat hit
// unless so many later kills happened that the record wrapped away (accepted
// — by then the dead actor's memory is normally recycled anyway).
static const uint32_t RING_CAPACITY = 256;
static const uint32_t RING_TAG = 0x314c494b; // "KIL1" little-endian

struct KillDedup {
    uint32_t tag;
    uint32_t seq;
    uint32_t victim; // resolved victim actor pointer
    uint32_t handle; // generation identity, prevents allocator reuse from suppressing a new kill
};

static KillDedup s_ring[RING_CAPACITY];
static uint32_t s_killCount = 0; // total lethal-branch hits, monotonic
static bool s_installed = false;

// Several accepted impacts can enqueue different actions for one victim.
// Key ownership by the actual action, not just the actor. No game pointer
// retained here is dereferenced later; resolution validates actor identity.
struct BallistaFlight { uint32_t chore, victim, handle, owner; };
static BallistaFlight s_ballistaFlights[0x10000] = {};

#ifdef DEBUG

// Opt-in capture log alongside Stronghold2.exe. Isolated killtrace builds
// retain release behavior elsewhere. Preserve the live x87/SSE state around
// ALL CRT/OS logging calls: the lethal hook still has an HP value in st(0).
#include <cstdarg>
#include <cstdio>

static void klog(const char *fmt, ...) {
    alignas(16) unsigned char fpState[512];
    __asm__ volatile("fxsave %0\n\tfninit" : "=m"(fpState) : : "memory");
    static FILE *f = nullptr;
    static unsigned lines = 0;
    if (!f) {
        char path[MAX_PATH];
        const DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
        if (n && n < MAX_PATH) {
            char *slash = strrchr(path, '\\');
            if (slash && (slash - path) + sizeof("\\killtrace.txt") <= MAX_PATH) {
                strcpy(slash + 1, "killtrace.txt");
                f = fopen(path, "a");
                if (f) setvbuf(f, NULL, _IOFBF, 65536);
            }
        }
    }
    if (f) {
        fprintf(f, "pid=%lu,ms=%lu,", GetCurrentProcessId(), GetTickCount());
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        // Kill outcomes and lifecycle events are immediately reviewable;
        // batch fire/candidate lines to avoid opening/flushing per tile.
        if (++lines % 256 == 0 || strncmp(fmt, "res,", 4) == 0 ||
            strncmp(fmt, "reset", 5) == 0 || strncmp(fmt, "install", 7) == 0 ||
            strncmp(fmt, "snapshot", 8) == 0 || strncmp(fmt, "ballista", 8) == 0) fflush(f);
    }
    __asm__ volatile("fxrstor %0" : : "m"(fpState) : "memory");
}

#define KLOG(...) klog(__VA_ARGS__)

#else

#define KLOG(...)

#endif // DEBUG

static bool readable(const void *address, size_t len) {
    return observerReadable(address, len);
}

static inline uint32_t load32(uintptr_t address) {
    return *reinterpret_cast<const volatile uint32_t *>(address);
}

// Is `w` a live registry actor? Walk the registry vector (a fixed cap).
static bool inRegistry(uintptr_t begin, uintptr_t end, uint32_t w) {
    for (uintptr_t p = begin; p < end; p += 4) {
        if (load32(p) == w) {
            return true;
        }
    }

    return false;
}

// Read-only equivalent of the stock friendship lookup (RVA 0x219e0).
// Verified against 1.5.0: player+4 -> manager (RVA 0x6e8bcc), vector at
// +8/+12; its vtable+0x28 getter (RVA 0x37ef60) only bounds-checks/indexes
// this vector. No game function is called from the observer. An unreadable
// chain means unknown: withhold kill credit rather than risk a crash.
static bool gameIsFriendly(uint32_t a, uint32_t b) {
    if (a >= PLAYER_TABLE_SLOTS || b >= PLAYER_TABLE_SLOTS) return true;
    if (a == b) return true;
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    const uintptr_t playerSlot = base + PLAYER_TABLE_RVA + a * 4;
    const uintptr_t managerSlot = base + 0x6e8bcc;
    if (!readable((void *)playerSlot, 4) || !readable((void *)managerSlot, 4)) return true;
    const uintptr_t player = load32(playerSlot);
    if (!player) return false; // stock empty slot
    if (!readable((void *)player, 8)) return true;
    const uint32_t index = load32(player + 4);
    if (!index) return false;
    const uintptr_t manager = load32(managerSlot);
    if (!readable((void *)manager, 16)) return true;
    const uintptr_t begin = load32(manager + 8), end = load32(manager + 12);
    if (end < begin || ((end - begin) & 3u)) return true;
    if (index >= (end - begin) / 4) return false;
    const uintptr_t entry = begin + index * 4;
    if (!readable((void *)entry, 4)) return true;
    const uintptr_t relation = load32(entry);
    if (!relation || b >= 31) return false; // stock predicate's strict bound
    if (!readable((void *)relation, 0x28 + b * 4 + 4)) return true;
    return load32(relation + 0x28 + b * 4) <= 2;
}

// Fire-origin capture: runs at Fire::addGroundFire ENTRY (game thread). esp
// points at igniteArea's return site; arg1 ([esp+4]) is &packedTile. See the
// header block for the frame derivation. Integer-only.
extern "C" void __cdecl fireIgniteCapture(uint32_t espIn) {
    if (!readable((const void *)espIn, 0x5c)) {
        return;
    }

    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    const uint32_t ret1 = load32(espIn);

    if (ret1 != base + IGNITE_RET1_A && ret1 != base + IGNITE_RET1_B) {
        return; // unexpected caller — leave the tile unowned (fail-safe)
    }

    const uint32_t tilePtr = load32(espIn + 4);

    if (!readable((const void *)(uintptr_t)tilePtr, 4)) {
        return;
    }

    const uint32_t tileIndex = load32(tilePtr) >> 8;

    if (tileIndex >= 65536) {
        return;
    }

    const uint32_t producer = load32(espIn + 0x58);
    uint8_t owner = 0xfe; // origin unknown (buildings, pitch, …)

    if ((producer >= base + CART1_BEGIN && producer < base + CART1_END) ||
        (producer >= base + CART2_BEGIN && producer < base + CART2_END)) {
        // BurningCart drop: igniteArea saved the cart actor (the producer's
        // live ESI) at [ebp_ign-0x3c] == [esp+0x18].
        const uint32_t cart = load32(espIn + 0x18);
        const uintptr_t vec = base + ACTOR_MGR_RVA + 8;

        if (cart > 0x10000 && readable((const void *)(uintptr_t)cart, 0x178) &&
            readable((const void *)vec, 8)) {
            const uintptr_t begin = load32(vec);
            const uintptr_t end = load32(vec + 4);

            if (end > begin && ((end - begin) & 3u) == 0 && (end - begin) / 4 <= 0x10000 &&
                readable((const void *)begin, end - begin) &&
                inRegistry(begin, end, cart)) {
                const uintptr_t def = load32((uintptr_t)cart + OFF_ACTOR_DEF);

                if (readable((const void *)def, OFF_DEF_UNITTYPE + 4) &&
                    load32(def + OFF_DEF_UNITTYPE) == 0x67) { // Burning Cart
                    const uint32_t o = load32((uintptr_t)cart + OFF_ACTOR_OWNER);

                    if (o < PLAYER_TABLE_SLOTS) {
                        owner = (uint8_t)o;
                    }
                }
            }
        }
    } else if (producer >= base + FIRE_TICK_BEGIN && producer < base + FIRE_TICK_END) {
        // Spread child: the parent record is one orthogonal neighbour away.
        const uint32_t A = tileIndex >> 8;
        const uint32_t B = tileIndex & 0xff;

        for (int dA = -1; dA <= 1 && owner == 0xfe; ++dA) {
            for (int dB = -1; dB <= 1; ++dB) {
                if (!dA && !dB) {
                    continue;
                }

                const int nA = (int)A + dA;
                const int nB = (int)B + dB;

                if (nA < 0 || nA > 255 || nB < 0 || nB > 255) {
                    continue;
                }

                const uint8_t v = s_fireOwner[(nA << 8) | nB];

                if (v < PLAYER_TABLE_SLOTS) { // a real owner beats unknown
                    owner = v;
                    break;
                }
            }
        }
    }

    KLOG("fire,producer=0x%x,tile=%u,oldOwner=%u,owner=%u,savedCart=0x%x",
         producer - base, tileIndex, s_fireOwner[tileIndex], owner, load32(espIn + 0x18));
    s_fireOwner[tileIndex] = owner;
}

// Runs on the game thread at the lethal-blow moment. Integer-only (x87 is
// live: st0 feeds the re-emitted FST). `victimPtr` is ECX at the hook site —
// the sink's `this`, which combat paths are not consistent about: on some the
// dying REGISTRY ACTOR itself is passed (proven live: a capture whose ECX
// showed up one hit later as a stack candidate already at HP 0), on others a
// tile-list wrapper that the registry check rejects. So ECX is seeded into
// the candidate list like any stack dword, and is NOT used to exclude
// candidates — an earlier revision excluded `w == victimPtr`, which on the
// actor-passing paths deleted the dying actor from its own candidate window
// and silently dropped almost every kill (one test battle: 2 of 37 lethal
// hits tallied). `ebpIn`/`espIn` are the frame of `applyFinalDamage`.
// Owner of the blaze at `tileIndex`, with a one-tile neighbour fallback:
// a victim mid-move can take its killing blow on the tile it is ENTERING
// while actor +0x30 still names the one it is leaving, and a spread child
// that inherited unknown from a corner can sit beside owned tiles of the
// same blaze. Returns 0xfe/0xff (unknown/never-ignited) when nothing owned
// is adjacent — a fire death then stays unattributed rather than guessing.
static uint8_t fireOwnerForTile(uint32_t tileIndex) {
    if (tileIndex >= 65536) {
        return 0xff;
    }

    const uint8_t direct = s_fireOwner[tileIndex];

    if (direct < PLAYER_TABLE_SLOTS) {
        return direct;
    }

    const uint32_t A = tileIndex >> 8;
    const uint32_t B = tileIndex & 0xff;

    for (int dA = -1; dA <= 1; ++dA) {
        for (int dB = -1; dB <= 1; ++dB) {
            if (!dA && !dB) {
                continue;
            }

            const int nA = (int)A + dA;
            const int nB = (int)B + dB;

            if (nA < 0 || nA > 255 || nB < 0 || nB > 255) {
                continue;
            }

            const uint8_t v = s_fireOwner[(nA << 8) | nB];

            if (v < PLAYER_TABLE_SLOTS) {
                return v;
            }
        }
    }

    return direct;
}

// A direct source is authoritative only on a validated stock caller path.
static uint32_t damageSourceOwner(uint32_t frame, uint32_t code) {
    if (code != 0x40000013 && code != 0x40000014) return 0xff;
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    for (int depth = 0; depth < 8; ++depth) {
        if (!readable((void *)(uintptr_t)frame, 20)) break;
        const uint32_t ret = load32(frame + 4);
        const uintptr_t expectedRet = base + (code == 0x40000013 ? 0xc7271 : 0xc8341);
        if (ret == expectedRet) {
            const uintptr_t source = load32(frame + 16); // damage argument 3
            const uintptr_t expectedVtable = base + (code == 0x40000013 ? 0x5a8224 : 0x5a834c);
            if (readable((void *)source, 0x5c) && load32(source) == expectedVtable) {
                const uint32_t owner = load32(source + 0x58);
                return owner < PLAYER_TABLE_SLOTS ? owner : 0xff;
            }
            return 0xff;
        }
        const uint32_t next = load32(frame);
        if (next <= frame || next - frame > 4096) break;
        frame = next;
    }
    return 0xff;
}

// ---------------------------------------------------------------------------
// TUNNELS. A finished tunnel's tick
// FUN_004c9e90 (RVA 0xc9e90, ESI = the tunnel actor, owner +0x58) calls the
// area-damage routine FUN_00424be0(&tile, 100000.0, 10000.0, 0x20000017, ...)
// which damages units, buildings and walls around the tunnel end. Unit deaths
// arrive at the lethal sink with code 0x20000017 and no attacker actor on the
// stack. The call returns to RVA 0xca0cf; FUN_00424be0's prologue is
// `push ebp; mov ebp,esp; sub esp,0x50; ...; push ebx; push esi` so the
// tunnel (the caller's ESI) is saved at [its ebp-0x58]. A building the tunnel
// destroys collapses synchronously under the same call (FUN_00496ac0 launches
// the units standing on it through the knockback routine), so the tunnel
// frame is also on the stack at those launches.
// ---------------------------------------------------------------------------
static const uint32_t TUNNEL_DAMAGE_CODE = 0x20000017;
static const uintptr_t TUNNEL_AREA_DAMAGE_RET = 0xca0cf;

// Owner slot of the tunnel whose area damage is running beneath `frame`
// (an EBP of some callee), 0xff when none is found or the chain is invalid.
static uint32_t tunnelOwnerFromFrames(uint32_t frame) {
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    for (int depth = 0; depth < 24; ++depth) {
        if (frame < 0x58 || !readable((void *)(uintptr_t)(frame - 0x58), 0x60)) break;
        if (load32(frame + 4) == base + TUNNEL_AREA_DAMAGE_RET) {
            const uint32_t tunnel = load32(frame - 0x58);
            if (!readable((void *)(uintptr_t)tunnel, 0x5c)) return 0xff;
            const uint32_t owner = load32(tunnel + OFF_ACTOR_OWNER);
            KLOG("tunnel,frame=0x%x,depth=%d,tunnel=0x%x,owner=%u", frame, depth, tunnel, owner);
            return owner < PLAYER_TABLE_SLOTS ? owner : 0xff;
        }
        const uint32_t next = load32(frame);
        if (next <= frame || next - frame > 0x10000) break;
        frame = next;
    }
    return 0xff;
}

// A building the tunnel damages collapses LATER, from its own tick
// (FUN_004995e0 -> FUN_00496ac0, chain 0x99685 <- 0x96902 <- 0x3774ab, no
// tunnel frame): record which player's area damage last hit each building.
// FUN_00424be0 damages the tile's building at RVA 0x424c97..0x424cae with
// EAX = the building (vt+0x60) and EBP = its own frame. Any non-tunnel area
// damage overwrites the entry with "unknown", so the latest blast decides.
// Heuristic: a building a tunnel only damaged and something else later
// destroys without area damage is still credited to the tunnel's owner.
struct BuildingBlast { uint32_t building, owner; };
static const uint32_t BUILDING_BLAST_CAPACITY = 128;
static BuildingBlast s_buildingBlasts[BUILDING_BLAST_CAPACITY];
static uint32_t s_buildingBlastNext = 0;

static void setBuildingDestroyer(uint32_t building, uint32_t owner) {
    if (!building) return;
    for (BuildingBlast &b : s_buildingBlasts) {
        if (b.building == building) {
            if (b.owner != owner) {
                KLOG("buildingBlast,building=0x%x,owner=%u", building, owner);
            }
            b.owner = owner;
            return;
        }
    }
    if (owner >= PLAYER_TABLE_SLOTS) return;
    KLOG("buildingBlast,building=0x%x,owner=%u,new", building, owner);
    s_buildingBlasts[s_buildingBlastNext++ % BUILDING_BLAST_CAPACITY] = {building, owner};
}

extern "C" void buildingDamageCapture(uint32_t building, uint32_t frame) {
    if (!building || !readable((void *)(uintptr_t)frame, 8)) return;
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    // `frame` is FUN_00424be0's own EBP: the tunnel test starts right there.
    const uint32_t owner = load32(frame + 4) == base + TUNNEL_AREA_DAMAGE_RET
                               ? tunnelOwnerFromFrames(frame)
                               : 0xff;
    setBuildingDestroyer(building, owner);
}

// SIEGE / ATTACKED BUILDINGS. Everything
// that hits a building with an attacker — catapult and trebuchet stones
// (FUN_004d3830 @0x4d39ce passes the projectile's +0xb8/+0xbc), melee —
// calls its vt+0x64, FUN_00468d90(damage, code, x, handle) for S2Building and
// Tower alike (vtables 0x99de4c/0x9a1fe4). It resolves `handle` through the
// actor registry, STORES the attacker pointer back into its own argument slot
// [ebp+0x10] (or zeroes [ebp+0x14] when not found), then calls the shared
// damage function FUN_00786a40 as `this` = the building, returning to RVA
// 0x68dfc. So when the lethal hook sees that return address, the building's
// killing blow and its attacker are both in hand: remember the attacker's
// owner as the building's destroyer for the later collapse.
static const uintptr_t BUILDING_DAMAGE_RET = 0x68dfc;

static void buildingKillCapture(uint32_t building, uint32_t ebpIn) {
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    if (!readable((void *)(uintptr_t)ebpIn, 8) || load32(ebpIn + 4) != base + BUILDING_DAMAGE_RET)
        return;
    const uint32_t frame = load32(ebpIn); // FUN_00468d90's EBP
    if (!readable((void *)(uintptr_t)frame, 0x18) || !load32(frame + 0x14)) return;
    const uint32_t attacker = load32(frame + 0x10);
    if (!readable((void *)(uintptr_t)attacker, 0x5c)) return;
    const uint32_t owner = load32(attacker + OFF_ACTOR_OWNER);
    KLOG("buildingKill,building=0x%x,attacker=0x%x,owner=%u", building, attacker, owner);
    setBuildingDestroyer(building, owner < PLAYER_TABLE_SLOTS ? owner : 0xff);
}

static uint32_t buildingDestroyerOwner(uint32_t building) {
    if (!building) return 0xff; // empty entries hold building 0, owner 0
    for (const BuildingBlast &b : s_buildingBlasts)
        if (b.building == building) return b.owner;
    return 0xff;
}

// FUN_00496ac0 (a collapsing building clearing its footprint) keeps the
// building at [ebp-8]. `frame` is its EBP.
static const uintptr_t COLLAPSE_LAUNCH_RET = 0x96ccd; // its knockback call
static uint32_t collapseOwner(uint32_t frame) {
    if (frame < 8 || !readable((void *)(uintptr_t)(frame - 8), 4)) return 0xff;
    const uint32_t building = load32(frame - 8);
    const uint32_t owner = buildingDestroyerOwner(building);
    KLOG("collapse,building=0x%x,owner=%u", building, owner);
    return owner;
}

#ifdef DEBUG
// Diagnostic: the first return addresses up the EBP chain, as RVAs.
static void logFrameChain(const char *tag, uint32_t frame) {
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uint32_t r[8] = {};
    for (int i = 0; i < 8; ++i) {
        if (!readable((void *)(uintptr_t)frame, 8)) break;
        r[i] = load32(frame + 4) - base;
        const uint32_t next = load32(frame);
        if (next <= frame || next - frame > 0x10000) break;
        frame = next;
    }
    KLOG("%s,chain=0x%x/0x%x/0x%x/0x%x/0x%x/0x%x/0x%x/0x%x", tag, r[0], r[1], r[2], r[3],
         r[4], r[5], r[6], r[7]);
}
#else
#define logFrameChain(tag, frame) ((void)0)
#endif

// `killerType` is the killing actor's unit type, 0xffffffff when the kill
// has no attacker actor (fire, pit, ballista flight).
static void tallyKill(uint32_t seq, uint32_t victimActor, uint32_t victimType,
                      uint32_t victimOwner, uint32_t killerOwner, uint32_t killerType) {
    const uint32_t handle = load32((uintptr_t)victimActor + 0x154);
    for (const KillDedup &prev : s_ring) {
        if (prev.tag == RING_TAG && prev.victim == victimActor && prev.handle == handle) {
            KLOG("res,%u,0x%x,vOwner=%u,vType=0x%x,killer=%u,kType=0x%x,ringSkip", seq,
                 victimActor, victimOwner, victimType, killerOwner, killerType);
            return;
        }
    }
    KillDedup &slot = s_ring[seq % RING_CAPACITY];
    slot = {RING_TAG, seq, victimActor, handle};
    const int victimRow = killRowFromUnitId(victimType);
    if (victimRow >= 0 && killerOwner < PLAYER_TABLE_SLOTS) {
        ++s_killsByType[killerOwner][victimRow];
        KLOG("res,%u,0x%x,vOwner=%u,vType=0x%x,killer=%u,kType=0x%x,tally,total=%u", seq,
             victimActor, victimOwner, victimType, killerOwner, killerType,
             s_killsByType[killerOwner][victimRow]);
    } else {
        KLOG("res,%u,0x%x,vOwner=%u,vType=0x%x,killer=%u,kType=0x%x,drop", seq, victimActor,
             victimOwner, victimType, killerOwner, killerType);
    }
    (void)victimOwner;
    (void)killerType; // diagnostic log only: a lord's kill counts like any unit's
}

// The pit and completed ballista death bypass applyFinalDamage.
static void killCaptureOwned(uint32_t victim, uint32_t killer) {
    if (!readable((void *)(uintptr_t)victim, 0x178)) return;
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    const uintptr_t vec = base + ACTOR_MGR_RVA + 8;
    if (!readable((void *)vec, 8)) return;
    const uintptr_t begin = load32(vec), end = load32(vec + 4);
    if (end <= begin || ((end - begin) & 3) || (end - begin) / 4 > 0x10000 ||
        !readable((void *)begin, end - begin) || !inRegistry(begin, end, victim)) return;
    const uintptr_t def = load32((uintptr_t)victim + OFF_ACTOR_DEF);
    if (!readable((void *)def, OFF_DEF_UNITTYPE + 4)) return;
    const uint32_t type = load32(def + OFF_DEF_UNITTYPE);
    if (killRowFromUnitId(type) < 0) return;
    const uint32_t owner = load32((uintptr_t)victim + OFF_ACTOR_OWNER);
    if (killer >= PLAYER_TABLE_SLOTS || gameIsFriendly(killer, owner)) killer = 0xff;
    KLOG("special,victim=0x%x,type=0x%x,owner=%u,killer=%u", victim, type, owner, killer);
    tallyKill(s_killCount++, victim, type, owner, killer, 0xffffffffu);
}

extern "C" void killCaptureSpecial(uint32_t victim, uint32_t source, uint32_t accepted) {
    if (!(accepted & 0xff) || !readable((void *)(uintptr_t)source, 0x5c)) return;
    killCaptureOwned(victim, load32((uintptr_t)source + OFF_ACTOR_OWNER));
}

static BallistaFlight *ballistaFlight(uint32_t chore, bool insert) {
    if (!chore || chore == 1) return nullptr;
    BallistaFlight *free = nullptr;
    for (uint32_t n = 0; n < 0x10000; ++n) {
        auto &entry = s_ballistaFlights[((chore >> 4) + n) & 0xffff];
        if (entry.chore == chore) return &entry;
        if (entry.chore == 1 && !free) free = &entry; // deleted slot
        if (!entry.chore) return insert ? (free ? free : &entry) : nullptr;
    }
    return insert ? free : nullptr; // bounded, fail closed if full
}

extern "C" void killCaptureBallista(uint32_t victim, uint32_t chore, uint32_t frame) {
    // This is inside the stock impulse routine, immediately before admission.
    // The ballista impact at d1005 and catapult splash at d42a6 preserve the
    // projectile in EBX. The earlier catapult splash path at d3caf uses EBX
    // as a hit counter instead; its parent frame saved the projectile at -3c.
    if (auto old = ballistaFlight(chore, false)) old->chore = 1;
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    if (frame < 16 || !readable((void *)(uintptr_t)(frame - 16), 24) ||
        !readable((void *)(uintptr_t)victim, 0x178)) return;
    const uintptr_t caller = load32(frame + 4);
    uint32_t source = 0;
    uint32_t owner = 0xff;
    if (caller == base + 0xd100a || caller == base + 0xd42ab) {
        source = load32(frame - 16); // impulse routine's saved EBX
    } else if (caller == base + 0xd3cb4) {
        const uintptr_t parent = load32(frame);
        if (parent < 0x3c || !readable((void *)(parent - 0x3c), 4)) return;
        source = load32(parent - 0x3c);
    } else if (caller == base + COLLAPSE_LAUNCH_RET) {
        // Units on a collapsing building's upper layer: its destroyer.
        owner = collapseOwner(load32(frame));
        if (owner >= PLAYER_TABLE_SLOTS) return;
    } else {
        // Any other knockback is credited only under a tunnel's area damage.
        logFrameChain("launch", frame);
        owner = tunnelOwnerFromFrames(frame);
        if (owner >= PLAYER_TABLE_SLOTS) return;
    }
    if (source) {
        if (!readable((void *)(uintptr_t)source, 0x5c)) return;
        owner = load32((uintptr_t)source + OFF_ACTOR_OWNER);
    }
    const uint32_t handle = load32((uintptr_t)victim + 0x154);
    if (owner >= PLAYER_TABLE_SLOTS) return;
    auto entry = ballistaFlight(chore, true);
    if (!entry) { KLOG("ballistaCapacity,chore=0x%x", chore); return; }
    *entry = {chore, victim, handle, owner};
    KLOG("ballistaFlight,chore=0x%x,victim=0x%x,handle=0x%x,owner=%u,caller=0x%x,source=0x%x",
         chore, victim, handle, owner, (uint32_t)(caller - base), source);
}

// Resolve this action only: other queued ballista actions retain their owner.
extern "C" void killResolveBallista(uint32_t victim, uint32_t chore, uint32_t died) {
    auto entry = ballistaFlight(chore, false);
    if (!entry) return;
    const BallistaFlight flight = *entry;
    entry->chore = 1;
    if (!readable((void *)(uintptr_t)victim, 0x178)) return;
    const uint32_t handle = load32((uintptr_t)victim + 0x154);
    if (flight.victim != victim || flight.handle != handle) return;
    const uint32_t owner = flight.owner;
    KLOG("ballistaResolve,chore=0x%x,victim=0x%x,handle=0x%x,owner=%u,died=%u,hp=0x%x",
         chore, victim, handle, owner, died, load32((uintptr_t)victim + 0x4c));
    if (died) killCaptureOwned(victim, owner);
}

extern "C" void killCapturePit(uint32_t victim, uint32_t source, uint32_t frame) {
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    if (!readable((void *)(uintptr_t)frame, 8) || load32(frame + 4) != base + 0xc834f ||
        !readable((void *)(uintptr_t)source, 4) || load32(source) != base + 0x5a834c) return;
    killCaptureSpecial(victim, source, 1);
}

// FUN_00496ac0's footprint pass removes units standing in a collapsing
// building (state 3, no death animation) at RVA 0x496d4f, EAX = the actor.
extern "C" void collapseCrushCapture(uint32_t victim, uint32_t frame) {
    const uint32_t owner = collapseOwner(frame);
    if (owner < PLAYER_TABLE_SLOTS) killCaptureOwned(victim, owner);
}

extern "C" void __cdecl killCapture(uint32_t victimPtr, uint32_t ebpIn, uint32_t espIn) {
    if (!victimPtr) {
        return;
    }

    const uint32_t seq = s_killCount;

    KLOG("cap,%u,0x%x,0x%x,0x%x", seq, victimPtr, ebpIn, espIn);

    // [ebp+0xc] of applyFinalDamage = the damage code; 0x10000008 marks
    // ground-fire damage (Fire::tick's vt+0x2c).
    uint32_t damageCode = 0;

    if (readable((const void *)(uintptr_t)(ebpIn + 0xc), 4)) {
        damageCode = load32((uintptr_t)ebpIn + 0xc);
    }
    KLOG("damage,%u,code=0x%x", seq, damageCode);
    buildingKillCapture(victimPtr, ebpIn); // a building's killing blow: its destroyer

    uint32_t stack[STACK_WORDS];

    if (readable((const void *)espIn, sizeof(stack))) {
        memcpy(stack, (const void *)espIn, sizeof(stack));
    } else {
        memset(stack, 0, sizeof(stack));
    }

    // Resolve every stack dword that names a live registry actor, plus the
    // sink's `this` (ECX) under the same checks — on the actor-passing combat
    // paths the dying actor is only reachable that way.
    KillCandidate candidates[MAX_CANDIDATES];
    uint32_t candidateCount = 0;
    {
        const uintptr_t vec = (uintptr_t)GetModuleHandleA(NULL) + ACTOR_MGR_RVA + 8;

        if (readable((const void *)vec, 8)) {
            const uintptr_t begin = load32(vec);
            const uintptr_t end = load32(vec + 4);

            if (end > begin && ((end - begin) & 3u) == 0 && (end - begin) / 4 <= 0x10000 &&
                readable((const void *)begin, end - begin)) {
                // `off` is the dword offset the pointer was found at;
                // UINT32_MAX marks the sink's own `this` (ECX).
                const auto addCandidate = [&](uint32_t w, uint32_t off) {
                    (void)off; // only read by the DEBUG capture log

                    if (w <= 0x10000) {
                        return;
                    }

                    if (!readable((const void *)(uintptr_t)w, 0x178) || !inRegistry(begin, end, w)) {
                        return;
                    }

                    KillCandidate &c = candidates[candidateCount];
                    c.ptr = w;
                    c.owner = load32((uintptr_t)w + OFF_ACTOR_OWNER);
                    c.unitType = 0xffffffffu;

                    const uintptr_t def = load32((uintptr_t)w + OFF_ACTOR_DEF);

                    if (readable((const void *)def, OFF_DEF_UNITTYPE + 4)) {
                        c.unitType = load32(def + OFF_DEF_UNITTYPE);
                    }

                    c.hpBits = load32((uintptr_t)w + OFF_ACTOR_HP);
                    c.state = load32((uintptr_t)w + OFF_ACTOR_STATE);
                    ++candidateCount;
                    KLOG("cand,%u,%u,0x%x,%u,0x%x,0x%x,%u", seq, off, c.ptr, c.owner, c.unitType,
                         c.hpBits, c.state);
                };

                if (candidateCount < MAX_CANDIDATES) {
                    addCandidate(victimPtr, 0xffffffffu); // ECX: dying actor on some paths
                }

                for (uint32_t off = 0;
                     off + 4 <= STACK_WORDS * 4 && candidateCount < MAX_CANDIDATES; off += 4) {
                    addCandidate(stack[off / 4], off);
                }
            }
        }
    }

    // HP-as-bits predicates, no x87: dead reads as 0, -0.0 or any negative
    // float (sign bit set).
    const auto deadHp = [](uint32_t bits) {
        return bits == 0 || (bits & 0x80000000u) != 0;
    };

    uint32_t victimActor = 0;
    uint32_t victimOwner = 0xffffffffu;
    uint32_t victimType = 0xffffffffu;
    uint32_t killerOwner = 0xffu;
    uint32_t killerType = 0xffffffffu;

    for (uint32_t i = 0; i < candidateCount; ++i) {
        const KillCandidate &c = candidates[i];

        // Victims are combat units only (kUnits + the Lord):
        // on fire paths Fire::tick's frames carry other burning/dying
        // NON-UNIT actors (projectiles, wildlife, wrappers), and taking the
        // first dead candidate loses or mis-picks the real victim when many
        // things die in the same blaze (measured live in test battles:
        // defs 0x26/0x2b/0x2e are non-combatants that
        // die through the same branch). Earlier, the "Lord" here was
        // type 0x12, which is really the campfire Peasant: peasants were
        // eligible victims and killers while the real lord (0x20) was neither.
        const bool victimEligible = killRowFromUnitId(c.unitType) >= 0;

        if (victimEligible && (deadHp(c.hpBits) || c.state == 3)) {
            victimActor = c.ptr;
            victimOwner = c.owner;
            victimType = c.unitType;
            break;
        }
    }

    if (victimActor) {
        if (damageCode == LORD_CASCADE_DAMAGE_CODE) {
            // The dead lord's army dying with it: nobody's kill.
        } else if (damageCode == 0x40000013 || damageCode == 0x40000014) {
            killerOwner = damageSourceOwner(ebpIn, damageCode);
            if (killerOwner < PLAYER_TABLE_SLOTS && gameIsFriendly(killerOwner, victimOwner))
                killerOwner = 0xff;
        } else if (damageCode == TUNNEL_DAMAGE_CODE) {
            // No attacker actor exists; the tunnel's owner is the killer.
            killerOwner = tunnelOwnerFromFrames(ebpIn);
            if (killerOwner < PLAYER_TABLE_SLOTS && gameIsFriendly(killerOwner, victimOwner))
                killerOwner = 0xff;
        } else if (damageCode == FIRE_DAMAGE_CODE) {
            // Ground-fire death: the killer is the fire's origin owner, not
            // any actor on the stack (Fire::tick's frames only carry the
            // victim's tile-list neighbours). Friendly victims (own army,
            // allies) are losses, never kills.
            const uint32_t packedTile = load32((uintptr_t)victimActor + 0x30);
            const uint8_t fireOwner = fireOwnerForTile(packedTile >> 8);
            KLOG("fireVictim,%u,handle=0x%x,tile=%u,direct=%u,resolved=%u,vOwner=%u",
                 seq, load32((uintptr_t)victimActor + 0x154), packedTile >> 8,
                 (packedTile >> 8) < 65536 ? s_fireOwner[packedTile >> 8] : 0xff,
                 fireOwner, victimOwner);

            if (fireOwner < PLAYER_TABLE_SLOTS) {
                if (!gameIsFriendly(fireOwner, victimOwner)) {
                    killerOwner = fireOwner;
                }
            }
        } else {
            for (uint32_t i = 0; i < candidateCount; ++i) {
                const KillCandidate &c = candidates[i];

                // Killers are combat units only (kUnits + the Lord, who fights):
                // the candidate window also picks up healthy NON-COMBATANT
                // registry actors — projectiles/fire/wildlife with bogus owner
                // slots (measured on test battles: defs
                // 0x26/0x2b ownerless, 0x2e owner-10 tallied phantom kills) —
                // which must never be attributed a player's kill.
                const bool killerEligible = killRowFromUnitId(c.unitType) >= 0;

                if (killerEligible && c.ptr != victimActor && c.owner != 0xffffffffu &&
                    c.owner != victimOwner && !deadHp(c.hpBits) && c.state != 3) {
                    killerOwner = c.owner;
                    killerType = c.unitType;
                    break;
                }
            }
        }
    }

    ++s_killCount;

    if (!victimActor) {
        KLOG("res,%u,0,none", seq);
        return; // nothing identifiable (no tally, no dedup record)
    }

    tallyKill(seq, victimActor, victimType, victimOwner, killerOwner, killerType);
}

// Return-address global: must NOT be static (GAS cannot resolve local statics).
uintptr_t g_lethalBranchReturn = 0;
uintptr_t g_fireIgniteReturn = 0;

// After pushfl/pushal the original stack starts at EBX+36. EBX anchors
// the GPR image while ESP is aligned for the isolated observer call.
// The saved-register image occupies [ebx+0x00..0x1C]: PUSHA pushes
// EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI in that order, so from ebx the image reads
// REVERSED: EDI at +0x00, ESI +0x04, EBP +0x08, ESP +0x0C, EBX +0x10,
// EDX +0x14, ECX +0x18, EAX +0x1C. (An earlier revision
// indexed EBP/ECX at +0x28/+0x38, which is 0x20 too high: it read game stack
// slots above the image instead of the saved registers. The tally survived
// because attribution comes from the stack-window scan, not these args.)
__declspec(naked) static void lethalBranchHook() {
    __asm__ volatile(OBSERVER_ENTER
                     "lea 0x24(%ebx), %eax\n\t"   /* original esp */
                     "movl %eax, 8(%esp)\n\t"    /* espIn */
                     "movl 0x8(%ebx), %edx\n\t"  /* original ebp */
                     "movl %edx, 4(%esp)\n\t"    /* ebpIn */
                     "movl %ecx, (%esp)\n\t"     /* victim */
                     "call _killCapture\n\t"
                     OBSERVER_LEAVE
                     "movl 0xc(%ebp), %eax\n\t"   /* re-emitted stock */
                     "fstps 0x4c(%ecx)\n\t"       /* d9 59 4c: store AND pop */
                     "jmp *_g_lethalBranchReturn\n\t");
}

// Fire::addGroundFire entry: capture (and re-emit) the 6-byte prologue
// `push ebp; mov ebp,esp; sub esp,0x20`. Integer-only — igniteArea keeps no
// x87 value live into this call that the ABI does not already spill. The
// observer nevertheless preserves all FP state around OS/CRT calls.
__declspec(naked) static void addGroundFireHook() {
    __asm__ volatile(OBSERVER_ENTER
                     "lea 0x24(%ebx), %eax\n\t"   /* original esp (ret1) */
                     "movl %eax, (%esp)\n\t"     /* espIn */
                     "call _fireIgniteCapture\n\t"
                     OBSERVER_LEAVE
                     "pushl %ebp\n\t"             /* re-emitted stock */
                     "movl %esp, %ebp\n\t"
                     "subl $0x20, %esp\n\t"
                     "jmp *_g_fireIgniteReturn\n\t");
}

uintptr_t g_ballistaAdmitReturn = 0, g_pitDeathReturn = 0;
uintptr_t g_ballistaRecoverReturn = 0, g_ballistaDeathReturn = 0;
uintptr_t g_flightOffMapReturn = 0, g_flightBlockedReturn = 0;
uintptr_t g_buildingDamageReturn = 0, g_collapseCrushReturn = 0;

// Preserve the newly constructed EAX action and stock priority push.
__declspec(naked) static void ballistaAdmitHook() {
    __asm__ volatile(OBSERVER_ENTER
                     "movl %esi, (%esp)\n\t"
                     "movl 0x1c(%ebx), %eax\n\t" // stock EAX = action
                     "movl %eax, 4(%esp)\n\t"
                     "movl %ebp, 8(%esp)\n\t"
                     "call _killCaptureBallista\n\t"
                     OBSERVER_LEAVE
                     "pushl $0x80000000\n\t"
                     "jmp *_g_ballistaAdmitReturn\n\t");
}

__declspec(naked) static void ballistaRecoverHook() {
    __asm__ volatile(OBSERVER_ENTER
                     "movl %esi, (%esp)\n\t"
                     "movl 0x10(%ebx), %eax\n\t"
                     "movl %eax, 4(%esp)\n\t"
                     "movl $0, 8(%esp)\n\t"
                     "call _killResolveBallista\n\t"
                     OBSERVER_LEAVE
                     "movl %edi, 0x1fc(%esi)\n\t"
                     "jmp *_g_ballistaRecoverReturn\n\t");
}

__declspec(naked) static void ballistaDeathHook() {
    __asm__ volatile(OBSERVER_ENTER
                     "movl %esi, (%esp)\n\t"
                     "movl 0x10(%ebx), %eax\n\t"
                     "movl %eax, 4(%esp)\n\t"
                     "movl $1, 8(%esp)\n\t"
                     "call _killResolveBallista\n\t"
                     OBSERVER_LEAVE
                     "movl $3, 0x20(%esi)\n\t"
                     "jmp *_g_ballistaDeathReturn\n\t");
}

// A flight can also end with no landing at all (seen in test battles):
// FUN_004e9140 (the GENERIC_ACT_FLY_BACKWARDS step, called only from the
// action switch FUN_004e92c0 with EBX = the action) sets state 3 when the
// next position leaves the map (RVA 0xe92b1, ESI = actor), and its landing
// test FUN_004e8aa0 sets state 3 when the flight path hits a building or
// rises above terrain it cannot cross (RVA 0xe8bf2, EDI = actor; it reuses
// EBX, so the action is the caller's EBX saved at [ebp-0x1c]). The actor is
// then removed without the death animation or the eb8e0 death step. Both
// sites are `mov dword [reg+0x20], 3` guarded by a state != 4 test.
__declspec(naked) static void flightOffMapHook() {
    __asm__ volatile(OBSERVER_ENTER
                     "movl %esi, (%esp)\n\t"
                     "movl 0x10(%ebx), %eax\n\t"  /* stock EBX = action */
                     "movl %eax, 4(%esp)\n\t"
                     "movl $1, 8(%esp)\n\t"
                     "call _killResolveBallista\n\t"
                     OBSERVER_LEAVE
                     "movl $3, 0x20(%esi)\n\t"
                     "jmp *_g_flightOffMapReturn\n\t");
}

__declspec(naked) static void flightBlockedHook() {
    __asm__ volatile(OBSERVER_ENTER
                     "movl %edi, (%esp)\n\t"
                     "movl -0x1c(%ebp), %eax\n\t" /* caller's EBX = action */
                     "movl %eax, 4(%esp)\n\t"
                     "movl $1, 8(%esp)\n\t"
                     "call _killResolveBallista\n\t"
                     OBSERVER_LEAVE
                     "movl $3, 0x20(%edi)\n\t"
                     "jmp *_g_flightBlockedReturn\n\t");
}

// FUN_00424be0 at RVA 0x424c97: `mov ecx,[ebp+0x1c]; fld dword [ebp+0xc]`
// (8b 4d 1c d9 45 0c), EAX = the building about to take area damage.
__declspec(naked) static void buildingDamageHook() {
    __asm__ volatile(OBSERVER_ENTER
                     "movl 0x1c(%ebx), %eax\n\t"  /* stock EAX = building */
                     "movl %eax, (%esp)\n\t"
                     "movl %ebp, 4(%esp)\n\t"
                     "call _buildingDamageCapture\n\t"
                     OBSERVER_LEAVE
                     "movl 0x1c(%ebp), %ecx\n\t"
                     "flds 0xc(%ebp)\n\t"
                     "jmp *_g_buildingDamageReturn\n\t");
}

__declspec(naked) static void collapseCrushHook() {
    __asm__ volatile(OBSERVER_ENTER
                     "movl 0x1c(%ebx), %eax\n\t"  /* stock EAX = actor */
                     "movl %eax, (%esp)\n\t"
                     "movl %ebp, 4(%esp)\n\t"
                     "call _collapseCrushCapture\n\t"
                     OBSERVER_LEAVE
                     "movl $3, 0x20(%eax)\n\t"
                     "jmp *_g_collapseCrushReturn\n\t");
}

// This branch has passed the stock pit eligibility checks. ESI is the
// victim; EDI is the pit in the verified caller. Re-emit its byte store.
__declspec(naked) static void pitDeathHook() {
    __asm__ volatile(OBSERVER_ENTER
                     "movl %esi, (%esp)\n\t"
                     "movl %edi, 4(%esp)\n\t"
                     "movl %ebp, 8(%esp)\n\t"
                     "call _killCapturePit\n\t"
                     OBSERVER_LEAVE
                     "movb %bl, 0x2a8(%esi)\n\t"
                     "jmp *_g_pitDeathReturn\n\t");
}

void installKillTracker() {
    if (s_installed) return;
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    static const uint8_t ballistaStock[] = {0x68, 0x00, 0x00, 0x00, 0x80};
    static const uint8_t pitStock[] = {0x88, 0x9e, 0xa8, 0x02, 0x00, 0x00};
    static const uint8_t recoverStock[] = {0x89, 0xbe, 0xfc, 0x01, 0x00, 0x00};
    static const uint8_t deathStock[] = {0xc7, 0x46, 0x20, 0x03, 0x00, 0x00, 0x00};
    static const uint8_t offMapStock[] = {0xc7, 0x46, 0x20, 0x03, 0x00, 0x00, 0x00};
    static const uint8_t blockedStock[] = {0xc7, 0x47, 0x20, 0x03, 0x00, 0x00, 0x00};
    static const uint8_t buildingDamageStock[] = {0x8b, 0x4d, 0x1c, 0xd9, 0x45, 0x0c};
    static const uint8_t crushStock[] = {0xc7, 0x40, 0x20, 0x03, 0x00, 0x00, 0x00};
    struct Site { uintptr_t rva; const uint8_t *stock; size_t len; void *hook; DWORD protect; };
    Site sites[] = {
        {LETHAL_BRANCH_RVA, LETHAL_BRANCH_STOCK, 6, (void *)lethalBranchHook, 0},
        {ADD_GROUND_FIRE_RVA, ADD_GROUND_FIRE_STOCK, 6, (void *)addGroundFireHook, 0},
        {0xe413b, ballistaStock, 5, (void *)ballistaAdmitHook, 0},
        {0xe42b5, pitStock, 6, (void *)pitDeathHook, 0},
        {0xeb921, recoverStock, 6, (void *)ballistaRecoverHook, 0},
        {0xeb8e0, deathStock, 7, (void *)ballistaDeathHook, 0},
        {0xe92b1, offMapStock, 7, (void *)flightOffMapHook, 0},
        {0xe8bf2, blockedStock, 7, (void *)flightBlockedHook, 0},
        {0x24c97, buildingDamageStock, 6, (void *)buildingDamageHook, 0},
        {0x96d4f, crushStock, 7, (void *)collapseCrushHook, 0},
    };
    constexpr size_t siteCount = sizeof(sites) / sizeof(sites[0]);
    for (const auto &s : sites)
        if (!readable((void *)(base + s.rva), s.len) ||
            memcmp((void *)(base + s.rva), s.stock, s.len)) return;
    // Acquire every code-page permission before any mutation. Failure leaves
    // every site stock, rather than silently installing half the tracker.
    for (size_t i = 0; i < siteCount; ++i) {
        if (!VirtualProtect((void *)(base + sites[i].rva), sites[i].len,
                            PAGE_EXECUTE_READWRITE, &sites[i].protect)) {
            while (i) {
                --i; DWORD ignored;
                VirtualProtect((void *)(base + sites[i].rva), sites[i].len, sites[i].protect, &ignored);
            }
            return;
        }
    }
    memset(s_fireOwner, 0xff, sizeof(s_fireOwner));
    g_lethalBranchReturn = base + LETHAL_BRANCH_RVA + 6;
    g_fireIgniteReturn = base + ADD_GROUND_FIRE_RVA + 6;
    g_ballistaAdmitReturn = base + 0xe4140;
    g_ballistaRecoverReturn = base + 0xeb927;
    g_ballistaDeathReturn = base + 0xeb8e7;
    g_pitDeathReturn = base + 0xe42bb;
    g_flightOffMapReturn = base + 0xe92b8;
    g_flightBlockedReturn = base + 0xe8bf9;
    g_buildingDamageReturn = base + 0x24c9d;
    g_collapseCrushReturn = base + 0x96d56;
    for (const auto &s : sites) {
        auto target = (uint8_t *)(base + s.rva);
        const int32_t displacement = (int32_t)((uintptr_t)s.hook - (uintptr_t)target - 5);
        target[0] = 0xe9;
        memcpy(target + 1, &displacement, 4);
        for (size_t i = 5; i < s.len; ++i) target[i] = 0x90;
        FlushInstructionCache(GetCurrentProcess(), target, s.len);
    }
    for (int i = (int)siteCount - 1; i >= 0; --i) {
        DWORD ignored;
        VirtualProtect((void *)(base + sites[i].rva), sites[i].len, sites[i].protect, &ignored);
    }
    s_installed = true;
    KLOG("install,format=5,base=0x%lx,ballista=2,pit=1,flight=2,tunnel=1", (unsigned long)base);
}

static const uint32_t s_zeroKills[kKillRowCount] = {};

const uint32_t *killCountsForSlot(int slot) {
    if (slot < 0 || slot >= PLAYER_TABLE_SLOTS) {
        return s_zeroKills;
    }

    for (int i = 0; i < kKillRowCount; ++i) {
        if (s_killsByType[slot][i]) {
            KLOG("snapshot,slot=%d,killRow=%d,kills=%u", slot, i, s_killsByType[slot][i]);
        }
    }
    return s_killsByType[slot];
}

void resetKillCounts() {
    KLOG("reset,all,count=%u", s_killCount);
    memset(s_killsByType, 0, sizeof(s_killsByType));
    memset(s_ring, 0, sizeof(s_ring));
    memset(s_ballistaFlights, 0, sizeof(s_ballistaFlights));
    memset(s_fireOwner, 0xff, sizeof(s_fireOwner));
    memset(s_buildingBlasts, 0, sizeof(s_buildingBlasts));
    s_buildingBlastNext = 0;
    s_killCount = 0;
}

void resetKillCountsForSlot(int slot) {
    if (slot >= 0 && slot < PLAYER_TABLE_SLOTS) {
        memset(s_killsByType[slot], 0, sizeof(s_killsByType[slot]));
        KLOG("resetSlot,%d,count=%u", slot, s_killCount);
    }
}
