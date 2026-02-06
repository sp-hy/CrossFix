#include "modloader.h"
#include "virtual_hd.h"
#include <Windows.h>
#include <MinHook.h>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <algorithm>
#include <filesystem>
#include <cstdint>
#include <memory>

// ============================================================================
// Function pointer types
// ============================================================================
typedef HANDLE(WINAPI* pfnCreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* pfnCreateFileMappingW)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);
typedef HANDLE(WINAPI* pfnCreateFileMappingA)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCSTR);
typedef LPVOID(WINAPI* pfnMapViewOfFile)(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
typedef BOOL(WINAPI* pfnUnmapViewOfFile)(LPCVOID);
typedef BOOL(WINAPI* pfnCloseHandle)(HANDLE);
typedef BOOL(WINAPI* pfnReadFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL(WINAPI* pfnSetFilePointerEx)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD);
typedef DWORD(WINAPI* pfnSetFilePointer)(HANDLE, LONG, PLONG, DWORD);
typedef BOOL(WINAPI* pfnGetFileSizeEx)(HANDLE, PLARGE_INTEGER);

// Originals
static pfnCreateFileW        oCreateFileW        = nullptr;
static pfnCreateFileMappingW oCreateFileMappingW = nullptr;
static pfnCreateFileMappingA oCreateFileMappingA = nullptr;
static pfnMapViewOfFile      oMapViewOfFile      = nullptr;
static pfnUnmapViewOfFile    oUnmapViewOfFile    = nullptr;
static pfnCloseHandle        oCloseHandle        = nullptr;
static pfnReadFile           oReadFile           = nullptr;
static pfnSetFilePointerEx   oSetFilePointerEx   = nullptr;
static pfnSetFilePointer     oSetFilePointer     = nullptr;
static pfnGetFileSizeEx      oGetFileSizeEx      = nullptr;

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
    uint8_t* cachedView = nullptr;
    uint64_t viewSize = 0;
    int mapViewRefCount = 0;
};
static std::unordered_map<std::string, std::unique_ptr<DatState>> g_datStates;

// Which .dat key does each file handle / mapping / view belong to
static std::unordered_map<HANDLE, std::string> g_handleToDatKey;
static std::unordered_map<HANDLE, std::string> g_mappingToDatKey;
static std::unordered_map<LPCVOID, std::string> g_viewToDatKey;

// Track VirtualAlloc'd buffers we returned from MapViewOfFile (for Unmap detection)
static std::unordered_set<LPCVOID> g_virtualViews;

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
                // Only build virtual view and intercept when this archive has mods
                if (state->virtualHd.HasMods()) {
                    LARGE_INTEGER fileSize;
                    if (oGetFileSizeEx(h, &fileSize) && fileSize.QuadPart > 0) {
                        HANDLE hMap = oCreateFileMappingW(h, NULL, PAGE_READONLY, 0, 0, NULL);
                        if (hMap) {
                            LPVOID realView = oMapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
                            if (realView) {
                                uint8_t* virtualBuf = state->virtualHd.CreateVirtualView((const uint8_t*)realView);
                                oUnmapViewOfFile(realView);
                                if (virtualBuf) {
                                    state->viewSize = state->virtualHd.GetVirtualSize();
                                    DWORD oldProtect;
                                    VirtualProtect(virtualBuf, (SIZE_T)state->viewSize, PAGE_READONLY, &oldProtect);
                                    std::lock_guard<std::mutex> lock(g_mutex);
                                    state->cachedView = virtualBuf;
                                    g_virtualViews.insert(virtualBuf);
                                    g_viewToDatKey[virtualBuf] = datKey;
                                    std::cout << "[ModLoader] Built virtual view for " << datKey << ".dat (ReadFile path), size: " << state->viewSize << std::endl;
                                } else {
                                    std::cout << "[ModLoader] Could not build virtual view for " << datKey << ".dat (mods will not load)" << std::endl;
                                }
                            }
                            oCloseHandle(hMap);
                        }
                    }
                }
            } else {
                std::cout << "[ModLoader] Failed to build virtual layout for " << datKey << ".dat" << std::endl;
            }
        }

        // Only intercept this handle if we have a virtual view (i.e. archive has mods and view was built)
        if (state->cachedView) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_handleToDatKey[h] = datKey;
            g_datPosition[h] = 0;
        }
    }

    return h;
}

// ============================================================================
// CreateFileMappingW — tag .dat mapping handles
// ============================================================================
static HANDLE WINAPI HookedCreateFileMappingW(HANDLE hFile, LPSECURITY_ATTRIBUTES lpAttributes,
    DWORD flProtect, DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCWSTR lpName)
{
    std::string datKey;
    bool built = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_handleToDatKey.find(hFile);
        if (it != g_handleToDatKey.end()) {
            datKey = it->second;
            auto st = g_datStates.find(datKey);
            built = (st != g_datStates.end() && st->second && st->second->built);
        }
    }

    HANDLE hMapping = oCreateFileMappingW(hFile, lpAttributes, flProtect,
        dwMaximumSizeHigh, dwMaximumSizeLow, lpName);

    if (hMapping && !datKey.empty() && built) {
        std::cout << "[ModLoader] CreateFileMappingW on " << datKey << ".dat" << std::endl;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_mappingToDatKey[hMapping] = datKey;
    }

    return hMapping;
}

// ============================================================================
// CreateFileMappingA — same, in case the game uses the A variant
// ============================================================================
static HANDLE WINAPI HookedCreateFileMappingA(HANDLE hFile, LPSECURITY_ATTRIBUTES lpAttributes,
    DWORD flProtect, DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCSTR lpName)
{
    std::string datKey;
    bool built = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_handleToDatKey.find(hFile);
        if (it != g_handleToDatKey.end()) {
            datKey = it->second;
            auto st = g_datStates.find(datKey);
            built = (st != g_datStates.end() && st->second && st->second->built);
        }
    }

    HANDLE hMapping = oCreateFileMappingA(hFile, lpAttributes, flProtect,
        dwMaximumSizeHigh, dwMaximumSizeLow, lpName);

    if (hMapping && !datKey.empty() && built) {
        std::cout << "[ModLoader] CreateFileMappingA on " << datKey << ".dat" << std::endl;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_mappingToDatKey[hMapping] = datKey;
    }

    return hMapping;
}

// ============================================================================
// MapViewOfFile — for .dat mappings, return the full virtual ZIP buffer
// ============================================================================
static LPVOID WINAPI HookedMapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess,
    DWORD dwFileOffsetHigh, DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap)
{
    std::string datKey;
    DatState* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_mappingToDatKey.find(hFileMappingObject);
        if (it != g_mappingToDatKey.end()) {
            datKey = it->second;
            auto st = g_datStates.find(datKey);
            if (st != g_datStates.end() && st->second && st->second->built)
                state = st->second.get();
        }
    }

    if (!state) {
        return oMapViewOfFile(hFileMappingObject, dwDesiredAccess,
            dwFileOffsetHigh, dwFileOffsetLow, dwNumberOfBytesToMap);
    }

    std::cout << "[ModLoader] MapViewOfFile on " << datKey << ".dat" << std::endl;

    // Return cached view if we already built one
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (state->cachedView) {
            state->mapViewRefCount++;
            g_virtualViews.insert(state->cachedView);
            g_viewToDatKey[state->cachedView] = datKey;
            std::cout << "[ModLoader] Returning cached virtual view for " << datKey << " (refcount: " << state->mapViewRefCount << ")" << std::endl;
            return state->cachedView;
        }
    }

    std::cout << "[ModLoader] Building virtual view for " << datKey << "..." << std::endl;

    LPVOID realView = oMapViewOfFile(hFileMappingObject, FILE_MAP_READ, 0, 0, 0);
    if (!realView) {
        std::cout << "[ModLoader] Failed to map real " << datKey << ".dat for reading, error: " << GetLastError() << std::endl;
        return nullptr;
    }

    uint8_t* virtualBuf = state->virtualHd.CreateVirtualView((const uint8_t*)realView);
    oUnmapViewOfFile(realView);

    if (!virtualBuf) {
        std::cout << "[ModLoader] Failed to create virtual view for " << datKey << std::endl;
        return nullptr;
    }

    DWORD oldProtect;
    VirtualProtect(virtualBuf, (SIZE_T)state->virtualHd.GetVirtualSize(), PAGE_READONLY, &oldProtect);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        state->cachedView = virtualBuf;
        state->viewSize = state->virtualHd.GetVirtualSize();
        state->mapViewRefCount = 1;
        g_virtualViews.insert(virtualBuf);
        g_viewToDatKey[virtualBuf] = datKey;
    }

    std::cout << "[ModLoader] Returning virtual view for " << datKey << " at " << (void*)virtualBuf << ", size: " << state->viewSize << std::endl;
    return virtualBuf;
}

// ============================================================================
// UnmapViewOfFile — track refcount per .dat; do not free (ReadFile may still use it)
// ============================================================================
static BOOL WINAPI HookedUnmapViewOfFile(LPCVOID lpBaseAddress) {
    bool isOurs = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto viewIt = g_viewToDatKey.find(lpBaseAddress);
        if (viewIt != g_viewToDatKey.end()) {
            isOurs = true;
            std::string datKey = viewIt->second;
            g_virtualViews.erase(lpBaseAddress);
            g_viewToDatKey.erase(viewIt);
            auto st = g_datStates.find(datKey);
            if (st != g_datStates.end() && st->second) {
                st->second->mapViewRefCount--;
                if (st->second->mapViewRefCount < 0) st->second->mapViewRefCount = 0;
                std::cout << "[ModLoader] UnmapViewOfFile on virtual view for " << datKey << " (refcount: " << st->second->mapViewRefCount << ")" << std::endl;
            }
        }
    }

    if (isOurs)
        return TRUE;

    return oUnmapViewOfFile(lpBaseAddress);
}

// ============================================================================
// ReadFile — for .dat handles, serve from virtual view
// ============================================================================
static BOOL WINAPI HookedReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
    LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped)
{
    if (lpOverlapped) {
        return oReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
    }

    std::string datKey;
    uint64_t* pPos = nullptr;
    uint8_t* cachedView = nullptr;
    uint64_t viewSize = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto keyIt = g_handleToDatKey.find(hFile);
        if (keyIt != g_handleToDatKey.end()) {
            datKey = keyIt->second;
            auto st = g_datStates.find(datKey);
            if (st != g_datStates.end() && st->second && st->second->cachedView) {
                auto posIt = g_datPosition.find(hFile);
                if (posIt != g_datPosition.end()) {
                    pPos = &posIt->second;
                    cachedView = st->second->cachedView;
                    viewSize = st->second->viewSize;
                }
            }
        }
    }

    if (!pPos || !cachedView) {
        return oReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
    }

    uint64_t pos = *pPos;
    uint64_t remaining = (viewSize > pos) ? (viewSize - pos) : 0;
    DWORD toRead = (DWORD)(std::min)((uint64_t)nNumberOfBytesToRead, remaining);
    if (toRead > 0) {
        memcpy(lpBuffer, cachedView + pos, toRead);
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
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto keyIt = g_handleToDatKey.find(hFile);
        if (keyIt != g_handleToDatKey.end()) {
            datKey = keyIt->second;
            auto st = g_datStates.find(datKey);
            if (st != g_datStates.end() && st->second && st->second->cachedView)
                viewSize = st->second->viewSize;
        }
    }

    if (datKey.empty() || viewSize == 0) {
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
            if (st != g_datStates.end() && st->second && st->second->cachedView)
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
        g_mappingToDatKey.erase(hObject);
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
    allOk &= CreateHookHelper(L"kernel32", "CreateFileW",        (LPVOID)HookedCreateFileW,        (LPVOID*)&oCreateFileW);
    allOk &= CreateHookHelper(L"kernel32", "CreateFileMappingW", (LPVOID)HookedCreateFileMappingW, (LPVOID*)&oCreateFileMappingW);
    allOk &= CreateHookHelper(L"kernel32", "CreateFileMappingA", (LPVOID)HookedCreateFileMappingA, (LPVOID*)&oCreateFileMappingA);
    allOk &= CreateHookHelper(L"kernel32", "MapViewOfFile",      (LPVOID)HookedMapViewOfFile,      (LPVOID*)&oMapViewOfFile);
    allOk &= CreateHookHelper(L"kernel32", "UnmapViewOfFile",    (LPVOID)HookedUnmapViewOfFile,    (LPVOID*)&oUnmapViewOfFile);
    allOk &= CreateHookHelper(L"kernel32", "ReadFile",           (LPVOID)HookedReadFile,            (LPVOID*)&oReadFile);
    allOk &= CreateHookHelper(L"kernel32", "SetFilePointerEx",   (LPVOID)HookedSetFilePointerEx,   (LPVOID*)&oSetFilePointerEx);
    allOk &= CreateHookHelper(L"kernel32", "SetFilePointer",     (LPVOID)HookedSetFilePointer,     (LPVOID*)&oSetFilePointer);
    allOk &= CreateHookHelper(L"kernel32", "GetFileSizeEx",      (LPVOID)HookedGetFileSizeEx,     (LPVOID*)&oGetFileSizeEx);
    allOk &= CreateHookHelper(L"kernel32", "CloseHandle",        (LPVOID)HookedCloseHandle,        (LPVOID*)&oCloseHandle);

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
