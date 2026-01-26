#pragma once
#include <Windows.h>
#include <cstdint>

// Apply the dialog and text offset patches
bool ApplyDialogPatch(uintptr_t base);

// Update dialog values based on aspect ratio, fmv/menu flag, and battle flag
void UpdateDialogValues(float aspectRatio, bool isInFmvMenu, bool isInBattle);
