#pragma once

#include <cstddef>
#include <cstdint>

// Reuse of the game's own command codec.
//
// Every concrete Chore is a Dragonfly::Classified at object +0x00 and a
// Dragonfly::Serializable at object +0x04. The multiplayer send path encodes a
// command as one class-index byte followed by whatever the class's own
// `serial(Stream&)` writes, and the receive path rebuilds it with
// Class::createSharedInstance plus the same `serial` against an input stream.
//
// That is a complete, engine-authored, pointer-free command codec, so the
// recorder reuses it instead of hand-mapping 79 object layouts. The
// hand-recovered schemas in replayTrace.cpp are kept as an independent
// cross-check: a disagreement is a real defect, not a tie to break.
//
// Everything here must run on the simulation thread (the frame tick),
// because it calls stock game code and touches game allocations.

#pragma pack(push, 1)

// One registered command class, in NetGameMgr wire order.
struct ChoreClassRecord {
    uint32_t index;      // wire class-index byte
    uint32_t classRva;   // RVA of the static Class object, 0xffffffff if foreign
    uint32_t nameHash;   // FNV-1a of the stock class name
    uint32_t objectSize; // instance size the stock factory allocates
    char name[40];       // stock class name, NUL padded
};

struct ChoreClassSnapshotHeader {
    uint32_t magic;        // "CLS1" little-endian
    uint16_t version;
    uint16_t nameOffset;   // recovered Dragonfly::Class name field offset
    uint32_t total;        // registered class count
    uint16_t first;        // wire index of the first record in this snapshot
    uint16_t count;        // records in this snapshot
    uint32_t status;       // bit 0 imports resolved, bit 1 name offset proven
    uint32_t reserved;
};

// Result of the non-simulating serialize/deserialize round trip.
struct ChoreRoundTripHeader {
    uint32_t magic;        // "RTP1" little-endian
    uint16_t version;
    uint16_t classIndex;
    uint32_t payloadLength;
    uint32_t payloadHash;      // FNV-1a of the serialized bytes
    uint32_t sourceObjectHash; // FNV-1a of the source object's data words
    uint32_t rebuiltObjectHash;// FNV-1a of the rebuilt object's data words
    uint32_t objectSize;       // bytes compared
    uint32_t status;           // bit0 built, bit1 serialized, bit2 rebuilt,
                               // bit3 payload re-serialized identically,
                               // bit4 object images equal
};

#pragma pack(pop)

static_assert(sizeof(ChoreClassRecord) == 56, "chore class wire record changed");
static_assert(sizeof(ChoreClassSnapshotHeader) == 24,
              "chore class wire header changed");
static_assert(sizeof(ChoreRoundTripHeader) == 32,
              "chore round trip wire header changed");

// True once the stock imports, NetGameMgr class array, and World dispatch have
// all been resolved and validated. Safe to call from the simulation thread.
bool choreCodecReady();

// Serialize one live Chore exactly as the multiplayer send path would.
// Returns the payload byte count, or 0 on failure. `classIndexOut` receives the
// wire class index, or 0xffffffff when the class is not registered — an
// unregistered class must fail loudly rather than record an unusable command.
size_t choreCodecSerialize(void *chore, void *out, size_t cap,
                           uint32_t *classIndexOut);

// Number of registered command classes, or 0 when the codec is unavailable.
uint32_t choreCodecClassCount();

// True when `classIndex` names a LOBBY class -- one of the two setup commands a
// cold (New Skirmish / Kingmaker) start emits before the in-match epoch exists:
// `StartGameChore`, which carries the map path, the player slot table and the
// lord name and is the command that CREATES the match, and
// `UpdateGameDescriptionChore`, which precedes it.
//
// Playback needs this because origin classification cannot answer the question.
// Both are recorded as origin=system, which in-match means "the simulation
// re-derives this, do not inject" -- but a playback process has no lobby, so
// nothing will ever re-derive them. They are matched by NAME rather than by
// wire index so that the rule survives a class-table change; the index is a
// position in a per-build table and this decides whether a match starts at all.
bool choreCodecIsLobbyClass(uint32_t classIndex);

// Wire index of a registered class by its stock name, or 0xffffffff.
uint32_t choreCodecClassIndexByName(const char *name);

// Chronological class-table snapshot starting at `first`.
size_t choreCodecClassSnapshot(void *out, size_t cap, uint32_t first);

// Rebuild a command from a recorded stock payload and admit it through the
// exact World entry the UI uses. This is the playback primitive: it repeats the
// stock multiplayer receive sequence -- Class::createSharedInstance, the class's
// own serial against a BufferInputStream, then World dispatch -- so nothing
// about the command is reconstructed by us. Returns false and admits nothing if
// the class index is out of range or the object cannot be built.
// Optional identity of the admitted object, for exact execution correlation.
// The retained reference described in the implementation owns its lifetime.
bool choreCodecAdmit(uint32_t classIndex, const void *payload, size_t length,
                    void **admittedOut = nullptr);

// Build one instance of `className`, stamp `probe` over its data words,
// serialize, rebuild a second instance from those bytes, and compare. Neither
// instance is ever admitted, so this proves the codec without perturbing the
// simulation. A `probe` of 0 skips the stamp and round-trips the freshly
// constructed object, which is the only safe mode for classes that own a
// vector or a string. Returns bytes written to `out`, or 0.
size_t choreCodecRoundTrip(const char *className, uint32_t probe, void *out,
                           size_t cap);
