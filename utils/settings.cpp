#include "settings.h"
#include <fstream>
#include <algorithm>
#include <cctype>

Settings::Settings() {
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
        return false;
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
