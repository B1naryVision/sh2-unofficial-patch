#pragma once

#include <cstdint>

#include "prngGuard.h"
#include "replayRecorder.h"

// The sidecar (`.s2replay`) layout and the validation of every section in it.
//
// A sidecar is meant to be shared, so everything here treats its bytes as
// hostile: each function checks lengths before it reads and refuses anything
// that does not describe itself exactly. These are pure functions over byte
// buffers, with no game or Windows dependency, so tests/replayTests.cpp runs
// every refusal on the host.

static const uint32_t REPLAY_FORMAT_REFUSED = 0xffffffffu;

uint32_t replayFnv32(uint32_t hash, const void *data, uint32_t bytes);
static const uint32_t REPLAY_FNV32_SEED = 2166136261u;

// Little-endian packed four-character tag.
uint32_t replayTag(const char *fourcc);

#pragma pack(push, 1)
struct ReplayBundleHeader {
    char magic[8]; // "S2REPLAY"
    uint32_t version; // 1
    uint32_t headerSize;
    uint32_t flags; // REPLAY_BUNDLE_*
    uint32_t endReason; // REPLAY_END_*
    uint64_t checkpointHash; // FNV-1a 64 of the .sh2 bytes
    uint32_t checkpointSize;
    uint32_t sectionCount;
    uint32_t bodyBytes;
    uint32_t bodyHash; // FNV-1a 32 of the body
    uint32_t mpPhase; // NetGameMgr+4 byte at the checkpoint
    uint32_t netMode; // 2 = recorded in a networked session
    uint32_t classCount;
    uint32_t epochTick;
    uint32_t endRelative; // last job the recording saw, relative to the epoch
    uint32_t commandCount;
    uint32_t injectedCount;
    uint32_t retimedCount;
    // Injected commands never executed live. They are left out of CMDS.
    uint32_t unretimedCount;
    uint32_t execPlans;
    uint32_t exeImageSize;
    uint32_t exeTimeStamp;
    uint64_t recordedUtc; // FILETIME
    uint64_t updatedUtc; // FILETIME of this write
    uint32_t cutCommands; // REPLAY_BUNDLE_TRUNCATED: commands left out past the end
    uint32_t reserved[7];
};

struct ReplayBundleSection {
    uint32_t tag;
    uint32_t bytes;
};
#pragma pack(pop)

static_assert(sizeof(ReplayBundleHeader) == 144, "replay bundle header changed");
static_assert(sizeof(ReplayBundleSection) == 8, "replay bundle section changed");

static const uint32_t REPLAY_BUNDLE_COMPLETE = 1u << 0; // written when the match ended
static const uint32_t REPLAY_BUNDLE_RETIMED = 1u << 1; // execution schedule applied
static const uint32_t REPLAY_BUNDLE_MP_TIMING = 1u << 2; // replay with MP job cadence
static const uint32_t REPLAY_BUNDLE_TILE_ORDER = 1u << 3;
static const uint32_t REPLAY_BUNDLE_PRNG = 1u << 4;
// The recording went on past what the sidecar keeps (a buffer filled up):
// commands are exact through endRelative and the replay ends there.
static const uint32_t REPLAY_BUNDLE_TRUNCATED = 1u << 5;
// reserved[0] = RandomIter #0 cursor before (low 16) / after (high 16) the
// checkpoint save; playback's first job starts from "after".
static const uint32_t REPLAY_BUNDLE_ITER0_AT_SAVE = 1u << 6;
// reserved[1] = the recorder id of the install that recorded it. Retention
// only ever deletes replays carrying this install's id.
static const uint32_t REPLAY_BUNDLE_RECORDER_ID = 1u << 7;

enum : uint32_t {
    REPLAY_END_RECORDING = 0, // periodic snapshot of a match still in progress
    REPLAY_END_LEFT_MATCH = 1, // the match ended (menu exit, desync drop, game over exit)
    REPLAY_END_STOPPED = 2, // the recorder stopped on its own
    REPLAY_END_PROCESS_EXIT = 3,
    REPLAY_END_CRASH = 4,
    REPLAY_END_REPLACED = 5, // a save was loaded over the recorded match
    REPLAY_END_WIN = 6, // the local player's victory screen came up
    REPLAY_END_LOSE = 7, // the local player's defeat screen came up
    REPLAY_END_CAPACITY = 8, // the recording buffers were nearly full
};

// The whole file: header magic, version and size, the body length against the
// file length, and the body hash. Fills `out` either way.
bool replayValidateBundle(const uint8_t *file, uint32_t bytes, ReplayBundleHeader *out);

// The section tagged `tag` in a validated body. Refuses a section table that
// runs past the body.
bool replayFindSection(
    const uint8_t *body, uint32_t bytes, uint32_t sections, uint32_t tag, const uint8_t **data,
    uint32_t *size
);

// CMDS: a ReplayLogFileHeader and its records. Every record and payload must
// fit, the count must match, ticks must not go backwards and none may precede
// the epoch (a loaded-save replay has nothing before its checkpoint), and every
// class index must be in the current table or 0xffff. Returns false or fills
// `out`; the records follow the header in `data`.
bool replayValidateCommandLog(
    const void *data, uint32_t bytes, uint32_t classCount, uint32_t capacity,
    ReplayLogFileHeader *out
);

// TILE: {'TORD', 1, lists}, then per list {kind 0 tile / 1 region, length
// 2..64, actor slots head-first}. Returns the word count after the 3-word
// header, or REPLAY_FORMAT_REFUSED.
static const uint32_t REPLAY_TILE_MAGIC = 0x44524F54u;
static const uint32_t REPLAY_TILE_MAX_WORDS = 65536;
static const uint32_t REPLAY_TILE_MAX_MEMBERS = 64;
uint32_t replayValidateTileOrder(const void *data, uint32_t bytes, uint32_t *lists);

// EXOR: {'EXOR', 1, jobs}, then per job {relative job, n 1..256, n items}; an
// item is a vtable RVA with bit 31 set for an injected command. Returns the word
// count after the header, or REPLAY_FORMAT_REFUSED.
static const uint32_t REPLAY_EXOR_MAGIC = 0x524F5845u;
static const uint32_t REPLAY_EXOR_MAX_WORDS = 262144;
static const uint32_t REPLAY_EXOR_MAX_ITEMS = 256;
uint32_t replayValidateExecOrder(const void *data, uint32_t bytes, uint32_t *jobs);

// FLYS: per-shot projectile damage at the checkpoint. The shooter stores each
// Arrow's / Axe's / Spear's damage in the projectile, but their serializers skip
// that field, so a checkpoint load gives every missile in flight its
// constructor default. {'FLYS', 1, count} + count entries.
#pragma pack(push, 1)
struct ReplayFlyField {
    uint16_t slot; // index in the FlyMgr slot vector = handle low word (+0x78)
    uint16_t serial; // handle high word (+0x7a)
    uint32_t vtableRva; // the projectile's class
    uint32_t offset; // the unserialized field
    uint32_t value; // its live bits
};
#pragma pack(pop)
static_assert(sizeof(ReplayFlyField) == 16, "fly field layout");
static const uint32_t REPLAY_FLY_MAGIC = 0x53594C46u;
static const uint32_t REPLAY_FLY_MAX = 4096;
// True for exactly the (class, field) pairs the recorder captures:
// Arrow +0xd4, Axe +0xcc, Spear +0xd0.
bool replayFlyFieldKnown(uint32_t vtableRva, uint32_t offset);
// The captured field of a projectile class, 0 for any other class.
uint32_t replayFlyFieldOffset(uint32_t vtableRva);
// Returns the entry count (0 is valid), or REPLAY_FORMAT_REFUSED for a bad
// header or any (class, field) outside replayFlyFieldKnown.
uint32_t replayValidateFlyState(const void *data, uint32_t bytes);

// PRNG: a ReplayPrngFileHeader (v2..v5), its entries, v3+ one render-gate byte
// per entry, v5 one ReplayPrngUpdates per entry (1..8 updates), v4+ a
// {'MCLK', count} footer and count Monastery clocks. Entries must be strictly
// increasing in relative tick and clocks strictly increasing in slot.
struct ReplayPrngLayout {
    ReplayPrngFileHeader header;
    const ReplayPrngEntry *entries;
    const uint8_t *gates; // null before v3
    const ReplayPrngUpdates *updates; // null before v5
    const ReplayMonasteryClock *clocks;
    uint32_t clockCount;
};
bool replayValidatePrng(
    const void *data, uint32_t bytes, uint32_t maxEntries, ReplayPrngLayout *out
);
