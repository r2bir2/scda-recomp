# SCDA-PC-Port – Retrospektive als Faktengrundlage fürs Video

Stand: 14.10.2026. Dies ist kein Videoskript, sondern das Material dafür.

## Kennzeichnung

- **[belegt]**: steht so in der Projektdokumentation, im Git-Verlauf, in Logs oder in deinen eigenen Chatnachrichten. Fundstellen nenne ich als „Update N“ (Arbeitsprotokoll `docs/SCDA_WORKLOG.md` bzw. `PROJECT_STATUS.md`) oder als Datum deiner Nachricht.
- **[Schätzung]**: aus den Unterlagen hochgerechnet oder gezählt, mit Unsicherheit.
- **[unbekannt]**: in diesem Repository nicht dokumentiert. Muss von dir ergänzt oder extern recherchiert werden.

## Ausgewertete Quellen

- `AGENTS.md` (Arbeitsregeln)
- `PROJECT_STATUS.md` (~32.800 Zeilen)
- `docs/SCDA_WORKLOG.md` (~38.800 Zeilen, 662 nummerierte Updates)
- Git-Historie
- Laufzeit- und Build-Logs, Testordner
- `SC4_xbe_hack/` (dein XBE-Hack-Projekt)
- das Gesprächsprotokoll der Claude-Sitzung ab 27.08.2026, davon 656 eigene Nachrichten von dir

**Nicht eingesehen werden konnten:**
- die Codex-Sitzungen selbst (nur deren Ergebnisse in der Doku)
- die frühesten Tage vor dem 24.08.
- Reddit
- der Build des anderen Teams

---

## 1. Ausgangspunkt und ursprüngliches Ziel

### Wann begonnen?
- **[belegt]** Das Arbeitsprotokoll nennt als Projektbeginn den **19.08.2026**: „this whole project's history (from 2026-08-19 …)“.
- **[belegt]** Die ersten datierten Einträge stammen vom **24.08.2026**: Toolchain-Aufbau, erste Pipeline, „Windows host project“, erster kontrollierter Smoke-Test.
- **[unbekannt]** Was zwischen dem 19. und 24.08. genau passiert ist.

### Technischer Ausgangszustand
- **Grundlage war [xboxrecomp](https://github.com/sp00nznet/xboxrecomp)** von sp00nz, ein Werkzeugkasten für „statische Recompilation“ von Xbox-Spielen. Erste öffentliche Version vom 06.03.2026; es folgten mehrere Upstream-Versionen (v0.6, v0.7, v0.8, v0.9), die teils übernommen wurden. **[belegt: Git-Historie, Updates zu v0.6/v0.7/v0.9]**
- **Was die Pipeline am Anfang schon konnte [belegt: Worklog 24.–26.08.]:**
  - die `default.xbe` des Spiels einlesen
  - den x86-Maschinencode disassemblieren
  - daraus C-Code erzeugen (zehntausende Funktionen)
  - daraus eine Windows-EXE bauen
- **Was nicht funktionierte:** Die EXE startete, zeigte aber nichts. Deine Beschreibung vom 27.08.: „dass sich ein CMD-Fenster öffnet und der quasi unendlich lang rödelt“. Es gab weder ein Spielfenster noch Grafik oder Ton; das Spiel hing in der Initialisierung oder stürzte ab. **[belegt]**
- **Wichtig zum Verständnis:** Ein funktionierender Build bedeutet bei einer Recompilation noch lange nicht, dass das Spiel läuft. Jeder Übersetzungsfehler in einer der ~28.000–32.000 Funktionen kann irgendwo viel später als Absturz, Endlosschleife oder falsches Bild auftauchen.

### Statische Recompilation, einfach erklärt (fürs Video)
- Ein Emulator wie xemu baut die ganze Xbox in Software nach und führt das Spiel Befehl für Befehl aus, während es läuft.
- Eine statische Recompilation übersetzt das Spielprogramm vorher einmal komplett in normalen PC-Code, ähnlich wie man ein Buch übersetzt, statt es beim Vorlesen simultan zu dolmetschen.
- Was die Xbox-Hardware sonst erledigt (Betriebssystem-Kernel, Grafikchip NV2A, Soundchip MCPX, Dateisystem, Controller), muss man selbst nachbauen bzw. aus Emulatoren übernehmen.
- Das Ergebnis ist eine echte Windows-EXE, die das Spiel ausführt. Spieldaten wie Level, Texturen und Videos braucht sie weiterhin von der Original-Disc.

### Dein ursprüngliches Ziel und wie es sich verändert hat
- **Ursprünglich [belegt, deine Nachricht 02.09., 21:05]:** „Das primäre Ziel ist … den SCDA-Recompiler schrittweise zu einem tatsächlich lauffähigen Spiel zu bringen“. Die sichtbaren Etappen: stabiler Start → Spielfenster/Grafik → Ubisoft-Logo → Hauptmenü. Dazu (04.09.): „Bedenke bitte, dass ich gerne mind. einen Build wünsche, der mir mal das Ubisoft-Logo zeigt.“
- **Nach dem ersten spielbaren Level (27.09.) verschob sich das Ziel** von „läuft überhaupt“ zu „läuft gut und ist besser als auf der Konsole“ [belegt, Nachrichten ab 30.09.]:
  - höhere Auflösung, Breitbild, 60 FPS
  - Vulkan, echtes 5.1
  - Spielstände von Xbox/xemu weiterverwenden
  - Koop, Optionsdatei
  - Grafikfehler korrigieren, auch solche, die die Original-Hardware selbst hat (Papierzettel in JBA)
- **Am Ende (10.10.)** stand die Entscheidung, die Arbeit einzustellen; am 13.10. folgte trotzdem die Veröffentlichung als Release.
- **Deine Motivation laut eigener Aussage** (siehe Abschnitt 11): Double Agent selbst vernünftig spielen können und etwas für die Community schaffen.

### Verwendete Grundlagen und Projekte [belegt: NOTICE, Lizenzordner, Worklog]
- **xboxrecomp** (sp00nz und Mitwirkende: NoRain211, Ned, DarthSidious666, GTTeancum u. a.)
- **xemu**, der Original-Xbox-Emulator:
  1. **als „Wahrheit“ zum Vergleichen:** Läuft eine Stelle im Spiel unter xemu anders als im Recomp, ist das der Fehler.
  2. **als Code-Spender:** Der komplette Grafikteil (NV2A-Renderer, erst OpenGL, dann Vulkan) und der Soundchip-Code (APU, DSP) stammen aus xemu.
  3. **xemu-Beiträge:** PR #3047 (DSP-JIT für Echtzeit-5.1) und #949 (Render-Scale-Fix)
- **dsp56300** von Matt Borgerson (DSP-JIT), **liba52/a52dec** (Dolby-Digital-Decoder), **libsamplerate**
- **glslang, SPIRV-Tools, SPIRV-Reflect, Vulkan-Headers, Vulkan Memory Allocator, volk**
- **Cxbx-Reloaded** und **xboxdevwiki** als Nachschlagewerke, z. B. für die richtige Struktur von `NtQueryVirtualMemory` (01.09.)
- **Werkzeuge:**
  - Visual Studio 2022, CMake, Python mit Capstone (Disassembler)
  - WinDbg/CDB (Debugger)
  - extract-xiso, quickbms (UMD-Archive entpacken)
  - ReShade mit ShaderToggler (Grafikanalyse im XBE-Hack)
  - MSI Afterburner (deine Frametime-Messungen)
- **Ein Community-60-FPS-IPS-Patch** lieferte am Anfang die Stelle für 60 FPS. Der Urheber möchte nicht genannt werden; später wurde daraus ein eigener Schalter, im Koop sogar eine selbst gefundene Stelle (siehe Abschnitt 2).

### Rolle der KI-Agenten [belegt, Details in Abschnitt 5 und 10]
- **OpenAI Codex:** Erste Phase bis ca. 27.08. Ab dann im Wechsel mit Claude; laut deinen Übergabe-Prompts „Seit deiner letzten Sitzung hat Codex/Claude das Projekt weitergeführt“ (20.09., 22.09., 23.09.).
- **Claude Code (Anthropic):** Ab 27.08. Im Protokoll dieser Sitzung stehen rund 238.000 Antwortschritte von Modell „claude-sonnet-5“ und rund 34.000 von „claude-opus-5-5“ (Stand 14.10.).
- **OpenCode:** Es gibt eine Übergabedatei `docs/OPENCODE_HANDOFF.md` (Lifter-/FPU-Änderungen). Umfang **[unbekannt]**.
- **Ein weiterer Chat-Assistent als „Berater“:** Viele lange, sehr strukturierte englische und deutsche Steuerungs-Prompts hast du offenbar aus einem anderen Chat übernommen, z. B. „Ich würde dem Agenten Folgendes geben: …“ (04.09.), „Schreib ihm das: …“ (22.09.), „ich bekam noch den Tipp …“ (02.09.). Welcher Assistent das war, ist **[unbekannt]**.
- **Mehrere parallele Agenten:** Es gab Fälle, in denen Agenten gleichzeitig arbeiteten und kollidierten, z. B. „Update 428 … Nummer kollidiert mit AUDIO-UPDATE 428 des anderen Agenten“ (29.09.) und „Synced with a parallel agent session“ (Update 203). **[belegt]**
- **Kernaussage:** Fast der gesamte Code, alle Analysen, Debugger-Sitzungen und Messungen stammen von den KI-Agenten. Du hast nicht programmiert, sondern gesteuert, getestet, beobachtet, entschieden und Wissen über Spiel und Xbox eingebracht (Abschnitt 5).

---

## 2. Chronologischer Projektverlauf (Meilensteine)

Die Zuordnung „Idee von wem“ ist so genau wie belegbar. Wo nichts dazu steht, kam die Idee vom Agenten.

### Phase A: Pipeline und frühe Abstürze (19.08. – 31.08.)

| Datum | Meilenstein | Vorher → Nachher | Woher die Idee |
|---|---|---|---|
| 24.08. | **Erster erfolgreicher Build**, Pipeline XBE → Disassembly → C → EXE läuft reproduzierbar | vorher nichts; danach eine EXE, die startet, aber nichts zeigt | Agent (Codex) |
| 24.–27.08. | **„Seeds“ 1–33:** Der Disassembler erkennt Funktionsgrenzen falsch und schneidet Funktionen ab oder lässt sie weg. Fix: Funktionsanfänge per Hand bzw. aus Laufzeitbeobachtung „seeden“ | Abstürze und Hänger im Startcode | Agenten |
| 27.08. | **Live-Debugging-Sitzung in Visual Studio durch dich.** Du hast nach Anleitung Breakpoints gesetzt und Schritt für Schritt geprüft; das löste ein „Rätsel“ (sub_000464EC) und führte zu Seed 33 (Update „interactive Visual Studio session (the user, live)“) | Agent kam ohne Live-Sicht nicht weiter | **du** (Ausführung), Agent (Anleitung) |

### Phase B: Der lange Weg durch die Initialisierung (01.09. – 22.09.)

| Datum | Meilenstein | Bedeutung | Woher die Idee |
|---|---|---|---|
| 01.09. | **Laufwerk Z: = HDD-Cache:** Das Spiel sucht seine Archive (DynamicXbox.umd) auf Z:. Z: wird auf den Spielordner gelegt; großer Sprung im Startvorgang | erstes Mal deutlich weiter | **du** hast erklärt, dass Z: die Cache-Partition der Xbox ist (Nachrichten 01.09., 12:17–12:27); Agent hat die Umleitung vorgeschlagen |
| 01.09. | **NtQueryVirtualMemory-Hänger gelöst** mit Hilfe von Cxbx-Reloaded | Endlosschleife weg | **dein Vorschlag**, Cxbx-Doku zu nutzen (01.09., 20:52/21:09); belegt „At the user's suggestion“ |
| 01.09. | **Regel „xemu als Wahrheit“** | wurde zur wichtigsten Methode des ganzen Projekts | **deine Idee** (01.09., 21:22/21:30) |
| 02.09. | xemu-Ground-Truth-Sitzung: Du stellst fest, dass in xemu ein EvoX-BIOS lief → Umstellung auf Retail-BIOS und frische HDD | saubere Vergleichsbasis | **du** (02.09., 18:25/18:37) |
| 02.09. | Hinweis: Die INI-Dateien stecken in `DynamicXbox.umd`, entpackbar mit quickbms | Agent sucht nicht mehr nach losen INI-Dateien | **du** (02.09., 20:36) |
| 03.–11.09. | Dutzende Boot-Blocker (Allokator, Hashtabellen, Vtables, CRT-Konstruktoren, Speicher-Strukturen, Threads), jeweils mit Ursache behoben | Start läuft immer weiter | Agenten; Methodik teils von dir bzw. deinem Berater-Chat |
| 08.–09.09. | **Gezielte Upstream-Übernahme** in Stufen mit Checkpoints statt Komplett-Merge | Stabilität bleibt erhalten | **deine Vorgabe** (08.09., 19:02; 09.09.) |
| 09.09. | **Kurioser Fund:** Die Disassembly-Stufe war seit dem 24.08. nicht mehr neu gelaufen; Fixes landeten deshalb gar nicht im erzeugten Code (CACHE_VERSION) | erklärt mehrere „unerklärliche“ Abweichungen | Agent; ausgelöst durch die von **dir** am 05.09. bemerkte falsche Pfadangabe `menu\common.lin` |
| 11.09. | **Boot überlebt komplett ohne Absturz** (Worker-Thread-Fix fs:[4]) und **CPU-Kern-Pinning:** Die Xbox hat nur einen CPU-Kern, also laufen die Spiel-Threads auf einem Kern | erste Stabilität | Agent |
| 17.09. | **Erstes Fenster** wird erstellt und bleibt stabil (Update 167) | Spiel kommt zur Grafikinitialisierung | Agent |
| 17.–25.09. | **Die große common.lin-/Ringpuffer-Saga** (Abschnitt 4, Problem 1) | lange Hängepartie | — |
| 21.–22.09. | **Speicher-Alias-Fix** (0x4AB668, Update 229) und **Codegenerator-Fix** für einen kaputten Menübaum-Zeiger (Update 232) | Grafikgerät bleibt gültig, Menüsystem läuft weiter | Agenten; Architektur-Entscheidung „kein Workaround“ von dir / deinem Berater (22.09.) |

### Phase C: Erstes Bild bis spielbar (23.09. – 28.09.)

| Datum | Meilenstein | Woher die Idee |
|---|---|---|
| 23.09. | **Erstes echtes Bild**, Update 259b: Ubisoft-Bootlogo in korrekten Farben. Ursache der falschen Farben: Die MMX-Befehle `psubusw`/`paddusw` fehlten im Übersetzer | Agent; **du** hast bestätigt, dass das genau das erste Bild der echten Hardware ist (23.09., 12:30) |
| 25.09. | **Echte Ursache der Ringpuffer-Saga gefunden** (Update 320/321, generischer Fehler im Übersetzer) und **erster sichtbarer Ladebildschirm** (Update 339) | Agent |
| 26.09. | **Ubisoft-Logo-Video sichtbar** (Update 344b, `rdtsc` wurde nie übersetzt) und **Bink-Audio-Starvation gelöst** (Gerätethreads nicht mehr auf den Spielkern gepinnt, Update 345) | Agent |
| 27.09. | **Hauptmenü erreicht** (Update 347), Profil anlegen/laden (348), Neustart des Titels beim Levelwechsel (349), **erstes Level läuft** (353) | Agent; deine Live-Tests (27.09., 11:18–12:38) |
| 27.09. | **Echte xemu-Grafik eingebaut** (OpenGL-NV2A, Update 355). Danach mit dem sqrt-Fix (`cmpneqss` fehlte, jede Wurzel war 0): **erstes Level spielbar** | Du wolltest „die echte Grafik-Emulation aus xemu übernehmen“ (27.09., 16:59) und hast Vulkan ins Spiel gebracht, aber vorerst OpenGL gewählt. 21:38: „Ich kann verkünden, dass der Build tatsächlich wohl funktioniert … kann mich dort auch bewegen“ |
| 28.09. | Bildflackern weg, **Speichern funktioniert** (356) | Agent; deine Tests |

### Phase D: Vom „läuft“ zum „läuft gut“ (28.09. – 05.10.)

| Datum | Meilenstein | Woher die Idee |
|---|---|---|
| 29.09. | **Sams zuckende Arme behoben** (Update 406/407, FPU-Befehl `fxam` fehlte) | Agent; **du** hast den Fehler gemeldet und per xemu-Vergleich eingegrenzt |
| 29.09. | Audio-Timing: `fxtract`, APU-Takt; der 60-FPS-Patch wurde testweise zurückgenommen | **deine Vermutung**, der 60-FPS-Patch sei schuld (für Videos richtig, für Stimmen nicht) |
| 30.09. | **Musik läuft** (6 fehlende Funktionsanfänge, Update 450); **Spielstand-Schlüssel aus xemu**, damit Saves zwischen xemu und Recomp austauschbar sind (453) | **deine Idee**, Saves von Konsole/xemu nutzbar zu machen (30.09., 09:16) |
| 30.09. | **Level-Trigger nach Laden repariert** (Update 470–472) | **deine Beobachtung**, dass der Trigger nur nach Speichern+Laden ausfällt (30.09., 11:18–13:06) |
| 30.09. | **Höhere Auflösung, 60 FPS als Schalter, Breitbild** (Updates 473–481) | **deine Wünsche** (30.09., 13:58; 60-FPS-Idee 21:46) |
| 01.10. | **Vulkan-Renderer**, xemu-#949-Fix, Vulkan schneller als OpenGL (484–487) | **deine Entscheidung** für Vulkan (30.09., 21:57) |
| 01.10. | Fenster bleibt beim Levelwechsel erhalten (502); Speicherleck beim Levelende (505); Audio-Knistern (506/507) | **dein Wunsch** nach einem durchgehenden Fenster (30.09., 20:47) |
| 01.10. | Durchspielen bis Level 3 mit Laden und Speichern | **du** (01.10., 18:55) |
| 02.10. | **Bink-Videos fehlerfrei**, bytegleich zur PC-Version, nach einem generischen MOVSX-Fehler im Codegenerator (Updates 512–519) | Agent |
| 02.–04.10. | Leistungsarbeit, Boot-Logo-Skip | Agent; deine Messungen |
| 05.10. | **Echtes 5.1** über DSP-JIT, Dolby Digital am Receiver bestätigt (611) | **du** hast mit Toslink und Receiver getestet (05.10., 11:30) |
| 05.10. | **Optionsdatei** `scda_options.txt` (614) | **deine Idee** inklusive Liste der Optionen und Vorgabe „englisch, Sprachkürzel DE/EN“ (05.10., 12:44–14:10) |

### Phase E: Feinschliff und Ende (05.10. – 13.10.)

| Datum | Meilenstein | Woher die Idee |
|---|---|---|
| 05.–08.10. | Flackernde Papierzettel in JBA: tritt auch auf echter Hardware auf; trotzdem behoben (634) | **dein Fund und Wunsch**; deine Vermutung „Geometrie statt Licht“ (06.10.) |
| 08.–09.10. | Leistung: weniger GPU-Rückkopien, Pipelining | **deine Vorgaben** (08.10., 19:20; 09.10., 08:02) |
| 09.10. | **Koop mit zweitem Spieler**, Ziffernblock-Belegung, **60 FPS im Koop** über eine selbst gefundene Stelle (649) | **deine Idee** und Tastenbelegung (09.10., 11:06) |
| 09.10. | **Bloom-Geisterbild behoben** (653), Bloom-Gitter (654–656), Schatten-PCF (657), FXAA (658) | **deine Vergleichsfotos** Xbox One vs. PC und Spielstände |
| 10.10. | **Entscheidung, die Arbeit einzustellen**: anderer Port entdeckt | **du** |
| 13.10. | Release-Paket: `gamefiles`-/`user`-Ordner, ISO-Entpacken; GitHub-Upload; **Bloom-Randflackern bei 2×/4× behoben** (660); AudioDevice für alle Tonarten (662) | **deine Release-Anforderungen**; **dein entscheidender Hinweis „3× ist in Ordnung“** (Abschnitt 6) |

---

## 3. Aufwand und Größenordnung

| Kennzahl | Wert | Status |
|---|---|---|
| Zeitraum | 19.08. – 13.10.2026, **≈ 8 Wochen (56 Tage)** | belegt |
| Tage mit dokumentierter Aktivität | **47 Tage** | gezählt aus Worklog, Status und Chat |
| Tage mit Nachrichten von dir (Claude-Protokoll ab 27.08.) | **38 Tage** | belegt |
| Eigene Nachrichten an die Agenten (nur Claude-Protokoll) | **656**, dazu Codex-Sitzungen | belegt / unbekannt |
| Nummerierte Arbeitsschritte („Updates“) | **662** | belegt |
| Git-Commits (eigene) | **238**, alle 01.–22.09.; danach auf deinen Wunsch keine Commits mehr | belegt |
| Testläufe des Spiels | **über 650** (426 Läufe mit eigenem Protokoll + ~240 frühere Laufprotokolle + ~25 am 13.10.) | Schätzung, Untergrenze |
| Builds | **rund 250** mit eigenem Protokoll | Schätzung, Untergrenze |
| xemu-Vergleichsläufe | **> 100** | Schätzung |
| Debugger-Sitzungen/-Skripte (CDB) | **> 230** | Schätzung |
| Erzeugter C-Code | **≈ 3,6 Mio. Zeilen**, 32.382 Funktionen | belegt |
| Eigene bzw. angepasste Codezeilen | **≈ 50.000** (+22.000 an bestehenden Dateien, ~30.000 in neuen eigenen Dateien), dazu ~40.000 Zeilen übernommener und angepasster xemu-Grafikcode | Schätzung |
| Davon von Hand übersetzte Spielfunktionen (`recomp_manual.c`) | ≈ 6.600 Zeilen | belegt |
| Funktionsanfang-Korrekturen („Seeds“) | **4.110** | belegt; teils automatisch, teils einzeln gefunden |
| Dokumentation | ≈ 71.000 Zeilen (Worklog + Status) | belegt |
| Dokumentierte „Ursache gefunden/behoben“-Einträge | **mindestens ~80–115** | Schätzung aus Überschriften |
| Vom ersten EXE-Start bis spielbar | **24.08. → 27.09. ≈ 5 Wochen** | belegt |
| Vom ersten sichtbaren Bild bis spielbar | **23.09. → 27.09. = 4 Tage** | belegt |

### Besonders intensive Phasen
- **03.–05.09.:** über 1.000 Chat-Interaktionen allein am 04.09. (inkl. automatischer Meldungen), Boot-Blocker im Stundentakt
- **16.–17.09.:** 784 Interaktionen am 17.09. (inkl. automatischer Meldungen): Bink-Puffer, Worker-Threads, common.lin
- **17.–25.09.:** die common.lin-/Ringpuffer-Saga mit mehreren „STOPPING POINT“-Einträgen (Updates 291, 298) und Korrekturen eigener Schlüsse (Abschnitt 4)
- **28.09.:** über 30 Updates an einem Tag zum Armzucken (Updates 358–396)
- **02.–03.10.:** Leistungsarbeit, Updates 520–597

### Stellen mit sehr vielen Versuchen
- Ringpuffer/common.lin: Updates 171–321, ≈ 8 Tage
- Armzucken: Updates 357–407, ≈ 2 Tage
- Bloom/Grafik bei Skalierung: über 2 Wochen verteilt, von 08.09. (XBE-Hack) bis 13.10.
- Musik-Stottern: Updates 617–621, mehrere Sackgassen

---

## 4. Die größten technischen Probleme

### Problem 1: Die common.lin-/Ringpuffer-Saga (17.–25.09.)
1. **Symptom:** Der Start hing reproduzierbar im „Disc kann nicht gelesen werden“-Dialog des Spiels. Daten aus `common.lin`, einem Level-Archiv, waren beschädigt.
2. **Erste Vermutung:** Fehler beim Dateilesen (I/O) oder im Ringpuffer, den das Spiel zum Streamen nutzt; insbesondere die Formel für Schreibposition (Cursor) und Restplatz.
3. **Sackgassen und Fehlannahmen:**
   - am 21.09. eine „Korrektur“ der Cursor-Formel (statt fest 0x4000 die echte Übertragungsgröße), festgehalten als „dauerhaft behoben“
   - am 23.09. bewiesen falsch (Update 274/275): Die Original-Xbox rechnet wirklich mit 0x4000, die „Korrektur“ erzeugte selbst Müll und wurde zurückgenommen
   - dazu Theorien zu Destruktoren, Thread-Races, Allokatoren und einem „Stop-Zähler“, jeweils widerlegt
4. **Echte Ursache (Update 320, 25.09.):** ein allgemeiner Fehler im Übersetzer. Bei bedingten Sprüngen am Anfang eines Codeblocks, die aus mehreren Richtungen erreicht werden, las der erzeugte C-Code eine Hilfsvariable, die nie gesetzt wurde. Das kam **461-mal** im erzeugten Code vor. Folge: Die Funktion, die beim Laden prüft, ob Programmteile zueinander passen (VerifyImport), fand nie einen Treffer. Alle 89 Verknüpfungen eines Archivs liefen ins Leere, die Lesereihenfolge im Datenstrom geriet durcheinander, und erst das sah wie ein Ringpuffer-Fehler aus.
5. **Gefunden:** Byte-genauer Vergleich des Originalcodes mit Capstone gegen den erzeugten C-Code an genau der Stelle, an der ein „gefunden“-Sprung nie genommen wurde.
6. **Behoben:** generisch im Codegenerator (`_flag_states_agree`) plus Regressionstest, danach die komplette Pipeline neu erzeugt.
7. **Aufwand:** ≈ 8 Tage, mehr als 100 Updates (171–321). [Schätzung aus Update-Nummern]
8. **Warum interessant:** Ein winziger, allgemeiner Übersetzungsfehler versteckte sich hinter einem Symptom, das wie ein Spiellogik-Fehler aussah. Lehrbuchbeispiel „Symptom ≠ Ursache“.

### Problem 2: Falsche Funktionsgrenzen (gesamtes Projekt, v. a. 24.08.–25.09.)
1. **Symptom:** Abstürze, Endlosschleifen, „unmögliche“ Werte, Speicherfehler an ständig wechselnden Stellen.
2. **Ursache:** Der Disassembler muss erraten, wo eine Funktion anfängt und endet. Bei Xbox-Code klappt das oft nicht:
   - Funktionen werden nur über Tabellen (Vtables) aufgerufen
   - Funktionen teilen sich Endstücke
   - Sprungtabellen sind rückwärts aufgebaut
3. **Lösung:** 4.110 „Seeds“ (bekannte Funktionsanfänge), Ausschlussliste falscher Anfänge, mehrere generische Verbesserungen am Disassembler, etwa eine systematische Lösung für fehlende, nur über Tabellen erreichbare Funktionsanfänge (Update 326).
4. **Warum interessant:** Das ist das Grundproblem jeder statischen Recompilation. Ein Emulator hat es nicht, weil er erst zur Laufzeit sieht, was Code ist.

### Problem 3: Speicher und Grafikchip – das 0x4AB668-Rätsel (21.–22.09.)
1. **Symptom:** Ein gültiger Zeiger des Grafiktreibers wurde plötzlich zu Müll, und der Start hing in einer Warteschleife auf den Grafikchip.
2. **Vermutung:** Fehlende Grafikchip-Emulation (Update 222), wurde widerlegt (Update 223).
3. **Ursache:** Die Xbox hat einen gemeinsamen Speicher, den die Grafik über mehrere „Fenster“ sieht (zusammenhängender und gekachelter Speicher). Im Recomp waren diese Fenster nicht dieselben Bytes. Eine Kopie in ein Grafikfenster überschrieb deshalb fremde Daten.
4. **Lösung:** Architektur statt Pflaster: dieselbe physische Speicherseite über mehrere virtuelle Ansichten (Update 229).

### Problem 4: Fehlende oder falsch übersetzte Prozessorbefehle (gesamtes Projekt)
Kleine Lücken im Übersetzer mit großen Folgen:

| Befehl | Folge |
|---|---|
| `psubusw`/`paddusw` (MMX) | Ubisoft-Logo in falschen Farben (Update 259) |
| `rdtsc` (Zeitzähler) | Video-Timer lieferte Müll (344b) |
| `cmpneqss` | **jede Quadratwurzel ergab 0**; mit dem Fix wurde das Level spielbar (355/356) |
| `fxam` (FPU-Zahlenklassifizierung) | **Sams Arme zuckten** (406) |
| `fxtract` | Audio-Timing (428) |
| `movsx` mit negativem Wert | **Blockfehler in allen Bink-Videos** (512–515) |
| `lahf`, `pushfd`/`popfd`/`cpuid`, Float-Vergleiche an Zusammenführungspunkten | Abstürze und Hänger |

**Warum interessant:** Ein einziger fehlender Befehl unter Tausenden zeigt sich nicht als Absturz, sondern als ein ganz bestimmter, oft absurder Grafik- oder Tonfehler.

### Problem 5: Xbox-spezifische Kernel- und Hardware-Funktionen
- Die Xbox-Varianten von Windows-Funktionen sind anders aufgebaut:
  - `NtQueryVirtualMemory`: andere Feldreihenfolge, Hinweis aus Cxbx-Reloaded
  - `ObReferenceObjectByHandle`: war nur ein Platzhalter (Update 170)
  - `NtCreateFile`: fehlender Fehlercode, deshalb Disc-Fehler-Falle (05./06.09.)
  - `MmCreateKernelStack`
  - Thread-Priorität `KeSetBasePriorityThread`: wirkte nicht; das verursachte das Musik-Stottern (Update 621)
- Laufwerke: D:/Z:/E:, die HDD-Cache-Partition, Spielstand-Signaturen mit Konsolenschlüsseln
- **Ein-Kern-Xbox:** Die Spiel-Threads mussten auf einen PC-Kern gelegt werden, sonst kam es zu Datenrennen, die auf der echten Xbox nie passieren können. Die Geräte-Threads (Grafik, Ton) durften aber gerade **nicht** auf denselben Kern, sonst verhungerte der Ton (Update 345).

### Problem 6: Grafik – vom eigenen Mini-Renderer zu xemus NV2A, dann Vulkan
- Zuerst gab es einen selbstgebauten CPU-Rasterizer: Er reichte für Logo und Ladebildschirm, nicht für das Spiel (schwarzes 3D, Update 354).
- Dann kam die Entscheidung, xemus echten Grafikchip-Nachbau zu übernehmen (Update 355). Ab da war das Spiel sichtbar.
- Danach folgten OpenGL → Vulkan, höhere Auflösung und unzählige Skalierungsfehler:
  - Bloom-Geisterbild: ein eigener Leistungs-Shortcut im Textur-Cache (653)
  - Lichtgitter bei Skalierung (654–656)
  - Randflackern bei geraden Skalierungsfaktoren (660)
  - Schatten-„Zacken“, Papier-Z-Fighting (634)

### Problem 7: Audio – erst stumm, dann zu schnell, dann knisternd, dann 5.1
- kein Ton → Gerätethreads verhungerten (345)
- Stimmen 2–3× zu schnell → mehrere Ursachen, u. a. `fxtract`, APU-Takt (428/429)
- keine Musik → 6 fehlende Funktionsanfänge (450)
- Knistern → Takt an der Soundkarte statt an der Systemuhr ausrichten (506/507)
- 48-ms-Musikwiederholungen → Thread-Priorität (617–621, mit mehreren Sackgassen: IRQ-Latenz, Wait-Boost)
- echtes 5.1 → xemus DSP-Code plus DSP-JIT, Dolby-Digital-Durchleitung (611)

### Problem 8: Speichern und Laden
- Speichern ging erst nicht, dann ja (356)
- Level-Trigger nach Laden kaputt → falscher Funktionsanfang mitten in einer Funktion, ein „doppeltes Freigeben“ (470/471)
- Saves sind signiert → Schlüssel aus xemu ausgelesen, damit Saves zwischen xemu und PC wandern (453; Test in xemu am 01.10., Update 498)

### Problem 9: Leistung
- Grundproblem: Der Grafikteil kopierte Bilder ständig von der Grafikkarte zurück in den „Xbox-Speicher“ und wartete darauf, bis zu 15-mal pro Bild.
- Lösungen:
  - Schattenkarten auf der GPU lassen
  - Zugriffsverfolgung über Speicherschutz
  - verzögerte Kopien
  - Vertexdaten im Befehlsstrom statt mit vollem GPU-Stopp (651)
- Ergebnis:
  - Koop von ~48 auf ~53,6 FPS
  - Einzelspieler-Drehtest ~57–59 FPS
  - teils noch Einbrüche auf 33–35 FPS an bestimmten Stellen; bei dir zusätzlich durch Jump Desktop belastet

---

## 5. Deine eigenen Beiträge (konkret und belegt)

### a) Fachwissen über Xbox und Spiel, das die KI nicht hatte
- Z: ist die HDD-Cache-Partition; X/Y/Z enthalten Level und Musik (01.09.) → führte zur Laufwerksumleitung, einem der ersten großen Sprünge.
- Die INI-Dateien liegen in `DynamicXbox.umd`, entpackbar mit quickbms (02.09.).
- Title-ID 5553005e, TDATA ist auf echter Hardware leer (02.09.).
- `HalReturnToFirmware` ist nur der Xbox-Neustart und für den PC-Port unnötig (03.09.).
- `common.lin` liegt unter `\Default\DataXb\LMaps`, nicht unter `menu\` (05.09.) → war ein echter Unterschied zu xemu.
- Dein xemu lief mit einem EvoX-BIOS → Umstellung auf Retail für saubere Vergleiche (02.09.).
- Die Xbox hat 64 MB, deine eigene 128 MB (01.09.) → Speicher-Hypothese.
- Spielverhalten: Level in Abschnitten nachgeladen (02.09.), Wache verhält sich in xemu genauso (30.09.), Pausenmenü- und Profil-Abläufe in xemu (27.09.).

### b) Methodik und Steuerung
- **„Bei schwierigen Fragen xemu aufmachen und vergleichen“** (01.09.). Das wurde zur Grundregel des ganzen Projekts.
- **Cxbx-Reloaded als Referenz** vorgeschlagen (01.09.) → Fix für `NtQueryVirtualMemory`.
- **Upstream nur stufenweise mit Checkpoints übernehmen** (08./09.09.).
- **Autonomie-Regel** (10.09.) und **Timeout-Regeln** (17.09., 24.09.), weil Agenten stundenlang auf Ergebnisse warteten, die nie kamen.
- **Modell-/Reasoning-Eskalation** (20.09.).
- **„Keine Symptom-Pflaster, echte Ursachen“** als Dauerregel. Genau deshalb wurde z. B. bei 0x4AB668 eine saubere Speicherarchitektur gebaut statt eines Workarounds (22.09.).
- **Übergaben zwischen Codex und Claude** organisiert, mit festen Übergabe-Prompts und der Pflicht, die Doku zu lesen. Mehrfach hast du zudem englische Fachprompts eines weiteren Chat-Assistenten eingebracht, der dir als „Berater“ diente.

### c) Tests, Beobachtungen, Muster erkennen
- **27.08.:** Live-Debugging in Visual Studio per Hand, mit Breakpoints und Schrittzählung. Dokumentiert als Durchbruch für Seed 33.
- **23.09.:** Bestätigung, dass der erste Screenshot dem echten Xbox-Bootlogo entspricht.
- **27.–28.09.:** Erste Spieltests mit genauen Beschreibungen: Hänger bei „Neues Spiel“, Absturz beim Missionsvideo, Flackern alle ~½ s, unsichtbare Waffe, zuckende Arme, flackerndes Eis „alle 1–2 Sekunden auf dieselbe Art“.
- **29.09.:** Ton-Beobachtungen Szene für Szene, z. B. „Intro okay, Button-Sounds super schnell, Missionsvideo schnell mit Tonpausen“.
- **30.09.:** **Level-Trigger-Eingrenzung**: Das Problem tritt nur auf, wenn vor der Folie gespeichert **und** geladen wurde. Der Agent fand daraufhin die Ursache (Updates 470–472).
- **01.10.:** Frametime-Graphen mit MSI Afterburner und F9-Messungen an bestimmten Stellen. Spielstände gezielt für Fehlerstellen angelegt (Eis, Lampe, Sicherungskasten, Regal, JBA-Papier, Geldzug, Cozumel). Mit Fly-Mod schneller zu Teststellen geflogen.
- **01.10.:** Spielstand vom Recomp in xemu geladen, ob er dort korrekt ist (Update 498).
- **05.10.:** 5.1 am Receiver über Toslink bestätigt.
- **06.10.:** Vermutung „nicht Licht, sondern Geometrie des Papiers“ bei JBA. Die Ursache war Z-Fighting der Papierfläche mit dem Boden, also ein Geometrie-/Tiefenproblem.
- **08.10.:** Selbst korrigiert: „verschwundenes Papier habe ich mir nur eingebildet“.
- **09.–10.10.:** Vergleichsfotos von Xbox One (Abwärtskompatibilität) und PC für Bloom, Schatten und Kanten.
- **13.10.:** **„3× ist in Ordnung“, bei 2× und 4× flackert es.** Dieser Hinweis führte direkt zur Ursache (gerade vs. ungerade Faktoren, Update 660).

### d) Produkt- und Feature-Ideen
- höhere Auflösung, Breitbild, 60 FPS (30.09.); Idee 30 FPS in Menü/Videos und 60 im Spiel (30.09., 21:46)
- Fenster bleibt beim Levelwechsel offen, proportional skalierbar (30.09.)
- Vulkan statt OpenGL (27.09. angeregt, 30.09. entschieden)
- Saves von Konsole/xemu nutzbar machen (30.09.)
- Optionsdatei mit allen Schaltern, englisch, Sprachkürzel (05.10.)
- Koop mit zweitem Controller, eigene Ziffernblock-Belegung, 60 FPS im Koop, umschaltbar (09.10.)
- Pipelining „nächstes Bild vorbereiten“, Befehle in Teilstücken an die GPU (08./09.10.)
- Release-Konzept: `gamefiles`/`user`, ISO direkt nutzbar, Meldung bei fehlenden Spieldateien (13.10.)
- AudioDevice für HDMI/S/PDIF/Toslink verallgemeinern (13.10.)
- Wunschliste für später: siehe Abschnitt 9

### e) Wo du Agenten korrigiert oder umgelenkt hast (Beispiele)
- **01.09.:** „Problem ist nicht, dass das Spiel 30 min läuft, sondern dass 30 min nur das CMD-Fenster offen ist … Mir wäre das Wichtigste, dass nun erstmal überhaupt etwas kommt.“
- **01.09.:** Nachgefragt, ob ein Hänger wirklich behoben oder nur ausgeklammert war.
- **04.09.:** „Keine allgemeinen Refactorings, die nicht zum aktuellen Blocker beitragen.“
- **11.09.:** „Du bist hier nicht an einer echten Sackgasse … bitte ohne Rückfrage weiterarbeiten.“
- **17.09./24.09.:** Wartezeiten begrenzt, weil Agenten Wake-ups verschliefen („9:13 ist um – keine Reaktion von dir“, 24.09.).
- **29.09.:** 60-FPS-Patch als Ursache testen lassen statt Ton global zu „korrigieren“.
- **08.10.:** Vollständige, klickbare Dateipfade verlangt.
- **09.10.:** Leistungsmessungen relativiert: Jump Desktop läuft im Hintergrund, die FPS-Zahlen im Test unterscheiden sich von deinen.
- **10.10.:** Bloom-Fix bewertet: „sieht anders aus, aber nicht großartig besser“. Das führte zur Suche nach der eigentlichen Ursache (Texturcache-Fehler, 653).

---

## 6. Irrwege und kuriose Momente

1. **„Dauerhaft behoben“, und doch falsch:** Die Ringpuffer-Korrektur vom 21.09. stand als dauerhafter Fix in den Notizen. Zwei Tage später zeigte sich, dass die Original-Xbox genau die „falsche“ Formel benutzt. Die echte Ursache war ein Übersetzerfehler an 461 Stellen (Abschnitt 4, Problem 1).
2. **Die Zeitmaschine im Werkzeug:** Am 09.09. kam heraus, dass die Disassembly-Stufe seit dem 24.08. nicht mehr neu gelaufen war. Fixes dort landeten also über zwei Wochen lang gar nicht im erzeugten Code.
3. **Sams zuckende Arme:** Ursache war die „Atmungs-Animation“ im Leerlauf. Sie nutzt Perlin-Rauschen, und dieses ruft `fmod` auf, das intern Zahlen mit dem FPU-Befehl `fxam` klassifiziert. Den hatte der Übersetzer nur als Kommentar ausgegeben, also nahm `fmod` zufällige Pfade und die Arme sprangen jedes Bild (Update 406).
4. **Jede Wurzel war null:** Das erste Level wurde erst spielbar, als auffiel, dass `sqrt` immer 0 lieferte, weil der Befehl `cmpneqss` fehlte (Update 355).
5. **Videos in Klötzchen:** Alle Bink-Videos hatten Blockfehler. Schuld war ein einzelner Fall im Codegenerator, eine Vorzeichenerweiterung (`MOVSX`) mit negativem Koeffizienten. Nach dem Fix waren die Videos bytegleich zur PC-Version (Updates 512–519).
6. **Das gerade/ungerade-Rätsel (13.10.):** Das Bloom-Flackern am Rand trat bei 2× und 4× auf, bei 3× nicht. Bei geraden Faktoren fällt die Mitte eines Originalpixels genau zwischen zwei hochskalierte Pixel; beim Verkleinern wurde ein nie beschriebener Randstreifen mitgemittelt. Bei 3× liegt die Mitte zufällig auf einem beschriebenen Pixel. Dein Hinweis „3× ist in Ordnung“ war der Schlüssel.
7. **Der Geist im Bloom (09.10.):** Ein Geisterbild neben hellen Lichtern stammte nicht aus der Bloom-Mathematik, sondern aus einem eigenen Leistungs-Shortcut: Der Textur-Cache meldete „geändert“ nur einmal pro Speicherbereich, obwohl zwei Texturen denselben Bereich lasen (Update 653).
8. **Die ein-Kern-Xbox:** Die Xbox hat einen CPU-Kern. Erst als die Spiel-Threads auf einen PC-Kern gelegt wurden, verschwanden zufällige Speicherfehler. Danach fehlte der Ton, weil Grafik und Ton mit auf demselben Kern landeten (Update 345).
9. **Musik stottert alle 48 ms:** Nach Spuren zu Interrupt-Latenz und Zeitscheiben war es am Ende die Thread-Priorität. Das Spiel hebt den Musik-Thread an, aber die Übersetzung übergab einen Xbox-Zeiger statt eines Windows-Handles, also passierte nichts (Update 621).
10. **Der Fehler, den die Xbox selbst hat:** Die flackernden Papierzettel in JBA tun das auch auf echter Hardware (Z-Fighting). Du wolltest es trotzdem weg haben; jetzt ist es im PC-Port besser als auf der Konsole (Update 634).
11. **Falscher Verdacht „fehlender Grafiktreiber“:** Update 222 vermutete fehlende Grafikchip-Emulation, Update 223 korrigierte: echter Speicherfehler (Problem 3).
12. **Du hast dich selbst korrigiert:** „Die Sache mit dem verschwundenen Papier … habe ich mir tatsächlich nur eingebildet“ (08.10.). Gutes Beispiel für ehrliches Testen.

---

## 7. Stand des Ports am Ende (13.10.2026)

| Bereich | Stand | Status |
|---|---|---|
| Start/Boot | Startet zuverlässig; Release-Layout mit ISO-Entpacken, Versionsprüfung, Meldungen bei fehlenden Dateien/DLLs | getestet 13.10. |
| Menüs | Hauptmenü, Profile, Optionen, Pausenmenü, Laden/Speichern | belegt |
| Gameplay | Level spielbar; durchgespielt mindestens bis Level 3 (01.10.); Tests in mehreren Leveln (Iceland, HQ, JBA, Cozumel, Prison, Geldzug). **Ein vollständiger Durchlauf aller Level ist nicht dokumentiert** | belegt / unbekannt |
| Koop | Split-Screen mit zwei Controllern oder Tastatur-Ziffernblock, 60 FPS möglich | belegt (09.10.) |
| Grafik | xemu-NV2A auf Vulkan (oder OpenGL); Bloom-, Schatten- und Kantenfixes; FXAA | belegt |
| Auflösung | 1×–4× intern (bis 1920 Zeilen), 16:9 oder 4:3 | belegt |
| Framerate | 60 FPS (30 wählbar); Einzelspieler ~57–59 FPS im Drehtest, an Stellen Einbrüche (33–35 FPS gemeldet, teils durch Jump Desktop); Koop ~53,6 FPS | belegt (Messwerte stammen von deinem PC) |
| Steuerung | XInput-Controller (2 Spieler), Tastatur für Spieler 1 und 2 | belegt |
| Audio | Stereo, 5.1 PCM, Dolby Digital an den Receiver; Musik, Stimmen, Videos; Ausgabegerät wählbar (HDMI/S/PDIF) | belegt |
| Laden/Speichern | funktioniert; Saves mit xemu austauschbar (mit eigenem Schlüssel) | belegt |
| Stabilität | Gelegentlicher Absturz beim Laden (Vulkan „Device Lost“), seltene Startabbrüche (10.10., Ursache unbekannt) | belegt |
| Bekannte Bugs | Schatten-Zacken an manchen Kanten (Glättung ungetestet); Bild etwas dunkler als Xbox One; Nachtsicht im Split-Screen hellt die andere Hälfte auf; Rest-Flackern bei JBA-Papier; „Zacken aus der Wand“ (einmal gesehen) | belegt |
| Fehlende Features | Xbox Live / System Link (LAN-Koop nur voruntersucht); weitere Wünsche siehe Abschnitt 9 | belegt |
| Workarounds für Spieler | SmartScreen-Warnung beim ersten Start; Spiel selbst bereitstellen (Original-Xbox-Disc US-094) | belegt |
| Komfort | Optionsdatei, Fenster merken, Monitorwahl, Vollbild, Intro überspringen, Hintergrund-Stumm, Fenster bleibt beim Levelwechsel, Einzelinstanz-Schutz | belegt |

### Wie weit von „guten Gewissens veröffentlichbar“ entfernt?
- **Technisch war der Port spielbar und für Interessierte veröffentlichbar.** Er wurde am 13.10. als v1.0 mit Anleitung bereitgestellt. Echte Blocker gab es keine mehr.
- **Für einen „polished“ Release hätten gefehlt (Einschätzung):**
  1. **Stabilität:** den seltenen Ladeabsturz (Device Lost) und die seltenen Startabbrüche finden
  2. **ein kompletter Durchlauf** aller Level und des Koop-Modus als Test
  3. **Leistung** auf schwächeren PCs geprüft; bisher nur ein Testrechner
  4. **Restliche Grafikfehler:** Schattenkanten, Helligkeit vs. Xbox One, Nachtsicht im Split-Screen
  5. **Bedienkomfort:** Optionen im Spiel statt Textdatei, Tastenbelegung im Menü
  6. **Signierte EXE** gegen die SmartScreen-Warnung
  7. **Doku/Support:** Issue-Vorlagen, FAQ (vorbereitet, nicht umgesetzt)

---

## 8. Vergleich zum anderen Port

### Was belegt ist
- Name/Team laut deiner Reddit-Fassung: Discord-Nutzer **TGP482** mit Hilfe von **Joshhhuaaa** (bekannt von „SC Enhanced“). Technik laut dir: **DirectX 12**. Du nennst es ebenfalls einen XBE-Recomp („der andere scdaxbe recomb“, 13.10.).
- Du hast ihren Build am 10.10. getestet: „läuft tatsächlich besser als meiner“, „exceeds my goals“.
- Auf deinem PC liefen am 10.10. (`SCDAOffline.exe`) und 11.10. (`SCDA.exe`) Programme dieses Namens. Windows hat dazu jeweils nur einen Speicher-Hinweis protokolliert (RADAR_PRE_LEAK), keinen Absturz.
- Bei ihrem Build kam bei dir keine SmartScreen-Warnung (13.10.). Ob er signiert ist: **[unbekannt]**.

### Was für einen fairen Vergleich fehlt [unbekannt, muss recherchiert werden]
- Feature-Liste: Auflösung, FPS, Koop, Audio/5.1, Spielstände, Mods
- Stabilität und Leistung auf gleichem PC und gleichen Szenen (z. B. Lampe, Sicherungskasten, Koop, Geldzug)
- Architektur: eigener DX12-Renderer statt xemu-NV2A? Wie viel Hand-Portierung? Teil-Decompilation?
- Benötigtes Spielmaterial: welche Version, ISO, Installation
- Lizenz und Quellcode

### Strukturelle Unterschiede, sachlich vermutet [Schätzung]
- **Grafik:** Dein Port nutzt einen **Nachbau des Xbox-Grafikchips** (NV2A aus xemu). Das ist originalgetreu, aber teuer, weil jede Grafikoperation „wie auf der Xbox“ ausgeführt wird, inklusive Rückkopien in den Xbox-Speicher. Ein **direkter DX12-Renderer** kann die Spielgrafik nativ und deutlich effizienter umsetzen, braucht aber viel spielspezifische Handarbeit. Das würde den Unterschied in Leistung und Polish plausibel erklären, ist aber unbestätigt.
- **Team und Vorwissen:** zwei Personen mit Vorerfahrung (SC Enhanced) vs. du als Nicht-Programmierer mit KI-Agenten.

### Was dein Projekt besonders oder anders gelöst hat [belegt]
- Echtes 5.1 inklusive Dolby-Digital-Durchleitung über S/PDIF/HDMI aus dem Original-DSP-Code
- Spielstände kompatibel mit xemu bzw. Konsole (Signaturschlüssel)
- Koop mit zwei Spielern an einem PC, 60 FPS im Koop über einen selbst gefundenen Schalter
- Behebung eines Fehlers, den die Original-Hardware selbst hat (JBA-Papier)
- ISO direkt verwendbar, inklusive Versionsprüfung
- Vollständig dokumentierter Weg (~71.000 Zeilen Protokoll), offener Quellcode
- **Methodisch:** fast vollständig KI-gestützt, mit einem Nicht-Programmierer als Projektleiter

---

## 9. Was als Nächstes gekommen wäre

### Direkt vor dem Stopp offen (09.–10.10., belegt)
- Schatten-Zacken an der Kiste (Slot 1, Regal-Szene): **Ursache gefunden** (Rastertreppe der Schattenkarte bei flachem Licht), breitere Schatten-Glättung gebaut, **visuell ungetestet**
- Blaues Flackern am Rand bei 4× → **am 13.10. behoben** (mit 2×)
- Bild dunkler als Xbox One (Belichtung)
- Nachtsicht im Split-Screen hellt die andere Hälfte auf
- Leistung: Aufwand pro Zeichenbefehl im Grafikthread
- seltener Ladeabsturz (Device Lost)
- „Zacken aus der Wand“ (einmal gesehen)
- „Weg B“: eigenen Bloom statt des Spiel-Blooms, falls dir der korrigierte nicht gefällt

### Wunschliste (docs/SCDA_FUTURE_BACKLOG.md, Ideen-Notizen, belegt)
- Weitere Sichtweite, weniger Nebel
- Intro- bzw. „Drücke Start“-Bildschirm sofort überspringbar
- Pre-Release-Intromusik bzw. eigene WAV
- Hochauflösende Texturen für SC-20K/FN Five-seveN
- KI-hochskalierte Bink-Videos
- Ego-Perspektive nur beim Zielen
- Flugmodus per Taste
- Musikstottern im Ochotsk-Level prüfen
- Split-Screen auf zwei Fenster/Monitore
- Koop über LAN/System Link (Voruntersuchung vorhanden)
- Surround über Toslink → am 13.10. umgesetzt
- Schnellstart mit letztem Spielstand, Level-Cache vorab füllen
- Nachtsicht-Störeffekt („interference“) in HQ abschaltbar (10.10.)

### Einordnung
- **Keine echten technischen Blocker mehr.** Alles Offene ist Feinschliff, Komfort oder Stabilitätsarbeit.
- **Am ehesten „echte“ Arbeit wären noch gewesen:**
  - der seltene Ladeabsturz
  - die Leistung auf schwächeren PCs
  - System Link (größeres Projekt)
- **Du warst also sehr nah an deinem persönlichen Ziel.** Am 09.10. schriebst du sinngemäß, damit seist du „wohl zufrieden“ (Bloom). Am 10.10. fing die Feinarbeit an Schatten und Kantenglättung an.

---

## 10. Rückblick und Learnings (sachlich)

### Was mit KI-Agenten überraschend gut funktionierte
- **Ausdauer bei Detektivarbeit:** byte-genaue Vergleiche, Debugger-Skripte, hunderte Messläufe, Muster in riesigen Logs
- **Generische Korrekturen** am Übersetzer, die viele Fehler auf einmal lösten (Flag-Merge, MOVSX, x87-Befehle)
- **Fremden Code integrieren:** xemu-Renderer, DSP-JIT, Vulkan
- **Saubere Dokumentation**, die Übergaben zwischen verschiedenen Agenten möglich machte

### Wo KI Zeit verschwendet hat
- **Lange Sackgassen** mit selbstbewusst „bewiesenen“ Thesen, die später korrigiert wurden (Ringpuffer, „fehlender Grafiktreiber“, Hash-Lookup-Kette mit Updates 125/135/137/139 als Selbstkorrekturen)
- **Passives Warten** auf Ergebnisse oder Wake-ups, die nie kamen; deshalb deine Timeout-Regeln
- **Messfehler in eigenen Werkzeugen** (Update 126 „dwo()-Messfehler“, Update 650 „Koop-Readbacks waren Messfehler“)
- **Übermäßig tiefe Analyse statt pragmatischer Schritte**; deshalb deine Regeln „billigster falsifizierbarer Schritt“ und „kein Refactoring ohne Blocker-Bezug“
- **Kontext ging verloren:** über 300 Kontext-Zusammenfassungen (`/compact`) in der Claude-Sitzung

### Wann menschliche Kontrolle besonders wichtig war
- **Spielwissen und Wahrnehmung:** Bildfehler, Ton-Tempo, Spielabläufe, „das ist das echte Bootlogo“, „3× ist okay“
- **Prioritäten und Ziel:** erst Logo und Menü, dann Spielbarkeit, dann Qualität
- **Qualitätsmaßstab:** keine Pflaster, Original-Hardware bzw. xemu als Maßstab
- **Abbruch- und Wechselentscheidungen:** Upstream stufenweise, Vulkan, Projektende
- **Sicherheit:** keine Schlüssel und keine Spieldaten veröffentlichen, nicht parallel zum Spieler testen

### Was man heute anders machen könnte (Vorschläge)
- Von Anfang an einen festen Regressionstest-Satz (Spielstände plus automatische Bildvergleiche) und automatische Builds
- Früher die Original-Bytes als Referenz prüfen (Capstone), bevor eine Ursache als „bewiesen“ gilt
- Weniger parallele Agenten am selben Code, bzw. klare Zuständigkeiten
- Früher entscheiden, ob man den Xbox-Grafikchip nachbildet oder nativ rendert (Leistung)

### Offener entwickeln: Vor- und Nachteile
- **Deine Vorgehensweise:** wenige große Reddit-Updates statt kleiner Zwischenstände. Daten und Anzahl der Posts sind im Repository **[unbekannt]**.
- **Vorteile:** Ruhe zum Arbeiten, keine Erwartungen von außen, kein Druck durch halbfertige Zwischenstände, keine Diskussionen über Legalität oder Builds zu früh.
- **Nachteile:**
  - **Überschneidungen werden spät sichtbar:** Das andere Team arbeitete parallel am gleichen Ziel; das wurde erst kurz vor dem Ende bekannt.
  - **Kein früher Austausch** (z. B. mit Leuten von SC Enhanced oder xemu), der Sackgassen hätte abkürzen können.
  - **Keine Tester mit anderen PCs**, kein früher Fehlerbericht.
- **Häufigere kleine Updates hätten geholfen:**
  - **Sichtbarkeit:** Wer am selben Spiel arbeitet, findet einen schneller (Reddit, Discord, GitHub).
  - **Zusammenarbeit statt Parallelarbeit:** Arbeitsteilung wäre denkbar gewesen, etwa dein 5.1-/Save-/Koop-Wissen kombiniert mit deren DX12-Renderer.
  - **Motivation und Community:** Fortschritte wie „erstes Bild“, „Hauptmenü“ und „erstes Level“ sind gute Kurz-Posts.
  - Ob das Ergebnis dann anders ausgegangen wäre, bleibt **[Spekulation]**.

---

## 11. Die menschliche Seite – Stichpunkte (keine Gefühle erfunden)

### Objektive Umstände
- 8 Wochen, 47 Arbeitstage, 656 dokumentierte eigene Nachrichten allein an Claude, als Nicht-Programmierer
- Das Ziel war spielen können **und** etwas für die Community schaffen (deine Aussage)
- Der persönliche Zielzustand war praktisch erreicht: spielbar, 60 FPS, Koop, 5.1, Saves
- Am 10.10. hast du vom anderen Team erfahren, deren Build getestet und offen anerkannt, dass er weiter ist
- Du hast trotzdem veröffentlicht, mit Code, Anleitung und Credits, und empfiehlst öffentlich deren Port
- Auf Wunsch des 60-FPS-Patch-Urhebers hast du dessen Namen überall entfernt

### Mögliche Gesprächsthemen
- Warum KI-gestützte Entwicklung einem Nicht-Programmierer so ein Projekt überhaupt ermöglicht
- Was man in 8 Wochen über Xbox-Hardware, Recompilation und Fehlersuche lernt
- Was es heißt, wenn zwei Projekte parallel entstehen: Bestätigung, dass das Ziel richtig war, und dass die Community am Ende profitiert
- Warum dein Build trotzdem verfügbar bleibt: Code als Dokumentation, 5.1/Koop/Saves-Lösungen, Lernmaterial
- Momente, die dir selbst etwas bedeutet haben (z. B. „Ich kann verkünden, dass der Build tatsächlich wohl funktioniert“, 27.09.; Receiver zeigt Dolby Digital, 05.10.). Ob und wie du diese Gefühle beschreibst, entscheidest du.

---

## 12. Vorgeschichte: XBE- und xemu-Hack

### Was im Repository dokumentiert ist (`SC4_xbe_hack/`, 06.–14.09.2026) [belegt]
- **Kein Vorlauf, sondern ein paralleles Projekt** während der Recompilation: gezielte Byte-Patches an der Original-XBE für xemu, ausdrücklich getrennt vom PC-Port.
- **Ziel:** störende Effekte in xemu bei höherer Render-Auflösung abschalten, ohne Nachtsicht oder Videos zu beschädigen.
- **Gesicherte XBE-Patches:**
  - Rauch-Compositing überspringen (Offset `0x002EC475`)
  - Verpixelungs-Effekt aus (`0x005D239C`)
  - Bloom-/Rand-Combiner aus (`0x005D78F4`, Nachtsicht bleibt erhalten)
  - Ergebnis: `default_scda_graphics_fixes.xbe` bzw. `scda_graphics_fixes.iso`, plus eine 30-FPS-Variante
- **Werkzeuge:** ReShade mit ShaderToggler in xemu, ein selbst gebautes ReShade-Add-on zur Shader-Aufzeichnung, ein instrumentierter xemu-Build (Pushbuffer-/NV2A-Mitschnitt)
- **Skalierungsproblem in xemu (08.09.):** „Bei 1× Render-Skalierung ist der Bloom-Rand unauffällig; bei 2×/3× entstehen links/oben grüne Streifen“. Das ist **exakt die Fehlerfamilie, die im PC-Port bis zum 13.10. gejagt wurde.** Damals vermutet: falsche Pixelgrößen-Werte (`surfaceSize`). Im PC-Port fand man am Ende zwei echte Ursachen: Textur-Cache (653) und Verkleinerung bei geraden Faktoren (660).
- **60-FPS:**
  - Der Community-IPS-Patch ändert genau **1 Byte** (`0x002E3387`): die Bildwiederholrate des Renderers, „Present Interval“ 2 → 1. Er wurde im XBE-Hack verglichen und gemessen (~59–60 FPS in xemu).
  - **Im PC-Port:**
    - zunächst übernommen
    - am 29.09. zurückgenommen (Videos und Ton zu schnell)
    - dann als eigener Schalter umgesetzt (Update 477)
    - im Koop über eine **selbst gefundene** zweite Stelle im Grafiktreiber (Update 649)

### Was fehlt [unbekannt]
- Deine Arbeit mit xemu bzw. der XBE **vor** dem 19.08. Im Repository gibt es dazu nur Spuren: deine Ordner auf Z:, die IPS-Datei, deine xemu-Konfigurationen und ReShade-Profile. Bitte selbst ergänzen: Zeitraum, Motivation, was du davon vorher schon selbst gefunden hattest.

### Was davon dem PC-Port half [belegt/abgeleitet]
- Kenntnis der Effekte (Bloom, Rauch, Nachtsicht) und Testszenen
- ReShade-/ShaderToggler-Erfahrung
- Wissen, dass Skalierungsfehler am Rand auftreten
- xemu-Erfahrung mit Vulkan vs. OpenGL (deine Nachricht 27.09.)
- Die 60-FPS-Stelle und die Erkenntnis, dass 60 FPS Video- und Tontempo beeinflussen kann

---

## 13. Projekt in Zahlen (für eine Einblendung)

- **≈ 8 Wochen** Entwicklung (19.08.–13.10.2026), **47 Tage** mit dokumentierter Arbeit [belegt/gezählt]
- **662** dokumentierte Arbeitsschritte, **238** Git-Commits (bis 22.09.) [belegt]
- **> 650** Testläufe, **~250** Builds, **> 100** xemu-Vergleichsläufe [Schätzung, Untergrenze]
- **≈ 3,6 Millionen** Zeilen automatisch erzeugter C-Code, **32.382** übersetzte Funktionen [belegt]
- **≈ 50.000** Zeilen eigener bzw. angepasster Code, plus ~40.000 Zeilen übernommener xemu-Grafikcode [Schätzung]
- **4.110** Korrekturen von Funktionsanfängen [belegt]
- **≈ 80–115** dokumentierte gefundene und behobene Ursachen [Schätzung]
- **≈ 71.000** Zeilen Projektdokumentation [belegt]
- **Mindestens 3 KI-Agenten/Werkzeuge** (Codex, Claude Code mit Sonnet 5 und Opus 5.5, OpenCode) plus ein weiterer Chat-Assistent als Berater [belegt/teils unbekannt]
- **656** eigene Nachrichten allein an Claude [belegt]
- **≈ 5 Wochen** vom ersten EXE-Start bis spielbar; **4 Tage** vom ersten sichtbaren Bild (Ubisoft-Logo, 23.09.) bis zum ersten spielbaren Level (27.09.) [belegt]
- **36 MB** Release-ZIP; das Spiel selbst ist nicht enthalten [belegt]

---

## 14. Material fürs Videoskript

### A) Die 10 wichtigsten Meilensteine
1. 24.08. – Erster Build: Die Pipeline erzeugt eine Windows-EXE
2. 01.09. – Durchbruch beim Start: Z:-Cache-Laufwerk und NtQueryVirtualMemory (Cxbx-Hinweis)
3. 11.09. – Erster kompletter Startlauf ohne Absturz
4. 17.09. – Erstes Spielfenster
5. 23.09. – Erstes echtes Bild: Ubisoft-Bootlogo
6. 25.09. – Echte Ursache der Ringpuffer-Saga gefunden; erster Ladebildschirm
7. 27.09. – Hauptmenü und erstes spielbares Level (mit xemu-Grafik)
8. 30.09./01.10. – Musik, Speichern/Laden, höhere Auflösung, 60 FPS, Vulkan
9. 05.10. – Echtes 5.1 mit Dolby Digital am Receiver
10. 09.10. – Koop mit zwei Spielern und 60 FPS; danach Release (13.10.)

### B) Die 5 interessantesten technischen Probleme
1. Ringpuffer-Saga: „bewiesene“ Ursache falsch, echter Fehler im Übersetzer an 461 Stellen
2. Zuckende Arme: Perlin-Rauschen → `fmod` → fehlender FPU-Befehl `fxam`
3. Gemeinsamer Xbox-Speicher mit mehreren „Fenstern“ (0x4AB668)
4. Bloom-Randflackern: gerade vs. ungerade Skalierung
5. Ein-Kern-Xbox auf einem Mehrkern-PC: Threads pinnen, aber nicht alle

### C) 5 „Das hätte ich vorher nicht erwartet“-Momente
1. Jede Quadratwurzel im Spiel ergab 0, bis ein einzelner Befehl ergänzt wurde
2. Die Videos waren wegen einer einzigen Vorzeichen-Regel im Generator kaputt
3. Ein Werkzeug hatte über zwei Wochen alte Analysen wiederverwendet (09.09.)
4. Die Xbox selbst hat den Papier-Flackerfehler, der PC-Port jetzt nicht mehr
5. Die Musik stotterte, weil eine Thread-Priorität mit dem falschen Ausweis angefragt wurde

### D) Die 5 wichtigsten eigenen Beiträge von dir
1. Methode „xemu als Wahrheit“ und Cxbx-Reloaded als Referenz vorgeschlagen (01.09.)
2. Xbox-/Spielwissen: Z:-Cache, UMD-Archive, Pfade, BIOS (01.–05.09.)
3. Präzise Spieltests und Eingrenzungen: Level-Trigger nach Laden, Ton Szene für Szene, „3× ist okay“
4. Projektsteuerung mehrerer KI-Agenten: Regeln, Übergaben, Prioritäten, „keine Pflaster“
5. Produktideen: Optionsdatei, Koop, 5.1-Test, Saves mit xemu, Release-Konzept mit ISO

### E) Die 5 wichtigsten Learnings
1. Symptom ≠ Ursache. Lange Saga, kleiner allgemeiner Fehler.
2. Eine Referenz (xemu, Original-Bytes) schlägt jede Vermutung.
3. KI braucht Leitplanken: Timeouts, Prioritäten, Qualitätsregeln.
4. Der Mensch bleibt für Wahrnehmung, Ziel und Entscheidungen zentral.
5. Früh und oft öffentlich zeigen findet Mitstreiter und vermeidet Doppelarbeit.

### F) Wichtigste Unterschiede zum anderen Port (soweit belegt)
- Deren Build: weiter fortgeschritten und runder (deine Aussage), DirectX 12, Zweierteam mit SC-Enhanced-Erfahrung
- Dein Build: xemu-Grafikchip-Nachbau (originalgetreu, aber teurer), echtes 5.1 inkl. Dolby Digital, Koop mit 60 FPS, xemu-kompatible Saves, Open Source mit vollständigem Protokoll
- Für einen fairen Vergleich fehlen Messungen an gleichen Szenen und deren Feature-Liste (Abschnitt 8)

### G) Was dein Build am Ende konnte
Spielbar mit Menüs, Speichern und Laden, 1–4× Auflösung, Breitbild, 60 FPS, Koop mit zwei Spielern, Stereo/5.1/Dolby Digital, Vulkan, korrigierter Bloom, FXAA, Optionsdatei, Release mit ISO-Unterstützung.

### H) Was noch gefehlt hätte
Seltene Abstürze beim Laden, Komplett-Durchlauf als Test, Leistung auf schwächeren PCs, Schattenkanten und Helligkeit, Optionen im Spielmenü, signierte EXE, System Link.

### I) 5 Szenen, die sich gut zeigen lassen
1. Das CMD-Fenster, das „unendlich rödelt“, gegen den ersten Screenshot vom Ubisoft-Logo (`tools/debug/fbdump_update259close/frame000.png`, falsche Farben vorher / richtige nachher), dann der Ladebildschirm (`tools/debug/update339_fb_screen_200s.png`)
2. Erstes Level mit schwarzem 3D (`tools/debug/update353_first_level_ui.png`), dann mit xemu-Grafik spielbar
3. Sams zuckende Arme vorher/nachher (Spielstand an der Dialogstelle im ersten Level)
4. Bloom bei 2×: Randflackern vorher (Differenzbild) und nachher; dazu Xbox-One-Vergleich der Lampe
5. Koop mit Controller und Tastatur-Ziffernblock bei 60 FPS; dazu Receiver-Display mit „Dolby Digital“

Dazu passend: JBA-Papierflackern (`docs/bug_screenshots/jba_paper_flicker_1.png`), Sicherungskasten mit Bloom, Regal mit Schatten-Zacken.

### J) Fakten für Zuschauer ohne Technikwissen
- Das Spiel wurde nicht emuliert, sondern einmal komplett in PC-Code übersetzt: 3,6 Millionen Zeilen, die kein Mensch geschrieben hat.
- Die Xbox hat nur einen Prozessorkern. Einige Fehler entstanden nur, weil der PC mehrere hat.
- Ein Fehler, der auf der echten Xbox existiert (flackerndes Papier), ist im PC-Port behoben.
- Spielstände sind „unterschrieben“; mit dem eigenen Konsolenschlüssel wandern sie zwischen Xbox, Emulator und PC.
- USA- und EU-Version sind dieselbe Disc (redump: US-094, „USA, Europe“). Die Xbox-360-Version ist ein ganz anderes Spiel.
- Ein großer Teil der Fehlersuche war Detektivarbeit, bei der man das gleiche Spiel im Emulator und im PC-Port nebeneinander laufen ließ und den ersten Unterschied suchte.
