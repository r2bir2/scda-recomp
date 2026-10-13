@echo off
rem Diagnose (docs Update 435): protokolliert, welche Dateien das Spiel in
rem den Z:-Cache aufnimmt ([SCDA-CACHE]) und welche Platz-Antwort der Kernel je Laufwerk gibt
rem ([VOLSIZE]). Zweck: klaeren, warum die Missionsmusik nie in den
rem Cache kopiert wird (Action-Musik fehlt).
rem Update 449: zusaetzlich [MIXER]-Zeilen vom Software-Musikmischer
rem (RECOMP_MIXER_TRACE) und [APU-V]-Stimmenlisten (RECOMP_APU_VOICES).

set RECOMP_NV2A_XEMU=1
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_XC_LANGUAGE=3
set RECOMP_SAVE_TRACE=1
set RECOMP_CACHE_TRACE=1
set RECOMP_MIXER_TRACE=1
set RECOMP_APU_VOICES=1
rem Update 449: Debug-Meldungen der Engine (sub_004528E7) als [SCDA-DBG]
set RECOMP_DEBUG_PRINT=1

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
