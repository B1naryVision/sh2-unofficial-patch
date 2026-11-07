#pragma once

#include <cstdint>

// Forward seek during a replay: runs the simulation as fast as the machine
// allows until a target job, then hands back to normal playback.
//
// No simulation code changes. The seek replaces the pacer's update count for
// each frame (replayViewerSetUpdateOverride), so a frame runs a hundred or more
// updates instead of a handful, and the per-job pump stops the frame's update
// loop at the target job exactly. The random cursor is already restored per
// update (prngGuard), so where frames fall cannot reach the simulation; a run
// skipped this way matched an unskipped one at every one of 18,083 jobs (see
// docs/features/replay-seeking.md). Sound effects and speech are muted and
// particles switched off for the length of a seek; music plays on.

void installReplaySeeker();
bool replaySeekerInstalled();

// Asks for a seek to `targetJob` (relative to the playback epoch). Any thread;
// the frame tick starts it. Clamped to the end of the recording. A target at or
// behind the current job is ignored: only forward seeks exist so far.
void replaySeekerRequest(uint32_t targetJob);

// Stops a running seek where it is. Any thread.
void replaySeekerCancel();

// True while a seek runs (also while it waits behind the in-game menu), with
// its target job.
bool replaySeekerActive(uint32_t *targetJob);

// The player's pump, once per job of a playback. Mid-function: float-free; its
// only call is QueryPerformanceCounter, during a seek. At the target it ends the
// frame's update loop.
void replaySeekerOnJob(uint32_t relativeJob);
