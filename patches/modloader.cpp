#include "modloader.h"
#include "virtual_hd.h"
#include <Windows.h>
#include <MinHook.h>
#include <iostream>
#include <string>
#include <unordered_map>
#include <mutex>
#include <algorithm>
#include <filesystem>
#include <cstdint>
#include <memory>

// ============================================================================
// Function pointer types
// ============================================================================
typedef HANDLE(WINAPI* pfnCreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL(WINAPI* pfnCloseHandle)(HANDLE);
typedef BOOL(WINAPI* pfnReadFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL(WINAPI* pfnSetFilePointerEx)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD);
typedef DWORD(WINAPI* pfnSetFilePointer)(HANDLE, LONG, PLONG, DWORD);
typedef BOOL(WINAPI* pfnGetFileSizeEx)(HANDLE, PLARGE_INTEGER);

// Originals
static pfnCreateFileW      oCreateFileW      = nullptr;
static pfnCloseHandle      oCloseHandle      = nullptr;
static pfnReadFile         oReadFile         = nullptr;
static pfnSetFilePointerEx oSetFilePointerEx = nullptr;
static pfnSetFilePointer   oSetFilePointer   = nullptr;
static pfnGetFileSizeEx    oGetFileSizeEx    = nullptr;

// ============================================================================
// Global state
// ============================================================================
static std::string g_modsDir;
static bool g_hooksInstalled = false;
static std::mutex g_mutex;

// Per-.dat state: key = base name (e.g. "hd", "cdrom")
struct DatState {
    VirtualHd virtualHd;
    bool built = false;
    uint64_t viewSize = 0;
    bool useReadFileSynthesis = false;  // serve via ReadFile only (no virtual buffer)
};
static std::unordered_map<std::string, std::unique_ptr<DatState>> g_datStates;

// Which .dat key does each file handle belong to
static std::unordered_map<HANDLE, std::string> g_handleToDatKey;

// Per-handle file position for ReadFile path
static std::unordered_map<HANDLE, uint64_t> g_datPosition;

// ============================================================================
// Helpers
// ============================================================================
// If path ends with .dat (case-insensitive), return the base name (e.g. "hd", "cdrom"); else empty string.
static std::string GetDatKeyFromPathW(const wchar_t* path) {
    if (!path) return {};
    std::wstring p(path);
    std::replace(p.begin(), p.end(), L'/', L'\\');
    std::wstring lower = p;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    const wchar_t* suffix = L".dat";
    size_t suffixLen = wcslen(suffix);
    if (lower.size() < suffixLen) return {};
    if (lower.compare(lower.size() - suffixLen, suffixLen, suffix) != 0) return {};
    size_t nameStart = lower.find_last_of(L"\\/");
    size_t start = (nameStart == std::wstring::npos) ? 0 : nameStart + 1;
    size_t nameLen = lower.size() - suffixLen - start;
    if (nameLen == 0) return {};
    std::string key;
    key.reserve(nameLen);
    for (size_t i = 0; i < nameLen; i++)
        key += (char)lower[start + i];
    return key;
}

// Only intercept .dat files in the game's data folder (e.g. data\hd.dat), not save.dat in Documents etc.
static bool IsGameDataPathW(const wchar_t* path) {
    if (!path) return false;
    std::wstring p(path);
    std::replace(p.begin(), p.end(), L'/', L'\\');
    std::wstring lower = p;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    // Relative path "data\hd.dat" or "data/hd.dat"
    if (lower.size() >= 5 && (lower.compare(0, 5, L"data\\") == 0 || lower.compare(0, 5, L"data/") == 0))
        return true;
    // Path contains \data\ or /data/ (e.g. "C:\game\data\hd.dat")
    return lower.find(L"\\data\\") != std::wstring::npos || lower.find(L"/data/") != std::wstring::npos;
}

// Blacklist: do not intercept these .dat keys (e.g. save files)
static bool IsBlacklistedDatKey(const std::string& datKey) {
    return datKey == "save";
}

// ============================================================================
// CreateFileW — tag .dat handles, build layout on first open per dat type
// ============================================================================
static HANDLE WINAPI HookedCreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess,
    DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    HANDLE h = oCreateFileW(lpFileName, dwDesiredAccess, dwShareMode,
        lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

    std::string datKey = GetDatKeyFromPathW(lpFileName);
    if (h != INVALID_HANDLE_VALUE && !datKey.empty() && IsGameDataPathW(lpFileName) && !IsBlacklistedDatKey(datKey)) {
        std::wcout << L"[ModLoader] Intercepted .dat open: " << lpFileName << std::endl;

        std::string modsSubdir = g_modsDir + "/" + datKey;
        DatState* state = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto& ptr = g_datStates[datKey];
            if (!ptr) ptr = std::make_unique<DatState>();
            state = ptr.get();
        }

        if (!state->built) {
            if (state->virtualHd.Build(h, modsSubdir)) {
                state->built = true;
                if (state->virtualHd.HasMods()) {
                    state->viewSize = state->virtualHd.GetVirtualSize();
                    state->useReadFileSynthesis = true;
                    std::cout << "[ModLoader] Using ReadFile synthesis for " << datKey << ".dat (" << state->viewSize << " bytes)" << std::endl;
                }
            } else {
                std::cout << "[ModLoader] Failed to build virtual layout for " << datKey << ".dat" << std::endl;
            }
        }

        if (state->useReadFileSynthesis) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_handleToDatKey[h] = datKey;
            g_datPosition[h] = 0;
        }
    }

    return h;
}

// ============================================================================
// ReadFile — for .dat handles, serve from ReadFile synthesis
// ============================================================================
static BOOL WINAPI HookedReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
    LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped)
{
    if (lpOverlapped) {
        return oReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
    }

    std::string datKey;
    DatState* state = nullptr;
    uint64_t* pPos = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto keyIt = g_handleToDatKey.find(hFile);
        if (keyIt != g_handleToDatKey.end()) {
            datKey = keyIt->second;
            auto st = g_datStates.find(datKey);
            if (st != g_datStates.end() && st->second && st->second->useReadFileSynthesis) {
                state = st->second.get();
                auto posIt = g_datPosition.find(hFile);
                if (posIt != g_datPosition.end())
                    pPos = &posIt->second;
            }
        }
    }

    if (!pPos || !state) {
        return oReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
    }

    uint64_t pos = *pPos;
    uint64_t viewSize = state->viewSize;
    uint64_t remaining = (viewSize > pos) ? (viewSize - pos) : 0;
    DWORD toRead = (DWORD)(std::min)((uint64_t)nNumberOfBytesToRead, remaining);

    if (toRead > 0) {
        size_t got = state->virtualHd.ReadAtVirtualOffset(hFile, pos, lpBuffer, toRead,
            oReadFile, oSetFilePointerEx);
        toRead = (DWORD)got;
        *pPos = pos + toRead;
    }
    if (lpNumberOfBytesRead)
        *lpNumberOfBytesRead = toRead;
    return TRUE;
}

// ============================================================================
// SetFilePointerEx — for .dat handles, track position for ReadFile path
// ============================================================================
static BOOL WINAPI HookedSetFilePointerEx(HANDLE hFile, LARGE_INTEGER liDistanceToMove,
    PLARGE_INTEGER lpNewFilePointerHigh, DWORD dwMoveMethod)
{
    std::string datKey;
    uint64_t viewSize = 0;
    bool intercept = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto keyIt = g_handleToDatKey.find(hFile);
        if (keyIt != g_handleToDatKey.end()) {
            datKey = keyIt->second;
            auto st = g_datStates.find(datKey);
            if (st != g_datStates.end() && st->second && st->second->useReadFileSynthesis) {
                viewSize = st->second->viewSize;
                intercept = true;
            }
        }
    }

    if (!intercept) {
        return oSetFilePointerEx(hFile, liDistanceToMove, lpNewFilePointerHigh, dwMoveMethod);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_datPosition.find(hFile);
    if (it == g_datPosition.end()) return oSetFilePointerEx(hFile, liDistanceToMove, lpNewFilePointerHigh, dwMoveMethod);

    int64_t offset = (int64_t)liDistanceToMove.QuadPart;
    uint64_t newPos = 0;
    switch (dwMoveMethod) {
        case FILE_BEGIN:   newPos = (offset >= 0) ? (uint64_t)offset : 0; break;
        case FILE_CURRENT: newPos = (int64_t)it->second + offset; if (newPos < 0) newPos = 0; break;
        case FILE_END:     newPos = (int64_t)viewSize + offset; if (newPos < 0) newPos = 0; break;
        default:           return oSetFilePointerEx(hFile, liDistanceToMove, lpNewFilePointerHigh, dwMoveMethod);
    }
    if (newPos > viewSize) newPos = viewSize;
    it->second = newPos;
    if (lpNewFilePointerHigh)
        lpNewFilePointerHigh->QuadPart = (LONGLONG)newPos;
    return TRUE;
}

// ============================================================================
// SetFilePointer — for hd.dat handles, track position (32-bit API)
// ============================================================================
static DWORD WINAPI HookedSetFilePointer(HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod) {
    LARGE_INTEGER li;
    li.LowPart = (DWORD)lDistanceToMove;
    li.HighPart = lpDistanceToMoveHigh ? *lpDistanceToMoveHigh : 0;
    LARGE_INTEGER newPos = {};
    if (!HookedSetFilePointerEx(hFile, li, &newPos, dwMoveMethod))
        return INVALID_SET_FILE_POINTER;
    if (lpDistanceToMoveHigh)
        *lpDistanceToMoveHigh = (LONG)(newPos.HighPart);
    return (DWORD)newPos.LowPart;
}

// ============================================================================
// GetFileSizeEx — for .dat handles, return virtual size
// ============================================================================
static BOOL WINAPI HookedGetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize) {
    uint64_t viewSize = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto keyIt = g_handleToDatKey.find(hFile);
        if (keyIt != g_handleToDatKey.end()) {
            auto st = g_datStates.find(keyIt->second);
            if (st != g_datStates.end() && st->second && st->second->useReadFileSynthesis)
                viewSize = st->second->viewSize;
        }
    }
    if (viewSize > 0 && lpFileSize) {
        lpFileSize->QuadPart = (LONGLONG)viewSize;
        return TRUE;
    }
    return oGetFileSizeEx(hFile, lpFileSize);
}

// ============================================================================
// CloseHandle — clean up tracking
// ============================================================================
static BOOL WINAPI HookedCloseHandle(HANDLE hObject) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_handleToDatKey.erase(hObject);
        g_datPosition.erase(hObject);
    }
    return oCloseHandle(hObject);
}

// ============================================================================
// Hook helper
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
    std::string exeDir;
    size_t lastSlash = exePath.find_last_of("\\/");
    if (lastSlash != std::string::npos)
        exeDir = exePath.substr(0, lastSlash + 1);

    g_modsDir = exeDir + "mods";

    if (!std::filesystem::exists(g_modsDir) || !std::filesystem::is_directory(g_modsDir)) {
        std::cout << "[ModLoader] No mods/ directory found, mod loader disabled" << std::endl;
        return false;
    }

    std::cout << "[ModLoader] Mods directory found: " << g_modsDir << std::endl;

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        std::cout << "[ModLoader] MH_Initialize failed: " << MH_StatusToString(status) << std::endl;
        return false;
    }

    bool allOk = true;
    allOk &= CreateHookHelper(L"kernel32", "CreateFileW",      (LPVOID)HookedCreateFileW,      (LPVOID*)&oCreateFileW);
    allOk &= CreateHookHelper(L"kernel32", "ReadFile",         (LPVOID)HookedReadFile,         (LPVOID*)&oReadFile);
    allOk &= CreateHookHelper(L"kernel32", "SetFilePointerEx", (LPVOID)HookedSetFilePointerEx, (LPVOID*)&oSetFilePointerEx);
    allOk &= CreateHookHelper(L"kernel32", "SetFilePointer",   (LPVOID)HookedSetFilePointer,   (LPVOID*)&oSetFilePointer);
    allOk &= CreateHookHelper(L"kernel32", "GetFileSizeEx",    (LPVOID)HookedGetFileSizeEx,   (LPVOID*)&oGetFileSizeEx);
    allOk &= CreateHookHelper(L"kernel32", "CloseHandle",      (LPVOID)HookedCloseHandle,     (LPVOID*)&oCloseHandle);

    if (!allOk) {
        std::cout << "[ModLoader] Some hooks failed to install" << std::endl;
        MH_Uninitialize();
        return false;
    }

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
