#pragma once
#include <windows.h>
#include <cstddef>

void InstallHook(void *targetAddress, void *detourFunction, size_t instructionLength);
