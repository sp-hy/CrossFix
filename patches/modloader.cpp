#include "modloader.h"
#include "virtual_hd.h"
#include <MinHook.h>
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// ============================================================================
// Function pointer types
// ============================================================================
typedef HANDLE(WINAPI *pfnCreateFileW)(LPCWSTR, DWORD, DWORD,
                                       LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                       HANDLE);
typedef BOOL(WINAPI *pfnCloseHandle)(HANDLE);
typedef BOOL(WINAPI *pfnReadFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL(WINAPI *pfnSetFilePointerEx)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER,
                                          DWORD);
typedef DWORD(WINAPI *pfnSetFilePointer)(HANDLE, LONG, PLONG, DWORD);
typedef BOOL(WINAPI *pfnGetFileSizeEx)(HANDLE, PLARGE_INTEGER);

// Originals
static pfnCreateFileW oCreateFileW = nullptr;
static pfnCloseHandle oCloseHandle = nullptr;
static pfnReadFile oReadFile = nullptr;
static pfnSetFilePointerEx oSetFilePointerEx = nullptr;
static pfnSetFilePointer oSetFilePointer = nullptr;
static pfnGetFileSizeEx oGetFileSizeEx = nullptr;

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
  bool useReadFileSynthesis =
      false; // serve via ReadFile only (no virtual buffer)
};
static std::unordered_map<std::string, std::unique_ptr<DatState>> g_datStates;

// Which .dat key does each file handle belong to
static std::unordered_map<HANDLE, std::string> g_handleToDatKey;

// Per-handle file position for ReadFile path
static std::unordered_map<HANDLE, uint64_t> g_datPosition;

// ============================================================================
// Helpers
// ============================================================================
// If path ends with .dat (case-insensitive), return the base name (e.g. "hd",
// "cdrom"); else empty string.
static std::string GetDatKeyFromPathW(const wchar_t *path) {
  if (!path)
    return {};
  std::wstring p(path);
  std::replace(p.begin(), p.end(), L'/', L'\\');
  std::wstring lower = p;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
  const wchar_t *suffix = L".dat";
  size_t suffixLen = wcslen(suffix);
  if (lower.size() < suffixLen)
    return {};
  if (lower.compare(lower.size() - suffixLen, suffixLen, suffix) != 0)
    return {};
  size_t nameStart = lower.find_last_of(L"\\/");
  size_t start = (nameStart == std::wstring::npos) ? 0 : nameStart + 1;
  size_t nameLen = lower.size() - suffixLen - start;
  if (nameLen == 0)
    return {};
  std::string key;
  key.reserve(nameLen);
  for (size_t i = 0; i < nameLen; i++)
    key += (char)lower[start + i];
  return key;
}

// Only intercept .dat files in the game's data folder (e.g. data\hd.dat), not
// save.dat in Documents etc.
static bool IsGameDataPathW(const wchar_t *path) {
  if (!path)
    return false;
  std::wstring p(path);
  std::replace(p.begin(), p.end(), L'/', L'\\');
  std::wstring lower = p;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
  // Relative path "data\hd.dat" or "data/hd.dat"
  if (lower.size() >= 5 && (lower.compare(0, 5, L"data\\") == 0 ||
                            lower.compare(0, 5, L"data/") == 0))
    return true;
  // Path contains \data\ or /data/ (e.g. "C:\game\data\hd.dat")
  return lower.find(L"\\data\\") != std::wstring::npos ||
         lower.find(L"/data/") != std::wstring::npos;
}

// Blacklist: do not intercept these .dat keys (e.g. save files)
static bool IsBlacklistedDatKey(const std::string &datKey) {
  return datKey == "save";
}

// ============================================================================
// CreateFileW — tag .dat handles, build layout on first open per dat type
// ============================================================================
static HANDLE WINAPI HookedCreateFileW(
    LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
  HANDLE h = oCreateFileW(lpFileName, dwDesiredAccess, dwShareMode,
                          lpSecurityAttributes, dwCreationDisposition,
                          dwFlagsAndAttributes, hTemplateFile);

  std::string datKey = GetDatKeyFromPathW(lpFileName);
  if (h != INVALID_HANDLE_VALUE && !datKey.empty() &&
      IsGameDataPathW(lpFileName) && !IsBlacklistedDatKey(datKey)) {
    std::string modsSubdir = g_modsDir + "/" + datKey;
    DatState *state = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      auto &ptr = g_datStates[datKey];
      if (!ptr)
        ptr = std::make_unique<DatState>();
      state = ptr.get();
    }

    if (!state->built) {
      if (state->virtualHd.Build(h, modsSubdir)) {
        state->built = true;
        if (state->virtualHd.HasMods()) {
          state->viewSize = state->virtualHd.GetVirtualSize();
          state->useReadFileSynthesis = true;
        }
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
static BOOL WINAPI HookedReadFile(HANDLE hFile, LPVOID lpBuffer,
                                  DWORD nNumberOfBytesToRead,
                                  LPDWORD lpNumberOfBytesRead,
                                  LPOVERLAPPED lpOverlapped) {
  if (lpOverlapped) {
    return oReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead,
                     lpOverlapped);
  }

  std::string datKey;
  DatState *state = nullptr;
  uint64_t *pPos = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto keyIt = g_handleToDatKey.find(hFile);
    if (keyIt != g_handleToDatKey.end()) {
      datKey = keyIt->second;
      auto st = g_datStates.find(datKey);
      if (st != g_datStates.end() && st->second &&
          st->second->useReadFileSynthesis) {
        state = st->second.get();
        auto posIt = g_datPosition.find(hFile);
        if (posIt != g_datPosition.end())
          pPos = &posIt->second;
      }
    }
  }

  if (!pPos || !state) {
    return oReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead,
                     lpOverlapped);
  }

  uint64_t pos = *pPos;
  uint64_t viewSize = state->viewSize;
  uint64_t remaining = (viewSize > pos) ? (viewSize - pos) : 0;
  DWORD toRead = (DWORD)(std::min)((uint64_t)nNumberOfBytesToRead, remaining);

  if (toRead > 0) {
    size_t got = state->virtualHd.ReadAtVirtualOffset(
        hFile, pos, lpBuffer, toRead, oReadFile, oSetFilePointerEx);
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
static BOOL WINAPI HookedSetFilePointerEx(HANDLE hFile,
                                          LARGE_INTEGER liDistanceToMove,
                                          PLARGE_INTEGER lpNewFilePointerHigh,
                                          DWORD dwMoveMethod) {
  std::string datKey;
  uint64_t viewSize = 0;
  bool intercept = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto keyIt = g_handleToDatKey.find(hFile);
    if (keyIt != g_handleToDatKey.end()) {
      datKey = keyIt->second;
      auto st = g_datStates.find(datKey);
      if (st != g_datStates.end() && st->second &&
          st->second->useReadFileSynthesis) {
        viewSize = st->second->viewSize;
        intercept = true;
      }
    }
  }

  if (!intercept) {
    return oSetFilePointerEx(hFile, liDistanceToMove, lpNewFilePointerHigh,
                             dwMoveMethod);
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_datPosition.find(hFile);
  if (it == g_datPosition.end())
    return oSetFilePointerEx(hFile, liDistanceToMove, lpNewFilePointerHigh,
                             dwMoveMethod);

  int64_t offset = (int64_t)liDistanceToMove.QuadPart;
  uint64_t newPos = 0;
  switch (dwMoveMethod) {
  case FILE_BEGIN:
    newPos = (offset >= 0) ? (uint64_t)offset : 0;
    break;
  case FILE_CURRENT:
    newPos = (int64_t)it->second + offset;
    if (newPos < 0)
      newPos = 0;
    break;
  case FILE_END:
    newPos = (int64_t)viewSize + offset;
    if (newPos < 0)
      newPos = 0;
    break;
  default:
    return oSetFilePointerEx(hFile, liDistanceToMove, lpNewFilePointerHigh,
                             dwMoveMethod);
  }
  if (newPos > viewSize)
    newPos = viewSize;
  it->second = newPos;
  if (lpNewFilePointerHigh)
    lpNewFilePointerHigh->QuadPart = (LONGLONG)newPos;
  return TRUE;
}

// ============================================================================
// SetFilePointer — for hd.dat handles, track position (32-bit API)
// ============================================================================
static DWORD WINAPI HookedSetFilePointer(HANDLE hFile, LONG lDistanceToMove,
                                         PLONG lpDistanceToMoveHigh,
                                         DWORD dwMoveMethod) {
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
static BOOL WINAPI HookedGetFileSizeEx(HANDLE hFile,
                                       PLARGE_INTEGER lpFileSize) {
  uint64_t viewSize = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto keyIt = g_handleToDatKey.find(hFile);
    if (keyIt != g_handleToDatKey.end()) {
      auto st = g_datStates.find(keyIt->second);
      if (st != g_datStates.end() && st->second &&
          st->second->useReadFileSynthesis)
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
static bool CreateHookHelper(LPCWSTR moduleName, LPCSTR procName, LPVOID detour,
                             LPVOID *original) {
  MH_STATUS status = MH_CreateHookApi(moduleName, procName, detour, original);
  return (status == MH_OK);
}

// ============================================================================
// Mods folder structure (matches Steam game mods layout)
// ============================================================================
static const char *const g_modsSubdirs[] = {
    "cdrom",
    "cdrom/battle",
    "cdrom/battle/bg",
    "cdrom/battle/data",
    "cdrom/battle/effects",
    "cdrom/battle/effects/birth",
    "cdrom/battle/effects/black",
    "cdrom/battle/effects/blue",
    "cdrom/battle/effects/death",
    "cdrom/battle/effects/green",
    "cdrom/battle/effects/monster",
    "cdrom/battle/effects/rainbow",
    "cdrom/battle/effects/red",
    "cdrom/battle/effects/syoukan",
    "cdrom/battle/effects/tech",
    "cdrom/battle/effects/trap",
    "cdrom/battle/effects/white",
    "cdrom/battle/effects/yellow",
    "cdrom/battle/event",
    "cdrom/battle/face",
    "cdrom/battle/mes",
    "cdrom/battle/monster",
    "cdrom/battle/monster/7star",
    "cdrom/battle/monster/adk_boss",
    "cdrom/battle/monster/adk_krn",
    "cdrom/battle/monster/adk_ryu",
    "cdrom/battle/monster/adk_sp",
    "cdrom/battle/monster/adk_yst",
    "cdrom/battle/monster/adk_zako",
    "cdrom/battle/monster/akaoni",
    "cdrom/battle/monster/anda",
    "cdrom/battle/monster/asimusi",
    "cdrom/battle/monster/ato",
    "cdrom/battle/monster/awa",
    "cdrom/battle/monster/bairetta",
    "cdrom/battle/monster/bannin",
    "cdrom/battle/monster/batman",
    "cdrom/battle/monster/bigin",
    "cdrom/battle/monster/bom",
    "cdrom/battle/monster/c_aka",
    "cdrom/battle/monster/chouchin",
    "cdrom/battle/monster/col_p",
    "cdrom/battle/monster/d_hige",
    "cdrom/battle/monster/d_noppo",
    "cdrom/battle/monster/d_serge",
    "cdrom/battle/monster/d_wing",
    "cdrom/battle/monster/dai_h",
    "cdrom/battle/monster/daimou",
    "cdrom/battle/monster/dario",
    "cdrom/battle/monster/doggy",
    "cdrom/battle/monster/dor_a",
    "cdrom/battle/monster/dor_ay",
    "cdrom/battle/monster/dor_b",
    "cdrom/battle/monster/dor_bk",
    "cdrom/battle/monster/dor_br",
    "cdrom/battle/monster/dor_w",
    "cdrom/battle/monster/dowarf",
    "cdrom/battle/monster/dra_g",
    "cdrom/battle/monster/eiei",
    "cdrom/battle/monster/elf",
    "cdrom/battle/monster/fate",
    "cdrom/battle/monster/fugu",
    "cdrom/battle/monster/galgol",
    "cdrom/battle/monster/gas",
    "cdrom/battle/monster/gekijou",
    "cdrom/battle/monster/gobu",
    "cdrom/battle/monster/gorem",
    "cdrom/battle/monster/gremlin",
    "cdrom/battle/monster/haienako",
    "cdrom/battle/monster/haienoya",
    "cdrom/battle/monster/haneuo",
    "cdrom/battle/monster/hidla",
    "cdrom/battle/monster/hikou",
    "cdrom/battle/monster/hobgob",
    "cdrom/battle/monster/hone_h",
    "cdrom/battle/monster/hosi",
    "cdrom/battle/monster/imomu",
    "cdrom/battle/monster/iwao",
    "cdrom/battle/monster/jigoku",
    "cdrom/battle/monster/k_monk",
    "cdrom/battle/monster/kaeru",
    "cdrom/battle/monster/kagari",
    "cdrom/battle/monster/kageneko",
    "cdrom/battle/monster/kama",
    "cdrom/battle/monster/kamei",
    "cdrom/battle/monster/kamei_sp",
    "cdrom/battle/monster/ki",
    "cdrom/battle/monster/komodo",
    "cdrom/battle/monster/ktokage",
    "cdrom/battle/monster/kurage",
    "cdrom/battle/monster/kuro",
    "cdrom/battle/monster/kvin",
    "cdrom/battle/monster/lavos",
    "cdrom/battle/monster/leo",
    "cdrom/battle/monster/m_dama",
    "cdrom/battle/monster/m_garai",
    "cdrom/battle/monster/manekin",
    "cdrom/battle/monster/manet",
    "cdrom/battle/monster/mark",
    "cdrom/battle/monster/maturi",
    "cdrom/battle/monster/mayone",
    "cdrom/battle/monster/mb_knja",
    "cdrom/battle/monster/migel",
    "cdrom/battle/monster/mimi_l",
    "cdrom/battle/monster/mimi_s",
    "cdrom/battle/monster/mizu_c",
    "cdrom/battle/monster/mobechan",
    "cdrom/battle/monster/mouja",
    "cdrom/battle/monster/ms_robo",
    "cdrom/battle/monster/nai",
    "cdrom/battle/monster/natak",
    "cdrom/battle/monster/niwa",
    "cdrom/battle/monster/o_mushi",
    "cdrom/battle/monster/o_slime",
    "cdrom/battle/monster/opthe",
    "cdrom/battle/monster/optokage",
    "cdrom/battle/monster/p_gorem",
    "cdrom/battle/monster/paper",
    "cdrom/battle/monster/parecap",
    "cdrom/battle/monster/paresol",
    "cdrom/battle/monster/pipa",
    "cdrom/battle/monster/putera",
    "cdrom/battle/monster/putera_f",
    "cdrom/battle/monster/q_con",
    "cdrom/battle/monster/q_con2",
    "cdrom/battle/monster/rake",
    "cdrom/battle/monster/robo_g",
    "cdrom/battle/monster/rozali",
    "cdrom/battle/monster/ryu_b",
    "cdrom/battle/monster/ryu_bk",
    "cdrom/battle/monster/ryu_g",
    "cdrom/battle/monster/ryu_pw",
    "cdrom/battle/monster/ryu_red",
    "cdrom/battle/monster/ryu_y",
    "cdrom/battle/monster/sakana",
    "cdrom/battle/monster/scoop",
    "cdrom/battle/monster/sfin",
    "cdrom/battle/monster/shinshi",
    "cdrom/battle/monster/sidestep",
    "cdrom/battle/monster/skalf",
    "cdrom/battle/monster/skyf",
    "cdrom/battle/monster/slime",
    "cdrom/battle/monster/snow",
    "cdrom/battle/monster/soiso",
    "cdrom/battle/monster/squra",
    "cdrom/battle/monster/su_a",
    "cdrom/battle/monster/su_a2",
    "cdrom/battle/monster/su_b_hi",
    "cdrom/battle/monster/sun",
    "cdrom/battle/monster/tank",
    "cdrom/battle/monster/tenshi",
    "cdrom/battle/monster/tiba",
    "cdrom/battle/monster/tiger",
    "cdrom/battle/monster/tirano",
    "cdrom/battle/monster/toka",
    "cdrom/battle/monster/tokikui",
    "cdrom/battle/monster/tori",
    "cdrom/battle/monster/torimeca",
    "cdrom/battle/monster/tubo",
    "cdrom/battle/monster/umibouz",
    "cdrom/battle/monster/umidevi",
    "cdrom/battle/monster/usagi",
    "cdrom/battle/monster/vine",
    "cdrom/battle/monster/wing",
    "cdrom/battle/monster/wing_r",
    "cdrom/battle/monster/yamaneko",
    "cdrom/battle/monster/yoroi",
    "cdrom/battle/monster/yoroi2",
    "cdrom/battle/monster/yuka",
    "cdrom/battle/monster/zmbfly",
    "cdrom/battle/player",
    "cdrom/battle/player/banc",
    "cdrom/battle/player/carshu",
    "cdrom/battle/player/colcha",
    "cdrom/battle/player/dan",
    "cdrom/battle/player/doc",
    "cdrom/battle/player/dragon",
    "cdrom/battle/player/farga",
    "cdrom/battle/player/gren1",
    "cdrom/battle/player/gren2",
    "cdrom/battle/player/guill",
    "cdrom/battle/player/gyada",
    "cdrom/battle/player/harley",
    "cdrom/battle/player/hone",
    "cdrom/battle/player/hosi",
    "cdrom/battle/player/ilenes",
    "cdrom/battle/player/ishito",
    "cdrom/battle/player/jakotu",
    "cdrom/battle/player/jilbelt",
    "cdrom/battle/player/jyaness",
    "cdrom/battle/player/kabuo",
    "cdrom/battle/player/kid",
    "cdrom/battle/player/kinoko",
    "cdrom/battle/player/lazzuly",
    "cdrom/battle/player/lena",
    "cdrom/battle/player/liia",
    "cdrom/battle/player/lutianna",
    "cdrom/battle/player/malchera",
    "cdrom/battle/player/mell",
    "cdrom/battle/player/miki",
    "cdrom/battle/player/obachan",
    "cdrom/battle/player/ocha",
    "cdrom/battle/player/olha",
    "cdrom/battle/player/pierre",
    "cdrom/battle/player/poshul",
    "cdrom/battle/player/rablue",
    "cdrom/battle/player/radius",
    "cdrom/battle/player/riddle",
    "cdrom/battle/player/sel_yam2",
    "cdrom/battle/player/selju2",
    "cdrom/battle/player/slash",
    "cdrom/battle/player/spri",
    "cdrom/battle/player/steena",
    "cdrom/battle/player/sunef",
    "cdrom/battle/player/tu_m",
    "cdrom/battle/player/tu_mm",
    "cdrom/battle/player/tu_p",
    "cdrom/battle/player/tu_pm",
    "cdrom/battle/player/tu_pp",
    "cdrom/battle/player/tumaru",
    "cdrom/battle/player/yamaneko",
    "cdrom/battle/player/zappa",
    "cdrom/battle/player/zoah",
    "cdrom/battle/wepon",
    "cdrom/battle/wepon/axe",
    "cdrom/battle/wepon/kaji",
    "cdrom/battle/wepon/ken",
    "cdrom/battle/wepon/monster",
    "cdrom/battle/wepon/tue",
    "cdrom/dummy",
    "cdrom/effect",
    "cdrom/face",
    "cdrom/fieldobj",
    "cdrom/fieldobj/ex",
    "cdrom/fieldobj/player",
    "cdrom/fieldobj/player/adk_sp",
    "cdrom/fieldobj/player/banc",
    "cdrom/fieldobj/player/carshu",
    "cdrom/fieldobj/player/cl_bort",
    "cdrom/fieldobj/player/colcha",
    "cdrom/fieldobj/player/dan",
    "cdrom/fieldobj/player/doc",
    "cdrom/fieldobj/player/dragon",
    "cdrom/fieldobj/player/farga",
    "cdrom/fieldobj/player/gren",
    "cdrom/fieldobj/player/gren2",
    "cdrom/fieldobj/player/guill",
    "cdrom/fieldobj/player/gyada",
    "cdrom/fieldobj/player/harley",
    "cdrom/fieldobj/player/hone",
    "cdrom/fieldobj/player/hosi",
    "cdrom/fieldobj/player/ilenes",
    "cdrom/fieldobj/player/ishito",
    "cdrom/fieldobj/player/jakotu",
    "cdrom/fieldobj/player/jilbelt",
    "cdrom/fieldobj/player/jyaness",
    "cdrom/fieldobj/player/kabuo",
    "cdrom/fieldobj/player/kid",
    "cdrom/fieldobj/player/kinoko",
    "cdrom/fieldobj/player/lazzuly",
    "cdrom/fieldobj/player/lena",
    "cdrom/fieldobj/player/liia",
    "cdrom/fieldobj/player/lutianna",
    "cdrom/fieldobj/player/malchera",
    "cdrom/fieldobj/player/mamacha",
    "cdrom/fieldobj/player/mell",
    "cdrom/fieldobj/player/miki",
    "cdrom/fieldobj/player/neko",
    "cdrom/fieldobj/player/neko2",
    "cdrom/fieldobj/player/neko3",
    "cdrom/fieldobj/player/ocha",
    "cdrom/fieldobj/player/olha",
    "cdrom/fieldobj/player/pierre",
    "cdrom/fieldobj/player/poshul",
    "cdrom/fieldobj/player/rablue",
    "cdrom/fieldobj/player/radius",
    "cdrom/fieldobj/player/riddle",
    "cdrom/fieldobj/player/selju",
    "cdrom/fieldobj/player/slash",
    "cdrom/fieldobj/player/spri",
    "cdrom/fieldobj/player/steena",
    "cdrom/fieldobj/player/sunef",
    "cdrom/fieldobj/player/tu_m",
    "cdrom/fieldobj/player/tu_mm",
    "cdrom/fieldobj/player/tu_p",
    "cdrom/fieldobj/player/tu_pm",
    "cdrom/fieldobj/player/tu_pp",
    "cdrom/fieldobj/player/tumaru",
    "cdrom/fieldobj/player/yamaneko",
    "cdrom/fieldobj/player/zappa",
    "cdrom/fieldobj/player/zoah",
    "cdrom/map",
    "cdrom/map/etc",
    "cdrom/map/mapbin",
    "cdrom/map/window",
    "cdrom/menu",
    "cdrom/menu/keyitem",
    "cdrom/menu/player",
    "cdrom/menu/playtim",
    "cdrom/menu/title",
    "cdrom/prog",
    "cdrom/sound",
    "cdrom/sound/bgm",
    "cdrom/sound/effect",
    "cdrom/sound/effect/common",
    "cdrom/sound/exwave",
    "cdrom/sound/music",
    "cdrom/sound/wave",
    "cdrom2",
    "cdrom2/battle",
    "cdrom2/battle/monster",
    "cdrom2/battle/monster/yamaneko",
    "cdrom2/battle/player",
    "cdrom2/battle/player/banc",
    "cdrom2/battle/player/carshu",
    "cdrom2/battle/player/colcha",
    "cdrom2/battle/player/dan",
    "cdrom2/battle/player/doc",
    "cdrom2/battle/player/dragon",
    "cdrom2/battle/player/farga",
    "cdrom2/battle/player/gren1",
    "cdrom2/battle/player/gren2",
    "cdrom2/battle/player/guill",
    "cdrom2/battle/player/gyada",
    "cdrom2/battle/player/harley",
    "cdrom2/battle/player/hone",
    "cdrom2/battle/player/hosi",
    "cdrom2/battle/player/ilenes",
    "cdrom2/battle/player/ishito",
    "cdrom2/battle/player/jakotu",
    "cdrom2/battle/player/jilbelt",
    "cdrom2/battle/player/jyaness",
    "cdrom2/battle/player/kabuo",
    "cdrom2/battle/player/kid",
    "cdrom2/battle/player/kinoko",
    "cdrom2/battle/player/lazzuly",
    "cdrom2/battle/player/lena",
    "cdrom2/battle/player/liia",
    "cdrom2/battle/player/lutianna",
    "cdrom2/battle/player/malchera",
    "cdrom2/battle/player/mell",
    "cdrom2/battle/player/miki",
    "cdrom2/battle/player/obachan",
    "cdrom2/battle/player/ocha",
    "cdrom2/battle/player/olha",
    "cdrom2/battle/player/pierre",
    "cdrom2/battle/player/poshul",
    "cdrom2/battle/player/rablue",
    "cdrom2/battle/player/radius",
    "cdrom2/battle/player/riddle",
    "cdrom2/battle/player/sel_yam2",
    "cdrom2/battle/player/selju2",
    "cdrom2/battle/player/slash",
    "cdrom2/battle/player/spri",
    "cdrom2/battle/player/steena",
    "cdrom2/battle/player/sunef",
    "cdrom2/battle/player/tu_m",
    "cdrom2/battle/player/tu_mm",
    "cdrom2/battle/player/tu_p",
    "cdrom2/battle/player/tu_pm",
    "cdrom2/battle/player/tu_pp",
    "cdrom2/battle/player/tumaru",
    "cdrom2/battle/player/yamaneko",
    "cdrom2/battle/player/zappa",
    "cdrom2/battle/player/zoah",
    "cdrom2/battle/player_sd",
    "cdrom2/battle/player_sd/doc",
    "cdrom2/fieldobj",
    "cdrom2/fieldobj/player",
    "cdrom2/fieldobj/player/banc",
    "cdrom2/fieldobj/player/carshu",
    "cdrom2/fieldobj/player/colcha",
    "cdrom2/fieldobj/player/dan",
    "cdrom2/fieldobj/player/doc",
    "cdrom2/fieldobj/player/dragon",
    "cdrom2/fieldobj/player/farga",
    "cdrom2/fieldobj/player/gren",
    "cdrom2/fieldobj/player/gren2",
    "cdrom2/fieldobj/player/guill",
    "cdrom2/fieldobj/player/gyada",
    "cdrom2/fieldobj/player/harley",
    "cdrom2/fieldobj/player/hone",
    "cdrom2/fieldobj/player/hosi",
    "cdrom2/fieldobj/player/ilenes",
    "cdrom2/fieldobj/player/ishito",
    "cdrom2/fieldobj/player/jakotu",
    "cdrom2/fieldobj/player/jilbelt",
    "cdrom2/fieldobj/player/jyaness",
    "cdrom2/fieldobj/player/kabuo",
    "cdrom2/fieldobj/player/kid",
    "cdrom2/fieldobj/player/kinoko",
    "cdrom2/fieldobj/player/lazzuly",
    "cdrom2/fieldobj/player/lena",
    "cdrom2/fieldobj/player/liia",
    "cdrom2/fieldobj/player/lutianna",
    "cdrom2/fieldobj/player/malchera",
    "cdrom2/fieldobj/player/mamacha",
    "cdrom2/fieldobj/player/mell",
    "cdrom2/fieldobj/player/miki",
    "cdrom2/fieldobj/player/ocha",
    "cdrom2/fieldobj/player/olha",
    "cdrom2/fieldobj/player/pierre",
    "cdrom2/fieldobj/player/poshul",
    "cdrom2/fieldobj/player/rablue",
    "cdrom2/fieldobj/player/radius",
    "cdrom2/fieldobj/player/riddle",
    "cdrom2/fieldobj/player/selju",
    "cdrom2/fieldobj/player/slash",
    "cdrom2/fieldobj/player/spri",
    "cdrom2/fieldobj/player/steena",
    "cdrom2/fieldobj/player/sunef",
    "cdrom2/fieldobj/player/tu_m",
    "cdrom2/fieldobj/player/tu_mm",
    "cdrom2/fieldobj/player/tu_p",
    "cdrom2/fieldobj/player/tu_pm",
    "cdrom2/fieldobj/player/tu_pp",
    "cdrom2/fieldobj/player/tumaru",
    "cdrom2/fieldobj/player/yamaneko",
    "cdrom2/fieldobj/player/zappa",
    "cdrom2/fieldobj/player/zoah",
    "cdrom2/map",
    "cdrom2/map/etc",
    "cdrom2/map/mapbin",
    "cdrom2/map/window",
    "cdrom2/menu",
    "cdrom2/menu/player",
    "cdrom2/menu/player_sd",
    "cdrom2/menu/title",
    "font",
    "hd",
    "hd/battle",
    "hd/battle/battle",
    "hd/battle/btend",
    "hd/battle/canon",
    "hd/battle/elementfield",
    "hd/battle/face",
    "hd/battle/face/1999",
    "hd/battle/face/2021",
    "hd/battle/shapes",
    "hd/battle/shine",
    "hd/face",
    "hd/face/1999",
    "hd/face/2021",
    "hd/fieldobj",
    "hd/fieldobj/player",
    "hd/fieldobj/player/banc",
    "hd/fieldobj/player/carshu",
    "hd/fieldobj/player/colcha",
    "hd/fieldobj/player/dan",
    "hd/fieldobj/player/doc",
    "hd/fieldobj/player/dragon",
    "hd/fieldobj/player/farga",
    "hd/fieldobj/player/gren",
    "hd/fieldobj/player/gren2",
    "hd/fieldobj/player/guill",
    "hd/fieldobj/player/gyada",
    "hd/fieldobj/player/harley",
    "hd/fieldobj/player/hone",
    "hd/fieldobj/player/hosi",
    "hd/fieldobj/player/ilenes",
    "hd/fieldobj/player/ishito",
    "hd/fieldobj/player/jakotu",
    "hd/fieldobj/player/jilbelt",
    "hd/fieldobj/player/jyaness",
    "hd/fieldobj/player/kabuo",
    "hd/fieldobj/player/kid",
    "hd/fieldobj/player/kinoko",
    "hd/fieldobj/player/lazzuly",
    "hd/fieldobj/player/lena",
    "hd/fieldobj/player/liia",
    "hd/fieldobj/player/lutianna",
    "hd/fieldobj/player/malchera",
    "hd/fieldobj/player/mamacha",
    "hd/fieldobj/player/mell",
    "hd/fieldobj/player/miki",
    "hd/fieldobj/player/ocha",
    "hd/fieldobj/player/olha",
    "hd/fieldobj/player/pierre",
    "hd/fieldobj/player/poshul",
    "hd/fieldobj/player/rablue",
    "hd/fieldobj/player/radius",
    "hd/fieldobj/player/riddle",
    "hd/fieldobj/player/roulet0",
    "hd/fieldobj/player/roulet1",
    "hd/fieldobj/player/selju",
    "hd/fieldobj/player/slash",
    "hd/fieldobj/player/spri",
    "hd/fieldobj/player/steena",
    "hd/fieldobj/player/sunef",
    "hd/fieldobj/player/tu_m",
    "hd/fieldobj/player/tu_mm",
    "hd/fieldobj/player/tu_p",
    "hd/fieldobj/player/tu_pm",
    "hd/fieldobj/player/tu_pp",
    "hd/fieldobj/player/tumaru",
    "hd/fieldobj/player/yamaneko",
    "hd/fieldobj/player/zappa",
    "hd/fieldobj/player/zoah",
    "hd/fieldobj/player_sd",
    "hd/fieldobj/player_sd/doc",
    "hd/map",
    "hd/map/etc",
    "hd/map/Fin",
    "hd/map/HomeAnother",
    "hd/map/jac_b07i",
    "hd/map/mapbin",
    "hd/menu",
    "hd/menu/keyitem",
    "hd/menu/menuelem",
    "hd/menu/menutim",
    "hd/menu/player",
    "hd/menu/playtim",
    "hd/menu/playtim/1999",
    "hd/menu/playtim/2021",
    "hd/menu/shapes",
    "hd/menufaceicon",
    "hd/menufaceicon/1999",
    "hd/menufaceicon/2021",
    "hd/staff",
    "hd/title",
    "hd/userguide",
    "hd/window",
    "lang",
    "lang/battle",
    "lang/battle/event",
    "font",
    "movie",
    "ui",
};

static void EnsureModsFolderStructure() {
  std::error_code ec;
  std::filesystem::create_directories(g_modsDir, ec);
  if (ec)
    return;
  for (const char *sub : g_modsSubdirs) {
    std::filesystem::path p(g_modsDir);
    for (const char *it = sub; *it;) {
      const char *end = it;
      while (*end && *end != '/')
        ++end;
      p /= std::string(it, end);
      it = *end ? end + 1 : end;
    }
    std::filesystem::create_directories(p, ec);
  }
}

// ============================================================================
// Public API
// ============================================================================
bool InitModLoader(const std::string &exePath) {
  std::string exeDir;
  size_t lastSlash = exePath.find_last_of("\\/");
  if (lastSlash != std::string::npos)
    exeDir = exePath.substr(0, lastSlash + 1);

  g_modsDir = exeDir + "mods";

  // Create mods folder and standard subfolders if missing
  EnsureModsFolderStructure();

  if (!std::filesystem::exists(g_modsDir) ||
      !std::filesystem::is_directory(g_modsDir))
    return false;

  MH_STATUS status = MH_Initialize();
  if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    return false;

  bool allOk = true;
  allOk &= CreateHookHelper(L"kernel32", "CreateFileW",
                            (LPVOID)HookedCreateFileW, (LPVOID *)&oCreateFileW);
  allOk &= CreateHookHelper(L"kernel32", "ReadFile", (LPVOID)HookedReadFile,
                            (LPVOID *)&oReadFile);
  allOk &= CreateHookHelper(L"kernel32", "SetFilePointerEx",
                            (LPVOID)HookedSetFilePointerEx,
                            (LPVOID *)&oSetFilePointerEx);
  allOk &= CreateHookHelper(L"kernel32", "SetFilePointer",
                            (LPVOID)HookedSetFilePointer,
                            (LPVOID *)&oSetFilePointer);
  allOk &=
      CreateHookHelper(L"kernel32", "GetFileSizeEx",
                       (LPVOID)HookedGetFileSizeEx, (LPVOID *)&oGetFileSizeEx);
  allOk &= CreateHookHelper(L"kernel32", "CloseHandle",
                            (LPVOID)HookedCloseHandle, (LPVOID *)&oCloseHandle);

  if (!allOk) {
    MH_Uninitialize();
    return false;
  }

  status = MH_EnableHook(MH_ALL_HOOKS);
  if (status != MH_OK) {
    MH_Uninitialize();
    return false;
  }

  g_hooksInstalled = true;
  return true;
}
