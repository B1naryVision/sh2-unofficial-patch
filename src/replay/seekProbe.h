#pragma once

// Replay seeking, phase 0 measurements. Debug builds only (make debug); a
// no-op otherwise. During a playback:
//   F7        toggle the iterator-0 write watch (every draw, with its site)
//   F8        turbo on / off (Shift+F8: on without particles)
//   F9        save a keyframe probe (SaveGame mid-playback, timed)
//   Shift+F9  load the last keyframe probe (ends the playback)
//   F11       restart the replay at the next quiet job (Shift+F11: at once)
// Every job of every playback is fingerprinted. Results go to seek_probe.txt
// next to the exe. See docs/features/replay-seeking.md.
void installSeekProbe();
