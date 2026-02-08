#pragma once
#include "../utils/memory.h"
#include <Windows.h>

// Apply the disable pause on focus loss patch to the game
bool ApplyDisablePausePatch(uintptr_t base);
