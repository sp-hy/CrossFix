[img]https://i.imgur.com/3VKuMH5.png[/img]

Enhancements for the PC version of [b]Chrono Cross Radical Dreamers Edition[/b].

Supports the latest Steam release v1.0.1.0

[b][size=5]Features[/size][/b]
[list]
[*][b]Widescreen[/b] - Dynamic aspect ratio (16:9, 21:9, 32:9), correct camera boundaries, UI textures, and scaled battle/dialog/shop/save menus
[*][b]60 FPS Mode[/b] - Unlock the frame rate to 60 FPS everywhere
[*][b]Resolution scale (1x-16x)[/b] - Renders the 3D picture at any multiple of the PS1 resolution instead of the game's fixed 4x, framebuffer effects such as the battle swirl included
[*][b]Background Play[/b] - Keep the game running when tabbed out (prevents pause on focus loss)
[*][b]Mod Loader[/b] - Load replacement assets from a [i]mods/[/i] folder instead of editing .dat files (experimental)
[*][b]Texture Replacer[/b] - Replace in-game textures with custom .png or .dds files in [i]mods/textures/[/i] (hash-based filenames; use texture dump to discover hashes)
[*][b]Voices[/b] - Play custom MP3 voice lines during field and battle dialogue; voice packs in [i]mods/<pack>/voices/[/i] (or [i]mods/voices/[/i]) by scene, dialog index, page, and speaker, the same layout OpenCross reads
[*][b]PGXP[/b] - Precise sub-pixel 3D vertex positions, culling correction and perspective-correct textures
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

[b][size=5]Notes[/size][/b]
[list]
[*][b]60 FPS mode:[/b] With [i]60fps_mode=1[/i] the field draws 60 frames a second at normal speed while you walk around freely. Room changes, the first moments in a new room, movies, the title and ending keep the game's own pace (30 FPS), as do menus and shops. RDE's fast (F2) and slow (F1) modes work as usual; while either is on, the field runs at 30 FPS. Battles already run at 60 FPS. When a character turns to start a scripted walk, the field runs a single frame at the normal pace so the turn is handed over as at 30 FPS; without it such characters froze in place in cutscenes.
[*][b]Frame stats:[/b] add [i]fps_stats=1[/i] to settings.ini to print, every 300 field frames, how many ran at a clean 60 fps, how many missed a vblank and how many ran at the normal 30 fps pace (and how many of those were turn hand-offs). Battles print the same per 300 battle frames. The game always presents 60 times a second, so an FPS overlay cannot show this.
[*][b]PGXP:[/b] The game projects 3D vertices to whole pixels (most models and rooms to a quarter pixel in RDE), which makes geometry wobble. With [i]pgxp_enabled=1[/i] (the default) each projection is recomputed at full precision, including the fractions the game drops when it animates models and combines their joint and object matrices, and the polygons that use it are drawn at the precise position, with perspective-correct textures and culling decided from the precise positions. Polygons the game builds in 2D (menus, sprites) are left alone. F6 switches PGXP off and on while you play (with the game window focused), to compare with the original picture.
[*][b]Mod Loader:[/b] Create a [i]mods/[/i] folder next to the game executable. Place replacement assets using the same path structure as inside the .dat files (e.g. [i]mods/map/mapbin/[/i] for map BINs). Set [i]mod_loader_enabled=1[/i] in settings.ini (default). Texture dump disables the mod loader while active.
[*][b]Texture Replacer:[/b] (1) Set [i]texture_dump_enabled=1[/i], run the game, and visit the area/UI you want to mod—textures are dumped to [i]dump/[/i] with filenames like [i]256x256_0123456789abcdef.dds[/i]. (2) Edit or create a replacement keeping the same name, or use the hash in a new file named [i]WIDTHxHEIGHT_<16hex>.png[/i] or [i].dds[/i] (e.g. [i]256x256_0123456789abcdef.png[/i]). (3) Put replacement files in [i]mods/textures/[/i]. (4) Set [i]texture_dump_enabled=0[/i] and [i]texture_replace_enabled=1[/i], then launch the game.
[*][b]Voices:[/b] With [i]voices_enabled=1[/i], the mod plays an MP3 when a dialogue page is shown and stops it when the page changes or the window closes. Every folder in [i]mods/[/i] is searched in name order: [i]mods/<pack>/voices/...[/i]; a folder called [i]voices[/i] is searched as [i]mods/voices/voices/...[/i], then as [i]mods/voices/...[/i]; the first file found wins. [i]voice_volume[/i] (0-100) sets the loudness. Field lines: [i]voices/<scene>/<dialog>-<page>-<character>.mp3[/i] (scene = text room id, dialog = message index, page from 0, character = speaker name such as [i]serge[/i], [i]kid[/i] or [i]npc[/i]; a line without a speaker tries [i]npc[/i], [i]serge[/i], the character its name tag names (else [i]kid[/i]), then the room's last speaker). Battle lines: [i]voices/battle/<battle id>/<msg>-<page>-<unit>.mp3[/i], then [i]voices/battle/<battle id>/<msg>-<page>.mp3[/i] (e.g. [i]voices/battle/386/0-0.mp3[/i]); unit = the lowercase name, without spaces, of the unit whose script shows the message (not always the speaker).
[/list]

[b][size=5]Acknowledgements[/size][/b]
[list]
[*][url=https://github.com/stoofin/roomviewer-rde]roomviewer-rde[/url] - Understanding the BIN format and co-ord system for 2D backdops & layers
[*][url=https://github.com/SpecialKO/SpecialK]SpecialK[/url] - Existing CC plugin clock tricks & upscaling
[*][url=https://discord.com/invite/uZXjg55Aa9]Moogles & Mods Discord[/url] - Great resource/hub for CC modding
[*][url=https://github.com/stenzek/duckstation]Duckstation[/url] - Understanding the PSX rendering & widescreen hack
[/list]
