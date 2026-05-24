#include "log.h"

#ifdef DEBUG
#include <fstream>
#endif

static constexpr size_t kRingSize = 10;
static uintptr_t s_ring[kRingSize] = {};
static size_t s_head = 0;
static size_t s_count = 0;

void logPushContext(uintptr_t value) {
    s_ring[s_head] = value;
    s_head = (s_head + 1) % kRingSize;

    if (s_count < kRingSize) {
        ++s_count;
    }
}

#ifdef DEBUG
void logFlush(const char *path) {
    std::ofstream f(path);
    if (!f.is_open()) {
        return;
    }
    f << "[SH2 Patch] Last " << std::dec << s_count << " context(s):\n";
    size_t oldest = (s_head + kRingSize - s_count) % kRingSize;
    for (size_t i = 0; i < s_count; ++i) {
        size_t idx = (oldest + i) % kRingSize;
        f << "  [" << i << "] = 0x" << std::hex << s_ring[idx] << "\n";
    }
}
#endif
