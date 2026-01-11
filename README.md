# Chrono Cross - CrossFix

Fixes and enhancements for the PC version of Chrono Cross.

## Features

- **Widescreen Support** - Play in 16:9, 21:9, or 32:9 aspect ratios
- **60 FPS Mode** - Unlock the frame rate from 30 to 60 FPS
- **Background Play** - Keep the game running when tabbed out

## Installation

1. Download the latest release
2. Extract `winmm.dll` and `settings.ini` to your game folder (where `CHRONOCROSS.exe` is)
3. Launch the game

## Settings

Edit `settings.ini` to configure the fixes:

```ini
# Enable or disable the widescreen patch
# Must be used with the setting ScreenType: Full
# 2D backdrops currently not working & will look weird
# 0 = disabled, 1 = enabled
widescreen_enabled=1

# Widescreen aspect ratio mode
# 0 = 16:9 (standard widescreen)
# 1 = 21:9 (ultrawide)
# 2 = 32:9 (super ultrawide)
widescreen_mode=0

# Enable or disable the double FPS mode
# Should be used with the slowdown mode (Press F1 in game), Should provide a smooth 60 everywhere
# Disable if you use another tool like SpecialK
# 0 = disabled (30 field/60 battle FPS), 1 = enabled (60 FPS everywhere)
double_fps_mode=0

# Disable pause when window loses focus
# 0 = game pauses when window is inactive (default behavior)
# 1 = game & music continue running when window is inactive
disable_pause_on_focus_loss=1
```

## Notes

- Back up your game files before using
- The widescreen fix requires the in-game setting **ScreenType: Full**
- For 60 FPS, enable the in-game slowdown mode (press F1)
- Widescreen 2D textures are WIP, so the backdrops are currently disconnected from the characters.
