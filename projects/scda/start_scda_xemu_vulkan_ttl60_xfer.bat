@echo off
rem Gleicher 2x-Vulkan/TCG/60-FPS-Lauf wie der TTL60-Profiltest,
rem zusaetzlich pro Surface/Texture [VK-XFER]-Adressen und Aufrufer.
rem In einer 3D-Szene 20 Sekunden ruhig stehen, dann Spiel schliessen.

set RECOMP_NV2A_XEMU=1
set RECOMP_RENDERER=vulkan
set RECOMP_NV2A_TCG=1
set RECOMP_60FPS=1
set RECOMP_SURFACE_SCALE=2
set RECOMP_WIDESCREEN=1
set RECOMP_GPU_PROFILE=1
set RECOMP_VK_SURFACE_TTL=60
set RECOMP_VK_TRACE_XFER=1
set RECOMP_VK_TRACE_EVICT=1
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_XC_LANGUAGE=3
set RECOMP_SAVE_TRACE=1

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
