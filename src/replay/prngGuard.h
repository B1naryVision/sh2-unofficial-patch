#pragma once

#include <cstddef>
#include <cstdint>

// Tick-quantized capture and restore of RandomIter #1.
//
// Why this exists. `GlowWorm::Game` embeds two `Dragonfly::RandomIter`s and the
// draw is inlined at every call site (three instructions against
// `Game+0x40080`/`+0x4007c`), so there is no single function to hook. A
// census of all 61 consumer functions found iterator 1
// is SHARED: `S2Governor` draws the AI's invasion size and troop mix from it,
// while `GameView`'s environment FX, `GameScreen::draw`, the particle engine
// and the sound/music/speech pickers draw from the same cursor on a
// frame-driven schedule. A human session and a spectator playback render
// different frame profiles, so the shared cursor drifts and the simulation's
// draws land at a different place in the table. That is the last residual.
//
// Because `S2Governor` is simulation, the stream cannot simply be dropped from
// the canonical fingerprint. The fix, in its cheap form: record the cursor once per simulation tick
// and put it back at the same point during playback.
//
// The simulation tick body is
// `FUN_0076aca0` (RVA 0x36aca0). It bumps the tick counter, serializes
// iterator 0, runs five simulation subsystem updates and the emitter manager,
// and only then --
// gated on the FX flag at RVA 0xc800b0 -- calls the particle system
// `FUN_007bd930`. So inside a tick every simulation draw precedes the only
// in-tick presentation consumer. Forcing the cursor at tick entry therefore
// makes every simulation draw of that tick see the recorded stream, no matter
// how many frames rendered in between.
//
// Three tick hook sites, verified together against relocated stock bytes:
//
// Render gates are also replayed at update entry (v3 Sheep drawn flag), since
// even simulation code can condition a draw on a render-side flag.
//
// The entry/done sites have no branch
// targeting the overwritten range:
//   ENTRY    RVA 0x36aca0  d9 41 08 ff 41 0c   fld [ecx+8]; inc [ecx+0xc]
//   SIM_DONE RVA 0x36acff  83 3d <fxflag> 00   cmp dword [fxflag], 0
//   SIM_END  RVA 0x36acf5  b9 <emitter manager> mov ecx, imm32
// SIM_DONE precedes the particle-system call but follows FireBaseEmitter's
// render-dependent draws. SIM_END samples before the emitter manager instead;
// that is the canonical iterator-1 witness in both release and diagnostic
// builds. prngphase/prngsteps retain raw post-emitter cursors for diagnosis.
// A fourth site (Monastery ctor RVA 0x739ba) initializes its omitted +0x164
// prayer clock to zero. All four sites are verified before any are patched.
//
// Replay injection is NOT driven from either of these hooks. Both
// were tried and both put an injected command ahead of the same tick's
// autonomous admissions, which inverts the recorded within-tick order. The
// drain lives at the tick-counter bump in `FUN_00767950` instead; see
// replayPlayer.h. These two hooks remain witness-only.

#pragma pack(push, 1)

// First simulation update in a job. `relativeTick` is (job tick - in-match epoch),
// the same relative clock the command log and the fingerprint samples use,
// because a fresh process does not resume the recorded absolute clock (7.3).
struct ReplayPrngEntry {
    uint32_t relativeTick;
    uint16_t startCursor;   // iterator 1 at tick entry, after any restore
    uint16_t simEndCursor;  // iterator 1 before presentation emitter updates
    // v2: the same pair for RandomIter #0 (Game+0x40074 table,
    // Game+0x40078 cursor). Recorded because `prngcur0` became a carrier at
    // the first divergence of a test match and because FireAction draws from it.
    //
    // Capture is unconditional; RESTORE is not, and must not be switched on
    // without evidence. Iterator 1 earns its restore by being SHARED with
    // presentation, so a human session and a spectator playback genuinely
    // cannot agree on it. If iterator 0 turns out to be simulation-only then a
    // `prngcur0` divergence means the simulation itself diverged, and forcing
    // the cursor would hide that rather than fix it. In MP the gap between
    // samples also contains unsampled simulation updates. Use prngsteps.bin
    // before attributing a gap to consumers outside the simulation body.
    uint16_t startCursor0;
    uint16_t simEndCursor0;
};

struct ReplayPrngFileHeader {
    char magic[8];          // "SH2RPRN"
    uint32_t version;       // 4 -- v1 had no iterator-0 columns; v3 appends one
                            // render-gate byte per entry after the entries
                            // (bit k = Sheep drawn flag at the job's k-th update)
    uint32_t entrySize;     // sizeof(ReplayPrngEntry)
    uint32_t entryCount;
    uint32_t epochTick;     // the recording process's in-match epoch
    uint32_t firstRelative;
    uint32_t lastRelative;
    uint32_t droppedTicks;  // ticks past capacity, never overwritten
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

struct PrngGuardStatus {
    uint32_t magic;         // "PRG1"
    uint16_t version;
    uint16_t installed;     // bit 0 entry hook, bit 1 sim-done hook
    uint32_t invocations;   // tick-body entries seen since the epoch
    uint32_t distinctTicks;
    uint32_t duplicateTicks;// entry ran twice for one relative tick
    uint32_t recorded;      // entries in the record buffer
    uint32_t droppedTicks;
    uint32_t badState;      // Game pointer or iterator table failed validation
    uint32_t streamEntries; // entries in a loaded stream
    uint32_t streamEpoch;
    uint32_t applying;      // 1 once a stream is loaded and armed
    uint32_t applied;       // ticks whose cursor was restored
    uint32_t missing;       // ticks inside the stream window with no entry
    uint32_t outsideWindow; // ticks before or after the recorded window
    uint32_t skipped;       // stream entries passed over (tick never reached)
    uint32_t preApplyMismatch; // ticks where the live cursor already differed
    int32_t worstDelta;     // signed live-minus-recorded, largest magnitude
    uint32_t lastRelative;
    uint32_t lastStart;
    uint32_t lastSimEnd;
    // Iterator 0. `iter0Applying` is 0 unless the restore was explicitly armed.
    // `iter0BetweenTickMoves` counts gaps where the cursor differed from the
    // previous tick's end -- the frame-driven-consumer test. `iter0Applied` and
    // `iter0PreMismatch` mirror the iterator-1 counters.
    uint32_t iter0Applying;
    uint32_t iter0Applied;
    uint32_t iter0PreMismatch;
    uint32_t iter0BetweenTickMoves;
    uint32_t iter0TickGaps;
    uint32_t lastStart0;
    uint32_t lastSimEnd0;
};

#pragma pack(pop)

static_assert(sizeof(ReplayPrngEntry) == 12, "prng entry wire format changed");
static_assert(sizeof(ReplayPrngFileHeader) == 36, "prng file header changed");
static_assert(sizeof(PrngGuardStatus) == 108, "prng guard status changed");

// Verifies the four replay sites and installs them all or nothing.
void installPrngGuard();

// The end-of-simulation iterator-1 cursor for the most recent completed tick.
// Returns false until the first tick has been observed; the fingerprint falls
// back to a live read in that case and reports it in its coverage word.
bool prngGuardSimEndCursor(uint16_t *cursorOut, uint32_t *relativeTickOut);

// 0 = disarm and clear the record buffer, 1 = arm recording (the recorder's
// own on/off also gates it), 2 = clear a loaded stream and stop applying,
// 3 = disarm the iterator-0 restore, 4 = arm it. Iterator-0 restore defaults
// OFF: see ReplayPrngEntry for why it needs evidence before being switched on.
uint32_t prngGuardControl(uint32_t mode);

size_t prngGuardStatus(void *out, size_t cap);

// Writes the recorded per-tick stream. Returns bytes written, or 0.
uint32_t prngGuardSave(const char *path);

// Read-only, every simulation body update, including MP updates sharing a job.
// Saved beside prng.bin as prngsteps.bin; never consumed by cursor restoration.
void prngStepsReset();

// Loads a stream and arms restoration. Returns the entry count, or 0.
uint32_t prngGuardLoad(const char *path);
// Same, from an in-memory prng.bin image (header + entries).
uint32_t prngGuardLoadBytes(const void *data, uint32_t bytes);

// Jobs the iterator-1 record holds (about 19 hours at the ~7.4 jobs/s of a
// live multiplayer match).
static const uint32_t REPLAY_PRNG_CAPACITY = 524288;

// The recorded stream so far (game thread). `dropped` nonzero means the
// stream is incomplete and must not be replayed.
void prngGuardRecordView(const ReplayPrngEntry **entries, uint32_t *count,
                         uint32_t *dropped);
void prngGuardRecordClocks(const ReplayMonasteryClock **entries, uint32_t *count);

// ---------------------------------------------------------------------------
// Per-phase attribution of the in-tick iterator-1 draws.
//
// Why. The guard restores the cursor at tick ENTRY and captures it again at
// SIM_DONE, and both hooks sit inside the same synchronous tick body, so no
// frame can interleave between them. `simEndCursor - startCursor` is therefore
// the number of draws made by code running INSIDE the tick, and a later
// offline survey of every v2 container found that number differs between a
// reference and its playback on 5..19% of ticks in combat/fire test matches while
// being exact on quiet ones. That falsifies the earlier census's ordering
// argument, which held that the particle system -- called after SIM_DONE -- is
// the only presentation consumer inside the tick body.
//
// So something else in the tick body draws, and its count is not reproducible.
// The tick body is only 28 instructions:
//
//   0x36aca0  entry            (prngGuard ENTRY hook)
//   0x36acbd  CALL [0x900594]  Dragonfly, serializes iterator 0
//   0x36acc3  MOV ECX,0xae5f40 ; CALL 0x768780     <- phase hook 0
//   0x36accd  MOV ECX,0xae5e74 ; CALL 0x767f40     <- phase hook 1
//   0x36acd7  MOV ECX,0xae8728 ; CALL 0x769be0     <- phase hook 2
//   0x36ace1  MOV ECX,0xae8800 ; CALL 0x76a140     <- phase hook 3
//   0x36aceb  MOV ECX,0x107937c; CALL 0x775090     <- phase hook 4  (ActorMgr)
//   0x36acf5  MOV ECX,0xae88c8 ; CALL 0x76a610     <- phase hook 5
//   0x36acff  CMP [fxflag],0   (prngGuard SIM_DONE hook)
//   0x36ad19  CALL 0x7bd930    particle system, AFTER the witness
//
// Sampling the cursor at each of those six `MOV ECX,imm32` boundaries splits
// the in-tick draw count across seven phases and names the subsystem that
// carries the residual. Each site is exactly five bytes and re-emits as a
// literal `mov ecx, <relocated imm>`, so the detour is byte-exact. The whole
// function contains one branch, `JZ 0x36ad1e`, whose target is outside all six
// overwritten ranges.
//
// Read-only: the hooks sample, they never write the cursor.

#pragma pack(push, 1)

struct ReplayPhaseEntry {
    uint32_t relativeTick;
    uint16_t startCursor;     // iterator 1 at tick entry, after any restore
    uint16_t phaseCursor[6];  // at each MOV ECX,imm32 boundary
    uint16_t simEndCursor;    // at SIM_DONE, before the particle system
};

struct ReplayPhaseFileHeader {
    char magic[8];          // "SH2RPHS"
    uint32_t version;       // 1
    uint32_t entrySize;     // sizeof(ReplayPhaseEntry)
    uint32_t entryCount;
    uint32_t epochTick;
    uint32_t firstRelative;
    uint32_t lastRelative;
    uint32_t droppedTicks;
};

#pragma pack(pop)

static_assert(sizeof(ReplayPhaseEntry) == 20, "phase entry wire format changed");
static_assert(sizeof(ReplayPhaseFileHeader) == 36, "phase file header changed");

// Installed by installPrngGuard(), all six sites or none. Never installed if
// the iterator-1 guard itself refused its own stock-byte check.
uint32_t prngPhaseInstalled();

// Render gates recorded beside the entries (one byte per entry, see
// prngGuard.cpp "Render gates"); written after the entries in prng stream v3
// files and sidecar PRNG sections.
const uint8_t *prngGuardRecordGates();
void prngGuardGateStats(uint32_t *applied, uint32_t *overflow, bool *streamHasGates);
