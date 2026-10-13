status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG (unveraendert seit Update 205)

**MODEL RECOMMENDATION:** In Codex: starke, aber nicht die hoechste Analyse-
Stufe. In Claude: starkes Modell, hohe aber nicht maximale Reasoning-Tiefe.

**REASON:** Updates 206-208 haben das GOOD/ZERO-Fenster eng eingeklammert,
den bisher als "Fatal-Handler" missverstandenen `sub_0003FCA5` vollstaendig
als lokalisierten Disc-Fehler-Dialog identifiziert, und alle drei
Aufrufstellen dieses Dialogs bis zu einer konkreten, noch ungelesenen
Decoder-Funktion (`sub_00038A43`) zurueckverfolgt. Anspruchsvolle, aber mit
strukturiertem Vorgehen loesbare Arbeit -- STRONG bleibt angemessen. Bei
einem direkten Treffer in `sub_00038A43` (z.B. eine Groessenberechnung nahe
`0x8070`) MAXIMUM in Erwaegung ziehen.

**Seit Update 205 -- Zusammenfassung Updates 206-208 (diese Sitzung):**
- **206:** Neue Messmethode (gestaffelter Single-Attach + debuggerfreie
  `tasklist`-Sprungerkennung) klammert das gesamte Objekt-Leben (Erzeugen->
  Nullen->Fatal->Unmapping) auf ein Speicherwachstums-Fenster von ca.
  148.000-162.500 KB ein (geschaetzt < 1 Sekunde Echtzeit). Vier neue
  Funktionen im Fatal-Handler-Cleanup-Pfad gefunden und als generisches
  Assert-Logging/NV2A-Pushbuffer-Flush ausgeschlossen. Unmapping-Timing
  korrigiert: geschieht praktisch sofort nach Fatal, nicht erst nach
  Minuten.
- **207:** `sub_0003FCA5` vollstaendig gelesen und identifiziert als der
  STANDARD, LOKALISIERTE Xbox-"Disc-Fehler"-Dialog (5 Sprachvarianten
  derselben Meldung "CD verschmutzt/beschaedigt", per Capstone/UTF-16LE aus
  der XBE gelesen) -- kein generischer Crash-Handler. Bestaetigt: der
  Dialog ist ein Downstream-Symptom eines fehlgeschlagenen Lesevorgangs,
  nicht die Ursache.
- **208:** Alle drei Aufrufstellen des Dialogs identifiziert (per
  Guest-Adressbereichs-Abgleich, NICHT per cdb-Symbolnamen -- Host-Offset-
  Falle aus Update 205 erneut bestaetigt) und geprueft: zwei sind duenne
  Wrapper/Cleanup-Funktionen ohne Bulk-Zero. Die dritte fuehrt zu
  `sub_00038A43` (402 Bytes, Kategorie "game_video", NOCH NICHT
  vollstaendig gelesen) -- der Statuscode-4-Produzent, operiert auf einem
  kleineren (~0x100+ Byte) Decoder-/Reader-Objekt.

**Repository:** `upgrade/selective-upstream`, HEAD weiterhin `79e6c19` als
letzter Code-Commit (nur Doku-Commits seither).

**Aktueller Blocker (unveraendert):** Ringpuffer-Unterobjekt (`this`,
Vtable `0x4F6940`, historisch beobachtete Adresse `0x1135928`) ist am
Fatal-Callback vollstaendig genullt (`~0x8070` Bytes ab dem Allocator-
Boundary-Tag). Der konkrete Schreiber ist weiterhin nicht gefunden.

**Bewiesen (kumulativ, inkl. neu):** Exakte Ausdehnung `0x8070` Bytes (193);
Schreiber muss Chunk-Zeiger nutzen (195); nur 2 Threads beim Absturz (197);
Timer/APC nie genutzt (204); VM-Release nie aufgerufen (204); Objekt-Leben
< 1 Sekunde, eingegrenzt auf 148.000-162.500 KB Speicherwachstum (206);
Unmapping praktisch sofort nach Fatal (206); **NEU: `sub_0003FCA5` ist der
Disc-Fehler-Dialog, kein Crash-Handler (207); alle drei Dialog-
Aufrufstellen sind reine Fehlerbehandlung ohne Bulk-Zero (208).**

**Widerlegt (kumulativ, inkl. neu):** Beide Destruktorpfade (190);
Vtable-Familie (194); Allocator-Slots (195); Disk-Read-Kette (196);
Timer-Queue (204); Virtual-Memory-Release (204); `sub_0007DC43`,
`sub_0007DE25`, `sub_001CA0A5` (205); vier Fatal-Cleanup-Funktionen (206);
**NEU: `sub_0003FCA5` selbst + gesamte interne Kette (207); `sub_000356F1`,
`sub_00038BD5`/`sub_00038F96` (die drei Dialog-Aufrufer), `sub_000368A5`
(Handle-Cleanup) (208).**

**Offen (Stand nach Update 208), priorisiert:**
1. **`sub_00038A43` vollstaendig lesen** (402 Bytes/134 Insn, Kategorie
   game_video, NICHT unser Ringpuffer direkt, aber der Statuscode-
   Produzent) -- konkreter naechster Schritt, siehe Update 208.
2. `sub_00052A9C`, `sub_00052A2F`, `sub_00036541` (von sub_00038A43
   aufgerufen, ungeprueft).
3. Das exakte Speicherwachstums-Fenster `148.000-162.500 KB` noch nie
   direkt getroffen (Update 206) -- `detect_jump_then_attach.sh` mit noch
   feinerem Intervall erneut versuchen, falls (1)/(2) nichts liefern.
4. `sub_0007F412`/`sub_00064B00` an tiefen Host-Offsets -- weiterhin
   ungeklaert (Host-Binary-Disassemblierung noetig).
5. Lipsync-Worker-Restrisiko (Update 204); 26.08.2026-Bugreport
   (`sub_00056B22`); niedrigste Prioritaet: (a)/(d) aus 195, (b) aus 194.

**Naechster Test (Prioritaet):**
1. `sub_00038A43` (recomp_0001.c, Zeile ~35411) vollstaendig lesen --
   insbesondere `esi+0xFC`-Substruktur (moeglicher "Reader/Parent") und
   alle Groessen-/Zeigerberechnungen.
2. Falls dort kein Treffer: die drei Unterfunktionen kurz gegenlesen.
3. Falls weiterhin nichts: `detect_jump_then_attach.sh` mit `sleep 0` (nur
   tasklist-Eigenlatenz als Takt) und Schwelle `kb>150000` mehrfach
   wiederholen, um das 148-162K-Fenster direkt zu treffen.
4. Weiterhin: kein Dauerpolling, keine bedingten Breakpoints, kein
   erneutes Anfassen bereits ausgeschlossener Pfade ohne neue Evidenz.

**Relevante neue Artefakte:** `tools/debug/append_update20{6,7,8}.py`,
`tools/debug/update_handoff_20{6,8}.py`, `tools/debug/staggered_sample.sh`,
`tools/debug/detect_jump_then_attach.sh`, `tools/debug/jump_jump{1..7}*.log`,
`tools/debug/stagger_t*.log`.

**Laufende Prozesse / IN PROGRESS:** Alle Testprozesse dieser Sitzung
wurden beendet. Das explizit geschuetzte Paar `cdb.exe` PID `25164` /
`scda_recomp.exe` PID `27552` wurde NICHT beruehrt (ein einzelner,
lesender Sanity-Check in Update 205 ausgenommen). Upstream-Stand
unveraendert (`origin/main` bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
