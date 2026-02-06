#pragma once
#include <string>

// Initialize the mod loader. Call before the game opens hd.dat.
// exePath: full path to the game executable (used to locate mods/ folder)
// Returns true if hooks were installed, false if skipped (no mods dir) or failed.
bool InitModLoader(const std::string& exePath);
