#pragma once
#include "../utils/memory.h"
#include <Windows.h>

// Apply the double FPS patch to the game
bool ApplyDoubleFpsPatch(uintptr_t base);

// NOP the slow-motion icon draw (movss at +0x1ADFBF). Enable via hide_slow_icon.
bool ApplyHideSlowIconPatch(uintptr_t base);
