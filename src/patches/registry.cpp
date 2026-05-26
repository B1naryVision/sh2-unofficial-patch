#include "registry.h"
#include "introSkip.h"
#include "knightCatapultCrash.h"
#include "mpAiEnable.h"

void applyUnofficialPatches() {
    installKnightCatapultCrashFix();
    installMpAiEnable();
    installIntroSkip();
}
