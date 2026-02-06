#include "modloader.h"
#include "virtual_hd.h"
#include <Windows.h>
#include <MinHook.h>
#include <iostream>
#include <string>
#include <unordered_set>
#include <mutex>
#include <algorithm>
#include <filesystem>

// ============================================================================
// Types for original Win32 functions
// ============================================================================
typedef HANDLE(WINAPI* pfnCreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* pfnCreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL(WINAPI* pfnReadFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef DWORD(WINAPI* pfnSetFilePointer)(HANDLE, LONG, PLONG, DWORD);
typedef BOOL(WINAPI* pfnSetFilePointerEx)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD);
typedef DWORD(WINAPI* pfnGetFileSize)(HANDLE, LPDWORD);
typedef BOOL(WINAPI* pfnGetFileSizeEx)(HANDLE, PLARGE_INTEGER);
typedef BOOL(WINAPI* pfnCloseHandle)(HANDLE);

// Original function pointers (filled by MinHook)
static pfnCreateFileA     oCreateFileA     = nullptr;
static pfnCreateFileW     oCreateFileW     = nullptr;
static pfnReadFile        oReadFile        = nullptr;
static pfnSetFilePointer  oSetFilePointer  = nullptr;
static pfnSetFilePointerEx oSetFilePointerEx = nullptr;
static pfnGetFileSize     oGetFileSize     = nullptr;
static pfnGetFileSizeEx   oGetFileSizeEx   = nullptr;
static pfnCloseHandle     oCloseHandle     = nullptr;

// ============================================================================
// Virtual handle state
// ============================================================================
struct VirtualHandleState {
    HANDLE   realHandle;
    uint64_t virtualPos;
};

// Global state
static VirtualHd g_virtualHd;
static std::unordered_set<HANDLE> g_virtualHandles;
static std::mutex g_mutex;
static std::string g_modsDir;
static bool g_hooksInstalled = false;

// ============================================================================
// Helpers
// ============================================================================
static bool IsVirtualHandle(HANDLE h) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_virtualHandles.count(h) != 0;
}

static VirtualHandleState* GetState(HANDLE h) {
    return reinterpret_cast<VirtualHandleState*>(h);
}

static bool EndsWithHdDat(const char* path) {
    if (!path) return false;
    std::string p(path);
    std::replace(p.begin(), p.end(), '/', '\\');
    std::string lower = p;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    const char* suffix = "data\\hd.dat";
    size_t suffixLen = strlen(suffix);
    if (lower.size() >= suffixLen) {
        return lower.compare(lower.size() - suffixLen, suffixLen, suffix) == 0;
    }
    return false;
}

static bool EndsWithHdDatW(const wchar_t* path) {
    if (!path) return false;
    std::wstring p(path);
    std::replace(p.begin(), p.end(), L'/', L'\\');
    std::wstring lower = p;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    const wchar_t* suffix = L"data\\hd.dat";
    size_t suffixLen = wcslen(suffix);
    if (lower.size() >= suffixLen) {
        return lower.compare(lower.size() - suffixLen, suffixLen, suffix) == 0;
    }
    return false;
}

// ============================================================================
// Hooked CreateFileA
// ============================================================================
static HANDLE WINAPI HookedCreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess,
    DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    if (!EndsWithHdDat(lpFileName)) {
        return oCreateFileA(lpFileName, dwDesiredAccess, dwShareMode,
            lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
    }

    std::cout << "[ModLoader] Intercepted hd.dat open: " << lpFileName << std::endl;

    // Open the real file
    HANDLE realHandle = oCreateFileA(lpFileName, dwDesiredAccess, dwShareMode,
        lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

    if (realHandle == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }

    // Build virtual layout on first open
    if (!g_virtualHd.IsBuilt()) {
        if (!g_virtualHd.Build(realHandle, g_modsDir)) {
            std::cout << "[ModLoader] Failed to build virtual layout, passing through" << std::endl;
            return realHandle;
        }
    }

    // Allocate a VirtualHandleState — its pointer IS the sentinel handle
    VirtualHandleState* state = new VirtualHandleState();
    state->realHandle = realHandle;
    state->virtualPos = 0;

    HANDLE sentinel = reinterpret_cast<HANDLE>(state);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_virtualHandles.insert(sentinel);
    }

    std::cout << "[ModLoader] Virtual handle created, virtual size: "
              << g_virtualHd.GetVirtualSize() << " bytes" << std::endl;

    return sentinel;
}

// ============================================================================
// Hooked CreateFileW
// ============================================================================
static HANDLE WINAPI HookedCreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess,
    DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    if (!EndsWithHdDatW(lpFileName)) {
        return oCreateFileW(lpFileName, dwDesiredAccess, dwShareMode,
            lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
    }

    std::wcout << L"[ModLoader] Intercepted hd.dat open (W): " << lpFileName << std::endl;

    // Open the real file
    HANDLE realHandle = oCreateFileW(lpFileName, dwDesiredAccess, dwShareMode,
        lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

    if (realHandle == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }

    // Build virtual layout on first open
    if (!g_virtualHd.IsBuilt()) {
        if (!g_virtualHd.Build(realHandle, g_modsDir)) {
            std::cout << "[ModLoader] Failed to build virtual layout, passing through" << std::endl;
            return realHandle;
        }
    }

    // Allocate a VirtualHandleState — its pointer IS the sentinel handle
    VirtualHandleState* state = new VirtualHandleState();
    state->realHandle = realHandle;
    state->virtualPos = 0;

    HANDLE sentinel = reinterpret_cast<HANDLE>(state);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_virtualHandles.insert(sentinel);
    }

    std::cout << "[ModLoader] Virtual handle created, virtual size: "
              << g_virtualHd.GetVirtualSize() << " bytes" << std::endl;

    return sentinel;
}

// ============================================================================
// Hooked ReadFile
// ============================================================================
static BOOL WINAPI HookedReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
    LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped)
{
    if (!IsVirtualHandle(hFile)) {
        return oReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
    }

    VirtualHandleState* state = GetState(hFile);
    uint32_t bytesRead = 0;

    bool ok = g_virtualHd.ReadVirtual(state->realHandle, state->virtualPos,
        lpBuffer, nNumberOfBytesToRead, &bytesRead);

    state->virtualPos += bytesRead;

    if (lpNumberOfBytesRead) {
        *lpNumberOfBytesRead = bytesRead;
    }

    return ok ? TRUE : FALSE;
}

// ============================================================================
// Hooked SetFilePointer
// ============================================================================
static DWORD WINAPI HookedSetFilePointer(HANDLE hFile, LONG lDistanceToMove,
    PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod)
{
    if (!IsVirtualHandle(hFile)) {
        return oSetFilePointer(hFile, lDistanceToMove, lpDistanceToMoveHigh, dwMoveMethod);
    }

    VirtualHandleState* state = GetState(hFile);
    uint64_t virtualSize = g_virtualHd.GetVirtualSize();

    int64_t distance;
    if (lpDistanceToMoveHigh) {
        distance = (int64_t)(((uint64_t)(uint32_t)*lpDistanceToMoveHigh << 32) | (uint64_t)(uint32_t)lDistanceToMove);
    } else {
        distance = (int64_t)lDistanceToMove;
    }

    int64_t newPos;
    switch (dwMoveMethod) {
    case FILE_BEGIN:
        newPos = distance;
        break;
    case FILE_CURRENT:
        newPos = (int64_t)state->virtualPos + distance;
        break;
    case FILE_END:
        newPos = (int64_t)virtualSize + distance;
        break;
    default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_SET_FILE_POINTER;
    }

    if (newPos < 0) {
        SetLastError(ERROR_NEGATIVE_SEEK);
        return INVALID_SET_FILE_POINTER;
    }

    state->virtualPos = (uint64_t)newPos;

    if (lpDistanceToMoveHigh) {
        *lpDistanceToMoveHigh = (LONG)(state->virtualPos >> 32);
    }

    return (DWORD)(state->virtualPos & 0xFFFFFFFF);
}

// ============================================================================
// Hooked SetFilePointerEx
// ============================================================================
static BOOL WINAPI HookedSetFilePointerEx(HANDLE hFile, LARGE_INTEGER liDistanceToMove,
    PLARGE_INTEGER lpNewFilePointer, DWORD dwMoveMethod)
{
    if (!IsVirtualHandle(hFile)) {
        return oSetFilePointerEx(hFile, liDistanceToMove, lpNewFilePointer, dwMoveMethod);
    }

    VirtualHandleState* state = GetState(hFile);
    uint64_t virtualSize = g_virtualHd.GetVirtualSize();

    int64_t distance = liDistanceToMove.QuadPart;
    int64_t newPos;

    switch (dwMoveMethod) {
    case FILE_BEGIN:
        newPos = distance;
        break;
    case FILE_CURRENT:
        newPos = (int64_t)state->virtualPos + distance;
        break;
    case FILE_END:
        newPos = (int64_t)virtualSize + distance;
        break;
    default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (newPos < 0) {
        SetLastError(ERROR_NEGATIVE_SEEK);
        return FALSE;
    }

    state->virtualPos = (uint64_t)newPos;

    if (lpNewFilePointer) {
        lpNewFilePointer->QuadPart = (LONGLONG)state->virtualPos;
    }

    return TRUE;
}

// ============================================================================
// Hooked GetFileSize
// ============================================================================
static DWORD WINAPI HookedGetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh) {
    if (!IsVirtualHandle(hFile)) {
        return oGetFileSize(hFile, lpFileSizeHigh);
    }

    uint64_t virtualSize = g_virtualHd.GetVirtualSize();

    if (lpFileSizeHigh) {
        *lpFileSizeHigh = (DWORD)(virtualSize >> 32);
    }

    return (DWORD)(virtualSize & 0xFFFFFFFF);
}

// ============================================================================
// Hooked GetFileSizeEx
// ============================================================================
static BOOL WINAPI HookedGetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize) {
    if (!IsVirtualHandle(hFile)) {
        return oGetFileSizeEx(hFile, lpFileSize);
    }

    if (lpFileSize) {
        lpFileSize->QuadPart = (LONGLONG)g_virtualHd.GetVirtualSize();
    }

    return TRUE;
}

// ============================================================================
// Hooked CloseHandle
// ============================================================================
static BOOL WINAPI HookedCloseHandle(HANDLE hObject) {
    if (!IsVirtualHandle(hObject)) {
        return oCloseHandle(hObject);
    }

    VirtualHandleState* state = GetState(hObject);

    // Close the real handle
    if (state->realHandle && state->realHandle != INVALID_HANDLE_VALUE) {
        oCloseHandle(state->realHandle);
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_virtualHandles.erase(hObject);
    }

    delete state;

    std::cout << "[ModLoader] Virtual handle closed" << std::endl;
    return TRUE;
}

// ============================================================================
// Helper to create a single hook
// ============================================================================
static bool CreateHookHelper(LPCWSTR moduleName, LPCSTR procName, LPVOID detour, LPVOID* original) {
    MH_STATUS status = MH_CreateHookApi(moduleName, procName, detour, original);
    if (status != MH_OK) {
        std::cout << "[ModLoader] Failed to hook " << procName << ": "
                  << MH_StatusToString(status) << std::endl;
        return false;
    }
    std::cout << "[ModLoader] Hooked " << procName << std::endl;
    return true;
}

// ============================================================================
// Public API
// ============================================================================
bool InitModLoader(const std::string& exePath) {
    // Derive mods directory from exe path
    std::string exeDir;
    size_t lastSlash = exePath.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        exeDir = exePath.substr(0, lastSlash + 1);
    }

    g_modsDir = exeDir + "mods";

    // Check if mods directory exists
    if (!std::filesystem::exists(g_modsDir) || !std::filesystem::is_directory(g_modsDir)) {
        std::cout << "[ModLoader] No mods/ directory found, mod loader disabled" << std::endl;
        return false;
    }

    std::cout << "[ModLoader] Mods directory found: " << g_modsDir << std::endl;

    // Initialize MinHook
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        std::cout << "[ModLoader] MH_Initialize failed: " << MH_StatusToString(status) << std::endl;
        return false;
    }

    // Create hooks for all 7 Win32 APIs
    bool allOk = true;
    allOk &= CreateHookHelper(L"kernel32", "CreateFileA",      (LPVOID)HookedCreateFileA,      (LPVOID*)&oCreateFileA);
    allOk &= CreateHookHelper(L"kernel32", "CreateFileW",      (LPVOID)HookedCreateFileW,      (LPVOID*)&oCreateFileW);
    allOk &= CreateHookHelper(L"kernel32", "ReadFile",          (LPVOID)HookedReadFile,          (LPVOID*)&oReadFile);
    allOk &= CreateHookHelper(L"kernel32", "SetFilePointer",    (LPVOID)HookedSetFilePointer,    (LPVOID*)&oSetFilePointer);
    allOk &= CreateHookHelper(L"kernel32", "SetFilePointerEx",  (LPVOID)HookedSetFilePointerEx,  (LPVOID*)&oSetFilePointerEx);
    allOk &= CreateHookHelper(L"kernel32", "GetFileSize",       (LPVOID)HookedGetFileSize,       (LPVOID*)&oGetFileSize);
    allOk &= CreateHookHelper(L"kernel32", "GetFileSizeEx",     (LPVOID)HookedGetFileSizeEx,     (LPVOID*)&oGetFileSizeEx);
    allOk &= CreateHookHelper(L"kernel32", "CloseHandle",       (LPVOID)HookedCloseHandle,       (LPVOID*)&oCloseHandle);

    if (!allOk) {
        std::cout << "[ModLoader] Some hooks failed to install" << std::endl;
        MH_Uninitialize();
        return false;
    }

    // Enable all hooks
    status = MH_EnableHook(MH_ALL_HOOKS);
    if (status != MH_OK) {
        std::cout << "[ModLoader] MH_EnableHook failed: " << MH_StatusToString(status) << std::endl;
        MH_Uninitialize();
        return false;
    }

    g_hooksInstalled = true;
    std::cout << "[ModLoader] All hooks installed and enabled" << std::endl;

    return true;
}
