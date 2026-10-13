@echo off
rem 60 FPS + doppelte Render-Aufloesung + Breitbild (docs Update 481): wie
rem start_scda_xemu_60fps.bat, zusaetzlich RECOMP_SURFACE_SCALE=2 (xemu rendert
rem jede Flaeche mit 2x Kantenlaenge, Fenster 1280x960). Die Messung
rem [GPU-STAT], Fensterrate [FBWIN-RATE] alle 5 s im Log.

set RECOMP_NV2A_XEMU=1
set RECOMP_NV2A_TCG=1
set RECOMP_60FPS=1
set RECOMP_SURFACE_SCALE=2
rem Update 481: 16:9 (Dashboard-Breitbild-Flag), Fenster 1706x960
set RECOMP_WIDESCREEN=1
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_XC_LANGUAGE=3
set RECOMP_SAVE_TRACE=1

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
