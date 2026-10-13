@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cl.exe /nologo /W4 /I ..\..\src\kernel test_main.c ..\..\src\kernel\vsh_interp.c /Fe:test_vsh_interp.exe
if errorlevel 1 exit /b 1
"%~dp0test_vsh_interp.exe"
