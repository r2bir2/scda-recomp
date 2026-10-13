# -*- coding: utf-8 -*-
status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG fuer die Fortsetzung der mechanischen
Rueckwaerts-Verfolgung. **Scoping-Frage an den Nutzer faellig:** die
Spur reicht jetzt in das Textur-/Speicher-Allokations-Subsystem hinein,
mit unbekannter verbleibender Tiefe.

**Seit Update 226 -- Update 227 (diese Sitzung):** Die Ziel-Oberflaeche
der korrumpierenden Kopie wurde bis zu ihrem Konstruktor
(`sub_004B1377`, nur per Vtable aufgerufen) und einer 19-Eintraege-
Formatcode-Dispatch-Tabelle (`sub_004B38F7`) zurueckverfolgt. Bestaetigt:
`0xF0464000` (die Basisadresse) wird NICHT hier berechnet -- sie kommt
als fertiger Wert aus einer "Oberflaechen-Info"-Struktur, die von
`sub_004AE0A7`s Aufrufer geliefert wird (drei Ebenen tiefer als Update
226). **Bestaetigt per Log-Check: `AvSetDisplayMode` wurde in diesem
Lauf noch gar nicht erreicht** -- die Ziel-Oberflaeche ist also NICHT der
primaere Backbuffer, sondern eine generische Pixelformat-Konvertierung
zwischen zwei bereits existierenden Texturen/Render-Targets.

**Die eigentliche Allokation** von `0xF0464000` als physische/Tiled-
Basisadresse dieser Ziel-Textur liegt noch weiter oben in der Kette,
nicht erreicht. Die Technik (Live-One-Shot-Instrumentierung + `cdb`-
Stack-Trace) funktioniert zuverlaessig und mechanisch, aber die Anzahl
verbleibender Ebenen bis zur eigentlichen Textur-/Speicher-Erstellung
ist unbekannt.

**Empfehlung:** mit dem Nutzer klaeren, ob (a) der mechanische
Rueckwaerts-Trace fortgesetzt werden soll, bis die Allokationsstelle
gefunden ist (unbekannte Anzahl weiterer Runden), oder (b) ein direkterer
Ansatz gewaehlt wird -- z.B. den Contiguous-/Tiled-Memory-Allokator
dieses Projekts direkt darauf pruefen, ob er irgendwo den
Adressbereich um `0x4AB668` (das Device-Pointer-Global) reserviert/
ausschliesst, unabhaengig davon, welche konkrete Textur ihn zuerst trifft.

**Repository:** `upgrade/selective-upstream`. Zwei weitere Runden
temporaerer, vollstaendig zurueckgesetzter Instrumentierung in der
gitignoreten `recomp_0027.c` diese Runde; keine getrackten Dateien
geaendert (nur Dokumentation).

**Bewiesen (kumulativ, inkl. neu in Update 227):** Alle Eintraege aus
Updates 109-226 gueltig. NEU: die Ziel-Oberflaeche ist KEIN Backbuffer
(AvSetDisplayMode noch nicht erreicht); ihre Basisadresse wird als
Parameter durch mindestens 10 Funktionsebenen unveraendert durchgereicht,
nicht berechnet.

**Offen (Stand nach Update 227), priorisiert:**
1. Scoping-Entscheidung: Rueckwaerts-Trace fortsetzen vs. direkter
   Allokator-Audit (siehe Empfehlung oben).
2. Je nach Entscheidung: Allokationsstelle finden und gegen xemu/x86
   vergleichen, ODER Contiguous-/Tiled-Memory-Allokator direkt
   auditieren.
3. Semantisch korrekten Fix bestimmen und erst dann implementieren.
4. Restliche niedrigprioritaere Punkte aus frueheren Updates unveraendert.

**Instrumentierung:** Keine aktiv (dreimal vollstaendig zurueckgesetzt
und rueckgebaut bestaetigt ueber Updates 225-227).

**Relevante neue Artefakte:** `tools/debug/append_update227.py`,
`tools/debug/append_update227_status.py`, `tools/debug/update_handoff_227.py`
(reine Doku-Skripte).

**Laufende Prozesse / IN PROGRESS:** Keine Testprozesse laufen. Upstream-
Stand unveraendert (`origin/main` bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
