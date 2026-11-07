#pragma once

#include <cstddef>

// The teams part of a replay's file name, `Alice+Bob vs Carol+Dave`. Pure, so
// tests/replayTests.cpp runs it on the host; replaySession.cpp reads the
// players out of the game.

struct ReplayTitlePlayer {
    const wchar_t *name; // in-game name; may be empty
    const wchar_t *fallback; // colour name, used when the name is empty or shared
    int team; // players with the same value are one team
};

// Writes the title into `out` (room for `budget` characters plus the NUL):
// teams in order of their first player, players in the order given. Every
// character Windows refuses in a file name becomes `_`. Names are cut to ten
// characters, and shorter each time until the title fits. Returns its length, or 0 with
// `out` empty when nothing fits or there are no players.
size_t replayFormatTitle(const ReplayTitlePlayer *players, int count, wchar_t *out, size_t budget);
