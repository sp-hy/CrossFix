# Chrono Cross Widescreen Fix - Proxy DLL

This project provides a `winmm.dll` proxy DLL that patches the PC version of Chrono Cross to support widescreen resolutions and enhanced frame rates.

## Project Structure

```
crossfix/
├── dllmain.cpp              # Main entry point
├── settings.ini             # Configuration file
├── patches/                 # Game patches
│   ├── widescreen.cpp       # Widescreen fix implementation
│   ├── widescreen.h
│   ├── fps.cpp              # FPS unlock implementation
│   ├── fps.h
│   ├── pausefix.cpp         # Disable pause on focus loss
│   └── pausefix.h
├── utils/                   # Utility functions
│   ├── memory.cpp           # Memory patching utilities
│   ├── memory.h
│   ├── settings.cpp         # INI file parser
│   └── settings.h
└── winmm/                   # winmm.dll proxy implementation
    ├── winmm.h              # Proxy header
    ├── winmm.def            # Export definitions
    └── 32/
        └── winmm_32.asm     # 32-bit assembly wrappers
```

## Configuration

The DLL can be configured via `settings.ini` in the game directory:

```ini
# Enable or disable the widescreen patch
# 0 = disabled, 1 = enabled
widescreen_enabled=1

# Widescreen aspect ratio mode
# 0 = 16:9 (standard widescreen)
# 1 = 21:9 (ultrawide)
# 2 = 32:9 (super ultrawide)
widescreen_mode=0

# Enable or disable the double FPS mode
# 0 = disabled (30 FPS), 1 = enabled (60 FPS)
double_fps_mode=0

# Disable pause when window loses focus
# 0 = game pauses when window is inactive (default behavior)
# 1 = game continues running when window is inactive
disable_pause_on_focus_loss=1
```

## Building

1. Open `crossfix.sln` in Visual Studio
2. Select either configuration:
   - **Debug - Proxy | Win32** for debugging
   - **Release - Proxy | Win32** for production
3. Build the project
4. The output will be `winmm.dll` in the respective build folder

## Installation

1. Copy the compiled `winmm.dll` to the Chrono Cross game directory (where `CHRONOCROSS.exe` is located)
2. Copy `settings.ini` to the same directory
3. Adjust settings in `settings.ini` as desired
4. Launch the game normally
5. The widescreen patch will be applied automatically based on your settings

## How It Works

### Proxy DLL
The DLL acts as a proxy for `winmm.dll`, forwarding all Windows Multimedia API calls to the real system `winmm.dll` while allowing our code to run in the game's process.

### Widescreen Patch

The widescreen fix modifies two locations in the game code that handle horizontal offset calculations:
- `CHRONOCROSS.exe+18EE7B`
- `CHRONOCROSS.exe+18AD25`

Original code (8 bytes):
```asm
mov eax,[esi+60]  ; 8B 46 60
cdq               ; 99
add edi,eax       ; 03 F8
adc ecx,edx       ; 13 CA
```

The patch replaces these with a call to a hook function that adjusts the coordinate based on the selected aspect ratio:

#### 16:9 Mode (multiply by 3/4)
```asm
push eax
push edx
mov eax, edi
imul eax, eax, 3
sar eax, 2           ; divide by 4
mov edi, eax
pop edx
pop eax
; ... then original code
```

#### 21:9 Mode (multiply by 4/7)
```asm
push eax
push edx
push ebx
mov eax, edi
imul eax, eax, 4
mov ebx, 7
cdq
idiv ebx
mov edi, eax
pop ebx
pop edx
pop eax
; ... then original code
```

#### 32:9 Mode (multiply by 3/8)
```asm
push eax
push edx
mov eax, edi
imul eax, eax, 3
sar eax, 3           ; divide by 8
mov edi, eax
pop edx
pop eax
; ... then original code
```

### Double FPS Patch

The double FPS patch unlocks the game's frame rate from 30 FPS to 60 FPS by modifying timing-related values at two locations:
- `CHRONOCROSS.exe+188A6D`
- `CHRONOCROSS.exe+18557E`

When enabled, the patch writes the 4-byte value `0x0000200D` to both addresses, which adjusts the game's internal frame timing to run at 60 FPS instead of the default 30 FPS.

**Note:** This patch may affect game speed and physics. Test thoroughly to ensure gameplay remains stable.

### Disable Pause on Focus Loss Patch

This patch prevents the game from pausing when the window loses focus, allowing it to continue running in the background:
- `CHRONOCROSS.exe+184096`

When enabled, the patch changes the conditional jump instruction from `JNE` (Jump if Not Equal, `0F 85`) to `JE` (Jump if Equal, `0F 84`). This inverts the focus check logic, making the game continue running even when the window is inactive.

**Use case:** Useful for streaming, recording, or multitasking while the game runs in the background.

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
- Current settings loaded from INI
- Patch application status
- Any errors that occur

Press the **END** key to unload the DLL (console will remain until game exit).

## Notes

- This is a 32-bit DLL for the 32-bit PC version of Chrono Cross
- The DLL must be named `winmm.dll` to work as a proxy
- Back up the original game files before using
- If `settings.ini` is not found, defaults will be used (widescreen enabled, 16:9 mode)
- **Safety:** The DLL verifies it's running in `CHRONOCROSS.exe` before applying patches. If loaded by other executables in the game directory, it will exit gracefully without applying any modifications.
