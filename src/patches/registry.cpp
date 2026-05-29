#include "registry.h"
#include "introSkip.h"
#include "knightCatapultCrash.h"
#include "mpAiEnable.h"
#include "mpConnectCompleteCrash.h"

void applyUnofficialPatches() {
    installKnightCatapultCrashFix();
    installMpAiEnable();
    installIntroSkip();
    installMpConnectCompleteCrashFix();
}
