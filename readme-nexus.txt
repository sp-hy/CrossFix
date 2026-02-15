[img]https://i.imgur.com/3VKuMH5.png[/img]

Enhancements for the PC version of [b]Chrono Cross Radical Dreamers Edition[/b].

Supports the latest Steam release v1.0.1.0

[b][size=5]Features[/size][/b]
[list]
[*][b]Widescreen[/b] - Dynamic aspect ratio (16:9, 21:9, 32:9), correct camera boundaries, UI textures, and scaled battle/dialog/shop/save menus
[*][b]60 FPS Mode[/b] - Unlock the frame rate to 60 FPS everywhere
[*][b]Upscaling (2x/3x/4x)[/b] - High resolution rendering for 3D elements (experimental)
[*][b]Background Play[/b] - Keep the game running when tabbed out (prevents pause on focus loss)
[*][b]Mod Loader[/b] - Load replacement assets from a [i]mods/[/i] folder instead of editing .dat files (experimental)
[*][b]Texture Replacer[/b] - Replace in-game textures with custom .png or .dds files in [i]mods/textures/[/i] (hash-based filenames; use texture dump to discover hashes)
[*][b]Voices[/b] - Play custom MP3 voice lines during dialog; files are loaded from [i]mods/voices/[/i] by scene, dialog index, page, and speaker
[/list]

[size=5]Todo[/size]
[list]
[*][size=2]PXGP[/size]
[*][size=2]Voices (battle)[/size]
[/list]

[b][size=5]Installation[/size][/b]
[list=1]
[*]Download the latest release
[*]Extract [color=#3498db][i]d3d11.dll[/i][/color] to your game folder (where [i]CHRONOCROSS.exe[/i] is)
[*]Launch the game
[*](Optional) Edit the [i]settings.ini[/i] that is automatically generated on first run
[/list]
Wine/Proton users should use the following:
[code]ENABLE_GAMESCOPE_WSI=0 PROTON_USE_WINED3D=1 WINEDLLOVERRIDES="d3d11=n,b" %command%[/code]

Disable the frame rate limiter if playing on Steamdeck in gaming mode.

Note that upscaling is not currently compatible with wine/proton and will crash.

[b][size=5]Settings[/size][/b]
CrossFix creates a [i]settings.ini[/i] in your game folder with default values. You can modify it to suit your needs:

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

[b][size=5]Notes[/size][/b]
[list]
[*][b]Double FPS:[/b] Enable in-game slowdown (press F1) when using [i]double_fps_mode=1[/i]. Use [i]hide_slow_icon=1[/i] to hide the slow-motion icon.
[*][b]Mod Loader:[/b] Create a [i]mods/[/i] folder next to the game executable. Place replacement assets using the same path structure as inside the .dat files (e.g. [i]mods/map/mapbin/[/i] for map BINs). Set [i]mod_loader_enabled=1[/i] in settings.ini (default). Texture dump disables the mod loader while active.
[*][b]Texture Replacer:[/b] (1) Set [i]texture_dump_enabled=1[/i], run the game, and visit the area/UI you want to mod—textures are dumped to [i]dump/[/i] with filenames like [i]256x256_0123456789abcdef.dds[/i]. (2) Edit or create a replacement keeping the same name, or use the hash in a new file named [i]WIDTHxHEIGHT_<16hex>.png[/i] or [i].dds[/i] (e.g. [i]256x256_0123456789abcdef.png[/i]). (3) Put replacement files in [i]mods/textures/[/i]. (4) Set [i]texture_dump_enabled=0[/i] and [i]texture_replace_enabled=1[/i], then launch the game.
[*][b]Voices:[/b] With [i]voices_enabled=1[/i], the mod plays MP3 files from [i]mods/voices/[/i] when dialog is shown. Place files as [i]mods/voices/<sceneId>/<dialogIndex>-<page>-<characterName>.mp3[/i] (e.g. [i]mods/voices/123/5-0-serge.mp3[/i] for scene 123, dialog 5, first page, Serge). The game logs the requested path to the console if a file is missing.
[/list]

[b][size=5]Acknowledgements[/size][/b]
[list]
[*][url=https://github.com/stoofin/roomviewer-rde]roomviewer-rde[/url] - Understanding the BIN format and co-ord system for 2D backdops & layers
[*][url=https://github.com/SpecialKO/SpecialK]SpecialK[/url] - Existing CC plugin clock tricks & upscaling
[*][url=https://discord.com/invite/uZXjg55Aa9]Moogles & Mods Discord[/url] - Great resource/hub for CC modding
[*][url=https://github.com/stenzek/duckstation]Duckstation[/url] - Understanding the PSX rendering & widescreen hack
[/list]
