@echo off
rem Opt-in A/B: vier aliasierte A8R8G8B8-Mip-Readbacks als einen
rem Vulkan-Transfer-Submit zusammenfassen. Guest-RAM-Daten bleiben erhalten.
rem Gleiche Einstellungen wie start_scda_xemu_vulkan_ttl60_profile.bat.

set RECOMP_NV2A_XEMU=1
set RECOMP_RENDERER=vulkan
set RECOMP_NV2A_TCG=1
set RECOMP_60FPS=1
set RECOMP_SURFACE_SCALE=2
set RECOMP_WIDESCREEN=1
set RECOMP_GPU_PROFILE=1
set RECOMP_VK_SURFACE_TTL=60
set RECOMP_VK_BATCH_OVERLAP=1
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_XC_LANGUAGE=3
set RECOMP_SAVE_TRACE=1

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
