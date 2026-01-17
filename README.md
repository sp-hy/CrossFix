<img width="500px" alt="image" src="https://github.com/user-attachments/assets/319f1918-8419-4951-b10c-a5a1b6c4f42e" />

Enhancements for the PC version of Chrono Cross Radical Dreamers Edition.
Supports the latest Steam release v1.0.1.0

## Features

- **Dynamic Widescreen** - Automatically scales to any aspect ratio (16:9, 21:9, 32:9, etc.)
- **60 FPS Mode** - Unlock the frame rate to 60 FPS everywhere
- **4K Upscaling** - High resolution rendering for 3D elements
- **Background Play** - Keep the game running when tabbed out (prevents pause on focus loss)

## Todo:
- Unstretch Menus/FMVs/Dialogs
- PXGP
- Modding Framework to use mods folder instead of dat/zips

## Installation

1. Download the latest release
2. Extract `d3d11.dll` to your game folder (where `CHRONOCROSS.exe` is)
3. Launch the game
4. (Optional) Edit the `settings.ini` that is automatically generated on first run

## Settings

CrossFix creates a `settings.ini` in your game folder with default values. You can modify it to suit your needs:

```ini
# Chrono Cross Crossfix Settings

# Enable or disable the dynamic widescreen patch
# Must be used with the in-game setting ScreenType: Full
widescreen_enabled=1

# Enable or disable the double FPS mode
# Should be used with the in-game slowdown mode (Press F1)
double_fps_mode=1

# Disable pause when window loses focus
# 1 = game & music continue running when window is inactive
disable_pause_on_focus_loss=1

# Enable or disable 4K Upscaling
upscale_4k=1
```

## Notes

- The widescreen fix requires the in-game setting **ScreenType: Full**
- For double FPS mode, ensure the in-game slowdown mode is active (press F1). This should activate by default.


## Acknowledgements

- [roomviewer-rde](https://github.com/stoofin/roomviewer-rde) - Understanding the BIN format and co-ord system for 2D backdops & layers
- [SpecialK](https://github.com/SpecialKO/SpecialK) - Existing CC plugin clock tricks & upscaling
- [Moogles & Mods Discord](https://discord.com/invite/uZXjg55Aa9) - Great resource/hub for CC modding
- [Duckstation](https://github.com/stenzek/duckstation) - Understanding the PSX rendering & widescreen hack