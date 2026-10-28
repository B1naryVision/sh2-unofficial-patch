#pragma once

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

// Posts `text` as an ordinary chat message from the local player, through the
// stock send. Call from the frame tick only: it brackets the engine calls with
// fxsave/fninit. Returns false until a chat panel has been seen (the player has
// sent at least one line this process).
bool chatPost(const wchar_t *text, int length);
