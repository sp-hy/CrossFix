#pragma once
#include "../utils/memory.h"
#include <Windows.h>

// Apply the widescreen patch with a specific aspect ratio
bool ApplyWidescreenPatch(uintptr_t base, float aspectRatio);

// Auto-detect the user's aspect ratio and apply the appropriate widescreen
// patch
bool ApplyWidescreenPatchAuto(uintptr_t base, float *outAspectRatio = nullptr);

// Start dynamic resolution monitoring (for auto-detect mode)
void StartDynamicWidescreenMonitoring(uintptr_t base);

// Stop dynamic resolution monitoring
void StopDynamicWidescreenMonitoring();

// Restore default (non-widescreen) behavior
bool RestoreDefaultBehavior(uintptr_t base);

// Get the current widescreen ratio (for texture resizing, etc.)
float GetCurrentWidescreenRatio();
