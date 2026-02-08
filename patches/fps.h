#pragma once
#include "../utils/memory.h"
#include <Windows.h>

// Apply the double FPS patch to the game
bool ApplyDoubleFpsPatch(uintptr_t base);
