#pragma once

#include <cstdint>

// Replay keyframes (seeking phase 3): saves taken while a replay plays, so a
// seek can resume from the nearest one instead of from the start of the match.
//
// A keyframe is what the replay's own checkpoint is, taken later: the game's
// SaveGame, plus the state the save does not carry (tile order, projectile
// damage, Monastery clocks, the iterator-0 cursor; the same list the recorder
// captures), plus where the playback stands (player and random-stream
// positions, match rules) and the panel's tallies. One is taken in each minute
// of match time the playback passes through, at a frame with nothing queued,
// whether watching or skipping. Files live in a folder of this process under
// %TEMP%, kept while the same replay plays (a restart keeps them) and deleted
// when it ends or another save loads.

// Startup: deletes keyframe folders left by processes that are no longer running.
void installReplayKeyframes();

// The seeker's frame tick, while no load is pending. Takes a keyframe when the
// playback is in a minute that has none and the job is quiet. True when it did
// (that frame then held a save of about 200 ms).
bool replayKeyframesFrame();

// The newest keyframe at or before `job` that is either verified or at least
// `margin` jobs before it, so the skip from it to `job` is long enough for the
// check against the recording to catch a restore that left it.
bool replayKeyframeAtOrBefore(uint32_t job, uint32_t margin, uint32_t *keyframeJob);

// A run resumed from the keyframe at `job` has matched the recording long
// enough: it needs no margin from now on.
void replayKeyframesMarkVerified(uint32_t job);

// Resumes the playback from the keyframe at `keyframeJob`. Frame tick, at a
// quiet job, with no popup up. False when it could not; `*loaded` then says
// whether the world was replaced anyway, in which case the caller must restart
// the replay from its own save.
bool replayKeyframeRestore(uint32_t keyframeJob, bool *loaded);

// The session, before a save loads (not a keyframe): keyframes of any other
// replay, or of none, are deleted.
void replayKeyframesOnLoad(const wchar_t *path);

// Deletes every keyframe and its file.
void replayKeyframesForget();

// A load replaced the world: keyframes taken from now on belong to a new run.
void replayKeyframesBeginRun();

// Deletes the keyframe at `job`; with `thisRunFromJob`, instead every keyframe
// the current run took at or after `job` (taken from a world that had already
// left the recording).
void replayKeyframesDiscard(uint32_t job, bool thisRunFromJob);

// Automatic capture, on by default. A stock save changes live state as it
// writes it (2026-11-15 playtest), so keyframes are taken in a mode that
// changes nothing (keyframeSerial.h). A restore is not always exact (the
// pathfinder's region map is rebuilt by the load); the seeker checks every job
// of a run from a keyframe against the recording and redoes the seek when one
// differs.
void replayKeyframesSetEnabled(bool enabled);
bool replayKeyframesEnabled();

#ifdef DEBUG
// For the seek probe's round trip: a keyframe at the current job, now, when the
// job is quiet. `*keyframeJob` receives the job.
bool replayKeyframesCaptureNow(uint32_t *keyframeJob);

// For the seek probe: actor list chains the last capture kept and left out
// (a member that is not an actor), and the last restore rebuilt and skipped.
// Movement clusters the last capture kept, and the last restore rebuilt and
// skipped (members not back as recorded).
void replayKeyframesDebugClusters(uint32_t *saved, uint32_t *restored, uint32_t *skipped);

void replayKeyframesDebugChains(
    uint32_t *saved, uint32_t *foreign, uint32_t *restored, uint32_t *skipped
);

// For the seek probe: how many captures so far, and the last one's job, save
// time, file size and the number of handle caches it put back.
void replayKeyframesDebugLast(
    uint32_t *captures, uint32_t *job, uint32_t *saveUs, uint32_t *bytes, uint32_t *handles
);
#endif
