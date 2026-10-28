#pragma once
#include <cstdint>

// Read/write access to the Steam lobby the game is in, through the game's own
// Online::OnlineLobby object (VA 0x2acd918) and steam_api's ISteamMatchmaking.
// See docs/features/patch-handshake.md.
//
// Every function here calls into steam_api, which is free to use the FPU: call
// them only from a prologue hook, or from the frame tick inside cleanFpu().

static const int LOBBY_MAX_MEMBERS = 16;
static const int LOBBY_MAX_VALUE = 64;

// True while the game is in a Steam lobby (including during the match it starts).
bool lobbyActive();

// The lobby's SteamID, or 0.
uint64_t lobbyId();

// Steam's own ownership flag on the game's lobby object.
bool lobbyIsLocalOwner();

// Our own SteamID, or 0 before Steam is up.
uint64_t localSteamId();

// `member`'s Steam display name as UTF-16, or false. Untrusted, like all names.
bool personaName(uint64_t member, wchar_t *out, int cap);

// SteamID of the lobby owner as Steam reports it, or 0.
uint64_t lobbyOwner();

// Fills `out` with every member's SteamID, ourselves included. Returns the count.
int lobbyMembers(uint64_t *out, int max);

// Copies `member`'s value for `key` into `out` (always terminated, truncated to
// `cap`). Returns false when the member has no such key. The value is written by
// another player and is untrusted.
bool lobbyMemberData(uint64_t member, const char *key, char *out, int cap);

// Sets our own member data; Steam replicates it to everyone in the lobby.
void lobbySetMemberData(const char *key, const char *value);

// Lobby-wide data; only the owner's writes take effect.
bool lobbyData(const char *key, char *out, int cap);
void lobbySetData(const char *key, const char *value);
