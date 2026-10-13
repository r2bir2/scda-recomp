@echo off
setlocal
rem Sichtbarer A/B-Vergleich: normaler Starter plus stumme Ausgabe.
rem GPU-Queue-Opt-ins explizit aus; CPU-Fastpaths kommen aus dem Normalstarter.
set RECOMP_SMOKE_MUTE=1
set RECOMP_VK_BATCH_OVERLAP=0
set RECOMP_VK_SCDA_LAZY_BLIT=0
call "%~dp0start_scda_xemu_vulkan.bat"
