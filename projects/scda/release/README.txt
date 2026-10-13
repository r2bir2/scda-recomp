SCDA Recomp - Splinter Cell: Double Agent (original Xbox) for Windows
=====================================================================

A static recompilation of the original Xbox version of Tom Clancy's
Splinter Cell: Double Agent into a native Windows program. This is an
unofficial fan project, not affiliated with or endorsed by Ubisoft or
Microsoft. NO GAME FILES ARE INCLUDED - you need your own copy of the game.

YOU NEED YOUR OWN COPY OF THE GAME, AND IT MUST BE THE RIGHT ONE:
the ORIGINAL XBOX ("Xbox Classic") version of Splinter Cell: Double Agent,
USA/Europe release (one disc for both regions, serial US-094, default.xbe
build 2006-08-22) - the "version 2" that also came out on PS2, GameCube
and Wii.

The XBOX 360 version does NOT work. It is a completely different game
(other engine, levels and code), and so are the PC and PS3 versions. You
need an image (ISO/XISO) of the original Xbox disc or its unpacked files.
Other versions are detected at start and will most likely not work.


REQUIREMENTS
------------
- Windows 10 or 11, 64-bit
- A graphics card with Vulkan support (OpenGL can be chosen in the settings)
- An Xbox/XInput controller, or the keyboard (see "Controls")


SETUP
-----
1. Unzip this folder somewhere you can write to (e.g. your Documents or a
   games folder - not "Program Files").

2. Put the game (your own original Xbox copy - not the Xbox 360 version,
   see above) into the "gamefiles" folder, in one of two ways:

   a) The Xbox ISO (XISO, e.g. made with extract-xiso, or a full disc image)
      Just copy the .iso file into "gamefiles". On the first start it is
      unpacked once into a folder next to it (needs about 4 GB of free
      space and takes a few minutes, a window shows the progress). The ISO
      itself is not changed; you may delete it afterwards.

   b) The already unpacked game folder
      The folder that contains default.xbe together with the folders
      "Default", "Media" and "adv". Put that folder into "gamefiles"
      (or its contents directly into "gamefiles").

3. Start scda_recomp.exe.

If no game is found, a message tells you so.


FOLDERS
-------
  scda_recomp.exe     the game
  scda_options.txt    settings (explained inside the file)
  gamefiles\          your game (ISO or game folder)
  user\               created automatically, everything the game writes:
    UDATA\              your save games and profile  <-- back this up
    TDATA\, CACHE\      game data written at runtime
    hdd\                the emulated Xbox hard disk incl. the game's cache
                        (the partition .img files are sparse files: they
                        look big, but use little real disk space)
    scda_runtime.log    log of the last start (useful for bug reports)
    shader_cache_vk\    compiled shaders (fewer stutters on later starts)


SETTINGS (scda_options.txt)
---------------------------
Open it with any text editor. Changes take effect on the next start. If the
file is deleted, it is recreated with the defaults. 0 = off, 1 = on unless
stated otherwise; the default is given in brackets.

[Audio]
  Surround          0 = stereo through Windows,
                    1 = Dolby Digital 5.1 passed to an AV receiver (S/PDIF
                        or HDMI; set the volume on the receiver),
                    2 = 5.1 as multichannel PCM through Windows       (0)
  AudioDevice       empty = Windows default, or part of a device name,
                    e.g. AudioDevice = S/PDIF                     (empty)
  MuteInBackground  silent while the game window is not in front     (0)

[Graphics]
  RenderScale       internal resolution 1-4 x the Xbox's 480 lines
                    (2 = 960, 4 = 1920)                               (2)
  Widescreen        1 = 16:9, 0 = 4:3 as on the Xbox                  (1)
  FPS60             1 = 60 frames per second, 0 = 30 as on the Xbox;
                    videos always play at their own speed             (1)
  FPS60Coop         the same for split-screen co-op (needs FPS60 = 1) (1)
  BloomSmoothing    0-3, smooths the glow effect at RenderScale > 1;
                    0 = off, 3 = steadier, wider glow                 (2)
  AntiAliasing      smoothing of jagged edges                         (1)
  VSync             synchronise to the monitor (no tearing)           (1)
  Renderer          vulkan or opengl                             (vulkan)

[Window]
  Fullscreen        1 = borderless fullscreen, 0 = window             (0)
  Monitor           screen to start on, 1 = primary, 2 = second ...   (1)
  RememberWindow    keep window size and position when closing        (1)

[Game]
  Language          EN, DE, FR, ES, IT or JA                         (EN)
  SkipIntro         skip the logo and intro videos at start-up        (0)
  Keyboard          keyboard as player 1 in addition to a controller  (1)
  Keyboard2         numpad layout for player 2 in co-op (see below)   (1)


CONTROLS
--------
Controller: like on the Xbox. A second controller is player 2 in co-op.

Keyboard, player 1 (Keyboard = 1):
  Enter = START, Backspace = BACK, A/B/X/Y = A/B/X/Y
  E/S/D/F = left stick, I/J/K/L = right stick
  W/O = left/right trigger, 1/2 = White/Black, 3/4 = stick clicks
  Home/Delete/End/PageDown = D-pad

Keyboard, player 2 for co-op (Keyboard2 = 1, NumLock on):
  arrow keys = left stick, numpad 8/4/2/6 = right stick
  numpad 7/1/9/3 = A/B/X/Y, numpad + = White (LB), numpad , = Black (RB)
  numpad 0 = START, numpad / = BACK, numpad - / * = left/right trigger
  numpad 5 = left stick click


WHAT WORKS
----------
Boot, menus, new game, the levels, save/load, music, videos, real 5.1
surround, 60 FPS, widescreen, up to 4x internal resolution (Vulkan),
anti-aliasing and local split-screen co-op with two players.

Known issues: occasional graphics glitches (e.g. jagged shadow edges on
some objects), rare crash while loading. Xbox Live and System
Link are not supported.


SAVE GAMES FROM XEMU OR A REAL XBOX (AND BACK)
----------------------------------------------
Save games made with this program work here right away. Saves from xemu or
a real Xbox are signed with your console's keys, which this program does not
include (they are Microsoft/console data). To use them:

1. Read your keys once with the script tools\saves\xemu_read_kernel_keys.py
   from the source code on GitHub, while the game runs in xemu (start xemu
   with -s). For a real Xbox use an xemu with that console's eeprom.bin.
   Result: xbox_keys.bin (32 bytes). Never share this file.
2. Get the profile folder (e.g. 07C8B760B46D) from UDATA\5553005e\:
   from xemu's HDD image with tools\saves\xemu_hdd_fatx_ls.py, from a
   console via FTP.
3. Copy the whole profile folder into user\UDATA\5553005e\ and put
   xbox_keys.bin next to scda_recomp.exe. From then on this program signs
   like your xemu/console, so saves also work in the other direction.

Always move a complete profile folder (profile.sav, SaveMeta.xbx and the
slot files), never single slot files. Step-by-step guide, re-signing without
a key file and copying saves back into xemu: tools/saves/README.md in the
source code on GitHub.


CREDITS
-------
Built with and on the work of:
- xboxrecomp (static recompilation toolkit) and its contributors
- xemu (NV2A graphics renderer, MCPX audio code, DSP JIT pull request #3047,
  pull request #949)
- dsp56300 by Matt Borgerson (real-time 5.1 audio DSP)
- liba52 / a52dec (Dolby Digital decoding), libsamplerate
- glslang, SPIRV-Tools, SPIRV-Reflect, Vulkan-Headers,
  Vulkan Memory Allocator, volk
- Cxbx-Reloaded and xboxdevwiki (documentation)

Licences: see the "licenses" folder. This program contains components under
the GNU GPL v2 (or later) and the GNU LGPL; the complete source code is
published together with this release.
