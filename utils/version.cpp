#include "version.h"
#include <Windows.h>
#include <iostream>
#include <vector>

std::string GetExecutableVersion(const char *exePath) {
  // Get the size of the version info
  DWORD versionInfoSize = GetFileVersionInfoSizeA(exePath, NULL);
  if (versionInfoSize == 0) {
    return "";
  }

  // Allocate buffer for version info
  std::vector<BYTE> versionInfo(versionInfoSize);
  if (!GetFileVersionInfoA(exePath, 0, versionInfoSize, versionInfo.data())) {
    return "";
  }

  // Get the product version from the version info
  VS_FIXEDFILEINFO *fileInfo = nullptr;
  UINT fileInfoSize = 0;
  if (!VerQueryValueA(versionInfo.data(), "\\", (LPVOID *)&fileInfo,
                      &fileInfoSize)) {
    return "";
  }

  if (fileInfo == nullptr) {
    return "";
  }

  // Extract version numbers
  WORD majorVersion = HIWORD(fileInfo->dwProductVersionMS);
  WORD minorVersion = LOWORD(fileInfo->dwProductVersionMS);
  WORD buildVersion = HIWORD(fileInfo->dwProductVersionLS);
  WORD revisionVersion = LOWORD(fileInfo->dwProductVersionLS);

  // Format as "major.minor.build.revision"
  char versionStr[32];
  sprintf_s(versionStr, "%d.%d.%d.%d", majorVersion, minorVersion, buildVersion,
            revisionVersion);

  return std::string(versionStr);
}

bool CheckExecutableVersion(const char *exePath, const char *expectedVersion) {
  std::string actualVersion = GetExecutableVersion(exePath);

  if (actualVersion.empty()) {
    std::cout << "Warning: Could not read version information from executable"
              << std::endl;
    return false;
  }

  std::cout << "Detected game version: " << actualVersion << std::endl;

  if (actualVersion != expectedVersion) {
    std::cout << "Expected version: " << expectedVersion << std::endl;
    return false;
  }

  return true;
}
