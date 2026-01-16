#pragma once
#include <Windows.h>

// Initialize the ASM hook for widescreen 2D background transformation
// This hooks the instruction at CHRONOCROSS.exe+1CCF93 to modify image dimensions
bool InitWidescreen2DHook();

// Set the widescreen ratio for 2D background transformation
// ratio: the horizontal compression ratio (e.g., 0.75 for 16:9, 0.571 for 21:9)
void SetWidescreen2DRatio(float ratio);

// Enable or disable 2D widescreen transformation
void SetWidescreen2DEnabled(bool enabled);
