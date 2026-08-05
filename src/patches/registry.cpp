#include "registry.h"
#include "../core/config.h"
#include "attackHotkey.h"
#include "autoMarket/autoMarket.h"
#include "barracksCrash.h"
#include "endgameStats.h"
#include "introSkip.h"
#include "knightCatapultCrash.h"
#include "mapEdgeCrash.h"
#include "mpAiEnable.h"
#include "mpConnectCompleteCrash.h"
#include "settingsOverlay.h"
#include "shiftRecruit.h"
#include "siegeCampHotkey.h"
#include "stopTroopsHotkey.h"
#include "zoomSpeed.h"

void applyUnofficialPatches() {
    loadConfig();

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
    installShiftRecruit();
    installAutoMarket();
    installSiegeCampHotkey();

    // Last: it reads every other feature's loaded binding, and its window
    // subclass must sit outside the auto-market editor's so it sees input first.
    installSettingsOverlay();
}
