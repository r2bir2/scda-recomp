@echo off
rem Leistungsmessung (docs Update 474): wie start_scda_xemu.bat, zusaetzlich
rem tastet RECOMP_SAMPLE_MAIN in jedem Prozess 60 s nach dessen Start 20 s
rem lang ab, wo der Haupt-Spielthread seine Zeit verbringt ([SAMPLE] im Log).
rem Die Bildrate steht alle 5 s als [GPU-STAT] ... frames=... (+N) im Log.
rem Ablauf: Spielstand laden, dann einfach im Level normal spielen und sich
rem bewegen, mindestens 90 Sekunden nach dem Laden.

set RECOMP_NV2A_XEMU=1
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_XC_LANGUAGE=3
set RECOMP_SAVE_TRACE=1
set RECOMP_SAMPLE_MAIN=60,20
rem Update 474: alle Threads (Spiel, GPU, Audio, Fenster) im selben Fenster
set RECOMP_SAMPLE_ALL=60,20

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
