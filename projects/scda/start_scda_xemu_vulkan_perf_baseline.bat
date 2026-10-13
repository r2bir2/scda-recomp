@echo off
rem SCDA performance A/B: baseline. Same settings as perf_fastpaths,
rem except the two experimental fast paths are disabled.
set RECOMP_NV2A_XEMU=1
set RECOMP_RENDERER=vulkan
set RECOMP_NV2A_TCG=1
set RECOMP_60FPS=1
set RECOMP_SURFACE_SCALE=2
set RECOMP_WIDESCREEN=1
set RECOMP_GPU_PROFILE=1
set RECOMP_VK_STAGE_TIMING=1
set RECOMP_VK_TEXTURE_CLEAN_FAST=0
set RECOMP_VK_TEXTURE_CLEAN_FAST_VERIFY=0
set RECOMP_VK_UNIFORM_BULK_COPY=0
set RECOMP_VK_DRAW_CPU_SAMPLE=0
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_XC_LANGUAGE=3
set RECOMP_SAVE_TRACE=1
set RECOMP_SMOKE_MUTE=1
cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
