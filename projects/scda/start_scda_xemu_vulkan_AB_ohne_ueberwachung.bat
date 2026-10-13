@echo off
rem A/B-Test Spielstand (docs Update 495): wie start_scda_xemu_vulkan.bat,
rem aber OHNE Bildflaechen-Ueberwachung (RECOMP_NV2A_ACCESS_CB=0).

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
set RECOMP_NV2A_ACCESS_CB=0

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
