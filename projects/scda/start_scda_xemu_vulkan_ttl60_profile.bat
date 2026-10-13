@echo off
rem A/B-Test fuer laenger GPU-residente, CPU-ueberwachte Vulkan-Renderflaechen.
rem Gleiche Spiel-/Grafikeinstellungen wie start_scda_xemu_vulkan.bat.
rem Referenz: start_scda_xemu_vulkan_60fps_2x_wide.bat (Profil, TTL=5).
rem Test: dieses Skript (Profil, TTL=60 nur bei aktivem CPU-Zugriffswaechter).

set RECOMP_NV2A_XEMU=1
set RECOMP_RENDERER=vulkan
set RECOMP_NV2A_TCG=1
set RECOMP_60FPS=1
set RECOMP_SURFACE_SCALE=2
set RECOMP_WIDESCREEN=1
set RECOMP_GPU_PROFILE=1
set RECOMP_VK_SURFACE_TTL=60
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_XC_LANGUAGE=3
set RECOMP_SAVE_TRACE=1

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
