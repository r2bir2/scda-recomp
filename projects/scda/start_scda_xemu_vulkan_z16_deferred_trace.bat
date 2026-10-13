@echo off
rem Opt-in: proven full Z16 clear plus deferred retirement of the old image.
rem Main/Aux wait timing is enabled; use for correctness and synchronization analysis,
rem not for a direct FPS comparison.

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
set RECOMP_VK_Z16_DEFER_INVALIDATION=1
set RECOMP_VK_TRACE_EVICT=1
set RECOMP_VK_STAGE_TIMING=1
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_XC_LANGUAGE=3
set RECOMP_SAVE_TRACE=1

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
