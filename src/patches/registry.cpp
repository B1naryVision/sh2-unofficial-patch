#include "registry.h"
#include "../core/config.h"
#include "../replay/prngGuard.h"
#include "../replay/replayPlayer.h"
#include "../replay/replaySession.h"
#include "../replay/replayTrace.h"
#include "../replay/replayViewer.h"
#include "../replay/replayVision.h"
#include "../replay/spectatorGuard.h"
#include "attackHotkey.h"
#include "autoMarket/autoMarket.h"
#include "barracksCrash.h"
#include "endgameStats.h"
#include "handshake.h"
#include "introSkip.h"
#include "knightCatapultCrash.h"
#include "lobbyInProgressFilter.h"
#include "lobbyPanel.h"
#include "mapEdgeCrash.h"
#include "matchRules.h"
#include "mpAiEnable.h"
#include "mpConnectCompleteCrash.h"
#include "orderTrace.h"
#include "panSpeed.h"
#include "pingCommand.h"
#include "settingsOverlay.h"
#include "shiftRecruit.h"
#include "siegeCampHotkey.h"
#include "startupQuitCrash.h"
#include "stopTroopsHotkey.h"
#include "zoomLimit.h"
#include "zoomSpeed.h"

void applyUnofficialPatches() {
    loadConfig();

    installKnightCatapultCrashFix();
    installMpAiEnable();
    installIntroSkip();
    installMpConnectCompleteCrashFix();
    installLobbyInProgressFilter();
    installPingCommand();
    installMatchRules();
    installHandshake();
    installLobbyPanel();
    installEndgameStats();
    installBarracksCrashFix();
    installMapEdgeCrashFix();
    installStartupQuitCrashFix();
    installStopTroopsHotkey();
    installAttackHotkey();
    installZoomSpeed();
    installPanSpeed();
    installZoomLimit();
    installShiftRecruit();
    installAutoMarket();
    installSiegeCampHotkey();
    installOrderTrace();

    // Multiplayer replays (src/replay/). Every hook is stock-byte verified
    // and each module installs all of its sites or none. The session comes
    // last: it refuses to record unless the modules before it are in.
    installReplayTrace();
    installReplayPlayer();
    installPrngGuard();
    installSpectatorGuard();
    installReplayViewer();
    // After the spectator guard: its window subclass must sit outside the
    // guard's so the free-view key is seen before the guard swallows it.
    installReplayVision();
    installReplaySession();

    // Last: it reads every other feature's loaded binding, and its window
    // subclass must sit outside the auto-market editor's so it sees input first.
    installSettingsOverlay();
}
