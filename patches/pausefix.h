#pragma once
#include <Windows.h>
#include "../utils/memory.h"

// Apply the disable pause on focus loss patch to the game
bool ApplyDisablePausePatch(uintptr_t base);
