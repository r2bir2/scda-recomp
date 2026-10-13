@echo off
rem 60-FPS-Test (docs Update 477): wie start_scda_xemu.bat, plus
rem   RECOMP_NV2A_TCG=1  keine sofortige Rueckkopie der GPU-Flaechen (schnell;
rem                      bekannte Nebenwirkung: Videos stocken, gelbe Stellen)
rem   RECOMP_60FPS=1     Praesentationsintervall wie der 60-FPS-IPS-Patch
rem Die Bildrate steht alle 5 s als [GPU-STAT] ... frames=... (+N) im Log.

set RECOMP_NV2A_XEMU=1
set RECOMP_NV2A_TCG=1
set RECOMP_60FPS=1
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_XC_LANGUAGE=3
set RECOMP_SAVE_TRACE=1

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
