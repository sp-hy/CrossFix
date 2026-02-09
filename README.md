<img width="500px" alt="image" src="https://github.com/user-attachments/assets/319f1918-8419-4951-b10c-a5a1b6c4f42e" />

Enhancements for the PC version of Chrono Cross Radical Dreamers Edition.

Supports the latest Steam release v1.0.1.0

## Features

- **Dynamic Widescreen** - Automatically scales to any aspect ratio with correct camera boundaries (16:9, 21:9, 32:9, etc.)
- **Texture Resizing** - Pillarboxing for UI textures to prevent stretching on widescreen
- **UI Scaling** - Battle UI, healthbars, dialog boxes, and shop menus properly scale with aspect ratio
- **60 FPS Mode** - Unlock the frame rate to 60 FPS everywhere
- **Upscaling (2x/3x/4x)** - High resolution rendering for 3D elements (experimental)
- **Background Play** - Keep the game running when tabbed out (prevents pause on focus loss)
- **Mod Loader** - Load replacement assets from a `mods/` folder instead of editing .dat files (experimental)

## Todo

- PXGP
- Minor remaining UI bugs

## Installation

1. Download the latest release
2. Extract `d3d11.dll` to your game folder (where `CHRONOCROSS.exe` is)
3. Launch the game
4. (Optional) Edit the `settings.ini` that is automatically generated on first run

Wine/Proton users should use the following:
`ENABLE_GAMESCOPE_WSI=0 PROTON_USE_WINED3D=1 WINEDLLOVERRIDES="d3d11=n,b" %command%`

Note that upscaling is not currently compatible with wine/proton and will crash.

## Settings

CrossFix creates a `settings.ini` in your game folder with default values. You can modify it to suit your needs:

```ini
# Chrono Cross Crossfix Settings

# Enable or disable the dynamic widescreen patch
widescreen_enabled=1

# Enable or disable the double FPS mode
# Should be used with the in-game slowdown mode (Press F1)
double_fps_mode=1

# Disable pause when window loses focus
# 1 = game & music continue running when window is inactive
disable_pause_on_focus_loss=1

# Texture upscale: 1 = off, 2 = 2x, 3 = 3x, 4 = 4x (experimental, may cause crashes)
upscale_scale=1

# Force camera boundaries in rooms for consistent widescreen
# Disable if this breaks camera placement in a scene
boundary_overrides=1

# Mod loader: load replacement assets from mods/ folder instead of .dat files
# E.g. mods/map/mapbin/ instead of editing hd.dat
mod_loader_enabled=0

# Texture dumping for modding (dumps to /dump/ with hash-based filenames)
# Disables texture resizing when enabled
texture_dump_enabled=0

# Texture replacement from mods/dump (replacement .png by hash)
texture_replace_enabled=0

# Force POINT texture filtering (fixes lines around overlay textures when upscaling)
sampler_force_point=0
```

## Notes

- For double FPS mode, ensure the in-game slowdown mode is active (press F1). This should activate by default and hide the icon.
- **Mod Loader:** Create a `mods/` folder next to the game executable. Place replacement assets using the same path structure as inside the .dat files (e.g. `mods/map/mapbin/` for map BINs). Enable `mod_loader_enabled=1` in settings.ini.

## Acknowledgements

- [roomviewer-rde](https://github.com/stoofin/roomviewer-rde) - Understanding the BIN format and co-ord system for 2D backdops & layers
- [SpecialK](https://github.com/SpecialKO/SpecialK) - Existing CC plugin clock tricks & upscaling
- [Moogles & Mods Discord](https://discord.com/invite/uZXjg55Aa9) - Great resource/hub for CC modding
- [Duckstation](https://github.com/stenzek/duckstation) - Understanding the PSX rendering & widescreen hack
