#include "registry.h"
#include "knightCatapultCrash.h"
#include "mpAiEnable.h"

void applyUnofficialPatches() {
    installKnightCatapultCrashFix();
    installMpAiEnable();
}
