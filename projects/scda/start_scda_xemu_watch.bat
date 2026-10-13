@echo off
rem Diagnose (docs Update 457): Absturz nach Laden des Spielstands vor der
rem Folie. Ein Objekt bei Gast-Adresse 0x018791D8 wird im Level mit
rem Gleitkommadaten ueberschrieben. RECOMP_GUEST_WATCH ueberwacht (nur im
rem Level-Prozess nach dem Neustart) jeden Schreibzugriff dorthin und
rem protokolliert Schreiber + neuen Wert als [GUEST-WATCH] in scda_runtime.log.
rem Das Spiel kann dadurch etwas langsamer laufen.

set RECOMP_NV2A_XEMU=1
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_XC_LANGUAGE=3
set RECOMP_SAVE_TRACE=1
rem Update 471: Objekt 0x0188EB68 (Index 8052) steht nach dem Laden doppelt
rem in der Objekt-Hashkette (Kreis mit 0x02253788) -> Haenger in
rem sub_0005F23D. Ueberwacht Index (+4) und HashNext (+8).
set RECOMP_GUEST_WATCH=0188EB6C:8

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
