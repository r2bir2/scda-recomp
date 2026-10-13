@echo off
rem Leistungsmessung Vulkan (docs Update 489): wie
rem start_scda_xemu_vulkan_60fps_2x_wide_novsync.bat, plus
rem   RECOMP_VK_TRACE_XFER=1  welche Bildflaechen/Texturen jedes Frame ueber den
rem                           Xbox-Speicher kopiert werden ([VK-XFER] im Log)
rem An der langsamen Stelle stillstehen und F9 druecken: 10 s Messung aller
rem Threads ([SAMPLE-ALL] im Log). Danach das Spiel schliessen.

set RECOMP_NV2A_XEMU=1
set RECOMP_RENDERER=vulkan
set RECOMP_NV2A_TCG=1
set RECOMP_60FPS=1
set RECOMP_SURFACE_SCALE=2
set RECOMP_WIDESCREEN=1
set RECOMP_GPU_PROFILE=1
set RECOMP_VK_TRACE_XFER=1
set RECOMP_VSYNC=0
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_XC_LANGUAGE=3
set RECOMP_SAVE_TRACE=1

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
