#pragma once
#include "../utils/memory.h"
#include <Windows.h>

// 60 FPS mode: the field draws a frame every vblank at normal speed while it
// runs at its normal pace (guest hooks on the field frame driver).
// With stats on, every 300 field frames the console shows how many vblanks
// each frame took.
bool Apply60FpsPatch(uintptr_t base, bool stats);
