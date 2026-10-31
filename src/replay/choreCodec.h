#pragma once

#include <cstddef>
#include <cstdint>

// Reuse of the game's own command codec.
//
// Every concrete Chore is a Dragonfly::Classified at object +0x00 and a
// Dragonfly::Serializable at object +0x04. The multiplayer send path encodes a
// command as one class-index byte followed by whatever the class's own
// `serial(Stream&)` writes, and the receive path rebuilds it with
// Class::createSharedInstance plus the same `serial` against an input stream.
//
// That is a complete, engine-authored, pointer-free command codec, so the
// recorder reuses it instead of hand-mapping 79 object layouts.
//
// Everything here must run on the simulation thread (the frame tick),
// because it calls stock game code and touches game allocations.

// Serialize one live Chore exactly as the multiplayer send path would.
// Returns the payload byte count, or 0 on failure. `classIndexOut` receives the
// wire class index, or 0xffffffff when the class is not registered — an
// unregistered class must fail loudly rather than record an unusable command.
size_t choreCodecSerialize(void *chore, void *out, size_t cap, uint32_t *classIndexOut);

// Number of registered command classes, or 0 when the codec is unavailable.
uint32_t choreCodecClassCount();

// Whether playback may inject `classIndex`: a registered `*Chore` that is not
// lobby or session machinery (StartGameChore, UpdateGameDescriptionChore,
// GameReadyChore, EndJobChore, the peer and invite classes). Matched by name,
// so the rule survives a class-table change. A sidecar is shared input, and
// nothing a player records produces those classes.
bool choreCodecIsInjectable(uint32_t classIndex);

// Rebuild a command from a recorded stock payload and admit it through the
// exact World entry the UI uses. This is the playback primitive: it repeats the
// stock multiplayer receive sequence -- Class::createSharedInstance, the class's
// own serial against a BufferInputStream, then World dispatch -- so nothing
// about the command is reconstructed by us. Returns false and admits nothing if
// the class index is out of range or the object cannot be built.
// `admittedOut` receives the admitted object, for execution-order correlation.
bool choreCodecAdmit(
    uint32_t classIndex, const void *payload, size_t length, void **admittedOut = nullptr
);
