#pragma once
#include <map>
#include <string>
#include <vector>


class Settings {
public:
  Settings();
  bool Load(const std::string &filename);
  int GetInt(const std::string &key, int defaultValue = 0) const;
  bool GetBool(const std::string &key, bool defaultValue = false) const;
  std::string GetString(const std::string &key,
                        const std::string &defaultValue = "") const;
  bool SaveDefault(const std::string &filename);

  // Set a value in memory
  void Set(const std::string &key, const std::string &value);
  void SetInt(const std::string &key, int value);
  void SetBool(const std::string &key, bool value);

  // Update a specific key in the settings file (preserves comments and
  // formatting)
  bool UpdateFile(const std::string &filename, const std::string &key,
                  const std::string &value);

  // Returns absolute path to settings.ini next to the executable.
  static std::string GetSettingsPath();

  // Check if settings file exists (for first-run detection)
  static bool FileExists(const std::string &filename);

  // Returns true if this was the first run (settings file was just created)
  bool WasFirstRun() const { return m_wasFirstRun; }

  // Check if a key exists in the loaded settings
  bool HasKey(const std::string &key) const;

  // Get list of all required settings keys
  static std::vector<std::string> GetRequiredKeys();

private:
  std::map<std::string, std::string> values;
  std::string Trim(const std::string &str) const;
  bool m_wasFirstRun = false;
};
