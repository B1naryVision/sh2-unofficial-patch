#pragma once

#include <cstdint>

// Helpers shared by the replay modules.

// World+4 (RVA 0xd781d4): the engine's own "a game is running" flag.
bool replayInGame();

// Dragonfly::Screen::isPopupOn() through the exe's IAT (the in-game ESC menu is
// a popup). Game thread only; false when the imports cannot be resolved.
bool replayScreenPopupOn();

// Occupancy-list order at the checkpoint. A checkpoint load re-links each
// actor's tile (+0x74) and region (+0x80) list in restore order, not live
// order, and FUN_007875f0 slows an actor by 0.8 per actor ahead of it, so
// stacked starting troops would move at permuted speeds on playback. Captured
// in the same frame as the checkpoint save; the player re-links at its epoch.
// Words: per list {kind 0 tile / 1 region, length, actor slots head-first}.
void replayCaptureTileOrder();
// False when nothing was captured. `wordCount` excludes the 3-word file header.
bool replayTileOrderView(const uint32_t **words, uint32_t *wordCount, uint32_t *lists);
// Writes `tileorder.bin` ({'TORD', 1, lists} + words); bytes written or 0.
uint32_t replayTileOrderSave(const char *path);

// Per-shot projectile damage at the checkpoint. The shooter stores each
// Arrow's / Axe's / Spear's damage in the projectile, but their serializers
// (RVA 0xcf2c0 / 0xd4cb0 / 0xd5d60) skip that field, so a checkpoint load gives
// every missile in flight its constructor default (Arrow 750.0) and the first
// hit lands with the wrong damage (a hunter's 3000 arrow into a Bear hit for
// 750). Captured in the same frame as the checkpoint save; the
// player writes the values back at its epoch, matching each projectile by its
// FlyMgr (RVA 0x6e8800) slot, handle serial (+0x7a) and class.
struct ReplayFlyField {
    uint16_t slot;      // index in the FlyMgr slot vector = handle low word (+0x78)
    uint16_t serial;    // handle high word (+0x7a)
    uint32_t vtableRva; // the projectile's class
    uint32_t offset;    // the unserialized field
    uint32_t value;     // its live bits
};
static const uint32_t REPLAY_FLY_MAGIC = 0x53594C46u; // "FLYS", {magic, 1, count} + entries
static const uint32_t REPLAY_FLY_MAX = 4096;
// True for exactly the (class, field) pairs above; the player refuses others.
bool replayFlyFieldKnown(uint32_t vtableRva, uint32_t offset);
void replayCaptureFlyState();
// False when nothing was captured (count 0 is a valid capture).
bool replayFlyStateView(const ReplayFlyField **entries, uint32_t *count);
// Game thread; the FlyMgr slot vector {begin, count}, false if unreadable.
bool replayFlySlots(const uintptr_t **slots, uint32_t *count);

// Commands in the NetGameMgr lockstep job queue (RVA 0x17e6a30: ring of
// 0x18-byte job entries {vector begin, end, ..} at +0x10..+0x14, cursor +0x18,
// count +0x20), excluding EndJobChore markers. These are admitted but not yet
// executed, so a checkpoint saved now holds none of them while a log restarted
// now never sees their admission. 0xffffffff if the queue is unreadable.
uint32_t replayNetQueuedCommands();

// Deferred placements in the BuildingPlacer retry queue (placer VA 0x11b8c10,
// 16-byte records at +0x60..+0x64, SH2_PlaceBuilding_retryQueue RVA 0x714f0).
// Not serialized: a checkpoint saved while one is pending loses it. At match
// start the estates' stockpiles wait here for their estate centre for about
// two updates. 0xffffffff if unreadable.
uint32_t replayPendingPlacements();

// The game's own SaveGame(path) (RVA 0x2a2e0), called exactly as the ESC-menu
// save handler calls it. `suppressState` 0 = stock (single player); 2 = NOP
// the whole 21-byte SetState(6) announcement at RVA 0x2a37d for the duration,
// which a live networked session survives (as the mpSaveRecovery save does); 1 = only the
// push + call. The site is opcode-verified and restored immediately after.
// Game thread only.
bool replayCheckpointSave(const wchar_t *path, int suppressState);
