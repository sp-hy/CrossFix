#pragma once
#include <string>
#include <map>

class Settings {
public:
    Settings();
    bool Load(const std::string& filename);
    int GetInt(const std::string& key, int defaultValue = 0) const;
    bool GetBool(const std::string& key, bool defaultValue = false) const;
    std::string GetString(const std::string& key, const std::string& defaultValue = "") const;
    bool SaveDefault(const std::string& filename);


private:
    std::map<std::string, std::string> values;
    std::string Trim(const std::string& str) const;
};
