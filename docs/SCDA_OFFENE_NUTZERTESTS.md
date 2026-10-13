# Offene Tests/Fragen, die Nutzer-Input brauchen

Stand 2026-10-01.

1. **Vulkan im Level** (`start_scda_xemu_vulkan_60fps_2x_wide.bat`, schneller Modus):
   herumlaufen, Nachtsicht/Waermebild, Bloom/Rauch, Videos. Bildfehler im
   Vergleich zu OpenGL (`start_scda_xemu_60fps_2x_wide.bat`)? FPS laut Titelleiste?
2. **Vulkan normaler Modus** (`start_scda_xemu_vulkan.bat`): korrekt, aber im Level
   ca. 10 FPS (wie OpenGL ohne TCG) -- nur kurz pruefen, ob es abstuerzt.
3. **Titel-/Demo-Video** (Hintergrund von "Druecken Sie START" und die Demo nach
   Leerlauf): Blockfehler in beiden Renderern. In xemu sauber? (Ground Truth)
4. Einmaliger Vulkan-Absturz (DEVICE_LOST) beim Uebergang "Neues Spiel" -> Laden:
   falls er bei dir auftritt, bitte melden (Log scda_runtime.log behalten).
5. **Bink-Video nach Levelstart ueberspringen** (START/A waehrend des Videos):
   stuerzt das Spiel dabei ab? (Automatischer Lauf vom 01.10. 08:56 ist dort im
   Bink-Player abgestuerzt -- Verdacht: Ueberspringen.)
6. **Nebel/Dampf mit Vulkan 2x** (Bloom-Fix #949): noch pixelig?
7. **Ruckeln/Frametime auf 144-Hz-Monitor:** (a) `start_scda_xemu_vulkan_60fps_2x_wide_novsync.bat`
   (Fenster ohne VSync) -- gleichmaessiger? Tearing? (b) Monitor testweise auf 120 Hz
   (oder 60 Hz) stellen -- verschwinden die Ausschlaege im zweiten Graph?
8. **HUD-Zittern + blaues Dreieck an der Aussenlampe** (neuester Spielstand vom 01.10.):
   tritt in Vulkan UND OpenGL auf -- in xemu an derselben Stelle pruefen.
9. **Saubere Ueberwachung (Update 492) im Level** mit
   `start_scda_xemu_vulkan_messung.bat` (Schnellmodus): gelbe Stellen weg? Videos
   (Trainingshinweis, Zwischensequenzen) fluessig? FPS gleich oder besser? Absturz?
   Gegenprobe ohne: `set RECOMP_NV2A_ACCESS_CB=0`.
10. **Speichern im Schnellmodus** (seit Update 493 wird der Schreibpuffer vorbereitet):
    einmal im Level speichern und den Spielstand wieder laden.
11. **Objekte nach "Speichern -> im selben Spiel laden" verschoben** (Update 495, auch
    ohne Ueberwachung): in xemu dasselbe? Und mit OpenGL
    (`start_scda_xemu_60fps_2x_wide.bat`)?

## Nachtrag 2026-10-01 abends (Update 501-504)

12. **Lange Sitzung / Island bis zum Ende in einem Rutsch:** mit dem aktuellen Stand (Update 504,
    Speicherfreigabe wieder aus) kann nach sehr langer Spielzeit erneut "Disc verkratzt" kommen.
    Erst nach dem echten Speicher-Modell (naechster Schritt) erneut testen.
13. **Fensteruebergabe (Update 502):** Neues Spiel / Laden / Levelwechsel -- bleibt das letzte
    Bild stehen, bis der Ladebildschirm kommt? (Ladebalken fehlt im gehaltenen Bild -- fuer spaeter.)
14. **Videos bei 60 FPS:** welche laufen zu schnell? (Nur reine Videos ohne Menue/Text.)
