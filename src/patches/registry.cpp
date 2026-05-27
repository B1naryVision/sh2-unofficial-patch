#include "registry.h"
#include "introSkip.h"
#include "knightCatapultCrash.h"
#include "mpAiEnable.h"
#include "ballistaAutoFire.h"

void applyUnofficialPatches() {
    installKnightCatapultCrashFix();
    installMpAiEnable();
    installIntroSkip();
    installBallistaAutoFire();
}
