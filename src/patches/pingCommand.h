#pragma once

// Typing "!ping" in multiplayer chat measures the round trip to every peer and
// broadcasts the result as a normal chat message. See docs/features/ping-command.md.
void installPingCommand();
