#include "registry.h"
#include "../core/config.h"
#include "attackHotkey.h"
#include "autoMarket/autoMarket.h"
#include "barracksCrash.h"
#include "endgameStats.h"
#include "introSkip.h"
#include "knightCatapultCrash.h"
#include "lobbyInProgressFilter.h"
#include "mapEdgeCrash.h"
#include "mpAiEnable.h"
#include "mpConnectCompleteCrash.h"
#include "panSpeed.h"
#include "pingCommand.h"
#include "settingsOverlay.h"
#include "shiftRecruit.h"
#include "siegeCampHotkey.h"
#include "stopTroopsHotkey.h"
#include "zoomLimit.h"
#include "zoomSpeed.h"
#include "../replay/prngGuard.h"
#include "../replay/replayPlayer.h"
#include "../replay/replayRecorder.h"
#include "../replay/replaySession.h"
#include "../replay/replayTrace.h"
#include "../replay/replayViewer.h"
#include "../replay/replayVision.h"
#include "../replay/spectatorGuard.h"

void applyUnofficialPatches() {
    loadConfig();

    installKnightCatapultCrashFix();
    installMpAiEnable();
    installIntroSkip();
    installMpConnectCompleteCrashFix();
    installLobbyInProgressFilter();
    installPingCommand();
    installEndgameStats();
    installBarracksCrashFix();
    installMapEdgeCrashFix();
    installStopTroopsHotkey();
    installAttackHotkey();
    installZoomSpeed();
    installPanSpeed();
    installZoomLimit();
    installShiftRecruit();
    installAutoMarket();
    installSiegeCampHotkey();

    // Multiplayer replays (src/replay/).
    // Order matters: the command trace feeds the recorder; the recorder's
    // frame tick samples the match epoch before the player's reads it; the
    // viewer and the session come last. Every hook is stock-byte verified
    // and each module installs all of its sites or none.
    installReplayTrace();
    installReplayRecorder();
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
