@echo off
rem Diagnose Objektfehler nach Laden (docs Update 495): wie
rem start_scda_xemu_vulkan.bat, plus Protokoll jedes Ueberwachungs-Ereignisses
rem ([GPU-ACCESS-EV] im Log). Spielstand laden, Stelle ansehen, Spiel schliessen.

set RECOMP_NV2A_XEMU=1
set RECOMP_RENDERER=vulkan
set RECOMP_NV2A_TCG=1
set RECOMP_60FPS=1
set RECOMP_SURFACE_SCALE=2
set RECOMP_WIDESCREEN=1
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_XC_LANGUAGE=3
set RECOMP_SAVE_TRACE=1
set RECOMP_NV2A_ACCESS_TRACE=1
set RECOMP_GPU_PROFILE=1

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
