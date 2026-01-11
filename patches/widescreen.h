#pragma once
#include <Windows.h>
#include "../utils/memory.h"

// Widescreen modes
enum WidescreenMode {
    WIDESCREEN_16_9 = 0,  // 16:9 aspect ratio
    WIDESCREEN_21_9 = 1,  // 21:9 ultrawide
    WIDESCREEN_32_9 = 2   // 32:9 super ultrawide
};

// Apply the widescreen patch to the game with the specified mode
bool ApplyWidescreenPatch(uintptr_t base, WidescreenMode mode);
