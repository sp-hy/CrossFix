#define DR_MP3_IMPLEMENTATION
#include "voices.h"
#include "../utils/memory.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <dr_mp3.h>
#include <iostream>
#include <mmsystem.h>
#include <string>

#pragma comment(lib, "winmm.lib")

// =============================================================================
// Game Memory Offsets (relative to CHRONOCROSS.exe base address)
// =============================================================================
namespace Offsets {
constexpr uintptr_t HOOK_PROLOGUE = 0x5188F; // Hook entry point: push ebx
constexpr uintptr_t HOOK_EPILOGUE =
    0x518AD; // Hook exit point: mov ecx,[1891C18]
constexpr uintptr_t AFTER_EPILOGUE = 0x518B3; // Return address after hook
constexpr uintptr_t PAGINATION = 0xEBFD98;    // Dialog page index (4 bytes)

constexpr uintptr_t CALL1_TARGET = 0x26D60A; // First call destination
constexpr uintptr_t CALL2_TARGET = 0x51580;  // Second call destination

constexpr int PROLOGUE_SIZE = 30; // Bytes between prologue and epilogue
} // namespace Offsets

// =============================================================================
// Global State
// =============================================================================
namespace State {
struct DialogContext {
  uint32_t sceneId = 0;
  uint32_t characterId = 0;
  uint32_t dialogIndex = 0;
} current, last;

uintptr_t baseAddress = 0;
uintptr_t paginationAddress = 0;
uintptr_t trampolineAddress = 0;
uintptr_t continuationAddress = 0;

uint32_t lastPagination = 0;
bool paginationValid = false;
} // namespace State

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

static bool DecodeMp3S16(const char *path, drmp3_config &cfg,
                         drmp3_uint64 &frames, drmp3_int16 *&pcmOut) {
  frames = 0;
  cfg = {};
  pcmOut =
      drmp3_open_file_and_read_pcm_frames_s16(path, &cfg, &frames, nullptr);
  return pcmOut && frames;
}

bool Init() {
  if (g_init)
    return true;
  InitializeCriticalSection(&g_cs);
  g_init = true;
  return true;
}

void Shutdown() {
  if (!g_init)
    return;

  // Stop without holding the lock across waveOutReset (prevents
  // deadlock/freeze).
  HWAVEOUT h = nullptr;
  EnterCriticalSection(&g_cs);
  h = g_current;
  g_current = nullptr;
  LeaveCriticalSection(&g_cs);

  if (h)
    waveOutReset(h); // callback will queue cleanup

  DeleteCriticalSection(&g_cs);
  g_init = false;
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

void PlayFile(const char *filename) {
  if (!g_init)
    Init();
  Stop();

  drmp3_config cfg{};
  drmp3_uint64 frames = 0;
  drmp3_int16 *pcm = nullptr;

  if (!DecodeMp3S16(filename, cfg, frames, pcm)) {
    if (pcm)
      drmp3_free(pcm, nullptr);
    std::cout << "[Voices] Failed to decode: " << filename << "\n";
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
    std::cout << "[Voices] waveOutOpen failed: " << filename << "\n";
    FreePlayback(ctx);
    return;
  }

  ctx->hdr.lpData = (LPSTR)pcm;
  ctx->hdr.dwBufferLength =
      (DWORD)(frames * cfg.channels * sizeof(drmp3_int16));
  ctx->hdr.dwUser = (DWORD_PTR)ctx;

  r = waveOutPrepareHeader(ctx->hwo, &ctx->hdr, sizeof(WAVEHDR));
  if (r != MMSYSERR_NOERROR) {
    std::cout << "[Voices] waveOutPrepareHeader failed: " << filename << "\n";
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

    std::cout << "[Voices] waveOutWrite failed: " << filename << "\n";
    FreePlayback(ctx);
    return;
  }

  std::cout << "[Voices] Playing: " << filename << "\n";
}

} // namespace Audio

static std::string BuildVoiceFilename(uint32_t sceneId, uint32_t dialogIndex,
                                      uint32_t pagination,
                                      uint32_t characterId) {
  return "mods\\voices\\" + std::to_string(sceneId) + "\\" +
         std::to_string(dialogIndex) + "-" + std::to_string(pagination) + "-" +
         std::to_string(characterId) + ".mp3";
}

// =============================================================================
// Dialog Event Handlers
// =============================================================================
static void OnDialogDisplayed() {
  uint32_t page = 0;
  if (State::paginationAddress)
    page = *reinterpret_cast<volatile uint32_t *>(State::paginationAddress);

  Audio::Stop();
  Audio::PlayFile(BuildVoiceFilename(State::current.sceneId,
                                     State::current.dialogIndex, page,
                                     State::current.characterId)
                      .c_str());

  State::last = State::current;
  State::lastPagination = page;
  State::paginationValid = true;
}

static DWORD WINAPI PaginationMonitorThread(LPVOID) {
  for (;;) {
    Sleep(120);
    if (!State::paginationAddress || !State::paginationValid)
      continue;

    const uint32_t page =
        *reinterpret_cast<volatile uint32_t *>(State::paginationAddress);

    // New dialog index coming through? let the epilogue hook handle it.
    if (State::current.dialogIndex != State::last.dialogIndex) {
      State::lastPagination = page;
      continue;
    }

    if (page == State::lastPagination)
      continue;
    State::lastPagination = page;

    if (page == 0) {
      Audio::Stop();
      continue;
    }

    Audio::Stop();
    Audio::PlayFile(BuildVoiceFilename(State::last.sceneId,
                                       State::last.dialogIndex, page,
                                       State::last.characterId)
                        .c_str());
  }
  return 0;
}

// =============================================================================
// Assembly Hook Functions
// =============================================================================
extern "C" __declspec(naked) void VoicesPrologueHook() {
  __asm {
        mov dword ptr [State::current.sceneId], edx
        mov dword ptr [State::current.characterId], esi
        mov dword ptr [State::current.dialogIndex], edi
        jmp dword ptr [State::trampolineAddress]
  }
}

extern "C" __declspec(naked) void VoicesEpilogueHook() {
  __asm {
        pushad
        pushfd
        call OnDialogDisplayed
        popfd
        popad
        jmp dword ptr [State::continuationAddress]
  }
}

// =============================================================================
// Trampoline Construction Helpers
// =============================================================================
static inline void WriteRel32(uint8_t *at, uintptr_t dst, uintptr_t nextIp) {
  const int32_t rel = (int32_t)(dst - nextIp);
  std::memcpy(at, &rel, sizeof(rel));
}

bool BuildTrampoline(uintptr_t base) {
  constexpr int TRAMPOLINE_SIZE = Offsets::PROLOGUE_SIZE + 5; // + JMP
  void *tramp = VirtualAlloc(nullptr, TRAMPOLINE_SIZE, MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE);
  if (!tramp) {
    std::cerr << "[Voices] Failed to allocate trampoline\n";
    return false;
  }

  State::trampolineAddress = (uintptr_t)tramp;
  auto *t = (uint8_t *)tramp;

  std::memcpy(t, (const void *)(base + Offsets::HOOK_PROLOGUE),
              Offsets::PROLOGUE_SIZE);

  // Fix rel32 CALLs inside copied prologue (offsets are specific to this hook
  // site)
  WriteRel32(t + 12, base + Offsets::CALL1_TARGET,
             State::trampolineAddress + 16);
  WriteRel32(t + 22, base + Offsets::CALL2_TARGET,
             State::trampolineAddress + 26);

  // Jump to epilogue hook
  t[30] = 0xE9;
  WriteRel32(t + 31, (uintptr_t)VoicesEpilogueHook,
             State::trampolineAddress + 35);
  return true;
}

bool BuildContinuation(uintptr_t base) {
  constexpr int CONT_SIZE = 6 + 5; // original + JMP
  void *cont = VirtualAlloc(nullptr, CONT_SIZE, MEM_COMMIT | MEM_RESERVE,
                            PAGE_EXECUTE_READWRITE);
  if (!cont) {
    std::cerr << "[Voices] Failed to allocate continuation\n";
    return false;
  }

  State::continuationAddress = (uintptr_t)cont;
  auto *c = (uint8_t *)cont;

  std::memcpy(c, (const void *)(base + Offsets::HOOK_EPILOGUE), 6);

  c[6] = 0xE9;
  WriteRel32(c + 7, base + Offsets::AFTER_EPILOGUE,
             State::continuationAddress + 11);
  return true;
}

// =============================================================================
// Main Patch Application
// =============================================================================
bool ApplyVoicesPatch(uintptr_t base) {
  State::baseAddress = base;
  State::paginationAddress = base + Offsets::PAGINATION;
  State::paginationValid = false;

  Audio::Init();

  if (!BuildTrampoline(base)) {
    Audio::Shutdown();
    return false;
  }

  if (!BuildContinuation(base)) {
    VirtualFree((void *)State::trampolineAddress, 0, MEM_RELEASE);
    Audio::Shutdown();
    return false;
  }

  if (!InstallJmpHook(base + Offsets::HOOK_PROLOGUE, (void *)VoicesPrologueHook,
                      5)) {
    VirtualFree((void *)State::continuationAddress, 0, MEM_RELEASE);
    VirtualFree((void *)State::trampolineAddress, 0, MEM_RELEASE);
    Audio::Shutdown();
    std::cerr << "[Voices] Failed to install prologue hook\n";
    return false;
  }

  CreateThread(nullptr, 0, PaginationMonitorThread, nullptr, 0, nullptr);

  std::cout << "[Voices] Patch applied successfully\n";
  return true;
}
