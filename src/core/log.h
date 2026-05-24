#pragma once
#include <cstdint>

void Log_PushContext(uintptr_t value);

#ifdef DEBUG
void Log_Flush(const char *path);
#endif
