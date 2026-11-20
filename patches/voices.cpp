#define DR_MP3_IMPLEMENTATION
#include "voices.h"
#include "../utils/memory.h"
#include "../utils/rde_symbols.h"

#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <dr_mp3.h>
#include <fstream>
#include <iostream>
#include <mmsystem.h>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "winmm.lib")

// Voice lines for field and battle dialogue, in the voice-pack layout OpenCross
// uses (mods\<pack>\voices\...). The game-thread hooks only record which line
// opened; a worker thread follows its window's page and open state and does
// all file lookup, MP3 decoding and playback.

// =============================================================================
// Global State
// =============================================================================
namespace State {
enum class Kind { None, Field, Battle };

struct Line {
  Kind kind = Kind::None;
  int scene = 0;
  int dialog = 0;
  int speaker = 0;
  int tagSpeaker = 0;
  int prevSpeaker = 0;
  uintptr_t window = 0;
};

uintptr_t baseAddress = 0;
uintptr_t fieldContinuation = 0;
uintptr_t battleContinuation = 0;

CRITICAL_SECTION lineLock;
HANDLE lineEvent = nullptr;
Line pendingLine;
uint32_t pendingSerial = 0;

int speakerRoom = -1;
int roomPrevSpeaker = 0;

std::wstring modsDir;
} // namespace State

// =============================================================================
// Game memory access
// =============================================================================
template <typename T> static T ReadHost(uintptr_t address) {
  return *reinterpret_cast<volatile T *>(address);
}

template <typename T> static T ReadExe(uintptr_t offset) {
  return ReadHost<T>(State::baseAddress + offset);
}

static uintptr_t GuestHost(uint32_t guestAddr) {
  const uint32_t offset = guestAddr & 0x1FFFFFFF;
  if (offset >= rde::guest_ram_size)
    return 0;
  return State::baseAddress + rde::guest_ram_base + offset;
}

template <typename T> static T ReadGuest(uint32_t guestAddr) {
  const uintptr_t host = GuestHost(guestAddr);
  return host ? ReadHost<T>(host) : T{};
}

static uint32_t GuestRegister(int index) {
  return ReadExe<uint32_t>(rde::guest_regs + index * 4);
}

static std::string Utf8(const std::wstring &text) {
  const int size = WideCharToMultiByte(
      CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
  std::string out(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(),
                      size, nullptr, nullptr);
  return out;
}

// =============================================================================
// Voice Playback (dr_mp3 decode + waveOut) - lightweight, no system codecs
// =============================================================================
namespace Audio {

struct PlaybackContext {
  HWAVEOUT hwo = nullptr;
  WAVEHDR hdr{};
  void *pcm = nullptr;
};

static CRITICAL_SECTION g_cs;
static volatile HWAVEOUT g_current = nullptr;
static bool g_init = false;

static void FreePlayback(PlaybackContext *ctx) {
  if (!ctx)
    return;
  if (ctx->hwo) {
    waveOutUnprepareHeader(ctx->hwo, &ctx->hdr, sizeof(WAVEHDR));
    waveOutClose(ctx->hwo);
  }
  if (ctx->pcm)
    drmp3_free(ctx->pcm, nullptr);
  delete ctx;
}

static DWORD WINAPI CleanupWorkItem(LPVOID p) {
  FreePlayback(reinterpret_cast<PlaybackContext *>(p));
  return 0;
}

static void CALLBACK WaveOutCallback(HWAVEOUT hwo, UINT msg, DWORD_PTR,
                                     DWORD_PTR p1, DWORD_PTR) {
  if (msg != WOM_DONE)
    return;

  auto *hdr = reinterpret_cast<WAVEHDR *>(p1);
  auto *ctx = reinterpret_cast<PlaybackContext *>(hdr->dwUser);

  // Detach if we're still pointing at this device (NO waveOut calls under
  // lock).
  EnterCriticalSection(&g_cs);
  if (g_current == hwo)
    g_current = nullptr;
  LeaveCriticalSection(&g_cs);

  // Never do heavy work in callback: queue cleanup.
  QueueUserWorkItem(CleanupWorkItem, ctx, WT_EXECUTEDEFAULT);
}

static drmp3_int16 *DecodeMp3S16(const std::wstring &path, drmp3_config &cfg,
                                 drmp3_uint64 &frames) {
  frames = 0;
  cfg = {};
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    return nullptr;
  std::vector<char> bytes(static_cast<size_t>(file.tellg()));
  file.seekg(0);
  if (bytes.empty() || !file.read(bytes.data(), bytes.size()))
    return nullptr;
  drmp3_int16 *pcm = drmp3_open_memory_and_read_pcm_frames_s16(
      bytes.data(), bytes.size(), &cfg, &frames, nullptr);
  if (pcm && !frames) {
    drmp3_free(pcm, nullptr);
    return nullptr;
  }
  return pcm;
}

void Init() {
  if (g_init)
    return;
  InitializeCriticalSection(&g_cs);
  g_init = true;
}

void Stop() {
  if (!g_init)
    return;

  // IMPORTANT: do NOT call waveOutReset while holding g_cs.
  HWAVEOUT h = nullptr;
  EnterCriticalSection(&g_cs);
  h = g_current;
  g_current = nullptr;
  LeaveCriticalSection(&g_cs);

  if (h)
    waveOutReset(h); // triggers WOM_DONE -> cleanup queued from callback
}

void PlayFile(const std::wstring &path) {
  Stop();

  drmp3_config cfg{};
  drmp3_uint64 frames = 0;
  drmp3_int16 *pcm = DecodeMp3S16(path, cfg, frames);
  if (!pcm) {
    std::cout << "[Voices] Can't decode: " << Utf8(path) << "\n";
    return;
  }

  auto *ctx = new PlaybackContext{};
  ctx->pcm = pcm;

  WAVEFORMATEX wfx{};
  wfx.wFormatTag = WAVE_FORMAT_PCM;
  wfx.nChannels = (WORD)cfg.channels;
  wfx.nSamplesPerSec = cfg.sampleRate;
  wfx.wBitsPerSample = 16;
  wfx.nBlockAlign = wfx.nChannels * (wfx.wBitsPerSample / 8);
  wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

  MMRESULT r = waveOutOpen(&ctx->hwo, WAVE_MAPPER, &wfx,
                           (DWORD_PTR)WaveOutCallback, 0, CALLBACK_FUNCTION);
  if (r != MMSYSERR_NOERROR) {
    std::cout << "[Voices] waveOutOpen failed: " << Utf8(path) << "\n";
    FreePlayback(ctx);
    return;
  }

  ctx->hdr.lpData = (LPSTR)pcm;
  ctx->hdr.dwBufferLength =
      (DWORD)(frames * cfg.channels * sizeof(drmp3_int16));
  ctx->hdr.dwUser = (DWORD_PTR)ctx;

  r = waveOutPrepareHeader(ctx->hwo, &ctx->hdr, sizeof(WAVEHDR));
  if (r != MMSYSERR_NOERROR) {
    std::cout << "[Voices] waveOutPrepareHeader failed: " << Utf8(path) << "\n";
    FreePlayback(ctx);
    return;
  }

  // Set current *before* write so Stop() can always reset the correct handle.
  EnterCriticalSection(&g_cs);
  g_current = ctx->hwo;
  LeaveCriticalSection(&g_cs);

  r = waveOutWrite(ctx->hwo, &ctx->hdr, sizeof(WAVEHDR));
  if (r != MMSYSERR_NOERROR) {
    EnterCriticalSection(&g_cs);
    if (g_current == ctx->hwo)
      g_current = nullptr;
    LeaveCriticalSection(&g_cs);

    std::cout << "[Voices] waveOutWrite failed: " << Utf8(path) << "\n";
    FreePlayback(ctx);
    return;
  }

  std::cout << "[Voices] Playing: " << Utf8(path) << "\n";
}

} // namespace Audio

// =============================================================================
// Field file names
// =============================================================================

// Character name by speaker id (0-82); existing voice packs use these names.
static const wchar_t *const CHARACTER_NAMES[] = {
    L"serge",        L"kid",          L"guile",        L"norris",
    L"nikki",        L"viper",        L"riddel",       L"karsh",
    L"zoa",          L"marcy",        L"korcha",       L"luccia",
    L"poshul",       L"razly",        L"zappa",        L"orcha",
    L"radius",       L"fargo",        L"macha",        L"glenn",
    L"leena",        L"miki",         L"harle",        L"janice",
    L"draggy",       L"starky",       L"sprigg",       L"mojo",
    L"turnip",       L"neofio",       L"greco",        L"skelly",
    L"funguy",       L"irenes",       L"mel",          L"leah",
    L"van",          L"sneff",        L"steena",       L"doc",
    L"grobyc",       L"pierre",       L"orlha",        L"pip",
    L"pip",          L"pip",          L"pip",          L"pip",
    L"pip",          L"lynx",         L"glenn",        L"lynx",
    L"kid child",    L"balthasar",    L"dwarf",        L"dario",
    L"dark-serge",   L"dark-serge",   L"green-dragon", L"sky-dragon",
    L"black-dragon", L"water-dragon", L"fire-dragon",  L"earth-dragon",
    L"lynx",         L"dragon god",   L"dario",        L"glenn",
    L"karsh",        L"wazuki",       L"miguel",       L"garai",
    L"marge",        L"tia",          L"rosetta",      L"una",
    L"lisa",         L"sage",         L"direa",        L"zippa",
    L"gogh",         L"gogh",         L"orcha",
};
static constexpr int NUM_CHARACTER_NAMES =
    int(sizeof(CHARACTER_NAMES) / sizeof(CHARACTER_NAMES[0]));

// Speaker id of each {NAME%} tag a message can start with.
static const char *const NAME_TAGS[] = {
    "SERGE",  "KID",    "GUILE",  "NORRIS", "NIKKI",  "VIPER",  "RIDDEL",
    "KARSH",  "ZOAH",   "MARCY",  "KORCHA", "LUCCIA", "POSHUL", "RAZZLY",
    "ZAPPA",  "ORCHA",  "RADIUS", "FARGO",  "MACHA",  "GLENN",  "LEENA",
    "MIKI",   "HARLEY", "JANICE", "DRAGGY", "STARKY", "SPRIGG", "MOJO",
    "TURNIP", "NEOFIO", "GRECO",  "SKELLY", "FUNGUY", "IRENES", "MEL",
    "LEEA",   "VAN",    "SNEFF",  "STEENA", "DOC",    "GROBYC", "PIERRE",
    "ORLHA",  "PIP",    nullptr,  nullptr,  nullptr,  nullptr,  nullptr,
    "LYNX",
};
static constexpr int NUM_NAME_TAGS =
    int(sizeof(NAME_TAGS) / sizeof(NAME_TAGS[0]));
static constexpr int SPEAKER_KORCHA = 10;
static constexpr int SPEAKER_MACHA = 18;
static constexpr wchar_t KANJI_MOTHER = 0x6BCD;

static int NameTagSpeaker(const wchar_t *name, size_t length) {
  for (int id = 0; id < NUM_NAME_TAGS; ++id) {
    const char *tag = NAME_TAGS[id];
    if (!tag || std::strlen(tag) != length)
      continue;
    if (std::equal(name, name + length, tag,
                   [](wchar_t a, char b) { return a == wchar_t(b); }))
      return id;
  }
  return -1;
}

// The character named by the first {NAME%} tag (other than Serge's) in the
// run of tags a message starts with, or 0.
static int LeadingTagSpeaker(const wchar_t *text) {
  const wchar_t *at = text;
  for (;;) {
    while (*at == L' ' || *at == L'\t')
      ++at;
    if (*at != L'{')
      return 0;
    const wchar_t *close = std::wcschr(at, L'}');
    if (!close)
      return 0;
    const wchar_t *pct = std::wmemchr(at + 1, L'%', close - at - 1);
    if (pct) {
      const int id = NameTagSpeaker(at + 1, pct - at - 1);
      if (id > 0) {
        // JP: Korcha's tag followed by "mother" names his mother.
        if (id == SPEAKER_KORCHA && close[1] == KANJI_MOTHER)
          return SPEAKER_MACHA;
        return id;
      }
    }
    at = close + 1;
  }
}

static int MessageTagSpeaker(int room, int dialog) {
  if (ReadExe<int32_t>(rde::msg_room_loaded) != (room & 0x7FF))
    return 0;
  const int index = dialog & 0x1FF;
  auto *variant = ReadExe<const wchar_t *>(
      rde::msg_room_ex_rows +
      (index * rde::msg_room_ex_variants + 1) * sizeof(void *));
  auto *base =
      ReadExe<const wchar_t *>(rde::msg_room_rows + index * sizeof(void *));
  const wchar_t *text = variant ? variant : base;
  return text ? LeadingTagSpeaker(text) : 0;
}

static void AddName(std::vector<std::wstring> &names, std::wstring name) {
  if (std::find(names.begin(), names.end(), name) == names.end())
    names.push_back(std::move(name));
}

static void AddCharacter(std::vector<std::wstring> &names, int id) {
  if (id > 0 && id < NUM_CHARACTER_NAMES)
    AddName(names, CHARACTER_NAMES[id]);
}

static std::vector<std::wstring> FieldNames(const State::Line &line) {
  std::vector<std::wstring> names;
  if (line.speaker <= 0)
    AddName(names, L"npc");
  else if (line.speaker < NUM_CHARACTER_NAMES)
    AddName(names, CHARACTER_NAMES[line.speaker]);
  else
    AddName(names, std::to_wstring(line.speaker));
  if (line.speaker <= 0)
    AddName(names, L"serge");
  AddCharacter(names, line.tagSpeaker);
  if (line.speaker <= 0)
    AddCharacter(names, line.prevSpeaker);
  return names;
}

// fin_19 reads its text as 1501 (the ending id) and is also listed as 501.
static std::vector<int> FieldScenes(int room) {
  if (room == 501 || room == 1501)
    return {1501, 501};
  return {room};
}

static std::vector<std::wstring> CandidateFiles(const State::Line &line,
                                                int page) {
  std::vector<std::wstring> rels;
  const std::wstring dialogPage =
      std::to_wstring(line.dialog) + L"-" + std::to_wstring(page);
  if (line.kind == State::Kind::Battle) {
    rels.push_back(L"battle\\" + std::to_wstring(line.scene) + L"\\" +
                   dialogPage + L".mp3");
    return rels;
  }
  const std::vector<std::wstring> names = FieldNames(line);
  for (int scene : FieldScenes(line.scene))
    for (const std::wstring &name : names)
      rels.push_back(std::to_wstring(scene) + L"\\" + dialogPage + L"-" + name +
                     L".mp3");
  return rels;
}

// =============================================================================
// Voice folders: every folder in mods\ in name order (case-insensitive); a
// folder called "voices" is itself the voice root, any other one holds its
// lines in <folder>\voices.
// =============================================================================
static std::vector<std::wstring> VoiceRoots() {
  std::vector<std::wstring> folders;
  WIN32_FIND_DATAW found;
  HANDLE find = FindFirstFileW((State::modsDir + L"*").c_str(), &found);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
          !std::wcscmp(found.cFileName, L".") ||
          !std::wcscmp(found.cFileName, L".."))
        continue;
      folders.push_back(found.cFileName);
    } while (FindNextFileW(find, &found));
    FindClose(find);
  }
  std::sort(folders.begin(), folders.end(),
            [](const std::wstring &a, const std::wstring &b) {
              return _wcsicmp(a.c_str(), b.c_str()) < 0;
            });

  std::vector<std::wstring> roots;
  for (const std::wstring &folder : folders) {
    if (!_wcsicmp(folder.c_str(), L"voices"))
      roots.push_back(State::modsDir + folder + L"\\");
    else
      roots.push_back(State::modsDir + folder + L"\\voices\\");
  }
  return roots;
}

static std::wstring FindVoiceFile(const std::vector<std::wstring> &rels) {
  const std::vector<std::wstring> roots = VoiceRoots();
  for (const std::wstring &rel : rels)
    for (const std::wstring &root : roots) {
      const std::wstring path = root + rel;
      const DWORD attributes = GetFileAttributesW(path.c_str());
      if (attributes != INVALID_FILE_ATTRIBUTES &&
          !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return path;
    }
  return {};
}

static void PlayPage(const State::Line &line, int page) {
  static std::set<std::wstring> loggedMissing;
  const std::vector<std::wstring> rels = CandidateFiles(line, page);
  const std::wstring path = FindVoiceFile(rels);
  if (path.empty()) {
    Audio::Stop();
    if (loggedMissing.insert(rels.front()).second)
      std::cout << "[Voices] Not found: voices\\" << Utf8(rels.front()) << "\n";
    return;
  }
  Audio::PlayFile(path);
}

// =============================================================================
// Worker: follows the window of the newest line, plays each page when it is
// shown and stops when the window closes.
// =============================================================================
static DWORD WINAPI VoiceWorkerThread(LPVOID) {
  State::Line line;
  uint32_t serial = 0;
  int page = -1;
  bool battleWindowOpen = false;

  for (;;) {
    WaitForSingleObject(State::lineEvent, 50);

    EnterCriticalSection(&State::lineLock);
    if (State::pendingSerial != serial) {
      serial = State::pendingSerial;
      line = State::pendingLine;
      page = -1;
      battleWindowOpen = false;
    }
    LeaveCriticalSection(&State::lineLock);

    if (line.kind == State::Kind::None)
      continue;

    bool closed = false;
    int shown = 0;
    if (line.kind == State::Kind::Field) {
      closed = !ReadHost<uint8_t>(line.window + rde::msg_window_in_use);
      shown = ReadHost<int32_t>(line.window + rde::msg_window_page);
    } else {
      const uint8_t state =
          ReadGuest<uint8_t>(rde::btl_window_state + rde::btl_event_window);
      if (state == rde::btl_window_state_open)
        battleWindowOpen = true;
      closed = ReadGuest<uint32_t>(rde::mode_overlay_id) != rde::mode_overlay_battle ||
               (battleWindowOpen && state != rde::btl_window_state_open);
      if (battleWindowOpen)
        shown = ReadGuest<uint16_t>(rde::btl_window1_page);
    }

    if (closed) {
      Audio::Stop();
      line.kind = State::Kind::None;
      continue;
    }
    if (shown != page) {
      page = shown;
      PlayPage(line, page);
    }
  }
  return 0;
}

static void PostLine(const State::Line &line) {
  EnterCriticalSection(&State::lineLock);
  State::pendingLine = line;
  ++State::pendingSerial;
  LeaveCriticalSection(&State::lineLock);
  SetEvent(State::lineEvent);
}

// =============================================================================
// Game-thread events
// =============================================================================

// A field message window opened and its text was looked up.
static void __cdecl OnFieldLineOpened(uintptr_t window) {
  State::Line line;
  line.kind = State::Kind::Field;
  line.window = window;
  line.scene = ReadHost<int32_t>(window + rde::msg_window_room);
  line.dialog = ReadHost<int32_t>(window + rde::msg_window_message);
  line.speaker = (std::max)(ReadExe<int32_t>(rde::name_speaker_column), 0);
  line.tagSpeaker = MessageTagSpeaker(line.scene, line.dialog);

  if (line.scene != State::speakerRoom) {
    State::speakerRoom = line.scene;
    State::roomPrevSpeaker = 0;
  }
  line.prevSpeaker = State::roomPrevSpeaker;
  if (line.speaker > 0)
    State::roomPrevSpeaker = line.speaker;

  PostLine(line);
}

// A battle script E0 op is opening the event message window.
static void __cdecl OnBattleLineOpened() {
  if (ReadGuest<uint32_t>(rde::mode_overlay_id) != rde::mode_overlay_battle)
    return;
  const uint32_t script = GuestRegister(rde::guest_reg_s3);
  State::Line line;
  line.kind = State::Kind::Battle;
  line.scene = ReadGuest<uint16_t>(rde::btl_battle_id);
  line.dialog =
      ReadGuest<uint8_t>(script + 1) | (ReadGuest<uint8_t>(script + 2) << 8);
  PostLine(line);
}

// =============================================================================
// Assembly Hook Functions
// =============================================================================
extern "C" __declspec(naked) void VoicesFieldHook() {
  __asm {
        pushad
        pushfd
        push esi
        call OnFieldLineOpened
        add esp, 4
        popfd
        popad
        jmp dword ptr [State::fieldContinuation]
  }
}

extern "C" __declspec(naked) void VoicesBattleHook() {
  __asm {
        pushad
        pushfd
        call OnBattleLineOpened
        popfd
        popad
        jmp dword ptr [State::battleContinuation]
  }
}

// =============================================================================
// Hook installation
// =============================================================================
static inline void WriteRel32(uint8_t *at, uintptr_t dst, uintptr_t nextIp) {
  const int32_t rel = (int32_t)(dst - nextIp);
  std::memcpy(at, &rel, sizeof(rel));
}

// Copies the instructions the hook overwrites (position-independent at both
// sites) and jumps back after them.
static uintptr_t BuildContinuation(uintptr_t site, int size) {
  void *cont = VirtualAlloc(nullptr, size + 5, MEM_COMMIT | MEM_RESERVE,
                            PAGE_EXECUTE_READWRITE);
  if (!cont)
    return 0;
  auto *c = (uint8_t *)cont;
  std::memcpy(c, (const void *)site, size);
  c[size] = 0xE9;
  WriteRel32(c + size + 1, site + size, (uintptr_t)cont + size + 5);
  return (uintptr_t)cont;
}

static bool InstallHook(uintptr_t site, int size, void *hook,
                        uintptr_t &continuation, const char *name) {
  continuation = BuildContinuation(site, size);
  if (!continuation || !InstallJmpHook(site, hook, size)) {
    std::cerr << "[Voices] Failed to install " << name << " hook\n";
    return false;
  }
  return true;
}

// =============================================================================
// Main Patch Application
// =============================================================================
bool ApplyVoicesPatch(uintptr_t base) {
  State::baseAddress = base;

  wchar_t exePath[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
  std::wstring exeDir(exePath, length);
  exeDir.resize(exeDir.find_last_of(L"\\/") + 1);
  State::modsDir = exeDir + L"mods\\";

  InitializeCriticalSection(&State::lineLock);
  State::lineEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  Audio::Init();

  const bool field =
      InstallHook(base + rde::msg_window_open_text_ready,
                  rde::msg_window_open_text_ready_size, (void *)VoicesFieldHook,
                  State::fieldContinuation, "field");
  const bool battle = InstallHook(
      base + rde::hook_font_text_render_800d191c_obs,
      rde::hook_font_text_render_800d191c_obs_prologue,
      (void *)VoicesBattleHook, State::battleContinuation, "battle");
  if (!field && !battle)
    return false;

  CreateThread(nullptr, 0, VoiceWorkerThread, nullptr, 0, nullptr);

  std::cout << "[Mod] Voice Support Enabled\n";
  return true;
}
