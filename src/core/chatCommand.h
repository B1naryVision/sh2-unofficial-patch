#pragma once
#include <cstdint>

// Shared owner of the in-game chat send (IngameChatPanel::sendTypedMessage, RVA
// 0x24d8e0). Typed `!commands` are matched here, swallowed so they never reach
// the wire, and handed to the feature that registered them. See
// docs/features/ping-command.md.
//
// In-game chat is a lockstep command (ChatChore), so none of this exists in the
// lobby.

// Called from the chat send's prologue on the game thread.
typedef void (*ChatCommandFn)();

// `command` is matched case-insensitively against the whole typed line, ignoring
// trailing spaces. Register at install time; the hook is written on the first
// registration.
void registerChatCommand(const wchar_t *command, ChatCommandFn fn);

// Called when a chat line *executes*: in multiplayer that is the lockstep tick,
// the same on every client, so an observer may change simulation state from
// here. `senderPeer` is the sending player's network peer id (GameSpy::Network
// +0xdc on their machine). The text is another player's input and is untrusted.
typedef void (*ChatObserverFn)(uint32_t senderPeer, const wchar_t *text, int length);

// Register at install time; the vtable slot is swapped on the first registration.
void registerChatObserver(ChatObserverFn fn);

// Posts `text` as an ordinary chat message from the local player, through the
// stock send. Call from the frame tick only: it brackets the engine calls with
// fxsave/fninit. Lockstep drops chat outside a running match, so a line posted
// in the lobby goes nowhere. Returns false if the chat panel cannot be found.
bool chatPost(const wchar_t *text, int length);
