#pragma once

#include <cstddef>
#include <cstdint>

// Playback of a recorded command log in a fresh process.
//
// Playback injects local UI, remote peer and prior codec input. Remote input
// also includes host-generated autonomous commands. Everything the LOCAL
// simulation derives for itself -- AI decisions, the periodic economy and
// policy passes, the stochastic systems -- is deliberately NOT injected,
// because re-deriving it identically is what makes the replay the match.
//
// The recorded clock is absolute and a fresh process does not resume it, so
// scheduling is relative: an event recorded at tick T fires at
// playbackEpoch + (T - recordEpoch). The playback epoch is the first in-match
// frame after the load, sampled here.
//
// Injection site. Injection is driven by the simulation, not the frame: the
// main loop (FUN_00421e00) runs a do/while over `ticksThisFrame`, so under load
// several simulation updates pass between frames, and a frame-driven pump
// admitted events a tick late. The pump hooks the `call FUN_007676c0` inside
// NetGameMgr's job function FUN_00767950 (RVA 0x367a03, stock `e8 b8 fc ff ff`)
// and runs the instant that callee returns. That call is early in the job,
// after the job's autonomous admissions and before the drain at
// 0x767b29/0x767c6b, so an injected command is ordered after the autonomous
// ones (as every human command was in the recorded matches) and still lands in
// the queue that drain consumes. The tick-counter bump at the end of
// FUN_00767950 (RVA 0x367da5) got the order right too but is after the drain,
// so every injected command executed one job late. The retimer schedules each
// injected command at its live execution job - 1 to match this site.
//
// Three more sites, all verified with the pump before any is written: the MP
// job cadence (0x3679cc), the load's MP rules byte (0x2aec5) and the frame
// pacer's read of it (0x20e4e). See replayPlayer.cpp.

void installReplayPlayer();
bool replayPlayerInstalled();

// Validates a CMDS section (replayFormat.h) and copies it into the playback
// buffer, allocated here. Returns false on any refusal. Unload stops playback
// and frees it.
bool replayPlayerLoadBytes(const void *data, uint32_t bytes);
void replayPlayerUnload();

// 0 = stop, 1 = start, 2 = start with MP job cadence (recorded byte phase).
// Called from inside the stock LoadGame by the replay session, which is the
// checkpoint boundary by construction. Starting only arms the scheduler; the
// first in-match frame supplies the playback epoch. Returns the new state
// (2 = waiting for the epoch on success).
uint32_t replayPlayerControl(uint32_t mode, uint32_t multiplayerPhase = 0);

// The first in-match tick of the current playback, 0 until it is sampled.
uint32_t replayPlayerPlaybackEpoch();
// Playing (or finished) with an epoch: the current job relative to it.
bool replayPlayerPlaybackJob(uint32_t *relativeJob);
// Armed at all: waiting for the epoch, playing, or finished but not stopped.
bool replayPlayerArmed();

// Occupancy-list order restore. FUN_007875f0 slows an actor by 0.8 per actor
// ahead of it in its head-inserted tile list, and a checkpoint load re-links
// lists in restore order, not live order. Applied once at the playback epoch.
// Returns the list count, or false on refusal; Clear drops a loaded table.
bool replayPlayerLoadTileOrderBytes(const void *data, uint32_t bytes);
void replayPlayerClearTileOrder();

// In-flight projectile damage (sidecar FLYS section), written back once at the
// playback epoch.
bool replayPlayerLoadFlyStateBytes(const void *data, uint32_t bytes);
void replayPlayerClearFlyState();

// RandomIter #0 cursor at the checkpoint, written back once at the playback
// epoch. LoadGame draws it where the live save did not (Hovel vt+0xc, see
// replayPlayer.cpp). 0xffffffff = nothing recorded, leave it.
void replayPlayerSetIter0AtSave(uint32_t cursor);

// Intra-drain order plans (sidecar EXOR section), applied at the pump before
// each planned drain. The buffer is allocated here; Clear frees it.
bool replayPlayerLoadExecOrderBytes(const void *data, uint32_t bytes);
void replayPlayerClearExecOrder();
