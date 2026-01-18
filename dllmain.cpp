#include <Windows.h>
#include <iostream>
#include <string>
#include <algorithm>
#include <cctype>
#include "patches/widescreen.h"
#include "patches/widescreen2d.h"
#include "d3d11/d3d11_proxy.h"
#include "patches/fps.h"
#include "patches/pausefix.h"
#include "utils/settings.h"
#include "utils/version.h"

HANDLE crossfix_handle = GetCurrentProcess();

// Global flag to indicate if patches should be applied
bool g_versionCheckPassed = false;

DWORD WINAPI MainThread(LPVOID param) {
	uintptr_t base = (uintptr_t)GetModuleHandle(NULL);

	char exePath[MAX_PATH];
	if (GetModuleFileNameA(NULL, exePath, MAX_PATH) == 0) {
		FreeLibraryAndExitThread((HMODULE)param, 0);
		return 0;
	}

	char* exeName = strrchr(exePath, '\\');
	if (exeName == NULL) {
		exeName = exePath;
	} else {
		exeName++;
	}

	std::string exeNameLower(exeName);
	std::transform(exeNameLower.begin(), exeNameLower.end(), exeNameLower.begin(), ::tolower);

	if (exeNameLower != "chronocross.exe") {
		FreeLibraryAndExitThread((HMODULE)param, 0);
		return 0;
	}

	AllocConsole();
	FILE* f;
	freopen_s(&f, "CONOUT$", "w", stdout);

	std::cout << "CrossFix - v0.3" << std::endl;
	std::cout << std::endl;
	std::cout << "https://github.com/sp-hy/CrossFix" << std::endl;
	std::cout << std::endl;

	// Check executable version
	const char* expectedVersion = "1.0.1.0";
	if (!CheckExecutableVersion(exePath, expectedVersion)) {
		std::cout << std::endl;
		std::cout << "ERROR: Incompatible game version detected!" << std::endl;
		std::cout << "Patching has been disabled." << std::endl;
		std::cout << std::endl;
		// Don't apply any patches - just exit gracefully
		return 0;
	}

	// Version check passed - enable patching
	g_versionCheckPassed = true;

	std::cout << std::endl;
#ifdef _DEBUG
	std::cout << "DLL loaded successfully! Base address of the injected executable is: 0x" << std::hex << base << std::dec << std::endl;
#endif
	std::cout << std::endl;

	std::string settingsPath = "settings.ini";
	std::string exePathStr(exePath);
	size_t lastBackslash = exePathStr.find_last_of("\\/");
	if (lastBackslash != std::string::npos) {
		settingsPath = exePathStr.substr(0, lastBackslash + 1) + "settings.ini";
	}

	Settings settings;
	settings.Load(settingsPath);


	InitD3D11Proxy();

	bool widescreenEnabled = settings.GetBool("widescreen_enabled", true);

	if (widescreenEnabled) {
		std::cout << "Initializing dynamic widescreen patch..." << std::endl;
		
		if (ApplyWidescreenPatchAuto(base)) {
			SetWidescreen2DEnabled(true);
			
			if (!InitWidescreen2DHook()) {
				std::cout << "Failed to initialize 2D widescreen ASM hook!" << std::endl;
			}
			
			// Always start dynamic monitoring when widescreen is enabled
			StartDynamicWidescreenMonitoring(base);
		} else {
			std::cout << "Widescreen patch initialization failed" << std::endl;
		}
	} else {
		std::cout << "Widescreen patch disabled in settings" << std::endl;
	}

	if (settings.GetBool("double_fps_mode", false)) {
		ApplyDoubleFpsPatch(base);
	}

	if (settings.GetBool("disable_pause_on_focus_loss", true)) {
		ApplyDisablePausePatch(base);
	} else {
		std::cout << "Disable pause on focus loss disabled in settings" << std::endl;
	}

	return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
	switch (ul_reason_for_call) {
	case DLL_PROCESS_ATTACH:
		CreateThread(0, 0, MainThread, hModule, 0, 0);
		break;
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}
