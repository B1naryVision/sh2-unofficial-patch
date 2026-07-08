#include "registry.h"
#include "attackHotkey.h"
#include "barracksCrash.h"
#include "endgameStats.h"
#include "introSkip.h"
#include "knightCatapultCrash.h"
#include "mapEdgeCrash.h"
#include "mpAiEnable.h"
#include "mpConnectCompleteCrash.h"
#include "stopTroopsHotkey.h"
#include "zoomSpeed.h"

void applyUnofficialPatches() {
    installKnightCatapultCrashFix();
    installMpAiEnable();
    installIntroSkip();
    installMpConnectCompleteCrashFix();
    installEndgameStats();
    installBarracksCrashFix();
    installMapEdgeCrashFix();
    installStopTroopsHotkey();
    installAttackHotkey();
    installZoomSpeed();
}
