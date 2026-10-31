#pragma once

#include <cstddef>
#include <cstdint>

// Tick-quantized capture and restore of RandomIter #1.
//
// `GlowWorm::Game` embeds two `Dragonfly::RandomIter`s and the draw is inlined
// at every call site (three instructions against `Game+0x40080`/`+0x4007c`), so
// there is no single function to hook. Iterator 1 is SHARED: `S2Governor` draws
// the AI's invasion size and troop mix from it, while `GameView`'s environment
// FX, `GameScreen::draw`, the particle engine and the sound/music/speech
// pickers draw from the same cursor on a frame-driven schedule. A human session
// and a spectator playback render different frames, so the cursor would drift
// and the simulation's draws would land elsewhere in the table. The cursor is
// therefore recorded at the entry of every simulation update and put back at
// the same point during playback. Once per job is not enough: MP runs about
// five updates per job, and how many frames render between them depends on
// the Game Speed, so a replay watched faster than it was played gave the AI's
// draws in a job's later updates different values and diverged.
//
// The simulation tick body is `FUN_0076aca0` (RVA 0x36aca0). It bumps the tick
// counter, serializes iterator 0, runs five simulation subsystem updates and
// the emitter manager, and only then -- gated on the FX flag at RVA 0xc800b0 --
// calls the particle system. So inside a tick every simulation draw precedes
// the only in-tick presentation consumer, and forcing the cursor at tick entry
// gives every simulation draw of that tick the recorded stream.
//
// Four sites, verified together against relocated stock bytes, all or none:
//   ENTRY    RVA 0x36aca0  d9 41 08 ff 41 0c   fld [ecx+8]; inc [ecx+0xc]
//   SIM_END  RVA 0x36acf5  b9 <emitter manager> mov ecx, imm32
//   SIM_DONE RVA 0x36acff  83 3d <fxflag> 00   cmp dword [fxflag], 0
//   Monastery constructor RVA 0x739ba (see prngGuard.cpp)
// SIM_END samples the cursor before the emitter manager, whose draws are
// render-dependent; that is the recorded end-of-simulation witness. No branch
// targets any overwritten range.
//
// Render gates are also replayed at update entry (the Sheep drawn flag), since
// simulation code can condition a draw on a render-side flag.
//
// Every hook returns at once unless a recording or a playback is running, so a
// live match that is not being recorded pays one call and two compares per
// simulation update.

#pragma pack(push, 1)

// First simulation update in a job. `relativeTick` is (job tick - epoch), the
// same relative clock the command log uses.
struct ReplayPrngEntry {
    uint32_t relativeTick;
    uint16_t startCursor; // iterator 1 at tick entry, after any restore
    uint16_t simEndCursor; // iterator 1 before presentation emitter updates
    // The same pair for RandomIter #0 (Game+0x40074 table, Game+0x40078
    // cursor). Recorded as a witness only: iterator 0 is never restored, since
    // a difference there means the simulation itself diverged.
    uint16_t startCursor0;
    uint16_t simEndCursor0;
};

// v5, one per entry after the render-gate bytes: the job's later updates.
// `updates` is how many simulation updates the job ran (capped at
// REPLAY_PRNG_UPDATES_MAX), and `startCursor[k - 1]` is iterator 1 at the
// entry of update k, for 1 <= k < updates. Update 0 is the entry's own
// startCursor.
static const uint32_t REPLAY_PRNG_UPDATES_MAX = 8;
struct ReplayPrngUpdates {
    uint16_t updates;
    uint16_t startCursor[REPLAY_PRNG_UPDATES_MAX - 1];
};

struct ReplayPrngFileHeader {
    char magic[8]; // "SH2RPRN"
    uint32_t version; // 5 -- v3 appends one render-gate byte per entry after
                      // the entries (bit k = Sheep drawn flag at the job's
                      // k-th update); v4 appends the Monastery clocks; v5
                      // inserts a ReplayPrngUpdates per entry before them
    uint32_t entrySize; // sizeof(ReplayPrngEntry)
    uint32_t entryCount;
    uint32_t epochTick; // the recording process's epoch
    uint32_t firstRelative;
    uint32_t lastRelative;
    uint32_t droppedTicks; // ticks past capacity, never overwritten
};

// v4 footer, after entries + gate bytes: {'MCLK', count}, then count entries.
// Monastery's +0x164 stage clock is missing from its stock serializer. Restore
// it ONCE, before the first update, matching BuildingMgr slot/serial/class.
struct ReplayMonasteryClock {
    uint16_t slot, serial;
    uint32_t clock;
};
static const uint32_t REPLAY_CLOCK_MAGIC = 0x4b4c434du; // "MCLK"
static const uint32_t REPLAY_CLOCK_MAX = 4096;

#pragma pack(pop)

static_assert(sizeof(ReplayPrngEntry) == 12, "prng entry wire format changed");
static_assert(sizeof(ReplayPrngFileHeader) == 36, "prng file header changed");
static_assert(sizeof(ReplayPrngUpdates) == 16, "prng updates wire format changed");
static const uint32_t REPLAY_PRNG_VERSION = 5;

// Jobs the iterator-1 record holds (about 19 hours at the ~7.4 jobs/s of a
// live multiplayer match).
static const uint32_t REPLAY_PRNG_CAPACITY = 524288;

// Verifies the four sites and installs them all or none.
void installPrngGuard();
bool prngGuardInstalled();

// The record buffer: allocated by the replay session for a recording and
// freed when it ends. Reset empties it for a new recording.
bool prngGuardAcquireRecord();
void prngGuardReleaseRecord();
void prngGuardResetRecord();

// Loads a sidecar PRNG section and arms restoration (the stream buffer is
// allocated here). Returns the entry count, or 0 on refusal. Clear stops
// restoring and frees the stream.
uint32_t prngGuardLoadBytes(const void *data, uint32_t bytes);
void prngGuardClearStream();

// The recorded stream so far. `dropped` nonzero means it is incomplete.
void prngGuardRecordView(const ReplayPrngEntry **entries, uint32_t *count, uint32_t *dropped);
void prngGuardRecordClocks(const ReplayMonasteryClock **entries, uint32_t *count);
// One render-gate byte per recorded entry.
const uint8_t *prngGuardRecordGates();
// One ReplayPrngUpdates per recorded entry.
const ReplayPrngUpdates *prngGuardRecordUpdates();

// Playback: restores made at the entry of a job's later updates, and how many
// of those found the live cursor somewhere other than the recorded value (a
// draw the simulation would otherwise have taken from the wrong place).
void prngGuardRestoreStats(uint32_t *restored, uint32_t *corrected);
