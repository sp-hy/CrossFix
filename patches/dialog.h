#pragma once
#include <Windows.h>
#include <cstdint>

// Apply the dialog and text offset patches
bool ApplyDialogPatch(uintptr_t base);

// Main menu: when true, dialog patches are disabled.
bool IsMainMenuOpen();
// Game menu: in-game menu open (rde::menu_open_flag), e.g. for battle UI / container res.
bool IsGameMenuOpen();

// Update dialog values based on aspect ratio and battle flag
void UpdateDialogValues(float aspectRatio, bool isInBattle);
