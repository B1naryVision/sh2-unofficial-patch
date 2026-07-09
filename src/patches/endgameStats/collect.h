#pragma once
#include "snapshot.h"

// Assembles the endgame snapshot from the live player table, the session
// cache, and the unit tracker. Called from the Win/LoseScreen::OnActivate
// hooks (function-prologue sites, so float arithmetic is safe here).
const EndgameSnapshot &collectEndgameStats(bool won);
