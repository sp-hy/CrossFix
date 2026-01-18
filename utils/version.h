#pragma once
#include <string>

// Check if the executable has the expected product version
bool CheckExecutableVersion(const char* exePath, const char* expectedVersion);

// Get the product version string from an executable
std::string GetExecutableVersion(const char* exePath);
