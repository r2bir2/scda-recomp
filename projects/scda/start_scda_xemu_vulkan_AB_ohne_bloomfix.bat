@echo off
rem A/B-Test Bildfehler an der Lampe (docs Update 491): wie
rem start_scda_xemu_vulkan_60fps_2x_wide.bat, aber Bloom-/Gruener-Rand-Fix #949 AUS (docs Update 486).

set RECOMP_NV2A_XEMU=1
set RECOMP_RENDERER=vulkan
set RECOMP_NV2A_TCG=1
set RECOMP_60FPS=1
set RECOMP_SURFACE_SCALE=2
set RECOMP_WIDESCREEN=1
set RECOMP_GPU_PROFILE=1
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_XC_LANGUAGE=3
set RECOMP_SAVE_TRACE=1
set RECOMP_VK_SCALE_FIX=0

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
