#include "handshake.h"
#include "../core/chatCommand.h"
#include "../core/cleanFpu.h"
#include "../core/frameTick.h"
#include "../core/steamLobby.h"
#include "../version.h"
#include <cstdio>
#include <cstring>
#include <windows.h>

// See docs/features/patch-handshake.md.
//
// Each patched client writes `<protocol>;<version>;<rules hex>` into its own Steam
// lobby member data. Steam replicates member data to everyone in the lobby,
// including players who join later, so there is no request, reply or timeout: a
// member with no key is unpatched or on 0.7.0 or older.
static const char *MEMBER_KEY = "sh2up";

static const uint32_t REFRESH_MS = 1000;

static LobbyPlayer s_players[LOBBY_MAX_MEMBERS];
static int s_playerCount = 0;
static uint64_t s_publishedLobby = 0;
static uint32_t s_lastRefreshMs = 0;
static bool s_refreshed = false;
static bool s_reportPending = false;

static bool isDigit(char c) { return c >= '0' && c <= '9'; }

static int hexValue(char c) {
    if (isDigit(c)) {
        return c - '0';
    }

    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }

    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }

    return -1;
}

// The value was written by another player: accept exactly the shape we write and
// nothing else. Any deviation leaves the player marked as supporting no rules.
static bool parseMemberValue(const char *value, LobbyPlayer *player) {
    uint32_t protocol = 0;
    uint32_t rules = 0;
    int i = 0;
    int n = 0;

    for (n = 0; isDigit(value[i]) && n < 4; ++n, ++i) {
        protocol = protocol * 10 + (uint32_t)(value[i] - '0');
    }

    if (n == 0 || value[i++] != ';') {
        return false;
    }

    for (n = 0; (isDigit(value[i]) || value[i] == '.') && n < 15; ++n, ++i) {
        player->version[n] = value[i];
    }

    player->version[n] = 0;

    if (n == 0 || value[i++] != ';') {
        return false;
    }

    for (n = 0; hexValue(value[i]) >= 0 && n < 8; ++n, ++i) {
        rules = (rules << 4) | (uint32_t)hexValue(value[i]);
    }

    if (n == 0 || value[i] != 0) {
        return false;
    }

    player->protocol = protocol;
    player->rules = rules;

    return true;
}

static void publish(uint64_t lobby) {
    char value[LOBBY_MAX_VALUE];

    snprintf(value, sizeof(value), "%u;%s;%x", HANDSHAKE_PROTOCOL, PATCH_VERSION, RULES_SUPPORTED);
    lobbySetMemberData(MEMBER_KEY, value);
    s_publishedLobby = lobby;
}

static void refresh() {
    uint64_t members[LOBBY_MAX_MEMBERS];
    uint64_t lobby = lobbyId();
    uint64_t owner = 0;
    uint64_t self = 0;
    int count = 0;

    // Lower the count before rebuilding, so a reader never sees a half-built table.
    s_playerCount = 0;

    if (!lobby) {
        s_publishedLobby = 0;
        return;
    }

    // Once per lobby: member data lives as long as the lobby, and a new lobby
    // starts empty.
    if (lobby != s_publishedLobby) {
        publish(lobby);
    }

    owner = lobbyOwner();
    self = localSteamId();
    count = lobbyMembers(members, LOBBY_MAX_MEMBERS);

    for (int i = 0; i < count; ++i) {
        LobbyPlayer &p = s_players[i];
        char value[LOBBY_MAX_VALUE];

        memset(&p, 0, sizeof(p));
        p.steamId = members[i];
        p.isOwner = members[i] == owner;
        p.isLocal = members[i] == self;
        p.state = PATCH_NONE;
        personaName(members[i], p.name, 32);

        if (lobbyMemberData(members[i], MEMBER_KEY, value, sizeof(value))) {
            p.state = PATCH_UNREADABLE;

            if (parseMemberValue(value, &p)) {
                p.state = PATCH_FOUND;
            }
        }
    }

    s_playerCount = count;
}

int handshakePlayers(LobbyPlayer *out, int max) {
    int count = s_playerCount;

    if (count > max) {
        count = max;
    }

    memcpy(out, s_players, count * sizeof(LobbyPlayer));

    return count;
}

uint32_t handshakeCommonRules() {
    uint32_t common = RULES_SUPPORTED;

    if (s_playerCount == 0) {
        return 0;
    }

    for (int i = 0; i < s_playerCount; ++i) {
        // Every failure mode narrows to nothing: a protocol we do not speak is as
        // good as no patch at all.
        if (s_players[i].state != PATCH_FOUND || s_players[i].protocol != HANDSHAKE_PROTOCOL) {
            return 0;
        }

        common &= s_players[i].rules;
    }

    return common;
}

static void appendText(wchar_t *buf, int cap, int *len, const wchar_t *text) {
    for (int i = 0; text[i] && *len < cap - 1; ++i) {
        buf[(*len)++] = text[i];
    }

    buf[*len] = 0;
}

static void appendAscii(wchar_t *buf, int cap, int *len, const char *text) {
    for (int i = 0; text[i] && *len < cap - 1; ++i) {
        buf[(*len)++] = (wchar_t)(unsigned char)text[i];
    }

    buf[*len] = 0;
}

static void report() {
    wchar_t line[256];
    int len = 0;

    appendText(line, 256, &len, L"Patch:");

    // Always say something. A command that silently does nothing cannot be told
    // apart from one that is broken.
    if (s_playerCount == 0) {
        appendText(line, 256, &len, L" not in a Steam lobby");
        chatPost(line, len);

        return;
    }

    for (int i = 0; i < s_playerCount; ++i) {
        const LobbyPlayer &p = s_players[i];

        appendText(line, 256, &len, L" ");

        if (p.name[0]) {
            appendText(line, 256, &len, p.name);
        } else {
            appendText(line, 256, &len, L"?");
        }

        if (p.state == PATCH_FOUND) {
            appendText(line, 256, &len, L" ");
            appendAscii(line, 256, &len, p.version);
        } else if (p.state == PATCH_UNREADABLE) {
            appendText(line, 256, &len, L" unknown");
        } else {
            appendText(line, 256, &len, L" 0.7 or older");
        }

        if (p.isOwner) {
            appendText(line, 256, &len, L" (host)");
        }

        if (i + 1 < s_playerCount) {
            appendText(line, 256, &len, L",");
        }
    }

    chatPost(line, len);
}

static void requestReport() { s_reportPending = true; }

static void handshakeTick() {
    uint32_t now = GetTickCount();

    if (!s_refreshed || now - s_lastRefreshMs >= REFRESH_MS || s_reportPending) {
        s_refreshed = true;
        s_lastRefreshMs = now;
        cleanFpu(refresh);
    }

    if (s_reportPending) {
        s_reportPending = false;
        report();
    }
}

void installHandshake() {
    registerFrameTick(handshakeTick);
    registerChatCommand(L"!ver", requestReport);
}
