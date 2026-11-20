<img width="500px" alt="image" src="https://github.com/user-attachments/assets/319f1918-8419-4951-b10c-a5a1b6c4f42e" />

Enhancements for the PC version of Chrono Cross Radical Dreamers Edition.

Supports the latest Steam release v1.0.1.0

## Features

- **Widescreen** - Dynamic aspect ratio (16:9, 21:9, 32:9), correct camera boundaries, UI textures, and scaled battle/dialog/shop/save menus
- **60 FPS Mode** - Unlock the frame rate to 60 FPS everywhere
- **Resolution scale (1x-16x)** - Renders the 3D picture at any multiple of the PS1 resolution instead of the game's fixed 4x, framebuffer effects such as the battle swirl included
- **Background Play** - Keep the game running when tabbed out (prevents pause on focus loss)
- **Mod Loader** - Load replacement assets from a `mods/` folder instead of editing .dat files (experimental)
- **Texture Replacer** - Replace in-game textures with custom .png or .dds files in `mods/textures/` (hash-based filenames; use texture dump to discover hashes)
- **Voices** - Play custom MP3 voice lines during field and battle dialogue; voice packs in `mods/<pack>/voices/` (or `mods/voices/`) by scene, dialog index, page, and speaker, the same layout OpenCross reads
- **PGXP** - Precise sub-pixel 3D vertex positions, culling correction and perspective-correct textures

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

# ============================================
#   Performance / timing
# ============================================

# 60 FPS mode: the field runs at 60 FPS at normal speed (battles already do). 0 = 30 field / 60 battle, 1 = 60 field and battle
60fps_mode=1

# Keep running when window loses focus (no pause, music continues). 0 = pause when inactive, 1 = keep running
disable_pause_on_focus_loss=1

# ============================================
#   Textures / upscaling
# ============================================

# Resolution scale: 1-16 (4 is RDE's default).
resolution_scale=4

# Dump textures to /dump/ (hash filenames). Disables texture resizing and mod loader while on.
texture_dump_enabled=0

# Force point/nearest filtering (fixes lines on overlays when upscaling). 0 = default, 1 = pixelated
sampler_force_point=0

# ============================================
#   PGXP (geometry precision)
# ============================================

# Precise 3D geometry: less wobble, perspective-correct textures (F6 toggles)
pgxp_enabled=1

# ============================================
#   Modding
# ============================================

# Mod loader: load from mods/ instead of .dat (e.g. mods/map/mapbin/)
mod_loader_enabled=1

# Load replacement textures from mods/textures (by hash)
texture_replace_enabled=0

# Play voice-pack MP3s for field and battle dialogue (mods/<pack>/voices/... or mods/voices/...)
voices_enabled=0

# Volume of the voice lines, 0-100
voice_volume=100
```

## Notes

- **60 FPS mode:** With `60fps_mode=1` the field draws 60 frames a second at normal speed while you walk around freely. Room changes, the first moments in a new room, movies, the title and ending keep the game's own pace (30 FPS), as do menus and shops. RDE's fast (F2) and slow (F1) modes work as usual; while either is on, the field runs at 30 FPS. Battles already run at 60 FPS. When a character turns to start a scripted walk, the field runs a single frame at the normal pace so the turn is handed over as at 30 FPS; without it such characters froze in place in cutscenes.
- **Frame stats:** add `fps_stats=1` to settings.ini to print, every 300 field frames, how many ran at a clean 60 fps, how many missed a vblank and how many ran at the normal 30 fps pace (and how many of those were turn hand-offs). Battles print the same per 300 battle frames. The game always presents 60 times a second, so an FPS overlay cannot show this.
- **PGXP:** The game projects 3D vertices to whole pixels (most models and rooms to a quarter pixel in RDE), which makes geometry wobble. With `pgxp_enabled=1` (the default) each projection is recomputed at full precision, including the fractions the game drops when it animates models and combines their joint and object matrices, and the polygons that use it are drawn at the precise position, with perspective-correct textures and culling decided from the precise positions. Polygons the game builds in 2D (menus, sprites) are left alone. F6 switches PGXP off and on while you play (with the game window focused), to compare with the original picture.
- **Mod Loader:** Create a `mods/` folder next to the game executable. Place replacement assets using the same path structure as inside the .dat files (e.g. `mods/map/mapbin/` for map BINs). Set `mod_loader_enabled=1` in settings.ini (default). Texture dump disables the mod loader while active.
- **Texture Replacer:** (1) Set `texture_dump_enabled=1`, run the game, and visit the area/UI you want to mod—textures are dumped to `dump/` with filenames like `256x256_0123456789abcdef.dds`. (2) Edit or create a replacement keeping the same name, or use the hash in a new file named `WIDTHxHEIGHT_<16hex>.png` or `.dds` (e.g. `256x256_0123456789abcdef.png`). (3) Put replacement files in `mods/textures/`. (4) Set `texture_dump_enabled=0` and `texture_replace_enabled=1`, then launch the game.
- **Voices:** With `voices_enabled=1`, the mod plays an MP3 when a dialogue page is shown and stops it when the page changes or the window closes. Every folder in `mods/` is searched in name order (case-insensitive): `mods/<pack>/voices/...`; a folder called `voices` is searched as `mods/voices/voices/...`, then as `mods/voices/...`; the first file found wins. MP3 files only; packed (.zip, .7z, .rar) mods are not searched. `voice_volume` (0-100, default 100) sets the loudness of the lines.
  - **Field:** `voices/<scene>/<dialog>-<page>-<character>.mp3` (e.g. `voices/27/15-0-npc.mp3`). `scene` is the text room id (the map id, 1000 + the map id in the `fin_` ending rooms; `fin_19` tries 1501, then 501), `dialog` the message index in the room, `page` the 0-based page. `character` is the speaker's lowercase name (`serge`, `kid`, `guile` ... `orcha`, a number past the table); the names tried, first found wins: `npc` for no speaker or speaker 0 (else the speaker's name), then `serge` for speaker 0, then the character the line's text names: its leading name tag (`{KID%}` ...), else the speaker, else `kid` (so a line without a speaker or a tag also tries `-kid.mp3`; while Serge wears Lynx's body his lines try `lynx` there), then, for a line without a speaker, the last speaker of the same room.
  - **Battle:** `voices/battle/<battle id>/<msg>-<page>-<unit>.mp3`, then `voices/battle/<battle id>/<msg>-<page>.mp3` (e.g. `voices/battle/386/0-0-serge.mp3`, `voices/battle/386/0-0.mp3`). `battle id` is the battle's own id (`evt<id>.pac`, not the id whose text rows it reads), `msg` the battle script message index. `unit` is the name, as the battle shows it in the game's language without the enemy letter, of the unit whose script issued the message (a message of the battle's own script uses the first party member), lowercase, with spaces and `\ / : * ? " < > |` removed (`Dead Ahead` -> `deadahead`). That unit is not always the one speaking (battle 386's message 0 is Radius' line issued by Serge's script), so a pack should use the form without the unit unless the unit matters.
  - The console logs each played file and, once, each line with no file.

## Acknowledgements

- [roomviewer-rde](https://github.com/stoofin/roomviewer-rde) - Understanding the BIN format and co-ord system for 2D backdops & layers
- [SpecialK](https://github.com/SpecialKO/SpecialK) - Existing CC plugin clock tricks & upscaling
- [Moogles & Mods Discord](https://discord.com/invite/uZXjg55Aa9) - Great resource/hub for CC modding
- [Duckstation](https://github.com/stenzek/duckstation) - Understanding the PSX rendering & widescreen hack
