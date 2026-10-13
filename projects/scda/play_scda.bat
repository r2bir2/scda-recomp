@echo off
rem Starts SCDA with the settings in scda_options.txt next to the executable
rem (build\scda_upgrade\bin\RelWithDebInfo). Edit them with edit_scda_options.bat.
cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
