@echo off
rem Zeichen-Aufnahme Lampen-Flackern (docs Update 500): wie start_scda_xemu_vulkan.bat.
rem An der Flackerstelle F8 druecken (Spielfenster aktiv): 60 Bilder lang wird jeder
rem Zeichenbefehl ins Log geschrieben und die Bilder nach scratchpad\zeichen_aufnahme
rem gespeichert. Vor F8 laeuft das Spiel normal schnell. Danach Spiel schliessen.

set RECOMP_NV2A_XEMU=1
set RECOMP_RENDERER=vulkan
set RECOMP_NV2A_TCG=1
set RECOMP_60FPS=1
set RECOMP_SURFACE_SCALE=2
set RECOMP_WIDESCREEN=1
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_XC_LANGUAGE=3
set RECOMP_SAVE_TRACE=1
set RECOMP_FB_BURST_DIR=%~dp0..\..\scratchpad\zeichen_aufnahme

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
