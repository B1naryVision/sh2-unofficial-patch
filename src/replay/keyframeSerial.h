#pragma once

#include <cstdint>

// A keyframe save that leaves the world exactly as it found it.
//
// The stock save changes two kinds of live state as it writes them, which a
// playback cannot afford mid-match (2026-11-15 diff, docs/features/replay-seeking.md):
//
// - GlowWorm::Actor::serialize (VA 0x789600) writes an actor's path progress
//   (+0xdc, a float) as a whole number and stores that whole number back.
// - Every handle serializer (188 inlined copies, calling one of five
//   resolvers: actor 0x769360, building 0x768550, clan 0x774e20, projectile
//   0x76a000, element 0x767e40) refreshes the handle's cached pointer and,
//   when the handle no longer resolves, zeroes its id. The first version hooked
//   only the first two; clans were found by grouping every serializer-shaped
//   call site in the exe by its callee.
//
// In keyframe mode the path progress is written and read as its raw float
// bits, so keyframe files are not stock saves and only a keyframe load may read
// them. A handle that no longer resolves keeps its id in the file, as the live
// world holds it (the stock save writes 0), and every handle the save touched
// gets its cached pointer and id back afterwards. The loaded world therefore
// matches the live one. A load leaves every handle's cached pointer null; code
// must resolve the id rather than trust the cache (the ballista cave did not,
// see docs/features/ballista-auto-fire.md). Other saves are untouched.

void installKeyframeSerial();
bool keyframeSerialInstalled();

// Game thread, straight around one replayCheckpointSave of a keyframe. Begin
// fails when the hooks are not installed or the handle log cannot be
// allocated; the save must then not be taken. End puts every logged handle
// back and returns false when the log overflowed: some live handles then keep
// the stock save's changes, and the keyframe must be discarded. `handles`
// receives the number logged.
// `rawPath` receives how the file holds path progress, which its load needs.
bool keyframeSaveBegin(bool *rawPath);
bool keyframeSaveEnd(uint32_t *handles);

// Game thread, straight around the LoadGame of a keyframe file, with the
// `rawPath` its save reported.
void keyframeLoadBegin(bool rawPath);
void keyframeLoadEnd();

// How later keyframe saves hold path progress: raw float bits (default), or
// stock whole numbers with the live value put back after the save. The second
// exists to isolate a crash on restore (2026-11-15); the probe's Ctrl+F10
// switches it.
void keyframeSerialSetRawPath(bool raw);
bool keyframeSerialRawPath();
