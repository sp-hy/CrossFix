#pragma once
#include <Windows.h>
#include <cstdint>

// Initialize battle UI and menu patch system
bool ApplyBattleUIAndMenuPatch(uintptr_t base);

// Check if the game is currently in a battle
bool IsInBattle();

// Update battle UI and menu aspect ratio values
void UpdateBattleUIAndMenuValues(float aspectRatio, bool isMainMenuOpen);
