#pragma once
#include <cstddef>
#include <windows.h>

void installHook(void *targetAddress, void *detourFunction, size_t instructionLength);
