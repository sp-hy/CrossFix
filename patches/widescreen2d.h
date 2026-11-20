#pragma once
#include <Windows.h>

// Install the HD background layer hook (CHRONOCROSS.exe+1CCF51) and register
// the guest hooks that keep the scroll clamp in step with the room's limits
bool InitWidescreen2DHook();

// Set the widescreen ratio for 2D background transformation
// ratio: the horizontal compression ratio (e.g., 0.75 for 16:9, 0.571 for 21:9)
void SetWidescreen2DRatio(float ratio);

// Enable or disable 2D widescreen transformation
void SetWidescreen2DEnabled(bool enabled);
