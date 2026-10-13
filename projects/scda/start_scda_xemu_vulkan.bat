@echo off
rem Empfohlener Start (Stand docs Update 494): Vulkan-Renderer im schnellen
rem Modus. Seit Update 492/493 ueberwacht der Recomp CPU-Zugriffe auf GPU-
rem Bildflaechen, damit ist der schnelle Modus (RECOMP_NV2A_TCG=1) korrekt:
rem keine gelben Stellen, Videos laufen.
rem   RECOMP_60FPS=1           60-FPS-Schalter
rem   RECOMP_SURFACE_SCALE=2   doppelte Aufloesung (inkl. Bloom-Fix #949)
rem   RECOMP_WIDESCREEN=1      16:9
rem Faellt Vulkan beim Start aus, nimmt das Spiel automatisch OpenGL.

set RECOMP_NV2A_XEMU=1
set RECOMP_RENDERER=vulkan
set RECOMP_NV2A_TCG=1
set RECOMP_60FPS=1
set RECOMP_SURFACE_SCALE=2
set RECOMP_WIDESCREEN=1
rem Gepruefte CPU-Fastpaths: sichtbares A/B in Geldzug und Cozumel gleich.
rem Bei neuen Bildfehlern diese beiden Werte testweise auf 0 setzen.
set RECOMP_VK_TEXTURE_CLEAN_FAST=1
set RECOMP_VK_UNIFORM_BULK_COPY=1
set RECOMP_AC97_READY=1
set RECOMP_FB_WINDOW=1
set RECOMP_XC_LANGUAGE=3
set RECOMP_SAVE_TRACE=1

cd /d "%~dp0..\..\build\scda_upgrade\bin\RelWithDebInfo"
start "" scda_recomp.exe
