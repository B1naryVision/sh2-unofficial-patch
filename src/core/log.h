#pragma once
#include <cstdint>

void logPushContext(uintptr_t value);

#ifdef DEBUG
void logFlush(const char *path);
#endif
