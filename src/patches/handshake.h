#pragma once
#include <cstdint>

// Tells every player in a Steam lobby which patch version everyone else has, so
// the host can only offer match rules every player supports.
// See docs/features/patch-handshake.md.

// One bit per match rule, and one rule per *behaviour*. The lobby agrees on
// rules by ANDing these bits; the version string is display only. Two builds
// advertising the same bit are assumed to simulate it identically, so:
//
// - Never reuse a bit: an older build reads it as the old rule.
// - Never change what an existing bit does to the simulation. A revised fix or
//   option gets a new bit. Keep the old implementation behind the old bit and
//   advertise both, so a lobby with an older build still runs the old version
//   and old replays (which record the bits they ran with) still reproduce.
//   Dropping the old bit is the minimum: a mixed lobby then runs vanilla, which
//   is safe, rather than two versions of the rule, which desyncs.
// - Changes that leave the simulation identical (UI, logging, a refactor with
//   the same result) keep their bit.
static const uint32_t RULE_BALLISTA_AUTOFIRE = 1u << 0;
static const uint32_t RULE_DISBAND_FIX = 1u << 1;
static const uint32_t RULE_NO_MARKET = 1u << 2;
static const uint32_t RULE_CRIME_EXCLUSION_FIX = 1u << 3;
static const uint32_t RULE_RANGED_WALK_FIX = 1u << 4;
static const uint32_t RULE_UNIT_CAP_RAISE = 1u << 5;

// Bumped whenever the member-data format changes; a player on another protocol
// is treated as supporting nothing.
static const uint32_t HANDSHAKE_PROTOCOL = 1;

enum PatchState {
    PATCH_NONE, // no key: unpatched, or 0.7.0 and older, which predate the handshake
    PATCH_FOUND,
    PATCH_UNREADABLE, // a key we cannot parse; treated as supporting nothing
};

struct LobbyPlayer {
    uint64_t steamId;
    PatchState state;
    uint32_t protocol;
    uint32_t rules;
    bool isOwner;
    bool isLocal;
    char version[16];
    wchar_t name[32];
};

// Snapshot of the lobby as of the last refresh (about once a second).
int handshakePlayers(LobbyPlayer *out, int max);

// The rules this build can switch on and off per match, as advertised to the
// lobby. Set by matchRules at install, from the rules whose sites verified.
void handshakeSetSupportedRules(uint32_t rules);
uint32_t handshakeSupportedRules();

// The rules every player in the lobby supports; 0 outside a lobby.
uint32_t handshakeCommonRules();

void installHandshake();
