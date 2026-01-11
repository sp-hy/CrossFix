// dllmain.cpp : Defines the entry point for the DLL application.


// If using a proxy build config, include the necessary code to proxy calls to winmm.dll
#ifdef PROXY
    #include "winmm/winmm.h"
#else
    #include <Windows.h>
#endif

#include <iostream>
#include "patches/widescreen.h"
#include "patches/fps.h"
#include "utils/settings.h"


// Get handle from the injected process
HANDLE aoe_handle = GetCurrentProcess();

// Spawn thread to do work
DWORD WINAPI MainThread(LPVOID param) {
    // Get base address from the injected process
	uintptr_t base = (uintptr_t)GetModuleHandle(NULL);

	// Allocate a console
	AllocConsole();
	FILE* f;
	freopen_s(&f, "CONOUT$", "w", stdout);

	// Log a message
	std::cout << "DLL loaded successfully! Base address of the injected executable is: 0x" << std::hex << base << std::dec << std::endl;

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

	FreeConsole();
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
		// Close the console when the DLL is detached
		if (ul_reason_for_call == DLL_PROCESS_DETACH) {
			FreeConsole();
		}
		break;
	}
	return TRUE;
}

