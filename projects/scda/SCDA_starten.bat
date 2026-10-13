@echo off
rem Startet SCDA mit den Einstellungen aus scda_options.txt neben der EXE
rem (build\scda_upgrade\bin\RelWithDebInfo). Aendern: SCDA_Optionen_bearbeiten.bat
cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
