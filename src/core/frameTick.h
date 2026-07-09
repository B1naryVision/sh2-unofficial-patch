#pragma once

// Shared per-frame dispatcher on the game/sim thread.
//
// Owns the trampoline at the main-loop body site (RVA 0x300c0, the same site
// the stop-troops hotkey originally used — see docs/features/stop-troops-hotkey.md
// for how it was found). Patches that need to run code once per frame on the
// sim thread register a callback here instead of installing their own hook,
// so one proven site serves any number of consumers.
//
// The hook is installed lazily on the first registration. Callbacks run in
// registration order, with all general-purpose registers and flags preserved
// around the whole batch.
//
// Callbacks must not use float/double arithmetic: the site is mid-function,
// so the game may have live x87 register state that a callback's x87 usage
// would clobber. Read floats as raw bits (uint32_t) and convert later, on a
// safe path (e.g. at a function-prologue hook).

typedef void (*FrameTickFn)();

void registerFrameTick(FrameTickFn fn);
