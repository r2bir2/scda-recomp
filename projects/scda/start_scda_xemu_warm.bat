@echo off
rem Diagnose (docs Update 420): nach einmal F11 wird fuer 60 Spielbilder der
rem Waermeanteil (max(0,R-B)) jedes Zeichenaufrufs im Hauptbild
rem protokolliert. Zweck: den Zeichenaufruf finden, der die warmen
rem Lichtflecken am Eis malt (langsam waehrend der Messung).

set RECOMP_NV2A_XEMU=1
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_XC_LANGUAGE=3
set RECOMP_SAVE_TRACE=1
set RECOMP_WARM_TRACE=1

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
