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

// ============================================================================
// Function pointer types
// ============================================================================
typedef HANDLE(WINAPI* pfnCreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* pfnCreateFileMappingW)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);
typedef HANDLE(WINAPI* pfnCreateFileMappingA)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCSTR);
typedef LPVOID(WINAPI* pfnMapViewOfFile)(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
typedef BOOL(WINAPI* pfnUnmapViewOfFile)(LPCVOID);
typedef BOOL(WINAPI* pfnCloseHandle)(HANDLE);

// Originals
static pfnCreateFileW        oCreateFileW        = nullptr;
static pfnCreateFileMappingW oCreateFileMappingW = nullptr;
static pfnCreateFileMappingA oCreateFileMappingA = nullptr;
static pfnMapViewOfFile      oMapViewOfFile      = nullptr;
static pfnUnmapViewOfFile    oUnmapViewOfFile    = nullptr;
static pfnCloseHandle        oCloseHandle        = nullptr;

// ============================================================================
// Global state
// ============================================================================
static std::string g_modsDir;
static bool g_hooksInstalled = false;
static std::mutex g_mutex;

static VirtualHd g_virtualHd;
static bool g_virtualHdBuilt = false;

// Track hd.dat file handles
static std::unordered_set<HANDLE> g_hdDatFileHandles;

// Track hd.dat mapping handles
static std::unordered_set<HANDLE> g_hdDatMappings;

// Track VirtualAlloc'd buffers we returned from MapViewOfFile
static std::unordered_set<LPCVOID> g_virtualViews;

// Cached virtual view — avoid rebuilding on every MapViewOfFile call
static uint8_t* g_cachedVirtualView = nullptr;
static int g_cachedViewRefCount = 0;

// ============================================================================
// Helpers
// ============================================================================
static bool EndsWithHdDatW(const wchar_t* path) {
    if (!path) return false;
    std::wstring p(path);
    std::replace(p.begin(), p.end(), L'/', L'\\');
    std::wstring lower = p;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    const wchar_t* suffix = L"data\\hd.dat";
    size_t suffixLen = wcslen(suffix);
    if (lower.size() >= suffixLen)
        return lower.compare(lower.size() - suffixLen, suffixLen, suffix) == 0;
    return false;
}

// ============================================================================
// CreateFileW — tag hd.dat handles, build layout on first open
// ============================================================================
static HANDLE WINAPI HookedCreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess,
    DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    HANDLE h = oCreateFileW(lpFileName, dwDesiredAccess, dwShareMode,
        lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

    if (h != INVALID_HANDLE_VALUE && EndsWithHdDatW(lpFileName)) {
        std::wcout << L"[ModLoader] Intercepted hd.dat open: " << lpFileName << std::endl;

        if (!g_virtualHdBuilt) {
            if (g_virtualHd.Build(h, g_modsDir)) {
                g_virtualHdBuilt = true;
            } else {
                std::cout << "[ModLoader] Failed to build virtual layout" << std::endl;
            }
        }

        std::lock_guard<std::mutex> lock(g_mutex);
        g_hdDatFileHandles.insert(h);
    }

    return h;
}

// ============================================================================
// CreateFileMappingW — tag hd.dat mapping handles
// ============================================================================
static HANDLE WINAPI HookedCreateFileMappingW(HANDLE hFile, LPSECURITY_ATTRIBUTES lpAttributes,
    DWORD flProtect, DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCWSTR lpName)
{
    bool isHdDat = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        isHdDat = g_hdDatFileHandles.count(hFile) != 0;
    }

    HANDLE hMapping = oCreateFileMappingW(hFile, lpAttributes, flProtect,
        dwMaximumSizeHigh, dwMaximumSizeLow, lpName);

    if (hMapping && isHdDat && g_virtualHdBuilt) {
        std::cout << "[ModLoader] CreateFileMappingW on hd.dat" << std::endl;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_hdDatMappings.insert(hMapping);
    }

    return hMapping;
}

// ============================================================================
// CreateFileMappingA — same, in case the game uses the A variant
// ============================================================================
static HANDLE WINAPI HookedCreateFileMappingA(HANDLE hFile, LPSECURITY_ATTRIBUTES lpAttributes,
    DWORD flProtect, DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCSTR lpName)
{
    bool isHdDat = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        isHdDat = g_hdDatFileHandles.count(hFile) != 0;
    }

    HANDLE hMapping = oCreateFileMappingA(hFile, lpAttributes, flProtect,
        dwMaximumSizeHigh, dwMaximumSizeLow, lpName);

    if (hMapping && isHdDat && g_virtualHdBuilt) {
        std::cout << "[ModLoader] CreateFileMappingA on hd.dat" << std::endl;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_hdDatMappings.insert(hMapping);
    }

    return hMapping;
}

// ============================================================================
// MapViewOfFile — for hd.dat, build the full virtual ZIP in a new buffer
// ============================================================================
static LPVOID WINAPI HookedMapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess,
    DWORD dwFileOffsetHigh, DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap)
{
    bool isHdDat = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        isHdDat = g_hdDatMappings.count(hFileMappingObject) != 0;
    }

    if (!isHdDat || !g_virtualHdBuilt) {
        return oMapViewOfFile(hFileMappingObject, dwDesiredAccess,
            dwFileOffsetHigh, dwFileOffsetLow, dwNumberOfBytesToMap);
    }

    std::cout << "[ModLoader] MapViewOfFile on hd.dat" << std::endl;

    // Return cached view if we already built one
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_cachedVirtualView) {
            g_cachedViewRefCount++;
            g_virtualViews.insert(g_cachedVirtualView);
            std::cout << "[ModLoader] Returning cached virtual view at " << (void*)g_cachedVirtualView
                      << " (refcount: " << g_cachedViewRefCount << ")" << std::endl;
            return g_cachedVirtualView;
        }
    }

    std::cout << "[ModLoader] Building virtual view..." << std::endl;

    // Map the real file so we can read from it
    LPVOID realView = oMapViewOfFile(hFileMappingObject, FILE_MAP_READ, 0, 0, 0);
    if (!realView) {
        std::cout << "[ModLoader] Failed to map real hd.dat for reading, error: "
                  << GetLastError() << std::endl;
        return nullptr;
    }

    // Build the virtual ZIP in a VirtualAlloc'd buffer
    uint8_t* virtualBuf = g_virtualHd.CreateVirtualView((const uint8_t*)realView);

    // Done reading the real file
    oUnmapViewOfFile(realView);

    if (!virtualBuf) {
        std::cout << "[ModLoader] Failed to create virtual view" << std::endl;
        return nullptr;
    }

    // Make it read-only (like a normal file mapping)
    DWORD oldProtect;
    VirtualProtect(virtualBuf, (SIZE_T)g_virtualHd.GetVirtualSize(), PAGE_READONLY, &oldProtect);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_cachedVirtualView = virtualBuf;
        g_cachedViewRefCount = 1;
        g_virtualViews.insert(virtualBuf);
    }

    std::cout << "[ModLoader] Returning virtual view at " << (void*)virtualBuf
              << ", size: " << g_virtualHd.GetVirtualSize() << std::endl;

    return virtualBuf;
}

// ============================================================================
// UnmapViewOfFile — free our VirtualAlloc'd buffers instead of calling real unmap
// ============================================================================
static BOOL WINAPI HookedUnmapViewOfFile(LPCVOID lpBaseAddress) {
    bool isOurs = false;
    bool shouldFree = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        isOurs = g_virtualViews.count(lpBaseAddress) != 0;
        if (isOurs) {
            g_virtualViews.erase(lpBaseAddress);
            g_cachedViewRefCount--;
            std::cout << "[ModLoader] UnmapViewOfFile on virtual view (refcount: "
                      << g_cachedViewRefCount << ")" << std::endl;
            if (g_cachedViewRefCount <= 0) {
                shouldFree = true;
                g_cachedVirtualView = nullptr;
                g_cachedViewRefCount = 0;
            }
        }
    }

    if (isOurs) {
        if (shouldFree) {
            std::cout << "[ModLoader] Freeing virtual view at " << lpBaseAddress << std::endl;
            return VirtualFree((LPVOID)lpBaseAddress, 0, MEM_RELEASE);
        }
        return TRUE;  // Still referenced, don't free yet
    }

    return oUnmapViewOfFile(lpBaseAddress);
}

// ============================================================================
// CloseHandle — clean up tracking
// ============================================================================
static BOOL WINAPI HookedCloseHandle(HANDLE hObject) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_hdDatFileHandles.erase(hObject);
        g_hdDatMappings.erase(hObject);
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
