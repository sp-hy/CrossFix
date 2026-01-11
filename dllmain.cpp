// dllmain.cpp : Defines the entry point for the DLL application.


// If using a proxy build config, include the necessary code to proxy calls to winmm.dll
#ifdef PROXY
    #include "winmm/winmm.h"
#else
    #include <Windows.h>
#endif

#include <iostream>
#include <string>
#include <algorithm>
#include <cctype>
#include "patches/widescreen.h"
#include "patches/fps.h"
#include "patches/pausefix.h"
#include "utils/settings.h"


// Get handle from the injected process
HANDLE aoe_handle = GetCurrentProcess();

// Spawn thread to do work
DWORD WINAPI MainThread(LPVOID param) {
    // Get base address from the injected process
	uintptr_t base = (uintptr_t)GetModuleHandle(NULL);

	// Verify we're running in CHRONOCROSS.exe
	char exePath[MAX_PATH];
	if (GetModuleFileNameA(NULL, exePath, MAX_PATH) == 0) {
		// Could not get executable name, exit silently
		FreeLibraryAndExitThread((HMODULE)param, 0);
		return 0;
	}

	// Extract just the executable name from the full path
	char* exeName = strrchr(exePath, '\\');
	if (exeName == NULL) {
		exeName = exePath; // No path separator found, use the whole string
	} else {
		exeName++; // Skip the backslash
	}

	// Convert to lowercase for case-insensitive comparison
	std::string exeNameLower(exeName);
	std::transform(exeNameLower.begin(), exeNameLower.end(), exeNameLower.begin(), ::tolower);

	// Check if we're running in CHRONOCROSS.exe
	if (exeNameLower != "chronocross.exe") {
		// Not the target executable, exit silently without showing console
		FreeLibraryAndExitThread((HMODULE)param, 0);
		return 0;
	}

	// We're in CHRONOCROSS.exe - allocate a console and show status
	AllocConsole();
	FILE* f;
	freopen_s(&f, "CONOUT$", "w", stdout);

	// Log a message
	std::cout << "DLL loaded successfully! Base address of the injected executable is: 0x" << std::hex << base << std::dec << std::endl;
	std::cout << "Executable: " << exeName << std::endl;
	std::cout << "Confirmed: Running in CHRONOCROSS.exe" << std::endl;
	std::cout << std::endl;

	// Load settings from INI file
	Settings settings;
	if (!settings.Load("settings.ini")) {
		std::cout << "Warning: Could not load settings.ini, using defaults" << std::endl;
	}

	// Check if widescreen is enabled
	bool widescreenEnabled = settings.GetBool("widescreen_enabled", true);
	int widescreenModeInt = settings.GetInt("widescreen_mode", 0);

	if (widescreenEnabled) {
		// Validate and convert widescreen mode
		WidescreenMode mode = WIDESCREEN_16_9;
		if (widescreenModeInt >= 0 && widescreenModeInt <= 2) {
			mode = static_cast<WidescreenMode>(widescreenModeInt);
		} else {
			std::cout << "Warning: Invalid widescreen_mode value (" << widescreenModeInt << "), defaulting to 16:9" << std::endl;
		}

		// Apply widescreen patch with selected mode
		if (!ApplyWidescreenPatch(base, mode)) {
			std::cout << "Failed to apply widescreen patch!" << std::endl;
		}
	} else {
		std::cout << "Widescreen patch disabled in settings" << std::endl;
	}

	// Check if double FPS mode is enabled
	bool doubleFpsEnabled = settings.GetBool("double_fps_mode", false);
	
	if (doubleFpsEnabled) {
		// Apply double FPS patch
		if (!ApplyDoubleFpsPatch(base)) {
			std::cout << "Failed to apply double FPS patch!" << std::endl;
		}
	} else {
		std::cout << "Double FPS mode disabled in settings" << std::endl;
	}

	// Check if disable pause on focus loss is enabled
	bool disablePauseEnabled = settings.GetBool("disable_pause_on_focus_loss", false);
	
	if (disablePauseEnabled) {
		// Apply disable pause patch
		if (!ApplyDisablePausePatch(base)) {
			std::cout << "Failed to apply disable pause patch!" << std::endl;
		}
	} else {
		std::cout << "Disable pause on focus loss disabled in settings" << std::endl;
	}

	// Run thread loop until END key is pressed
	while (!GetAsyncKeyState(VK_END)) {
		// Main thread loop

		Sleep(1000);
	}

	std::cout << "Exiting..." << std::endl;
	Sleep(1000);

	HWND consoleWindow = GetConsoleWindow();
	FreeConsole();
	if (consoleWindow != NULL) {
		PostMessage(consoleWindow, WM_CLOSE, 0, 0);
	}

	FreeLibraryAndExitThread((HMODULE)param, 0);
	return 0;
}


// Main DLL entry point
BOOL APIENTRY DllMain(HMODULE hModule,
	DWORD  ul_reason_for_call,
	LPVOID lpReserved
)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		// Handle proxy calls to version.dll, if using a proxy build config
		#if PROXY
			setupWrappers();
		#endif

		// Create thread
		CreateThread(0, 0, MainThread, hModule, 0, 0);
		break;
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

