@echo off
setlocal
rem Sichtbarer A/B-Vergleich: exakt derselbe normale Starter wie Baseline.
rem Nur die zwei gesondert vermessenen GPU-Queue-Opt-ins sind eingeschaltet.
set RECOMP_SMOKE_MUTE=1
set RECOMP_VK_BATCH_OVERLAP=1
set RECOMP_VK_SCDA_LAZY_BLIT=1
call "%~dp0start_scda_xemu_vulkan.bat"
