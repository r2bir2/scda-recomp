@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cl.exe /nologo /W4 /I ..\..\templates\runtime test_main.c /Fe:test_mmx_ops.exe
if errorlevel 1 exit /b 1
"%~dp0test_mmx_ops.exe"
