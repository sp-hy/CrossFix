/*
 * Widescreen 2D System
 * 
 * This system handles widescreen transformations for 2D background layers in Chrono Cross.
 * Instead of relying on layer 0 as a reference, it uses actual room dimension data from
 * roomData.cpp to accurately scale and position each layer independently.
 * 
 * The hook intercepts layer processing during level load and:
 * 1. Extracts the room name from the file path
 * 2. Looks up the room's viewport dimensions (width, X offset) from RoomData
 * 3. Calculates the new scaled dimensions based on the widescreen ratio
 * 4. Applies transformations to each layer's width and X position
 * 
 * This approach ensures consistent and accurate widescreen rendering across all rooms.
 */

#include "widescreen2d.h"
#include "../data/roomData.h"
#include <cmath>

// Global variables
bool g_widescreen2DEnabled = true;
float g_widescreenRatio = 0.75f;

// Hook-related globals
static LPVOID g_hookAddress = nullptr;
static BYTE g_originalBytes[16];


// C++ wrapper function to get room data from path string
// This is called from assembly, so we use __cdecl and keep it simple
extern "C" const ViewportRect* __cdecl GetRoomDataFromPath(const char* path) {
    // Validate pointer
    if (!path || IsBadReadPtr(path, 1)) {
        return nullptr;
    }
    
    // Find the last backslash or forward slash
    const char* lastSlash = path;
    for (const char* p = path; *p && !IsBadReadPtr(p, 1); p++) {
        if (*p == '\\' || *p == '/') {
            lastSlash = p + 1;
        }
    }
    
    const char* filename = lastSlash;
    
    // Find the first dot
    const char* dot = nullptr;
    for (const char* p = filename; *p && !IsBadReadPtr(p, 1); p++) {
        if (*p == '.') {
            dot = p;
            break;
        }
    }
    
    // Extract room name
    char roomName[64];
    size_t len = dot ? (size_t)(dot - filename) : 0;
    if (len == 0) {
        for (const char* p = filename; *p && !IsBadReadPtr(p, 1); p++) len++;
    }
    if (len >= 64) len = 63;
    if (len == 0) return nullptr;
    
    for (size_t i = 0; i < len; i++) {
        roomName[i] = filename[i];
    }
    roomName[len] = '\0';
    
    // Look up room data
    std::string roomNameStr(roomName);
    return RoomData::get(roomNameStr);
}

// The cave where our custom code will execute
__declspec(naked) void HookCave() {
    __asm {
        // Execute the original instruction first
        mov eax, [ebp-70h]
        
        // Save all registers we'll use
        push ebx
        push ecx
        push edx
        push esi
        push edi
        pushfd
        
        // Check if widescreen is enabled
        mov cl, byte ptr [g_widescreen2DEnabled]
        test cl, cl
        jz skip_transform
        
        // Check if width is valid (skip invalid entries)
        movsx ecx, word ptr [ebp-38h]  // Read layer width as signed word
        test ecx, ecx
        jle skip_transform
        
        // === Get room data from path (for every layer) ===
        // Get the address of the room path string at [ebp+70h]
        lea esi, [ebp+70h]
        test esi, esi
        jz skip_transform
        
        // Save current ESP and align stack to 16 bytes for C++ call
        mov edi, esp
        and esp, 0FFFFFFF0h
        sub esp, 12              // Subtract 12 so after push (4 bytes) we're at 16-byte aligned
        
        // Call GetRoomDataFromPath(path)
        push esi                 // Now ESP is 16-byte aligned
        call GetRoomDataFromPath
        
        // Restore original stack pointer
        mov esp, edi

        
        // eax now contains ViewportRect* (or nullptr)
        test eax, eax
        jz skip_transform  // If no room data found, skip transformation entirely
        
        // Extract room width from ViewportRect (offset +8 for width field)
        mov ebx, [eax + 8]  // ebx = room width
        
        // Extract room X from ViewportRect (offset +0 for x field)
        mov edx, [eax + 0]  // edx = room X

        // Save room width for boundaries calculation later
        push ebx                      // Save original room width on stack
        
        // === Transform the layer ===
        // At this point:
        // ebx = room width (original)
        // edx = room X (original)
        // ecx = layer width (original, from earlier)
        
        // Save original room X for later calculation
        push edx                      // Save original room X on stack
        
        // Calculate new room width
        cvtsi2ss xmm0, ebx
        movss xmm1, dword ptr [g_widescreenRatio]
        mulss xmm0, xmm1
        cvttss2si edi, xmm0           // edi = new room width
        
        // Calculate centering offset for room
        sub ebx, edi                  // ebx = original room width - new room width
        sar ebx, 1                    // ebx = padding
        
        add edx, ebx                  // edx = new room X
        push edx                      // Save new room X on stack
        
        // Now transform the layer
        // Read layer's original width and X and SAVE them in registers
        movsx ebx, word ptr [ebp-38h]  // ebx = layer width (ORIGINAL - save it!)
        movsx edi, word ptr [ebp-3Ch]  // edi = layer X (ORIGINAL - save it!)
        
        // Calculate new layer width (use ecx for calculation)
        mov ecx, ebx                  // ecx = original width
        cvtsi2ss xmm0, ecx
        movss xmm1, dword ptr [g_widescreenRatio]
        mulss xmm0, xmm1
        cvttss2si ecx, xmm0           // ecx = new layer width
        
        // Validate new width
        test ecx, ecx
        jle cleanup_and_skip          // Skip if invalid
        cmp ecx, 10000
        jge cleanup_and_skip
        
        // Write new layer width
        mov word ptr [ebp-38h], cx
        
        // Calculate new layer X using formula: x_new = newRoomX + (x_old - roomX) * ratio
        // edi still contains original layer X
        mov esi, edi                  // esi = original layer X
        sub esi, [esp + 4]            // esi = x_old - roomX (read directly from stack)
        
        cvtsi2ss xmm0, esi
        movss xmm1, dword ptr [g_widescreenRatio]
        mulss xmm0, xmm1
        cvttss2si esi, xmm0           // esi = (x_old - roomX) * ratio
        
        add esi, [esp]                // esi = newRoomX + scaled offset (read directly from stack)
        
        // Write new layer X
        mov word ptr [ebp-3Ch], si
        
        // === Boundaries Patch ===
        // Calculate camera boundaries based on NEW room width
        // Formula: left_boundary = (new_room_width - 320) / 2, right_boundary = -left_boundary
        
        // Recalculate new room width from original room width
        // Original room width is at [esp+8]
        mov ebx, [esp + 8]            // ebx = original room width
        
        // Calculate new room width: original * ratio
        cvtsi2ss xmm0, ebx
        movss xmm1, dword ptr [g_widescreenRatio]
        mulss xmm0, xmm1
        cvttss2si ebx, xmm0           // ebx = new room width
        
        // Calculate (new_room_width - 320) / 2
        sub ebx, 320                  // ebx = new_room_width - 320
        sar ebx, 1                    // ebx = (new_room_width - 320) / 2 = left boundary
        
        // Clamp left boundary to minimum of 0
        test ebx, ebx                 // Check if ebx < 0
        jge left_boundary_ok          // If >= 0, skip clamping
        xor ebx, ebx                  // Set ebx to 0
    left_boundary_ok:
        
        // Calculate right boundary (negative of left)
        mov ecx, ebx                  // ecx = left boundary
        neg ecx                       // ecx = -left boundary = right boundary
        
        // Clamp right boundary to maximum of 0
        test ecx, ecx                 // Check if ecx > 0
        jle right_boundary_ok         // If <= 0, skip clamping
        xor ecx, ecx                  // Set ecx to 0
    right_boundary_ok:
        
        // Get base address of CHRONOCROSS.exe
        push eax
        push edx
        push 0
        call GetModuleHandleA
        test eax, eax
        jz skip_boundaries
        
        // Calculate address for left boundary (CHRONOCROSS.exe+719270)
        mov edi, eax                  // edi = base address
        add edi, 719270h              // edi = base + 0x719270
        
        // Write left boundary (2-byte signed)
        mov word ptr [edi], bx
        
        // Calculate address for right boundary (CHRONOCROSS.exe+719272)
        add edi, 2                    // edi = base + 0x719272
        
        // Write right boundary (2-byte signed)
        mov word ptr [edi], cx
        
skip_boundaries:
        pop edx
        pop eax
        
        // Clean up stack
        add esp, 12                   // Remove old room width, old room X, and new room X
        jmp skip_transform
        
cleanup_and_skip:
        add esp, 12                   // Remove all three pushed values

        
skip_transform:
        // Restore registers in reverse order
        popfd
        pop edi
        pop esi
        pop edx
        pop ecx
        pop ebx
        
        // CRITICAL: Restore EAX to the layer index (it was clobbered by GetRoomDataFromPath)
        mov eax, [ebp-70h]
        
        // Execute the second original instruction that we NOPed: mov edx,[ebp-38]
        // This loads the (now modified) width into edx
        mov edx, [ebp-38h]
        
        // eax contains layer index, edx contains width
        // Return
        ret
    }
}

bool InitWidescreen2DHook() {
    // Get the base address of CHRONOCROSS.exe
    HMODULE hModule = GetModuleHandleA("CHRONOCROSS.exe");
    if (!hModule) {
        return false;
    }
    
    // Calculate the absolute address of the hook point
    // CHRONOCROSS.exe+1CCF51: mov eax,[ebp-70] (3 bytes)
    // +1CCF54: mov edx,[ebp-38] (3 bytes)
    // Total: 6 bytes - enough for our 5-byte CALL + 1 NOP
    g_hookAddress = (LPVOID)((uintptr_t)hModule + 0x1CCF51);
    
#ifdef _DEBUG
    std::cout << "[Widescreen2D] Hook address: 0x" << std::hex << (uintptr_t)g_hookAddress << std::dec << std::endl;
#endif
    
    // Save original bytes (6 bytes to be safe)
    const int actualHookSize = 6;
    memcpy(g_originalBytes, g_hookAddress, actualHookSize);
    
    // Create the CALL instruction to our cave + NOP padding
    BYTE callInstruction[6];
    callInstruction[0] = 0xE8; // CALL opcode (relative)
    DWORD relativeAddress = (DWORD)((uintptr_t)HookCave - (uintptr_t)g_hookAddress - 5);
    memcpy(&callInstruction[1], &relativeAddress, 4);
    callInstruction[5] = 0x90; // NOP to pad to 6 bytes
    
    // Write the CALL instruction
    DWORD oldProtect;
    if (!VirtualProtect(g_hookAddress, actualHookSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    
    memcpy(g_hookAddress, callInstruction, actualHookSize);
    VirtualProtect(g_hookAddress, actualHookSize, oldProtect, &oldProtect);
    
#ifdef _DEBUG
    std::cout << "[Widescreen2D] ASM hook installed successfully" << std::endl;
#endif
    
    return true;
}

void SetWidescreen2DRatio(float ratio) {
    g_widescreenRatio = ratio;
#ifdef _DEBUG
    std::cout << "[Widescreen2D] Ratio set to: " << ratio << std::endl;
#endif
}

void SetWidescreen2DEnabled(bool enabled) {
    g_widescreen2DEnabled = enabled;
#ifdef _DEBUG
    std::cout << "[Widescreen2D] " << (enabled ? "Enabled" : "Disabled") << std::endl;
#endif
}
