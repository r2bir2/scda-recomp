status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG (unveraendert seit Update 205)

**MODEL RECOMMENDATION:** In Codex: starke, aber nicht die hoechste Analyse-Stufe.
In Claude: starkes Modell, hohe aber nicht maximale Reasoning-Tiefe.

**REASON:** Update 206 hat das GOOD/ZERO-Fenster erstmals eng eingeklammert
(auf ein Speicherwachstums-Fenster von ca. 148.000-162.500 KB, geschaetzt
< 1 Sekunde Echtzeit) und vier neue Funktionen im Fatal-Handler-Cleanup-Pfad
gefunden, gelesen und als Schreiber ausgeschlossen (Assert-Logging +
NV2A-Pushbuffer-Flush, beides generisch und objektunabhaengig). Die Analyse
war anspruchsvoll, aber mit strukturiertem Vorgehen loesbar -- STRONG bleibt
angemessen. Bei einem direkten Treffer im naechsten, noch feiner
eingegrenzten Fenster MAXIMUM in Erwaegung ziehen.

**Seit Update 205 -- Update 206 (diese Sitzung), gestaffelte Single-Attach-
Messung nach neuer, vom User vorgegebener Methode:**
1. **Erste Staffel (8 Zeitpunkte 10-32s, je ein einzelner Attach):** Alle
   acht Proben zeigten identisches "noch nicht initialisiert" (~52-56 KB,
   Arena unzugaenglich). Ergebnis: die tatsaechliche Boot-Dauer bis zum
   grossen Speichersprung betraegt HEUTE ca. 36 Sekunden (nicht 25-30s wie
   in Update 205 aus einem einzelnen Kontrollprozess abgeleitet) --
   Boot-Tempo variiert offenbar tagesabhaengig.
2. **Neue Technik -- debuggerfreie Sprungerkennung:** `tasklist`-Polling
   (nachweislich nebenwirkungsfrei) erkennt den Speichersprung (~65 KB auf
   ~173 KB), DANACH genau ein `cdb`-Attach. Ergebnis: GOOD/ZERO-Fenster auf
   ein enges Speicherwachstums-Intervall eingeklammert (Details siehe
   Update 206 in PROJECT_STATUS.md).
3. **Vier neue Funktionen im Fatal-Handler-Pfad gefunden und ausgeschlossen:**
   `sub_0049CC50`, `sub_0049CC10`, `sub_004A13D0`, `sub_004A12B0`/
   `sub_004A1360` -- generische Assert-Log- und NV2A-GPU-Pushbuffer-Flush-
   Routinen, beruehren nicht unser Objekt.
4. **Unmapping-Timing korrigiert:** Der Speicherbereich wird nicht erst
   nach Minuten unmapped (wie aus alten Prozessen in Update 204/205
   abgeleitet), sondern praktisch sofort (Millisekunden bis wenige
   Sekunden) nach Erreichen des Fatal-Handlers.

**Repository:** `upgrade/selective-upstream`, HEAD weiterhin `79e6c19` als
letzter Code-Commit (nur Doku-Commits seither).

**Aktueller Blocker (unveraendert):** Ringpuffer-Unterobjekt (`this`, Vtable
`0x4F6940`, historisch beobachtete Adresse `0x1135928`) ist am Fatal-
Callback vollstaendig genullt (`~0x8070` Bytes ab dem Allocator-Boundary-
Tag). Der konkrete Schreiber ist weiterhin nicht gefunden, aber das
Zeitfenster fuer seine Aktivitaet ist jetzt auf unter 1 Sekunde eingegrenzt.

**Bewiesen (kumulativ, inkl. neu in Update 206):** Exakte Ausdehnung
`0x8070` Bytes (193); Schreiber muss Chunk-Zeiger nutzen (195); Free() nullt
nie Nutzdaten (195); nur 2 Threads beim Absturz (197); 4 Thread-Erzeugungen
insgesamt (198-199); Timer/APC-Mechanismus wird nie genutzt (204);
Virtual-Memory-Release-Pfad wird nie aufgerufen (204); **NEU: Das gesamte
Objekt-Leben (Erzeugen->Nullen->Fatal->Unmapping) dauert < 1 Sekunde und
faellt in ein praezise per Speicherwachstum (148.000-162.500 KB)
eingegrenztes Fenster (206); Unmapping geschieht praktisch sofort nach dem
Fatal-Handler, nicht erst nach Minuten (206); ein deterministischer Anker
(`xbox_KeDelayExecutionThread`-Aufruf) liegt unmittelbar VOR diesem Fenster
(206).**

**Widerlegt (kumulativ, inkl. neu in Update 206):** Beide Destruktorpfade
(190); Vtable-Familie (194); Allocator-Slots (195); Disk-Read-Kette (196);
Timer-Queue (204); Virtual-Memory-Release (204); `sub_0007DC43`,
`sub_0007DE25`, `sub_001CA0A5` (205); **NEU: `sub_0049CC50`, `sub_0049CC10`,
`sub_004A13D0`, `sub_004A12B0`/`sub_004A1360` als Schreiber -- generisches
Assert-Logging und NV2A-Pushbuffer-Flush, objektunabhaengig (206).**

**Offen (Stand nach Update 206):**
1. **Das exakte Speicherwachstums-Fenster `kb=148.000-162.500` wurde noch
   nie direkt getroffen** -- alle bisherigen Treffer lagen entweder klar
   davor (noch im Delay-Call) oder klar danach (schon fatal+unmapped). Das
   ist die konkrete naechste Zielluecke.
2. **`sub_0007F412` und `sub_00064B00`** an den in Update 205 gefundenen
   tiefen Host-Offsets -- weiterhin ungeklaert (Host-Binary-Disassemblierung
   noetig).
3. **Ein bisher nicht identifizierter Mechanismus innerhalb des Lipsync-
   Workers** (Update 204).
4. **Die alte 26.08.2026-Bugreport-Verbindung** (`sub_00056B22`).
5. Niedrigste Prioritaet: (a)/(d) aus Update 195, (b) aus Update 194.

**Naechster Test (Prioritaet):**
1. `tools/debug/detect_jump_then_attach.sh` mit feinerem Poll-Intervall
   (`0.05-0.08s` statt `0.15s`, Schwellenwert `kb>150000`) mehrfach (5-8x)
   wiederholen, um das Fenster `148.000-162.500 KB` direkt zu treffen.
2. Bei Treffer: `k 10` (Call-Chain) UND eine per `s -d`-Suche dynamisch
   gefundene (nicht fest angenommene) Objektadresse gemeinsam lesen.
3. Weiterhin: kein Dauerpolling, keine bedingten Breakpoints, kein erneutes
   Anfassen bereits ausgeschlossener Pfade (Timer/APC, VM-Release, Bink,
   Destructor-/Vtable-Pfade, die vier neuen Update-206-Funktionen) ohne neue
   Evidenz.
4. Sobald das Fenster direkt getroffen ist: gezielte Codeanalyse NUR fuer
   den dann sichtbaren Call-Chain-Ausschnitt, danach ggf. xemu-Vergleich.

**Relevante neue Artefakte:** `tools/debug/staggered_sample.sh`,
`tools/debug/detect_jump_then_attach.sh`, `tools/debug/stagger_t*.log`,
`tools/debug/jump_jump{1..6}*.log`, `tools/debug/clean_check_*`,
`tools/debug/append_update206.py`.

**Laufende Prozesse / IN PROGRESS:** Alle Testprozesse dieser Runde wurden
nach ihrer jeweils einzigen Probe beendet. Das explizit geschuetzte Paar
`cdb.exe` PID `25164` / `scda_recomp.exe` PID `27552` wurde in dieser Runde
NICHT beruehrt. Upstream-Stand unveraendert (`origin/main` bei `766ecef`,
Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
