@echo off
rem Two 18-second phases marked by F7 in the framebuffer window.
rem Every 30th GPU frame also samples draw setup by render target and shader.
rem No screenshots or experimental renderer fast paths.
set RECOMP_SCDA_LIGHT_PROBE=1
set RECOMP_FB_DUMP=
set RECOMP_SMOKE_MUTE=1
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
set RECOMP_VK_DRAW_CPU_SAMPLE=1
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_XC_LANGUAGE=3
set RECOMP_SAVE_TRACE=1
cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
