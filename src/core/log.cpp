#include "log.h"

#ifdef DEBUG
#include <fstream>
#endif

static constexpr size_t kRingSize = 10;
static uintptr_t g_Ring[kRingSize] = {};
static size_t g_Head = 0;
static size_t g_Count = 0;

void Log_PushContext(uintptr_t value)
{
    g_Ring[g_Head] = value;
    g_Head = (g_Head + 1) % kRingSize;
    if (g_Count < kRingSize) ++g_Count;
}

#ifdef DEBUG
void Log_Flush(const char *path)
{
    std::ofstream f(path);
    if (!f.is_open()) return;
    f << "[SH2 Patch] Last " << std::dec << g_Count << " context(s):\n";
    size_t oldest = (g_Head + kRingSize - g_Count) % kRingSize;
    for (size_t i = 0; i < g_Count; ++i)
    {
        size_t idx = (oldest + i) % kRingSize;
        f << "  [" << i << "] = 0x" << std::hex << g_Ring[idx] << "\n";
    }
}
#endif
