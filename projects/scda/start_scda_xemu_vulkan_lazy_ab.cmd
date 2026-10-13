@echo off
setlocal
rem Controlled New York 1 same-save A/B: no screenshots or QPC/Eviction trace.
rem Usage: start_scda_xemu_vulkan_lazy_ab.cmd baseline|lazy

if /I "%~1"=="baseline" (
    set "RECOMP_VK_SCDA_LAZY_BLIT=0"
    set "run_name=baseline"
) else if /I "%~1"=="lazy" (
    set "RECOMP_VK_SCDA_LAZY_BLIT=1"
    set "run_name=lazy"
) else (
    echo Aufruf: %~nx0 baseline ODER lazy
    pause
    exit /b 2
)

set "archive=%~dp0..\..\scratchpad\perf_ttl_20261002\z16_quiet_lazy_ab_%run_name%.log"
if exist "%archive%" (
    echo Vorhandenes A/B-Log wird nicht ueberschrieben: %archive%
    pause
    exit /b 3
)
tasklist /fi "imagename eq scda_recomp.exe" | find /i "scda_recomp.exe" >nul
if not errorlevel 1 (
    echo Es laeuft bereits SCDA. Bitte zuerst das bestehende Spiel schliessen.
    pause
    exit /b 7
)

set "RECOMP_NV2A_XEMU=1"
set "RECOMP_RENDERER=vulkan"
set "RECOMP_NV2A_TCG=1"
set "RECOMP_60FPS=1"
set "RECOMP_SURFACE_SCALE=2"
set "RECOMP_WIDESCREEN=1"
set "RECOMP_GPU_PROFILE=1"
set "RECOMP_VK_SURFACE_TTL=60"
set "RECOMP_VK_BATCH_OVERLAP=1"
set "RECOMP_VK_Z16_PARTIAL_TEX=1"
set "RECOMP_VK_Z16_FULL_CLEAR_REUSE=1"
set "RECOMP_VK_Z16_DEFER_INVALIDATION=0"
set "RECOMP_VK_SURF_WAIT_PROBE=0"
set "RECOMP_VK_BLIT_EQ_PROBE=0"
set "RECOMP_VK_TRACE_EVICT=0"
set "RECOMP_VK_STAGE_TIMING=0"
set "RECOMP_AC97_READY=1"
set "RECOMP_FB_WINDOW=1"
set "RECOMP_XC_LANGUAGE=3"
set "RECOMP_SAVE_TRACE=1"

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
if errorlevel 1 exit /b 4
echo SCDA A/B: %run_name%. Nach dem Test das Spiel normal schliessen.
start /wait "" scda_recomp.exe
rem Loading a save relaunches SCDA into a child process.  Do not archive the
rem log when only the first process exits; wait for the last game process.
set /a waited_seconds=0
:wait_game
tasklist /fi "imagename eq scda_recomp.exe" | find /i "scda_recomp.exe" >nul
if errorlevel 1 goto game_closed
if %waited_seconds% geq 900 (
    echo SCDA laeuft nach 15 Minuten noch. Kein laufendes Log wird kopiert.
    pause
    exit /b 8
)
timeout /t 1 /nobreak >nul
set /a waited_seconds+=1
goto wait_game
:game_closed
if not exist "scda_runtime.log" (
    echo Kein Runtime-Log gefunden.
    pause
    exit /b 5
)
copy /b "scda_runtime.log" "%archive%" >nul
if errorlevel 1 (
    echo Das Runtime-Log konnte nicht gesichert werden.
    pause
    exit /b 6
)
echo Log gesichert: %archive%
pause
