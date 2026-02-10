#include "settings.h"
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <vector>

Settings::Settings() : m_wasFirstRun(false) {}

std::string Settings::GetSettingsPath() {
  char exePath[MAX_PATH];
  if (GetModuleFileNameA(NULL, exePath, MAX_PATH) != 0) {
    std::string exePathStr(exePath);
    size_t lastBackslash = exePathStr.find_last_of("\\/");
    if (lastBackslash != std::string::npos) {
      return exePathStr.substr(0, lastBackslash + 1) + "settings.ini";
    }
  }
  return "settings.ini";
}

bool Settings::FileExists(const std::string &filename) {
  struct stat buffer;
  return (stat(filename.c_str(), &buffer) == 0);
}

std::string Settings::Trim(const std::string &str) const {
  size_t first = str.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  size_t last = str.find_last_not_of(" \t\r\n");
  return str.substr(first, (last - first + 1));
}

bool Settings::Load(const std::string &filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    m_wasFirstRun = true;
    if (SaveDefault(filename)) {
      // Reload the newly created file
      file.open(filename);
      if (!file.is_open())
        return false;
    } else {
      return false;
    }
  }

  std::string line;
  while (std::getline(file, line)) {
    // Trim the line
    line = Trim(line);

    // Skip empty lines and comments
    if (line.empty() || line[0] == '#') {
      continue;
    }

    // Find the '=' separator
    size_t equalPos = line.find('=');
    if (equalPos == std::string::npos) {
      continue; // Skip lines without '='
    }

    // Extract key and value
    std::string key = Trim(line.substr(0, equalPos));
    std::string value = Trim(line.substr(equalPos + 1));

    // Remove inline comments from value
    size_t commentPos = value.find('#');
    if (commentPos != std::string::npos) {
      value = Trim(value.substr(0, commentPos));
    }

    // Store the key-value pair
    if (!key.empty()) {
      values[key] = value;
    }
  }

  file.close();

  // Validate that all required keys exist
  std::vector<std::string> requiredKeys = GetRequiredKeys();
  bool allKeysPresent = true;
  for (const auto &key : requiredKeys) {
    if (!HasKey(key)) {
      allKeysPresent = false;
      break;
    }
  }

  // If any required keys are missing, regenerate the settings file
  if (!allKeysPresent) {
    std::cout << "[CrossFix] Settings file is missing required keys. "
                 "Regenerating with defaults..."
              << std::endl;
    values.clear(); // Clear existing values
    m_wasFirstRun = true;
    if (!SaveDefault(filename)) {
      return false;
    }
    // Reload the regenerated file
    return Load(filename);
  }

  return true;
}

int Settings::GetInt(const std::string &key, int defaultValue) const {
  auto it = values.find(key);
  if (it == values.end()) {
    return defaultValue;
  }

  try {
    return std::stoi(it->second);
  } catch (...) {
    return defaultValue;
  }
}

bool Settings::GetBool(const std::string &key, bool defaultValue) const {
  auto it = values.find(key);
  if (it == values.end()) {
    return defaultValue;
  }

  // Check for various boolean representations
  std::string value = it->second;
  std::transform(value.begin(), value.end(), value.begin(), ::tolower);

  if (value == "1" || value == "true" || value == "yes" || value == "on") {
    return true;
  }
  if (value == "0" || value == "false" || value == "no" || value == "off") {
    return false;
  }

  return defaultValue;
}

std::string Settings::GetString(const std::string &key,
                                const std::string &defaultValue) const {
  auto it = values.find(key);
  if (it == values.end()) {
    return defaultValue;
  }
  return it->second;
}

void Settings::Set(const std::string &key, const std::string &value) {
  values[key] = value;
}

void Settings::SetInt(const std::string &key, int value) {
  values[key] = std::to_string(value);
}

void Settings::SetBool(const std::string &key, bool value) {
  values[key] = value ? "1" : "0";
}

bool Settings::UpdateFile(const std::string &filename, const std::string &key,
                          const std::string &value) {
  // Read entire file
  std::ifstream inFile(filename);
  if (!inFile.is_open()) {
    return false;
  }

  std::vector<std::string> lines;
  std::string line;
  bool found = false;

  while (std::getline(inFile, line)) {
    // Check if this line contains the key we want to update
    std::string trimmedLine = Trim(line);
    if (!trimmedLine.empty() && trimmedLine[0] != '#') {
      size_t equalPos = trimmedLine.find('=');
      if (equalPos != std::string::npos) {
        std::string lineKey = Trim(trimmedLine.substr(0, equalPos));
        if (lineKey == key) {
          // Replace this line with the new value
          line = key + "=" + value;
          found = true;
        }
      }
    }
    lines.push_back(line);
  }
  inFile.close();

  // If key wasn't found, append it
  if (!found) {
    lines.push_back(key + "=" + value);
  }

  // Write back to file
  std::ofstream outFile(filename);
  if (!outFile.is_open()) {
    return false;
  }

  for (const auto &l : lines) {
    outFile << l << std::endl;
  }
  outFile.close();

  // Update in-memory value
  values[key] = value;

  return true;
}

bool Settings::SaveDefault(const std::string &filename) {
  std::ofstream file(filename);
  if (!file.is_open()) {
    return false;
  }

  file << "# ============================================\n";
  file << "#   Chrono Cross - CrossFix Settings\n";
  file << "# ============================================\n\n";

  // --- Display ---
  file << "# ============================================\n";
  file << "#   Display\n";
  file << "# ============================================\n\n";
  file << "# Dynamic widescreen (auto-adjusts to resolution/aspect ratio).\n";
  file << "# Use with in-game ScreenType: Full.\n";
  file << "widescreen_enabled=1\n\n";
  file << "# Force camera boundaries in rooms (consistent widescreen).\n";
  file << "# Disable if it breaks camera placement in a scene.\n";
  file << "boundary_overrides=1\n\n";

  // --- Performance / timing ---
  file << "# ============================================\n";
  file << "#   Performance / timing\n";
  file << "# ============================================\n\n";
  file
      << "# Double FPS mode: 60 FPS in field (use with in-game slowdown/F1).\n";
  file << "# Disable if using another tool (e.g. SpecialK, GameScope, etc).\n";
  file << "# 0 = 30 field / 60 battle,  1 = 60 everywhere\n";
  file << "double_fps_mode=1\n\n";
  file << "# Hide the slow-motion icon when using double FPS / slowdown.\n";
  file << "hide_slow_icon=1\n\n";
  file << "# Keep running when window loses focus (no pause, music "
          "continues).\n";
  file << "# 0 = pause when inactive,  1 = keep running\n";
  file << "disable_pause_on_focus_loss=1\n\n";

  // --- Textures / upscaling ---
  file << "# ============================================\n";
  file << "#   Textures / upscaling\n";
  file << "# ============================================\n\n";
  file << "# Upscale: 1=off, 2=2x, 3=3x, 4=4x (experimental, may crash).\n";
  file << "upscale_scale=1\n\n";
  file << "# Internal: first-run upscale prompt already shown (do not edit).\n";
  file << "upscale_setup_completed=0\n\n";
  file << "# Dump textures to /dump/ (hash filenames). Disables upscale while "
          "on.\n";
  file << "texture_dump_enabled=0\n\n";
  file << "# Force point/nearest filtering (fixes lines on overlays when "
          "upscaling).\n";
  file << "# 0 = default filtering,  1 = pixelated, no bleed\n";
  file << "sampler_force_point=0\n\n";

  // --- Modding ---
  file << "# ============================================\n";
  file << "#   Modding\n";
  file << "# ============================================\n\n";
  file << "# Mod loader: load from mods/ instead of .dat (e.g. "
          "mods/map/mapbin/).\n";
  file << "mod_loader_enabled=1\n\n";
  file << "# Load replacement textures from mods/textures (by hash).\n";
  file << "texture_replace_enabled=0\n";

  file.close();
  return true;
}

bool Settings::HasKey(const std::string &key) const {
  return values.find(key) != values.end();
}

std::vector<std::string> Settings::GetRequiredKeys() {
  return {"widescreen_enabled",      "double_fps_mode",
          "hide_slow_icon",          "disable_pause_on_focus_loss",
          "upscale_scale",           "texture_dump_enabled",
          "texture_replace_enabled", "upscale_setup_completed",
          "boundary_overrides",      "mod_loader_enabled",
          "sampler_force_point"};
}
