#include "pingCommand.h"
#include "../core/chatCommand.h"
#include "../core/frameTick.h"
#include "../core/maintChannel.h"
#include <cstdint>
#include <cstring>

// See docs/features/ping-command.md.
static const int PING_MSG_LEN = 6; // FF <id> + 4-byte token

static const uint32_t COLLECT_TIMEOUT_MS = 1500;
static const uint32_t MAX_PLAUSIBLE_RTT_MS = 10000;
static const uint32_t REPLY_MIN_INTERVAL_MS = 200; // per-peer cap on how often we answer

struct PeerPing {
    MaintPeer peer;
    uint32_t replyMs;
    uint32_t lastRepliedMs;
    bool replied;
};

static PeerPing s_peers[MAINT_MAX_PEERS];
static int s_peerCount = 0;
static bool s_pending = false;
static uint32_t s_roundStartMs = 0;

static void appendChars(wchar_t *buf, int cap, int *len, const wchar_t *text, int count) {
    for (int i = 0; i < count && *len < cap - 1; ++i) {
        buf[(*len)++] = text[i];
    }

    buf[*len] = 0;
}

static void appendText(wchar_t *buf, int cap, int *len, const wchar_t *text) {
    int n = 0;

    while (text[n]) {
        ++n;
    }

    appendChars(buf, cap, len, text, n);
}

static void appendNumber(wchar_t *buf, int cap, int *len, uint32_t value) {
    wchar_t digits[12];
    int n = 0;

    if (value == 0) {
        digits[n++] = L'0';
    }

    while (value > 0 && n < 11) {
        digits[n++] = (wchar_t)(L'0' + (value % 10));
        value /= 10;
    }

    for (int i = n - 1; i >= 0; --i) {
        appendChars(buf, cap, len, &digits[i], 1);
    }
}

// Snapshots the active peers and sends each a request stamped with the send time,
// so a reply carries everything needed to work out the round trip.
static void startRound() {
    MaintPeer peers[MAINT_MAX_PEERS];
    uint32_t now = maintNowMs();
    int count = maintPeers(peers, MAINT_MAX_PEERS);

    // Lower the count before rebuilding: the reply handler walks this table on the
    // packet thread.
    s_peerCount = 0;
    s_pending = true;
    s_roundStartMs = now;

    for (int i = 0; i < count; ++i) {
        s_peers[i].peer = peers[i];
        s_peers[i].replyMs = 0;
        s_peers[i].replied = false;
        s_peers[i].lastRepliedMs = 0;
    }

    s_peerCount = count;

    for (int i = 0; i < count; ++i) {
        MaintOnlineId id = {peers[i].steamId, 1};
        maintSend(&id, MAINT_PING_REQUEST, &now, sizeof(now));
    }
}

static void reportRound() {
    wchar_t line[256];
    int len = 0;

    appendText(line, 256, &len, L"Ping:");

    // Always say something. A command that silently does nothing cannot be told
    // apart from one that is broken.
    if (s_peerCount == 0) {
        appendText(line, 256, &len, L" no other players");
        chatPost(line, len);

        return;
    }

    for (int i = 0; i < s_peerCount; ++i) {
        appendText(line, 256, &len, L" ");

        if (s_peers[i].peer.name[0]) {
            appendText(line, 256, &len, s_peers[i].peer.name);
        } else {
            appendText(line, 256, &len, L"?");
        }

        if (s_peers[i].replied) {
            appendText(line, 256, &len, L" ");
            appendNumber(line, 256, &len, s_peers[i].replyMs);
            appendText(line, 256, &len, L"ms");
        } else {
            appendText(line, 256, &len, L" n/a");
        }

        if (i + 1 < s_peerCount) {
            appendText(line, 256, &len, L",");
        }
    }

    if (len > 0) {
        chatPost(line, len);
    }
}

// Runs on the thread that reads P2P packets. Everything here treats the packet as
// hostile: fixed length, exact match, no loops over payload, no allocation, and a
// per-peer cap on how often we will answer.
static bool pingMaintMessage(const MaintOnlineId *sender, const uint8_t *msg, int length) {
    uint32_t token = 0;
    uint32_t now = 0;
    uint64_t from = sender->steamId;

    if (msg[1] != MAINT_PING_REQUEST && msg[1] != MAINT_PING_REPLY) {
        return false;
    }

    // Ours by id, so consume it even when malformed rather than let the stock
    // dispatcher log it.
    if (length != PING_MSG_LEN) {
        return true;
    }

    memcpy(&token, msg + 2, sizeof(token));
    now = maintNowMs();

    if (msg[1] == MAINT_PING_REQUEST) {
        for (int i = 0; i < s_peerCount; ++i) {
            if (s_peers[i].peer.steamId != from) {
                continue;
            }

            if (s_peers[i].lastRepliedMs &&
                now - s_peers[i].lastRepliedMs < REPLY_MIN_INTERVAL_MS) {
                return true;
            }

            s_peers[i].lastRepliedMs = now;
            break;
        }

        maintSend(sender, MAINT_PING_REPLY, &token, sizeof(token));

        return true;
    }

    if (s_pending) {
        uint32_t rtt = now - token;

        if (rtt <= MAX_PLAUSIBLE_RTT_MS) {
            for (int i = 0; i < s_peerCount; ++i) {
                if (s_peers[i].peer.steamId == from && !s_peers[i].replied) {
                    s_peers[i].replied = true;
                    s_peers[i].replyMs = rtt;
                    break;
                }
            }
        }
    }

    return true;
}

static void pingTick() {
    bool complete = true;

    if (!s_pending) {
        return;
    }

    for (int i = 0; i < s_peerCount; ++i) {
        if (!s_peers[i].replied) {
            complete = false;
            break;
        }
    }

    if (!complete && maintNowMs() - s_roundStartMs < COLLECT_TIMEOUT_MS) {
        return;
    }

    s_pending = false;
    reportRound();
}

void installPingCommand() {
    // Without the channel every peer would read n/a, which looks like a network fault.
    if (!maintChannelAvailable()) {
        return;
    }

    registerMaintHandler(pingMaintMessage);
    registerChatCommand(L"!ping", startRound);
    registerFrameTick(pingTick);
}
