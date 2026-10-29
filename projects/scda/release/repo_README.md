# SCDA Recomp

> **Just want to play?** Download the zip from the
> [Releases](../../releases) page, unzip it, put your Xbox ISO (or the
> unpacked game folder) into the `gamefiles` folder and start
> `scda_recomp.exe`. That's all - you do not need anything else from this
> page. The ISO is unpacked automatically on the first start.

> [!IMPORTANT]
> **The game is not included - you need your own copy.** It must be the
> **original Xbox ("Xbox Classic") version** of Splinter Cell: Double Agent,
> USA/Europe release (one disc, serial US-094, sold in both regions) - the
> "version 2" that also came out on PS2, GameCube and Wii.
> **The Xbox 360 version does not work**: it is a different game (different
> engine, levels and code), as are the PC and PS3 versions. You need an image
> of the original Xbox disc (ISO/XISO) or its unpacked files.

A static recompilation of the original Xbox version of **Tom Clancy's
Splinter Cell: Double Agent** into a native Windows program, built on the
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit.

This is an unofficial fan project, not affiliated with or endorsed by
Ubisoft or Microsoft. **No game files are included** - you need your own copy
of the game (original Xbox, USA/Europe release).

## Status

The project is finished in the sense that I stopped working on it; another
PC port of the game turned out to be the better base. What works: boot,
menus, new game, the levels, save/load, music, Bink videos, real 5.1
surround, 60 FPS, widescreen, up to 4x internal resolution (Vulkan renderer),
anti-aliasing and local split-screen co-op with two players. Known issues:
occasional graphics glitches, rare crash while loading.

## Playing

Download the zip from the Releases page and follow `README.txt` inside:
put the game ISO (or the unpacked game folder) into `gamefiles`, start
`scda_recomp.exe`. Settings are in `scda_options.txt`.

## Settings

All settings live in `scda_options.txt` next to `scda_recomp.exe` (plain
text, open it with any editor; each option is explained inside). Changes
take effect on the next start. Delete the file to get the defaults back.

| Option | Values (default in bold) | What it does |
|---|---|---|
| **[Audio]** | | |
| `Surround` | **0**, 1, 2 | 0 = stereo through Windows; 1 = Dolby Digital 5.1 passed through to an AV receiver (S/PDIF or HDMI, volume on the receiver); 2 = 5.1 as multichannel PCM through Windows |
| `AudioDevice` | **empty**, `hdmi`, `spdif` / `toslink` / `optical`, or part of a device name | Output for all sound modes: empty = Windows default, `hdmi` / `spdif` = first output of that type, otherwise the first device whose name contains the text (case-insensitive). The outputs are listed in `user\scda_runtime.log` (`[AUDIO]` lines). S/PDIF/Toslink carry 5.1 only as Dolby Digital (`Surround = 1`), HDMI also as PCM (`Surround = 2`) |
| `MuteInBackground` | **0**, 1 | Silent while the game window is not in front |
| **[Graphics]** | | |
| `RenderScale` | 1, **2**, 3, 4 | Internal resolution as a multiple of the Xbox's 480 lines (2 = 960, 4 = 1920) |
| `Widescreen` | 0, **1** | 16:9 (1) or 4:3 as on the Xbox (0) |
| `FPS60` | 0, **1** | 60 frames per second (1) or 30 as on the Xbox (0); videos always play at their own speed |
| `FPS60Coop` | 0, **1** | The same for split-screen co-op (only with `FPS60 = 1`) |
| `BloomSmoothing` | 0-3, **2** | Smooths what the glow effect samples at RenderScale > 1; 0 = off (small lights flicker in the glow), 3 = steadier, wider glow |
| `TextureFiltering` | 0, 1, **2** | 0 = as on the Xbox (textures switch abruptly to a blurrier detail level), 1 = smooth switch, 2 = smooth and sharp on surfaces seen at a shallow angle (anisotropic filtering) |
| `AntiAliasing` | 0, **1** | Smooths jagged edges of the finished picture (only at RenderScale 2-4; at 1 it is skipped because it would blur the HUD) |
| `VSync` | 0, **1** | Synchronise to the monitor (no tearing) |
| `Renderer` | **vulkan**, opengl | Graphics backend |
| `GraphicsCard` | **empty**, name part | Vulkan only: empty = the dedicated graphics card if there is one (e.g. NVIDIA instead of Intel on laptops); otherwise part of the card name, e.g. `GTX` |
| **[Window]** | | |
| `Fullscreen` | **0**, 1 | Borderless fullscreen (1) or window (0) |
| `Monitor` | **1**, 2, ... | Screen the game starts on (1 = primary) |
| `RememberWindow` | 0, **1** | Keep window size and position when closing (window mode) |
| **[Game]** | | |
| `Language` | **EN**, DE, FR, ES, IT, JA | Game language (text and speech, as far as your game version includes it) |
| `SkipIntro` | **0**, 1 | Skip the logo and intro videos at start-up |
| `NightVisionInterference` | **0**, 1 | Night-vision interference (flickering static and crackling sound) near lights in both JBA headquarters levels; 0 = clean as in the other missions, 1 = as on the Xbox |
| `Keyboard` | 0, **1** | Keyboard as player 1 in addition to a controller |
| `Keyboard2` | 0, **1** | Second keyboard layout (numpad, NumLock on) for player 2 in co-op; 0 = arrow keys are player 1's D-pad |

Keyboard layouts and more details: `README.txt` in the release zip. A second
Xbox/XInput controller works as player 2 as well.

## Save games from xemu or a real Xbox

They work, but need your console's signing keys, which are not included.
See [tools/saves/README.md](tools/saves/README.md): read the keys from xemu
once, copy the profile folder into `user\UDATA\5553005e\`, put
`xbox_keys.bin` next to the exe - or re-sign the saves.

## For developers: building from source

Players do not need this section. Requirements: Windows 10/11 x64, Visual Studio 2022 (C++ workload, CMake),
Python 3 with `capstone`, and your own `default.xbe` of the USA/Europe release
(XBE build time stamp `0x44EB67D5`).

The recompiled C code is **not** part of this repository (it is generated
from the game's executable). Generate it from your XBE, then build:

```powershell
# 1. disassembly
py -3 -m tools.disasm path\to\default.xbe `
  --seed-functions projects\scda\function_seeds.json `
  --exclude-seed-functions projects\scda\function_seed_excludes.json `
  -o tools\disasm\output\scda_v060 --force -v

# 2. function identification (see tools/func_id), output to
#    tools\func_id\output\scda_seeded_corrected

# 3. C code generation
py -3 -m tools.recomp path\to\default.xbe --all --split 1000 `
  --gen-dir tools\recomp\output\scda_v060\gen `
  --output-dir tools\recomp\output\scda_v060 `
  --disasm-dir tools\disasm\output\scda_v060 `
  --func-id-dir tools\func_id\output\scda_seeded_corrected `
  --exclude-manual projects\scda\src\recomp_manual.c `
  --io-functions projects\scda\io_functions.json --skip-binary-check --verbose

# 4. build
cmake -S projects\scda -B build\scda_upgrade -G "Visual Studio 17 2022" -A x64
cmake --build build\scda_upgrade --config RelWithDebInfo --target scda_recomp

# 5. release zip (EXE, DLLs, settings, README, licences)
py -3 projects\scda\release\make_release.py --version 1.0
```

`docs/SCDA_WORKLOG.md` and `PROJECT_STATUS.md` (German) document the whole
investigation, every fix and the diagnostic switches (`RECOMP_*`
environment variables).

## Licences

- The xboxrecomp toolkit and this project's own code: MIT (`LICENSE`).
- Code taken from xemu (NV2A renderer, MCPX APU, DSP): LGPL-2.1 /
  GPL-2.0-or-later; a52dec: GPL-2.0; further components: see `NOTICE`,
  `LICENSES/` and the `licenses` folder of the release.
- Because of the GPL components, the built executable as a whole is
  distributed under the **GPL-2.0-or-later**; this repository is its
  corresponding source.

## Credits

xboxrecomp and its contributors; the xemu project (NV2A renderer, MCPX audio,
DSP JIT PR #3047, PR #949); Matt Borgerson's dsp56300; liba52/a52dec;
libsamplerate; glslang, SPIRV-Tools, SPIRV-Reflect, Vulkan-Headers, Vulkan
Memory Allocator, volk; Cxbx-Reloaded and xboxdevwiki for documentation.
Built with the help of AI coding assistants (Claude, Codex).
