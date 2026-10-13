@echo off
rem Diagnose (docs Update 428): protokolliert die Audio-Stimmen des APU:
rem beim Start/Tonhoehen-Setzen ([APU-VOICE]) und alle ~2 s die aktiven
rem Stimmen mit Tonhoehe, Format und Pufferpositionen ([APU-V]) sowie den
rem APU-Takt ([APU-STATS]). Zweck: zu schnelle Dialoge/SFX gegen xemu
rem vergleichen (tools/debug/xemu_apu_voices.py).

set RECOMP_NV2A_XEMU=1
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_XC_LANGUAGE=3
set RECOMP_SAVE_TRACE=1
set RECOMP_APU_VOICES=1
set RECOMP_APU_PITCH_LOG=1
set RECOMP_APU_STATS=1

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
