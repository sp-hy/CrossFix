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
# Widescreen patch
# Set in-game ScreenType to "Full"
# Note: 2D backdrops currently not working
widescreen_enabled=1

# Aspect ratio
# 0 = 16:9, 1 = 21:9, 2 = 32:9
widescreen_mode=0

# 60 FPS mode
# Use with in-game slowdown mode (F1)
# Disable if using SpecialK or similar tools
double_fps_mode=0

# Prevent pause when tabbed out
disable_pause_on_focus_loss=1
```

## Notes

- Back up your game files before using
- The widescreen fix requires the in-game setting **ScreenType: Full**
- For 60 FPS, enable the in-game slowdown mode (press F1)
