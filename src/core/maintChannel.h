#pragma once
#include <cstdint>

// Shared owner of the GameSpy-emulation maintenance channel: the dispatcher hook
// (RVA 0x3d88c0), the send path, and the session's peer table. See
// docs/features/ping-command.md for the wire format and why custom ids are inert
// against an unpatched client.
//
// Every message id the patch sends is listed here, so two features cannot pick
// the same one.

static const uint8_t MAINT_MARKER = 0xff;

static const uint8_t MAINT_PING_REQUEST = 0x40;
static const uint8_t MAINT_PING_REPLY = 0x41;
static const uint8_t MAINT_HELLO = 0x42;
static const uint8_t MAINT_HELLO_REPLY = 0x43;

static const int MAINT_MAX_PEERS = 16;
static const int MAINT_MAX_NAME = 24;

// The engine's OnlineID as SendP2PPacket takes it.
struct MaintOnlineId {
    uint64_t steamId;
    uint32_t valid;
};

struct MaintPeer {
    uint64_t steamId;
    wchar_t name[MAINT_MAX_NAME];
};

// Called on the thread that reads P2P packets, from a function-prologue hook.
// `msg` is hostile input: check the length before reading anything. Return true
// to consume the message, false to let the next handler (and finally the stock
// dispatcher) see it.
typedef bool (*MaintHandlerFn)(const MaintOnlineId *sender, const uint8_t *msg, int length);

// Register at install time; the hook is written on the first registration.
void registerMaintHandler(MaintHandlerFn fn);

// Sends `FF <id>` plus `payloadLen` bytes, on the same channel and send type as
// the stock keepalive.
bool maintSend(const MaintOnlineId *to, uint8_t id, const void *payload, int payloadLen);

// Copies the session's active peers, excluding ourselves, into `out`. Returns the
// count (0 outside a session).
int maintPeers(MaintPeer *out, int max);

// Our own SteamID, from the peer-table entry `maintPeers` skips; 0 if not found.
uint64_t maintLocalSteamId();

// Dragonfly's millisecond clock, the same one the keepalive timestamps use.
uint32_t maintNowMs();

// True when the send import, its instance and the clock all resolved.
bool maintChannelAvailable();
