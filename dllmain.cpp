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

HANDLE crossfix_handle = GetCurrentProcess();

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

	std::cout << "CrossFix - v0.2" << std::endl;
	std::cout << std::endl;
	std::cout << "https://github.com/sp-hy/CrossFix" << std::endl;
	std::cout << std::endl;
#ifdef _DEBUG
	std::cout << "DLL loaded successfully! Base address of the injected executable is: 0x" << std::hex << base << std::dec << std::endl;
#endif
	std::cout << std::endl;

	Settings settings;
	settings.Load("settings.ini");

	InitD3D11Proxy();

	bool widescreenEnabled = settings.GetBool("widescreen_enabled", true);
	int widescreenModeInt = settings.GetInt("widescreen_mode", 0);

	if (widescreenEnabled) {
		float ratio2D = 0.75f; 
		WidescreenMode mode = WIDESCREEN_16_9;
		bool success = false;
		
		// Check if auto-detect is enabled (widescreen_mode = 0)
		if (widescreenModeInt == 0 || widescreenModeInt == WIDESCREEN_AUTO) {
			std::cout << "Auto-detecting widescreen mode from screen resolution..." << std::endl;
			
			// Use auto-detection
			if (ApplyWidescreenPatchAuto(base, &mode)) {
				// Get the 2D ratio for the detected mode
				ratio2D = GetWidescreenRatio2D(mode);
				success = true;
			} else {
				std::cout << "Auto-detection failed, widescreen patch not applied" << std::endl;
			}
		} else if (widescreenModeInt >= 1 && widescreenModeInt <= 3) {
			// Manual mode selection
			mode = static_cast<WidescreenMode>(widescreenModeInt);
			ratio2D = GetWidescreenRatio2D(mode);
			
			if (ApplyWidescreenPatch(base, mode)) {
				success = true;
			} else {
				std::cout << "Failed to apply widescreen patch!" << std::endl;
			}
		} else {
			std::cout << "Warning: Invalid widescreen_mode value (" << widescreenModeInt << "), use 0 for auto-detect or 1-3 for manual mode" << std::endl;
		}
		
		// Only initialize 2D widescreen if the 3D patch was successful
		if (success) {
			SetWidescreen2DRatio(ratio2D);
			SetWidescreen2DEnabled(true);
			
			if (!InitWidescreen2DHook()) {
				std::cout << "Failed to initialize 2D widescreen ASM hook!" << std::endl;
			}
			
			// Start dynamic monitoring if auto-detect was used
			if (widescreenModeInt == 0 || widescreenModeInt == WIDESCREEN_AUTO) {
				StartDynamicWidescreenMonitoring(base);
			}
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
