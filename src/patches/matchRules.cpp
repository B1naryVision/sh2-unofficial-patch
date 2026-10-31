#include "matchRules.h"
#include "../core/chatCommand.h"
#include "../core/cleanFpu.h"
#include "../core/config.h"
#include "../core/frameTick.h"
#include "../core/screens.h"
#include "../core/steamLobby.h"
#include "ballistaAutoFire.h"
#include "crimeExclusion.h"
#include "disbandRefundGuard.h"
#include "handshake.h"
#include "noMarket.h"
#include <cstdio>
#include <cstring>
#include <windows.h>

// See docs/features/match-rules.md.
//
// Two kinds of rule. An *option* is the host's choice. A *fix* is a bug fix that
// changes the simulation: never a choice, on whenever every player has it.
//
// Three phases, and only the last one changes the simulation:
//   lobby   - the host's choice plus every fix, narrowed to what everyone
//             supports, is published as lobby data for every player's panel.
//   start   - the host's client posts it as a chat line ending in a tag.
//   execute - every client sees that line on the same lockstep tick (ChatChore::
//             execute) and switches the tagged rules on there.
static const uintptr_t NETWORK_RVA = 0x26cd7d8; // GameSpy::Network
static const uintptr_t NET_HOST_PEER_OFF = 0x50; // the host's peer id, as this client knows it

// S2Game (VA 0x11781d0) +0x40089: when set, S2Game::post executes a command at
// once instead of queueing it for lockstep, so there is no other client to stay
// in step with. Reads 0 in a multiplayer match.
static const uintptr_t COMMANDS_IMMEDIATE_RVA = 0xd781d0 + 0x40089;

static const char *LOBBY_RULES_KEY = "sh2up_rules";
static const char *INI_SECTION = "matchrules";

static const uint32_t REFRESH_MS = 1000;
static const uint32_t POST_DELAY_MS = 2000; // after the match screen appears

// The tag that makes a chat line a rules line: `[sh2up:<protocol>:<hex rules>]`
// at the very end. The readable text before it is for players without the patch.
static const wchar_t *TAG_PREFIX = L"[sh2up:";

struct Rule {
    uint32_t bit;
    const char *label;
    bool fix;
    const char *iniKey; // options only: where the host's choice is remembered
    bool (*install)();
    void (*setEnabled)(bool);
};

static const Rule RULES[] = {
    {RULE_BALLISTA_AUTOFIRE, "Ballista auto-fire", false, "BallistaAutoFire",
     installBallistaAutoFire, ballistaAutoFireSetEnabled},
    {RULE_DISBAND_FIX, "Disband peasant fix", true, nullptr, installDisbandRefundGuard,
     disbandRefundGuardSetEnabled},
    {RULE_NO_MARKET, "No market", false, "NoMarket", installNoMarket, noMarketSetEnabled},
    {RULE_CRIME_EXCLUSION_FIX, "Stuck-worker crime fix", true, nullptr, installCrimeExclusion,
     crimeExclusionSetEnabled},
};

static const int RULE_COUNT = (int)(sizeof(RULES) / sizeof(RULES[0]));

static uint32_t s_supported = 0; // rules whose sites verified on this machine
static uint32_t s_fixes = 0; // the fix rules among them
static uint32_t s_chosen = 0; // the host's options, from the ini and the lobby panel
static uint32_t s_proposed = 0; // what the lobby shows for the next match
static uint32_t s_active = 0; // switched on in the current match
static bool s_committed = false; // a rules line has executed in this match
static bool s_posted = false; // as host, this match's rules line has been sent
static uint32_t s_gameSeenMs = 0;
static uint32_t s_lastRefreshMs = 0;
static uint32_t s_published = 0xffffffff;
static bool s_inSteamLobby = false; // cached by the refresh, which may call Steam
static bool s_wasInLobbyScreen = false;

static bool commandsImmediate() {
    return *(uint8_t *)((uintptr_t)GetModuleHandleA(NULL) + COMMANDS_IMMEDIATE_RVA) != 0;
}

// What the host offers for the next match: its options plus every fix, then only
// what everyone in the lobby can run.
static uint32_t hostProposal() { return (s_chosen | s_fixes) & handshakeCommonRules(); }

static uint32_t hostPeer() {
    return *(uint32_t *)((uintptr_t)GetModuleHandleA(NULL) + NETWORK_RVA + NET_HOST_PEER_OFF);
}

static void setActive(uint32_t rules) {
    for (int i = 0; i < RULE_COUNT; ++i) {
        RULES[i].setEnabled((rules & RULES[i].bit) != 0);
    }

    s_active = rules;
}

static int hexValue(wchar_t c) {
    if (c >= L'0' && c <= L'9') {
        return c - L'0';
    }

    if (c >= L'a' && c <= L'f') {
        return c - L'a' + 10;
    }

    if (c >= L'A' && c <= L'F') {
        return c - L'A' + 10;
    }

    return -1;
}

// Another player's text: accept exactly `[sh2up:<digits>:<1-8 hex>]` closing the
// line, and nothing else.
static bool parseTag(const wchar_t *text, int length, uint32_t *protocol, uint32_t *rules) {
    int prefixLen = (int)wcslen(TAG_PREFIX);
    int start = -1;
    int i = 0;
    int n = 0;
    uint32_t p = 0;
    uint32_t r = 0;

    for (i = length - prefixLen; i >= 0; --i) {
        if (wcsncmp(text + i, TAG_PREFIX, prefixLen) == 0) {
            start = i;
            break;
        }
    }

    if (start < 0) {
        return false;
    }

    i = start + prefixLen;

    for (n = 0; i < length && text[i] >= L'0' && text[i] <= L'9' && n < 4; ++n, ++i) {
        p = p * 10 + (uint32_t)(text[i] - L'0');
    }

    if (n == 0 || i >= length || text[i++] != L':') {
        return false;
    }

    for (n = 0; i < length && hexValue(text[i]) >= 0 && n < 8; ++n, ++i) {
        r = (r << 4) | (uint32_t)hexValue(text[i]);
    }

    if (n == 0 || i != length - 1 || text[i] != L']') {
        return false;
    }

    *protocol = p;
    *rules = r;

    return true;
}

// Simulation thread, at the tick the chat line executes. Every input to the
// decision is the same on every client: the line and its sender travel inside
// the lockstep command, and "already committed" follows from the command order.
static void onChatExecuted(uint32_t senderPeer, const wchar_t *text, int length) {
    uint32_t protocol = 0;
    uint32_t rules = 0;

    if (s_committed || !parseTag(text, length, &protocol, &rules)) {
        return;
    }

    if (senderPeer != hostPeer() || protocol != HANDSHAKE_PROTOCOL) {
        return;
    }

    // A rule this build cannot run: switching on the rest would still leave us
    // out of step with whoever runs that one, so stay vanilla. The host only
    // offers what everyone advertised, so this means a modified or broken host.
    if ((rules & ~s_supported) != 0) {
        return;
    }

    s_committed = true;
    setActive(rules);
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

static void postRules(uint32_t rules) {
    wchar_t line[200];
    wchar_t tag[32];
    int len = 0;
    bool first = true;

    appendText(line, 200, &len, L"Match rules:");

    for (int i = 0; i < RULE_COUNT; ++i) {
        if (!(rules & RULES[i].bit)) {
            continue;
        }

        if (!first) {
            appendText(line, 200, &len, L",");
        }

        appendText(line, 200, &len, L" ");
        appendAscii(line, 200, &len, RULES[i].label);
        first = false;
    }

    _snwprintf_s(tag, 32, _TRUNCATE, L" %ls%u:%x]", TAG_PREFIX, HANDSHAKE_PROTOCOL, rules);
    appendText(line, 200, &len, tag);
    chatPost(line, len);
}

// The lobby view, refreshed once a second. Steam calls, so inside cleanFpu.
static void refreshLobby() {
    char value[LOBBY_MAX_VALUE];
    uint32_t proposed = 0;

    s_inSteamLobby = lobbyActive();

    if (!s_inSteamLobby) {
        s_proposed = 0;
        s_published = 0xffffffff;
        return;
    }

    if (lobbyIsLocalOwner()) {
        proposed = hostProposal();

        if (proposed != s_published) {
            snprintf(value, sizeof(value), "%x", proposed);
            lobbySetData(LOBBY_RULES_KEY, value);
            s_published = proposed;
        }
    } else if (lobbyData(LOBBY_RULES_KEY, value, sizeof(value))) {
        for (int i = 0; value[i] && i < 8; ++i) {
            int h = hexValue((wchar_t)value[i]);

            if (h < 0) {
                proposed = 0;
                break;
            }

            proposed = (proposed << 4) | (uint32_t)h;
        }

        // Display only: narrow to what this lobby can actually run.
        proposed &= handshakeCommonRules();
    }

    s_proposed = proposed;
}

static void matchRulesTick() {
    uint32_t now = GetTickCount();
    bool inLobby = lobbyScreenUp();
    bool inGame = gameScreenUp();

    if (now - s_lastRefreshMs >= REFRESH_MS || s_lastRefreshMs == 0) {
        s_lastRefreshMs = now;
        cleanFpu(refreshLobby);
    }

    // A match with nobody to stay in step with: a bug fix needs no agreement, so
    // every fix is simply on. Everywhere else outside a Steam lobby (the menus),
    // vanilla. Never while a lockstep match is on screen: a client that lost its
    // Steam lobby mid-match must not switch a rule off while the others still run
    // it.
    if (!s_inSteamLobby && (!inGame || commandsImmediate())) {
        uint32_t solo = 0;

        if (inGame) {
            solo = s_fixes;
        }

        if (s_active != solo) {
            setActive(solo);
        }

        s_committed = false;
        s_posted = false;
        s_gameSeenMs = 0;
        s_wasInLobbyScreen = inLobby;

        return;
    }

    // Back to vanilla only once this client has plainly left the match: on the
    // way *into* the lobby screen. An edge, not a level, and never on the match
    // screen merely detaching: a defeat screen or a menu on one client, or a lobby
    // screen still attached on a slow one, must not switch a rule off while the
    // others are simulating with it.
    if (inLobby && !s_wasInLobbyScreen) {
        if (s_active) {
            setActive(0);
        }

        s_committed = false;
        s_posted = false;
        s_gameSeenMs = 0;
    }

    s_wasInLobbyScreen = inLobby;

    if (!inGame || s_posted) {
        return;
    }

    if (s_gameSeenMs == 0) {
        s_gameSeenMs = now;
        return;
    }

    if (now - s_gameSeenMs < POST_DELAY_MS) {
        return;
    }

    s_posted = true;

    // The narrowed choice as it stood in the lobby. Nothing to post means a
    // vanilla match, which needs no message at all.
    if (lobbyIsLocalOwner() && s_proposed) {
        postRules(s_proposed);
    }
}

int matchRulesView(MatchRuleView *out, int max) {
    uint32_t common = handshakeCommonRules();
    int n = 0;

    for (int i = 0; i < RULE_COUNT && n < max; ++i) {
        out[n].label = RULES[i].label;
        out[n].fix = RULES[i].fix;
        out[n].everyoneSupports = (common & RULES[i].bit) != 0;
        out[n].proposed = (s_proposed & RULES[i].bit) != 0;
        ++n;
    }

    return n;
}

// The host clicked a rule in the lobby panel. Persisted at once, so the host's
// lobby starts with the same rules next time.
bool matchRuleToggle(int index) {
    if (index < 0 || index >= RULE_COUNT || !lobbyIsLocalOwner()) {
        return false;
    }

    const Rule &rule = RULES[index];

    // A fix is not the host's to switch off.
    if (rule.fix || !(handshakeCommonRules() & rule.bit)) {
        return false;
    }

    s_chosen ^= rule.bit;

    if (s_chosen & rule.bit) {
        configSetString(INI_SECTION, rule.iniKey, "1");
    } else {
        configSetString(INI_SECTION, rule.iniKey, "0");
    }

    // Shown straight away; published to the lobby on the next refresh.
    s_proposed = hostProposal();

    return true;
}

void installMatchRules() {
    for (int i = 0; i < RULE_COUNT; ++i) {
        const Rule &rule = RULES[i];

        if (!rule.install()) {
            continue;
        }

        s_supported |= rule.bit;

        if (rule.fix) {
            s_fixes |= rule.bit;
        } else if (configInt(INI_SECTION, rule.iniKey, 0) == 1) {
            s_chosen |= rule.bit;
        }
    }

    handshakeSetSupportedRules(s_supported);

    if (!s_supported) {
        return;
    }

    registerChatObserver(onChatExecuted);
    registerFrameTick(matchRulesTick);
}
