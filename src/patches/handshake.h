#pragma once
#include <cstdint>

// Tells every player in a Steam lobby which patch version everyone else has, so
// the host can only offer match rules every player supports.
// See docs/features/patch-handshake.md.

// One bit per match rule. A build advertises only the rules it can switch on and
// off per match; nothing is advertised until a rule actually ships.
static const uint32_t RULE_BALLISTA_AUTOFIRE = 1u << 0;

static const uint32_t RULES_SUPPORTED = 0;

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

// The rules every player in the lobby supports; 0 outside a lobby.
uint32_t handshakeCommonRules();

void installHandshake();
