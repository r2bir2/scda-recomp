@echo off
rem Flacker-Aufnahme (docs Update 496): wie start_scda_xemu_vulkan.bat.
rem Spielstand laden; sobald es flackert, F11 druecken (Spielfenster aktiv):
rem 60 aufeinanderfolgende Bilder landen in scratchpad\flacker_bilder.
rem Zum Vergleich gern auch an der Lampenstelle einmal F11 druecken.

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
set RECOMP_FB_BURST_DIR=%~dp0..\..\scratchpad\flacker_bilder
set RECOMP_GPU_PROFILE=1

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
