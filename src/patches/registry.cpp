#include "registry.h"
#include "introSkip.h"
#include "knightCatapultCrash.h"
#include "mpAiEnable.h"
#include "unitCapRaise.h"

void applyUnofficialPatches() {
    installKnightCatapultCrashFix();
    installMpAiEnable();
    installIntroSkip();
    installUnitCapRaise();
}
