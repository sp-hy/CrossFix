#include "settings.h"
#include <fstream>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <sys/stat.h>
#include <vector>


Settings::Settings() : m_wasFirstRun(false) {
}

bool Settings::FileExists(const std::string& filename) {
    struct stat buffer;
    return (stat(filename.c_str(), &buffer) == 0);
}

std::string Settings::Trim(const std::string& str) const {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

bool Settings::Load(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        m_wasFirstRun = true;
        if (SaveDefault(filename)) {
            // Reload the newly created file
            file.open(filename);
            if (!file.is_open()) return false;
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
    return true;
}

int Settings::GetInt(const std::string& key, int defaultValue) const {
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

bool Settings::GetBool(const std::string& key, bool defaultValue) const {
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

std::string Settings::GetString(const std::string& key, const std::string& defaultValue) const {
    auto it = values.find(key);
    if (it == values.end()) {
        return defaultValue;
    }
    return it->second;
}

void Settings::Set(const std::string& key, const std::string& value) {
    values[key] = value;
}

void Settings::SetInt(const std::string& key, int value) {
    values[key] = std::to_string(value);
}

void Settings::SetBool(const std::string& key, bool value) {
    values[key] = value ? "1" : "0";
}

bool Settings::UpdateFile(const std::string& filename, const std::string& key, const std::string& value) {
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
    
    for (const auto& l : lines) {
        outFile << l << std::endl;
    }
    outFile.close();
    
    // Update in-memory value
    values[key] = value;
    
    return true;
}

bool Settings::SaveDefault(const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        return false;
    }

    file << "# Chrono Cross Crossfix Settings" << std::endl << std::endl;
    file << "# Enable or disable the dynamic widescreen patch" << std::endl;
    file << "# This will automatically detect and adjust to any resolution/aspect ratio" << std::endl;
    file << "# Must be used with the setting ScreenType: Full" << std::endl;
    file << "widescreen_enabled=1" << std::endl << std::endl;
    file << "# Enable or disable the double FPS mode" << std::endl;
    file << "# Should be used with the slowdown mode (Press F1 in game), Should provide a smooth 60 everywhere" << std::endl;
    file << "# Disable if you use another tool like SpecialK" << std::endl;
    file << "# Use with desktop mode if playing on Steamdeck" << std::endl;
    file << "# 0 = disabled (30 field/60 battle FPS), 1 = enabled (60 FPS everywhere)" << std::endl;
    file << "double_fps_mode=1" << std::endl << std::endl;
    file << "# Disable pause when window loses focus" << std::endl;
    file << "# 0 = game pauses when window is inactive (default behavior)" << std::endl;
    file << "# 1 = game & music continue running when window is inactive" << std::endl;
    file << "disable_pause_on_focus_loss=1" << std::endl << std::endl;
    file << "# Enable or disable Upscaling" << std::endl;
    file << "# WARNING: This feature is experimental and may cause crashes or instability" << std::endl;
    file << "# 0 = disabled, 1 = enabled" << std::endl;
    file << "upscale_enabled=0" << std::endl << std::endl;
    file << "# Upscale multiplier (only used when upscale_enabled=1)" << std::endl;
    file << "# Valid values: 2, 3, or 4" << std::endl;
    file << "# Higher values = better quality but more demanding & might crash" << std::endl;
    file << "upscale_scale=4" << std::endl << std::endl;
    file << "# Enable or disable texture dumping" << std::endl;
    file << "# Dumps textures to /dump/ directory with hash-based filenames" << std::endl;
    file << "# NOTE: Texture resizing is automatically disabled when dumping is enabled" << std::endl;
    file << "# 0 = disabled, 1 = enabled" << std::endl;
    file << "texture_dump_enabled=0" << std::endl << std::endl;
    file << "# Internal flag - set to 1 after first-run upscale setup is complete" << std::endl;
    file << "# Delete this line or set to 0 to re-trigger the setup prompt" << std::endl;
    file << "upscale_setup_completed=0" << std::endl;

    file.close();
    return true;
}

