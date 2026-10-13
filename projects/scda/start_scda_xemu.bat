@echo off
rem Startet den SCDA-Recomp mit der xemu-Grafik (OpenGL-NV2A aus xemu, docs Update 355).
rem Steuerung wie start_scda.bat (Framebuffer-Fenster im Vordergrund):
rem   Enter = START, Backspace = BACK, Pfeiltasten = Steuerkreuz,
rem   A/B/X/Y = A/B/X/Y, 1 = Weiss, 2 = Schwarz, 3/4 = Stick-Klick links/rechts,
rem   E/S/D/F = linker Stick, I/J/K/L = rechter Stick, W/O = Trigger links/rechts.
rem Ein angeschlossener Xbox-360/One/Series-Controller (XInput) geht ebenfalls.
rem Das Log landet in scda_runtime.log neben der EXE.

set RECOMP_NV2A_XEMU=1
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_XC_LANGUAGE=3
rem Diagnose Spielstand/Profil anlegen (docs Update 348), loggt nur beim Speichern:
set RECOMP_SAVE_TRACE=1

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
