@echo off
rem Wie start_scda_xemu.bat, aber mit doppelter interner Render-Aufloesung
rem (xemu-Render-Skalierung, docs Update 473): das Spiel rendert weiter
rem 640x480, die GPU zeichnet jede Flaeche mit 2x Kantenlaenge, das Fenster
rem ist 1280x960. RECOMP_SURFACE_SCALE=1..4.

set RECOMP_NV2A_XEMU=1
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_XC_LANGUAGE=3
set RECOMP_SAVE_TRACE=1
set RECOMP_SURFACE_SCALE=2

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
