#pragma once

void installShiftRecruit();

// Live value, for the settings overlay: how many units one Shift-click queues.
// 1 means the feature is off (the stock single recruit). Raising it above 1
// installs the patch if it is not already in — the write happens on the game
// thread at the next frame. Lowering it back to 1 leaves the hooks in place and
// simply queues one unit, so the switch is safe to flip at any time.
// shiftRecruitFailed() is true only when an install was rejected because the
// game's bytes did not match, which a restart would not fix.
int shiftRecruitMultiplier();
void shiftRecruitSetMultiplier(int multiplier);
bool shiftRecruitFailed();
