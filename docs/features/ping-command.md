# `!ping` — Round-Trip Latency in Multiplayer Chat

**Status**: Implemented, not yet tested against a second patched client
**Config**: none — always active. The command does nothing until a player types it, and the
install is all-or-nothing on its byte checks, so an unexpected build simply leaves the game
untouched.

---

## What it does

Typing `!ping` in multiplayer chat measures the round trip to every peer in the session
and broadcasts the result as an ordinary chat message:

```text
Ping: Alice 42ms, Bob 118ms, Carol n/a
```

The typed `!ping` itself never goes on the wire — it is swallowed by the patch, so other
players only see the result line. Because the result is posted through the game's own chat
path, **everyone sees it, including players without the patch**.

---

## Why the lobby browser's Latency column could not be used

The multiplayer browser has a `Latency` column that is present but permanently empty; it is
a finished shell around a stubbed value (see the end of this document). It cannot be filled
honestly: for a lobby you are not a member of, this Steam API surface offers no latency at
all — legacy `ISteamNetworking` has no ping call, the shipped `steam_api.dll` predates
`SteamNetworkingUtils`, and `ISteamMatchmakingServers::PingServer` needs an IP:port that P2P
lobbies do not have. Latency is only measurable once you are connected to the peer, which is
what this feature does instead.

---

## Protocol

The GameSpy-emulation layer carries small "maintenance" messages alongside game traffic. A
message is two bytes plus payload: `FF <id>`. `SendGSEmuKeepaliveMessage` builds exactly
`FF 13` and hands it to `SendP2PPacket`.

The patch adds two ids to that space:

| Id | Meaning | Payload |
| --- | --- | --- |
| `0x40` | ping request | 4-byte token (the sender's millisecond clock) |
| `0x41` | ping reply | the token, echoed unchanged |

Round trip is `now - token` on receipt of the reply, so no per-request state is kept.

**Routing is by channel, not by id.** The packet pump (`0x7dcd00`, `ReadP2PPacket`) switches
on the channel it was read from — table at `0x7dcdc4`, channels `0`..`3` — and channel `3` is
the branch that calls the maintenance dispatcher. Nothing upstream inspects the id, so a
custom id on channel `3` always reaches the dispatcher. Sending on the same channel and send
type as the stock keepalive (`1`, `3`) is therefore all that is required to be routed.

**Unknown ids are safe.** The stock receiver (VA `0x7d88c0`) reads the id from `msg[1]`,
handles `0x13`/`0x14`/`0x15`, and falls through a log line to a clean `ret 0xc`. An unpatched
client therefore logs one line and drops the packet. Nothing is corrupted and no reply comes
back, so that peer shows `n/a`.

### Addresses (VAs at ImageBase 0x400000)

| Item | Address |
| --- | --- |
| `GameSpy::Network` singleton | `0x2acd7d8` |
| Peer vector | `net+0x94` (begin) … `net+0x98` (end), stride `0x40` |
| Peer: SteamID / state / last keepalive / name / peer id | `+0x00` (8) / `+0x08` (1 = active) / `+0x14` / `+0x18` (`std::wstring`) / `+0x34` |
| Local peer id | `net+0xdc` — the peer list **includes us**; the stock keepalive loop skips the entry whose `+0x34` matches this, and so must any walk of it |
| Maintenance dispatcher | `0x7d88c0`, thiscall `(senderOnlineID*, msg*, len)`, `ret 0xc` |
| `OnlineInterface::SendP2PPacket` | IAT `0x900a1c`; `this` = `*(void**)0x900a44` (`ms_instance`, a data import) |
| `Dragonfly::Time::getMilliSeconds` | IAT `0x900894` |
| `IngameChatPanel` send | `0x64d8e0`, thiscall, no stack args (bare `ret`) |
| Typed text / its `_Mysize` / caret | `panel+0x2f8` (`std::wstring`) / `+0x308` / `+0x334` |
| `std::wstring::assign(const wstring&, pos, len)` | `0x4073e0`, thiscall, `ret 0xc` |

---

## How the message is broadcast

Rather than building a `ChatChore` in the DLL — which would mean reproducing the game's
allocator, refcount and Handle discipline — the patch writes its line into the chat panel's
**own input buffer** and calls the stock send, which then builds, refcounts and posts the
message exactly as it does for a typed one, and clears the input line afterwards.

The line is written with the game's own `std::wstring::assign` so the game's allocator owns
the result. The source only has to be readable, so it is a hand-built `{ptr, size, res}`
struct with a capacity above the inline threshold to force the heap-pointer read path.

Re-entry is handled with a flag: the send hook passes straight through while the patch is the
one posting.

**`panel+0x308` is the text's `_Mysize`, not a gate pointer** (`0x2f8 + 0x10`), and the stock
send uses it to refuse an empty line. Reading it as a "can I send?" precondition *before*
assigning the outgoing text is wrong twice over: swallowing `!ping` clears the input line, so
it is always zero at that point, and the assign is what makes it non-zero. The first build
did exactly that and silently posted nothing, which looked identical to the peer walk finding
nobody.

---

## Hardening

The receive path parses bytes from a remote peer, so it is deliberately dull: exact length
check (6 bytes) before anything is read, exact marker and id match, no loops over the payload,
no allocation, fixed-size peer table, and a per-peer floor on how often a request is answered
(`REPLY_MIN_INTERVAL_MS`) so a peer cannot make us send unbounded replies. A round trip above
`MAX_PLAUSIBLE_RTT_MS` is discarded rather than displayed.

Requests are sent only when a player types `!ping`, never on a timer — a timer would make
unpatched clients write a log line every few seconds.

**A reply is a claim, not a measurement.** A modified client can answer instantly regardless
of its real latency, or delay its answer. This is a diagnostic for honest and accidental lag,
which is the common case; it is not proof of anything against a determined liar.

---

## Threading and float safety

The chat send hook and the maintenance dispatcher hook are both **function-prologue** hooks,
where the x87 stack is empty by calling convention, so the reply path needs no protection.

The broadcast happens on the frame tick, which is a **mid-function** hook, and it calls real
engine code. That path brackets the calls with `fxsave` / `fninit` / `fxrstor`.

**`fxsave` requires a 16-byte-aligned destination and will fault otherwise.** GCC places an
`alignas(16)` local assuming the incoming stack is already 16-byte aligned, which is true for
compiler-generated callers and *not* true for a callback entered from the game's main loop.
The buffer is therefore over-allocated and aligned by hand at runtime. A build was verified to
emit `and edi, 0xfffffff0` before `fxsave [edi]`.

---

## Multiplayer Compatibility

**Requires both players to have the patch** for a number to appear — an unpatched peer never
replies and shows `n/a`. It is nonetheless **safe for version mismatch**: the request is a
maintenance message the stock build already knows how to ignore, no simulation state is
touched on either side, and the broadcast is an ordinary chat message that unpatched clients
display normally.

---

## The lobby browser's Latency column (unused, for reference)

Mapped while deciding where latency belonged, and left alone:

| Piece | Address | State |
| --- | --- | --- |
| Header label | `0x69cdac` | Localisation key `multiplayer/header/ping`, absent from `en-us.xml`, so the hard-coded fallback `"Latency"` is what shows |
| Sort binding | `0x7e0ad0` | Column 3 → lobby-data key `"Ping"`, numeric |
| Cell text | `0x7dea30` case 3 → `0x7debb5` | Builds a wstring from `0x908450`, which is `00 00 00 00` — an empty string, i.e. a stub |

Filling it would be a small patch if a source of numbers ever existed. It does not.
