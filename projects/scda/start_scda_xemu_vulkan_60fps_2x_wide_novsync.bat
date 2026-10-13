@echo off
rem Vulkan-Renderer im schnellen Modus (docs Update 485), wie
rem start_scda_xemu_60fps_2x_wide.bat, nur mit Vulkan statt OpenGL:
rem   RECOMP_NV2A_TCG=1        schnell (bekannt: Videos stocken, gelbe Stellen)
rem   RECOMP_60FPS=1           60-FPS-Schalter
rem   RECOMP_SURFACE_SCALE=2   doppelte Aufloesung
rem   RECOMP_WIDESCREEN=1      16:9
rem   RECOMP_GPU_PROFILE=1     Messwerte alle 5 s im Log ([GPU-PROF])
rem   RECOMP_VSYNC=0           Fenster ohne VSync: jedes Spielbild sofort zeigen
rem                            (Test gegen Ruckeln auf 144-Hz-Monitor, docs Update 488)

set RECOMP_NV2A_XEMU=1
set RECOMP_RENDERER=vulkan
set RECOMP_NV2A_TCG=1
set RECOMP_60FPS=1
set RECOMP_SURFACE_SCALE=2
set RECOMP_WIDESCREEN=1
set RECOMP_GPU_PROFILE=1
set RECOMP_VSYNC=0
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_XC_LANGUAGE=3
set RECOMP_SAVE_TRACE=1

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
