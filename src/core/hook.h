#pragma once
#include <windows.h>
#include <cstddef>

void installHook(void *targetAddress, void *detourFunction, size_t instructionLength);
