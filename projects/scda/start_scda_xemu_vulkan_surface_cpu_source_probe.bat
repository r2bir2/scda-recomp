@echo off
rem Diagnostic only: correlate the expensive 640x480 color download with
rem the native CPU access that requests it. No synchronization is skipped.

set RECOMP_NV2A_XEMU=1
set RECOMP_RENDERER=vulkan
set RECOMP_NV2A_TCG=1
set RECOMP_60FPS=1
set RECOMP_SURFACE_SCALE=2
set RECOMP_WIDESCREEN=1
set RECOMP_GPU_PROFILE=1
set RECOMP_VK_SURFACE_TTL=60
set RECOMP_VK_BATCH_OVERLAP=1
set RECOMP_VK_Z16_PARTIAL_TEX=1
set RECOMP_VK_Z16_FULL_CLEAR_REUSE=1
set RECOMP_VK_Z16_DEFER_INVALIDATION=0
set RECOMP_VK_TRACE_EVICT=1
set RECOMP_VK_STAGE_TIMING=1
set RECOMP_VK_SURF_WAIT_PROBE=1
set RECOMP_NV2A_ACCESS_TRACE=1
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_XC_LANGUAGE=3
set RECOMP_SAVE_TRACE=1

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
