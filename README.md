<img width="500px" alt="image" src="https://github.com/user-attachments/assets/319f1918-8419-4951-b10c-a5a1b6c4f42e" />

Enhancements for the PC version of Chrono Cross Radical Dreamers Edition.

Supports the latest Steam release v1.0.1.0

## Features

- **Widescreen** - Dynamic aspect ratio (16:9, 21:9, 32:9), correct camera boundaries, UI textures, and scaled battle/dialog/shop/save menus
- **60 FPS Mode** - Unlock the frame rate to 60 FPS everywhere
- **Upscaling (2x/3x/4x)** - High resolution rendering for 3D elements (experimental)
- **Background Play** - Keep the game running when tabbed out (prevents pause on focus loss)
- **Mod Loader** - Load replacement assets from a `mods/` folder instead of editing .dat files (experimental)
- **Texture Replacer** - Replace in-game textures with custom .png or .dds files in `mods/textures/` (hash-based filenames; use texture dump to discover hashes)
- **Voices** - Play custom MP3 voice lines during dialog; files are loaded from `mods/voices/` by scene, dialog index, page, and speaker

## Todo

- PXGP
- Voices (battle)

## Installation

1. Download the latest release
2. Extract `d3d11.dll` to your game folder (where `CHRONOCROSS.exe` is)
3. Launch the game
4. (Optional) Edit the `settings.ini` that is automatically generated on first run

Wine/Proton users should use the following:
`ENABLE_GAMESCOPE_WSI=0 PROTON_USE_WINED3D=1 WINEDLLOVERRIDES="d3d11=n,b" %command%`

Disable the frame rate limiter if playing on Steamdeck in gaming mode.

Note that upscaling is not currently compatible with wine/proton and will crash.

## Settings

CrossFix creates a `settings.ini` in your game folder with default values. You can modify it to suit your needs:

```ini
# ============================================
#   Display
# ============================================

# Dynamic widescreen (auto-adjusts to resolution/aspect ratio).
widescreen_enabled=1

# Force camera boundaries in rooms . Disable if it breaks camera placement.
boundary_overrides=1

# ============================================
#   Performance / timing
# ============================================

# Double FPS mode: 60 FPS in field. Use with in-game slowdown (F1). 0 = 30 field / 60 battle, 1 = 60 everywhere
double_fps_mode=1

# Hide the slow-motion icon when using double FPS / slowdown
hide_slow_icon=1

# Keep running when window loses focus (no pause, music continues). 0 = pause when inactive, 1 = keep running
disable_pause_on_focus_loss=1

# ============================================
#   Textures / upscaling
# ============================================

# Upscale: 1=off, 2=2x, 3=3x, 4=4x (experimental, may crash on some systems)
upscale_scale=1

# Dump textures to /dump/ (hash filenames). Disables texture resizing and mod loader while on.
texture_dump_enabled=0

# Force point/nearest filtering (fixes lines on overlays when upscaling). 0 = default, 1 = pixelated
sampler_force_point=0

# ============================================
#   Modding
# ============================================

# Mod loader: load from mods/ instead of .dat (e.g. mods/map/mapbin/)
mod_loader_enabled=1

# Load replacement textures from mods/textures (by hash)
texture_replace_enabled=0

# Play custom voice MP3s during dialog (mods/voices/<sceneId>/<dialog>-<page>-<speaker>.mp3)
voices_enabled=0
```

## Notes

- **Double FPS:** Enable in-game slowdown (press F1) when using `double_fps_mode=1`. Use `hide_slow_icon=1` to hide the slow-motion icon.
- **Mod Loader:** Create a `mods/` folder next to the game executable. Place replacement assets using the same path structure as inside the .dat files (e.g. `mods/map/mapbin/` for map BINs). Set `mod_loader_enabled=1` in settings.ini (default). Texture dump disables the mod loader while active.
- **Texture Replacer:** (1) Set `texture_dump_enabled=1`, run the game, and visit the area/UI you want to mod—textures are dumped to `dump/` with filenames like `256x256_0123456789abcdef.dds`. (2) Edit or create a replacement keeping the same name, or use the hash in a new file named `WIDTHxHEIGHT_<16hex>.png` or `.dds` (e.g. `256x256_0123456789abcdef.png`). (3) Put replacement files in `mods/textures/`. (4) Set `texture_dump_enabled=0` and `texture_replace_enabled=1`, then launch the game.
- **Voices:** With `voices_enabled=1`, the mod plays MP3 files from `mods/voices/` when dialog is shown. Place files as `mods/voices/<sceneId>/<dialogIndex>-<page>-<characterName>.mp3` (e.g. `mods/voices/123/5-0-serge.mp3` for scene 123, dialog 5, first page, Serge). The game logs the requested path to the console if a file is missing.

## Acknowledgements

- [roomviewer-rde](https://github.com/stoofin/roomviewer-rde) - Understanding the BIN format and co-ord system for 2D backdops & layers
- [SpecialK](https://github.com/SpecialKO/SpecialK) - Existing CC plugin clock tricks & upscaling
- [Moogles & Mods Discord](https://discord.com/invite/uZXjg55Aa9) - Great resource/hub for CC modding
- [Duckstation](https://github.com/stenzek/duckstation) - Understanding the PSX rendering & widescreen hack
