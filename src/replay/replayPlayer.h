#pragma once

#include <cstddef>
#include <cstdint>

// Playback in a fresh process.
//
// Playback injects local UI, remote peer and prior codec input. Remote input
// also includes host-generated autonomous commands. Everything the LOCAL
// simulation is supposed to derive for itself -- AI decisions, the periodic
// economy and policy passes, the stochastic systems -- is deliberately NOT
// injected, because re-deriving it identically is the property under test. The
// recorded autonomous commands are kept as witnesses and compared afterwards
// against what the replaying process actually admitted.
//
// The recorded clock is absolute and a fresh process does not resume it,
// so every scheduling decision is made against the in-match epoch each process
// samples for itself: an event recorded at tick T fires at
// playbackEpoch + (T - recordEpoch).
//
// Injection is driven by the SIMULATION tick body, not the frame tick.
// Draining the schedule from the frame callback quantises
// every injection to the frame, and the frame is not the tick. The main loop
// (FUN_00421e00) runs a do/while over `ticksThisFrame`, so under load two or
// more simulation ticks pass between frames; an event whose target fell on the
// first of them was admitted at the last one instead. 6,886 of 6,955 aligned
// pairs were exact and 60 were +1 -- and one of those 60 was a
// `CreateTroopChore` inside a recruitment burst, which spawned its unit a tick
// late, perturbed actor order, and diverged the whole back third of the match.
// The recorder stamps the true tick, so only playback aliased.
//
// The tick body is the right site by construction. One iteration of the main
// loop is: FUN_00767950 reads NetGameMgr+0x06 as tick X, executes the commands
// queued for X, then bumps the counter to X+1; the simulation body FUN_0076aca0
// then runs and reads X+1. A human clicking during the following frame is
// stamped X+1 and is executed by the next iteration's FUN_00767950. So
// admitting at the entry of the tick body that reads X puts the command in the
// queue the next FUN_00767950 drains as tick X -- exactly where the human's
// admission landed. `target == now` becomes true by construction and lateness
// becomes a real defect signal rather than ambient noise.
//
// Within-tick ORDER is part of clause 1, and it is what decides the exact site.
// Measured across three independent human matches (4, 6 and 7), all 80 ticks
// carrying both a human and an autonomous command put every human command AFTER
// every autonomous one -- none ordered the other way, none interleaved.
//
// Two sites inside the simulation tick body were tried and both inverted that.
// Injecting at tick ENTRY, and then at SIM_DONE after the six subsystem
// updates, each reproduced all 220 of a test match's events with identical payloads
// on identical ticks and still diverged at relative 2048, where the injected
// CreateSiegeEquipmentChore took order 0 ahead of two AI PlaceBuildingChores
// that were orders 0 and 1 in the reference. So the AI's admissions for tick X
// are issued LATER in X's window than any point in the body that reads X.
//
// The drain therefore sits at the last instruction of that window: the tick
// counter bump `INC word ptr [ESI+0x6]` at the end of `FUN_00767950`
// (RVA 0x367da5). At that point the counter still reads X, every autonomous
// admission for X has been made, and X's command queue has already been
// drained -- so an injected command is stamped X, ordered after the autonomous
// ones, and executed by the next `FUN_00767950` as tick X+1, which is exactly
// what a human admission recorded late in window X did.
//
// The site is one instruction wide in effect but eight bytes are taken (the
// INC plus the following `CMP byte ptr [ESI+0xa], 0`), because a 4-byte site
// cannot hold a 5-byte jump. Verified: the only branch into the range targets
// its first byte, so no jump lands inside the patch.

#pragma pack(push, 1)

struct ReplayPlayerStatus {
    uint32_t magic;   // "RPS1"
    uint16_t version;
    uint16_t state;   // 0 idle, 1 loaded, 2 waiting for epoch, 3 playing,
                      // 4 done, 5 issuing pre-epoch lobby commands
    uint32_t loadedEvents;
    uint32_t loadedBytes;
    uint32_t recordEpoch;
    uint32_t playbackEpoch;
    uint32_t currentTick;
    uint32_t cursorEvent;      // events consumed from the schedule
    uint32_t injected;         // commands admitted through the stock codec
    uint32_t injectFailures;   // commands the codec refused to rebuild
    uint32_t skippedAutonomous;// witnesses left for the simulation to re-derive
    uint32_t refusedUnclassified; // origin-0 native events; a hard stop
    uint32_t lateEvents;       // fired at least one tick after their target
    uint32_t maxLateness;      // worst lateness in ticks
    uint32_t firstTargetTick;
    uint32_t lastTargetTick;
    // Origin-0 events a newer caller-route classifier rescued at load time.
    // These are containers recorded before the route was known; the count is
    // reported so a rescue never looks like a clean recording.
    uint32_t reclassified;
    // Spectator contract. `localCommands` counts UI-origin commands admitted
    // while playback was armed: any nonzero value means live local input
    // reached the simulation and the session was not a spectator session.
    uint32_t localCommands;
    uint32_t blockedMessages;  // input messages the guard swallowed
    uint32_t guardHooked;      // 1 once the window subclass is in place
    // The direct-device half of prevention. The game polls GetAsyncKeyState in
    // four places, one of them a hotkey dispatcher that posts Chores, and no
    // window subclass can see that. `blockedPolls` counts presses suppressed at
    // the import thunk; `pollGuardHooked` is 0 until that thunk is redirected,
    // in which case the direct-device path was never guarded at all.
    uint32_t blockedPolls;
    uint32_t pollGuardHooked;
    // Times the schedule was drained from the simulation tick body. Zero while
    // playing means the tick-entry hook is not installed and the schedule
    // is being driven by nothing at all, which must fail a recording rather
    // than look like a match with no human commands.
    uint32_t simTickPumps;
    // Cold-start (lobby) accounting. `preEpochInjected` counts the lobby setup
    // commands issued from the main menu -- StartGameChore and
    // UpdateGameDescriptionChore -- which is how a container with no checkpoint
    // starts its match at all. `preEpochSkipped` counts pre-epoch admissions
    // left for the simulation to re-derive (the AI clan creation and opening
    // builds). All three stay 0 for a loaded-save container.
    uint32_t preEpochInjected;
    uint32_t preEpochSkipped;
    uint32_t preEpochFailures;
    // Frames on which the shared startup guard found the simulation paused and
    // un-paused it. The value is one-shot; after that stock `p`/ESC pause
    // behavior is preserved.
    uint32_t pauseClears;
};

#pragma pack(pop)

static_assert(sizeof(ReplayPlayerStatus) == 108, "replay player status changed");

// Registers the per-frame half of the scheduler: the epoch transition and the
// spectator-guard arming. Command injection is not driven from here.
void installReplayPlayer();

// Drains every scheduled event whose target tick has arrived. Called from the
// tick-counter bump hook, once per simulation tick, while the counter still
// reads the tick being closed.
void replayPlayerSimulationTick();

// Reads a container written by replayRecorderSave into the playback arena and
// validates its header. Returns the event count, or 0 on any refusal.
uint32_t replayPlayerLoad(const char *path);
// Same, from an in-memory commands.bin image. Returns the event count (1 for a
// valid empty log), 0 on refusal.
uint32_t replayPlayerLoadBytes(const void *data, uint32_t bytes);

// 0 = stop, 1 = start, 2 = experimental MP job cadence (recorded byte phase).
// Mode 2 requires an offline main-menu session and the verified cadence hook,
// unless `fromLoad` (armed from inside the stock LoadGame by the replay
// session). Starting only arms the scheduler; the first in-match frame supplies
// the playback epoch.
uint32_t replayPlayerControl(uint32_t mode, uint32_t multiplayerPhase = 0,
                             bool fromLoad = false);

// Playing (or finished) with an epoch: the current job relative to it.
bool replayPlayerPlaybackJob(uint32_t *relativeJob);
// Armed at all: waiting for the epoch, playing, or finished but not stopped.
bool replayPlayerArmed();

size_t replayPlayerStatus(void *out, size_t cap);

// Occupancy-list order restore (granted-troop starts). FUN_007875f0 slows an
// actor by 0.8 per actor ahead of it in its head-inserted tile list, and a
// checkpoint load re-links lists in restore order, not live order. Loads a
// `tileorder.bin` holding the live order; applied
// once at the playback epoch. Returns the list count (0 is a valid empty file),
// TILE_ORDER_REFUSED on refusal. Loading with an empty path clears it.
static const uint32_t TILE_ORDER_REFUSED = 0xffffffffu;
uint32_t replayPlayerLoadTileOrder(const char *path);
uint32_t replayPlayerLoadTileOrderBytes(const void *data, uint32_t bytes);
// Lists re-linked / skipped (membership or termination mismatch) at the epoch.
void replayPlayerTileOrderResult(uint32_t *applied, uint32_t *skipped);

// In-flight projectile damage (sidecar FLYS section, replayCommon.h), written
// back once at the playback epoch. Returns the entry count or FLY_STATE_REFUSED
// (bad header, or a class/field outside replayFlyFieldKnown).
static const uint32_t FLY_STATE_REFUSED = 0xffffffffu;
uint32_t replayPlayerLoadFlyStateBytes(const void *data, uint32_t bytes);
void replayPlayerClearFlyState();
// Entries loaded / written / skipped (projectile not back in its recorded slot).
void replayPlayerFlyStateResult(uint32_t *entries, uint32_t *applied, uint32_t *skipped);

// RandomIter #0 cursor at the checkpoint (sidecar flag 0x40), written back once
// at the playback epoch. LoadGame draws it where the live save did not (Hovel
// vt+0xc, see replayPlayer.cpp). 0xffffffff = nothing recorded, leave it.
void replayPlayerSetIter0AtSave(uint32_t cursor);
// Cursor the load left / cursor written (0xffffffff each until applied).
void replayPlayerIter0Result(uint32_t *loaded, uint32_t *written);

// Intra-drain order plans (execorder.bin). Same load /
// refusal contract as the tile order; applied at the pump before each planned
// drain.
static const uint32_t EXEC_ORDER_REFUSED = 0xffffffffu;
uint32_t replayPlayerLoadExecOrder(const char *path);
uint32_t replayPlayerLoadExecOrderBytes(const void *data, uint32_t bytes);
void replayPlayerExecOrderResult(uint32_t *applied, uint32_t *skipped);
