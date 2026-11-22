#pragma once

#include <cstdint>

// Seeking during a replay, both ways. Forward: run the simulation as fast as
// the machine allows until a target job, then hand back to normal playback.
// Back: at the next quiet job, reload the newest keyframe at or before the
// target (replayKeyframes.h), or the replay's own save when there is none,
// start the playback there at once, and seek forward to the target. A forward
// seek loads a keyframe too when one lies well ahead. See replaySeeker.cpp for
// the two rules every load follows.
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

// How many seeks were redone because the keyframe they resumed from had left
// the recording (for the seek probe).
uint32_t replaySeekerRepairs();

// Asks for a seek to `targetJob` (relative to the playback epoch). Any thread;
// the frame tick starts it. Clamped to the end of the recording. A target
// behind the current job rewinds; one on it ends any running seek.
void replaySeekerRequest(uint32_t targetJob);

// Stops a running seek where it is. Any thread.
void replaySeekerCancel();

// True while a seek runs (also while it waits behind the in-game menu, and
// while a rewind waits for its reload), with its target job.
bool replaySeekerActive(uint32_t *targetJob);

// The player's pump, once per job of a playback. Mid-function: float-free; its
// only call is QueryPerformanceCounter, during a seek. At the target it ends the
// frame's update loop.
void replaySeekerOnJob(uint32_t relativeJob);
