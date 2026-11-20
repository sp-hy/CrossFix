#include "fps.h"
#include "../utils/rde_symbols.h"
#include <iostream>
#include <vector>

// 60 FPS mode draws the field every vblank instead of every second one, at
// normal speed. The field frame driver counts the vblanks since its previous
// frame and runs that many logic ticks, then waits until the room's frame
// period (normally 2 vblanks, 1 in the opening) has passed. While the field
// runs at its normal pace, the wait is given a period of 1 instead of 2, so
// every frame runs one tick: the step the field's own slow-mode path uses,
// drawn twice as often. RDE's host clock, its fast and slow modes and every
// other screen keep their own pace; battles already run one frame per vblank.
//
// Script op CA (turn toward the following walk's goal) is a two-run handshake:
// the first run requests the turn and yields, the second advances only while
// the request is still pending, and the field takes requests up once per
// frame, after the frame's ticks. At two ticks per frame the second run comes
// before the pickup; at one tick per frame the pickup always comes first, so
// the op would request forever and the actor freeze. When a request taken up
// this frame still has its CA waiting, the frame keeps the normal period, so
// the next frame runs two ticks: the op runs again and advances, exactly as
// when its first run fell on the second tick of a 30 fps frame.
namespace {

uintptr_t g_base = 0;

// Set when the current field frame took the normal-pace path and may run at
// one vblank per frame (emulation thread only)
bool g_fieldFullRate = false;

// Turn requests made by op CA during the current field frame: the entity and
// the script pc of the op (emulation thread only)
struct TurnRequest {
  uint32_t entity;
  uint32_t pc;
};
std::vector<TurnRequest> g_turnRequests;

// Frame statistics (emulation thread only): field frames by the vblanks they
// took, split by whether they ran at one vblank per frame
constexpr uint32_t kStatsFrames = 300;
bool g_stats = false;
bool g_statsHavePrevious = false;
bool g_statsPreviousFullRate = false;
uint32_t g_statsPreviousVsync = 0;
uint32_t g_statsFrames = 0;
uint32_t g_statsFullRate[3] = {};
uint32_t g_statsNormalPace = 0;
uint32_t g_statsTurnHandoffs = 0;

// Battle frames by the vblanks they took (emulation thread only)
bool g_battleHavePrevious = false;
uint32_t g_battlePreviousVsync = 0;
uint32_t g_battleFrames = 0;
uint32_t g_battleVblanks[3] = {};

template <typename T> T ReadGuest(uint32_t guestAddr) {
  return *reinterpret_cast<volatile T *>(g_base + rde::Guest(guestAddr));
}

template <typename T> T ReadExe(uintptr_t offset) {
  return *reinterpret_cast<volatile T *>(g_base + offset);
}

volatile uint32_t &GuestRegister(int index) {
  return reinterpret_cast<volatile uint32_t *>(g_base + rde::guest_regs)[index];
}

// The field overlay is resident and its frame driver holds the instructions
// the hooks expect
bool FieldFrameDriverResident() {
  return ReadGuest<uint32_t>(rde::mode_overlay_id) == rde::mode_overlay_field &&
         ReadGuest<uint32_t>(rde::fld_frame_driver_plus_478) ==
             rde::fld_frame_driver_plus_478_word &&
         ReadGuest<uint32_t>(rde::fld_frame_driver_plus_574) ==
             rde::fld_frame_driver_plus_574_word &&
         ReadGuest<uint32_t>(rde::fld_frame_driver_plus_1020) ==
             rde::fld_frame_driver_plus_1020_word;
}

// Counts the frame that just ended: the main loop stored the vsync count at
// each frame, so the difference is the vblanks the previous frame took
void RecordFrameStats() {
  uint32_t vsync = ReadGuest<uint32_t>(rde::sys_last_vsync_count);
  if (g_statsHavePrevious) {
    uint32_t vblanks = vsync - g_statsPreviousVsync;
    if (!g_statsPreviousFullRate)
      g_statsNormalPace++;
    else
      g_statsFullRate[vblanks <= 1 ? 0 : vblanks == 2 ? 1 : 2]++;
    if (++g_statsFrames == kStatsFrames) {
      std::cout << "[FPS] last " << kStatsFrames << " field frames: 60 fps "
                << g_statsFullRate[0] << ", missed one vblank "
                << g_statsFullRate[1] << ", missed more "
                << g_statsFullRate[2] << ", normal 30 fps pace "
                << g_statsNormalPace << " (turn hand-offs "
                << g_statsTurnHandoffs << ")" << std::endl;
      g_statsFrames = g_statsNormalPace = g_statsTurnHandoffs = 0;
      g_statsFullRate[0] = g_statsFullRate[1] = g_statsFullRate[2] = 0;
    }
  }
  g_statsHavePrevious = true;
  g_statsPreviousVsync = vsync;
  g_statsPreviousFullRate = g_fieldFullRate;
}

// Guest hook: a battle frame starts. Counts the frame before it like
// RecordFrameStats; battle runs one frame per vblank, two in slow mode.
void __cdecl OnBattleFrame() {
  if (ReadGuest<uint32_t>(rde::mode_overlay_id) != rde::mode_overlay_battle ||
      ReadGuest<uint32_t>(rde::btl_frame_cycle) != rde::btl_frame_cycle_word) {
    g_battleHavePrevious = false;
    return;
  }
  uint32_t vsync = ReadGuest<uint32_t>(rde::sys_last_vsync_count);
  if (g_battleHavePrevious) {
    uint32_t vblanks = vsync - g_battlePreviousVsync;
    g_battleVblanks[vblanks <= 1 ? 0 : vblanks == 2 ? 1 : 2]++;
    if (++g_battleFrames == kStatsFrames) {
      std::cout << "[FPS] last " << kStatsFrames << " battle frames: 60 fps "
                << g_battleVblanks[0] << ", 2 vblanks (30 fps) "
                << g_battleVblanks[1] << ", 3+ vblanks " << g_battleVblanks[2]
                << std::endl;
      g_battleFrames = 0;
      g_battleVblanks[0] = g_battleVblanks[1] = g_battleVblanks[2] = 0;
    }
  }
  g_battleHavePrevious = true;
  g_battlePreviousVsync = vsync;
}

// Op CA is resident and holds the instructions its hook expects
bool FaceTargetOpResident() {
  return ReadGuest<uint32_t>(rde::mode_overlay_id) == rde::mode_overlay_field &&
         ReadGuest<uint32_t>(rde::fld_op_face_target_then_advance_plus_10c) ==
             rde::fld_op_face_target_then_advance_plus_10c_word &&
         ReadGuest<uint32_t>(rde::fld_op_face_target_then_advance_plus_11c) ==
             rde::fld_op_face_target_then_advance_plus_11c_word;
}

// Guest hook: op CA's first run sets the turn request of the entity in v1
void __cdecl OnFaceTargetRequest() {
  if (!FaceTargetOpResident())
    return;
  uint32_t entity = GuestRegister(rde::guest_reg_v1);
  if (entity - rde::guest_origin > rde::guest_ram_size - rde::fld_actor_size)
    return;
  g_turnRequests.push_back(
      {entity, ReadGuest<uint32_t>(entity + rde::fld_actor_script_pc)});
}

// A turn request made this frame was taken up while its op still waits for
// the second run
bool TurnRequestTakenEarly() {
  for (const TurnRequest &request : g_turnRequests)
    if ((ReadGuest<uint32_t>(request.entity + rde::fld_actor_state) &
         rde::fld_actor_state_turn_request) == 0 &&
        ReadGuest<uint32_t>(request.entity + rde::fld_actor_script_pc) ==
            request.pc)
      return true;
  return false;
}

// Guest hook: the speed gates of a new field frame start
void __cdecl OnFieldSpeedGates() {
  g_turnRequests.clear();
  if (!FieldFrameDriverResident())
    return;
  if (g_stats)
    RecordFrameStats();
  g_fieldFullRate = false;
}

// Guest hook: the frame passed the game's own speed gates (maps 8 and 9,
// variable 0x36, room change, movie, entry transition) and neither fast nor
// slow mode took its path. The input lock counts field frames and room 386 is
// left alone as RDE does, so both keep the normal pace; so does the full-pace
// frame slow mode inserts every 300 frames.
void __cdecl OnFieldNormalPace() {
  if (!FieldFrameDriverResident())
    return;
  g_fieldFullRate = ReadExe<uint8_t>(rde::sys_field_slow_latch) == 0 &&
                    ReadExe<uint8_t>(rde::sys_field_fast_latch) == 0 &&
                    ReadGuest<uint16_t>(rde::fld_input_lock_frames) == 0 &&
                    ReadGuest<uint16_t>(rde::fld_map_id) !=
                        rde::fld_speed_locked_map;
}

// Guest hook: the end-of-frame wait has loaded the frame period into a1.
// Only the normal period of 2 vblanks is shortened; the loop reloads it from
// guest memory on every pass, which is never written. A frame whose turn
// request was taken up before op CA's second run keeps the period of 2.
void __cdecl OnFieldFrameWait() {
  if (!g_fieldFullRate || !FieldFrameDriverResident())
    return;
  volatile uint32_t &period = GuestRegister(rde::guest_reg_a1);
  if (period != 2)
    return;
  if (TurnRequestTakenEarly()) {
    g_fieldFullRate = false;
    g_statsTurnHandoffs++;
    return;
  }
  period = 1;
  static bool announced = false;
  if (!announced) {
    announced = true;
    std::cout << "[Mod] 60 FPS mode: field running at 60 fps" << std::endl;
  }
}

// Hooks are registered once the guest has loaded its first mode overlay, after
// RDE has installed its own (see widescreen2d.cpp)
DWORD WINAPI RegisterGuestHooksThread(LPVOID) {
  while (ReadGuest<uint32_t>(rde::mode_overlay_id) == 0)
    Sleep(10);

  typedef void(__stdcall * RegisterHook_t)(uint32_t, void (*)());
  RegisterHook_t registerHook =
      reinterpret_cast<RegisterHook_t>(g_base + rde::register_hook);
  registerHook(rde::fld_frame_driver_plus_478, OnFieldSpeedGates);
  registerHook(rde::fld_frame_driver_plus_574, OnFieldNormalPace);
  registerHook(rde::fld_frame_driver_plus_1024, OnFieldFrameWait);
  registerHook(rde::fld_op_face_target_then_advance_plus_11c,
               OnFaceTargetRequest);
  if (g_stats)
    registerHook(rde::btl_frame_cycle, OnBattleFrame);
  std::cout << "[Mod] 60 FPS mode: field frame hooks registered" << std::endl;
  return 0;
}

// The hook registrar and RDE's latch hook, whose operands name the latches,
// the input lock and the map id read above
bool VerifyExe(uintptr_t base) {
  const unsigned char registerHookPrologue[11] = {
      0x83, 0xEC, 0x0C, 0x53, 0x8B, 0x5C, 0x24, 0x14, 0x55, 0x56, 0x57};
  return VerifyBytes(base + rde::register_hook, registerHookPrologue,
                     sizeof(registerHookPrologue), "60 FPS mode register_hook") &&
         VerifyOperand(base + rde::hook_speed_boost_800add80_obs_plus_c,
                       base + rde::Guest(rde::fld_input_lock_frames),
                       "60 FPS mode input lock") &&
         VerifyOperand(base + rde::hook_speed_boost_800add80_obs_plus_2a,
                       base + rde::Guest(rde::fld_map_id),
                       "60 FPS mode map id") &&
         VerifyOperand(base + rde::hook_speed_boost_800add80_obs_plus_47,
                       base + rde::sys_field_fast_latch,
                       "60 FPS mode fast latch") &&
         VerifyOperand(base + rde::hook_speed_boost_800add80_obs_plus_51,
                       base + rde::sys_field_slow_latch,
                       "60 FPS mode slow latch");
}

} // namespace

bool Apply60FpsPatch(uintptr_t base, bool stats) {
  if (!VerifyExe(base)) {
    std::cout << "[Mod] 60 FPS mode: skipped" << std::endl;
    return false;
  }
  g_base = base;
  g_stats = stats;
  HANDLE thread =
      CreateThread(nullptr, 0, RegisterGuestHooksThread, nullptr, 0, nullptr);
  if (!thread) {
    std::cout << "[Mod] 60 FPS mode: failed to start the hook thread"
              << std::endl;
    return false;
  }
  CloseHandle(thread);
  return true;
}
