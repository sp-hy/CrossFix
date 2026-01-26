#pragma once
#include <Windows.h>
#include <cstdint>

// Initialize battle patch system
bool ApplyBattlePatch(uintptr_t base);

// Check if the game is currently in a battle
bool IsInBattle();
