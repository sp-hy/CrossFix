#pragma once
#include <Windows.h>
#include "../utils/memory.h"

// Apply the double FPS patch to the game
bool ApplyDoubleFpsPatch(uintptr_t base);
