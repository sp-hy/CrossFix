#pragma once
#include <Windows.h>
#include <cstdint>

// Apply the dialog and text offset patches
bool ApplyDialogPatch(uintptr_t base);

// Check if the main menu is currently open
bool IsMainMenuOpen();

// Update dialog values based on aspect ratio and battle flag
void UpdateDialogValues(float aspectRatio, bool isInBattle);
