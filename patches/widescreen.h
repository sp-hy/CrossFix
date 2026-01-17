#pragma once
#include <Windows.h>
#include "../utils/memory.h"

// Widescreen modes
enum WidescreenMode {
    WIDESCREEN_AUTO = 0,  // Auto-detect from screen resolution
    WIDESCREEN_16_9 = 1,  // 16:9 aspect ratio
    WIDESCREEN_16_10 = 2, // 16:10 aspect ratio
    WIDESCREEN_21_9 = 3,  // 21:9 ultrawide
    WIDESCREEN_32_9 = 4   // 32:9 super ultrawide
};

// Apply the widescreen patch to the game with the specified mode
bool ApplyWidescreenPatch(uintptr_t base, WidescreenMode mode);

// Auto-detect the user's aspect ratio and apply the appropriate widescreen patch
bool ApplyWidescreenPatchAuto(uintptr_t base);

// Auto-detect and optionally return the detected mode
bool ApplyWidescreenPatchAuto(uintptr_t base, WidescreenMode* outMode);

// Get the 2D ratio for a given widescreen mode
float GetWidescreenRatio2D(WidescreenMode mode);

// Start dynamic resolution monitoring (for auto-detect mode)
void StartDynamicWidescreenMonitoring(uintptr_t base);

// Stop dynamic resolution monitoring
void StopDynamicWidescreenMonitoring();

// Restore default (non-widescreen) behavior
bool RestoreDefaultBehavior(uintptr_t base);
