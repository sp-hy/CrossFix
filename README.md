# Chrono Cross Widescreen Fix - Proxy DLL

This project provides a `winmm.dll` proxy DLL that patches the PC version of Chrono Cross to support widescreen resolutions.

## Project Structure

```
parasite/
├── dllmain.cpp              # Main entry point
├── patches/                 # Game patches
│   ├── widescreen.cpp       # Widescreen fix implementation
│   └── widescreen.h
├── utils/                   # Utility functions
│   ├── memory.cpp           # Memory patching utilities
│   └── memory.h
└── winmm/                   # winmm.dll proxy implementation
    ├── winmm.h              # Proxy header
    ├── winmm.def            # Export definitions
    └── 32/
        └── winmm_32.asm     # 32-bit assembly wrappers
```

## Building

1. Open `parasite.sln` in Visual Studio
2. Select either configuration:
   - **Debug - Proxy | Win32** for debugging
   - **Release - Proxy | Win32** for production
3. Build the project
4. The output will be `winmm.dll` in the respective build folder

## Installation

1. Copy the compiled `winmm.dll` to the Chrono Cross game directory (where `CHRONOCROSS.exe` is located)
2. Launch the game normally
3. The widescreen patch will be applied automatically

## How It Works

### Proxy DLL
The DLL acts as a proxy for `winmm.dll`, forwarding all Windows Multimedia API calls to the real system `winmm.dll` while allowing our code to run in the game's process.

### Widescreen Patch
The widescreen fix dynamically adjusts based on the current resolution by reading:
- Width: `CHRONOCROSS.exe+E2F3E0`
- Height: `CHRONOCROSS.exe+E2F3E4`

It modifies two locations in the game code that handle horizontal offset calculations:
- `CHRONOCROSS.exe+18EE7B`
- `CHRONOCROSS.exe+18AD25`

Original code:
```asm
mov eax,[esi+60]
cdq
add edi,eax
adc ecx,edx
```

Modified code dynamically calculates aspect ratio correction:
```asm
push eax
push edx
push ebx
push ecx

// Read resolution from memory
mov ebx, [g_resX]
mov eax, [ebx]        ; eax = width
mov ebx, [g_resY]
mov ebx, [ebx]        ; ebx = height

// Calculate: edi * (height*4) / (width*3)
imul ebx, 4           ; ebx = height * 4
imul eax, 3           ; eax = width * 3
mov ecx, eax
mov eax, edi
imul eax, ebx
cdq
idiv ecx              ; eax = adjusted value
mov edi, eax

pop ecx
pop ebx
pop edx
pop eax

; ... then original code
```

This approach automatically supports any aspect ratio:
- **16:9** (1920x1080, 2560x1440, etc.)
- **21:9** (2560x1080, 3440x1440, etc.)
- **32:9** (3840x1080, 5120x1440, etc.)
- Any custom resolution

## Adding More Patches

To add a new patch:

1. Create new files in `patches/` (e.g., `patches/framerate.cpp` and `patches/framerate.h`)
2. Implement your patch using the `WriteMemory` utility from `utils/memory.h`
3. Add the files to the Visual Studio project
4. Include your patch header in `dllmain.cpp`
5. Call your patch function in `MainThread()`

Example:
```cpp
// In dllmain.cpp
#include "patches/framerate.h"

// In MainThread()
if (!ApplyFrameratePatch(base)) {
    std::cout << "Failed to apply framerate patch!" << std::endl;
}
```

## Debug Console

The DLL opens a console window that shows:
- Base address of the game
- Patch application status
- Any errors that occur

Press the **END** key to unload the DLL (console will remain until game exit).

## Notes

- This is a 32-bit DLL for the 32-bit PC version of Chrono Cross
- The DLL must be named `winmm.dll` to work as a proxy
- Back up the original game files before using
