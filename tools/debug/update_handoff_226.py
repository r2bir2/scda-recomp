# -*- coding: utf-8 -*-
status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG. Naechster Schritt (Allokations-Stelle der
Ziel-Oberflaeche finden, ggf. xemu-Vergleich) ist weiterhin begrenzte
Nachverfolgungsarbeit in bereits bekanntem Code-Gebiet.

**Seit Update 225 -- Update 226 (diese Sitzung, KORREKTUR):** Update 225s
"Nullpitch"-Diagnose war ein Fehler in der eigenen Instrumentierung
(falsches Struct-Feld gelesen: `+0x104C` statt `+0x1048`). Korrigierte,
zeilenweise Erfassung zeigt: die Zieladresse schreitet ueber mindestens
115 Zeilen hinweg PERFEKT um 0xA00 (2560 Bytes) pro Zeile fort -- eine
vollstaendig gesunde, korrekt begrenzte ~640x480-Oberflaechenkopie
(Zielspanne `[0xF0464000, 0xF0590000)`, ca. 1,2 MB). **Kein
Kopierschleifen-Bug.** `0x004AB668` liegt einfach INNERHALB des normalen,
beabsichtigten Zielbereichs dieser Oberflaeche, nicht an einer durch
Overflow erreichten Grenze.

**Neue, praezisierte Frage:** woher bekommt diese Ziel-Oberflaeche ihre
physische/Tiled-Basisadresse (`dst_desc+0x18 = 0xF0464000`)? Zwei
Moeglichkeiten: (1) ein Platzierungs-Bug in dieser Rekompilierung's
Oberflaechen-/Speicher-Allokator (wahrscheinlicher, da die
Original-XBE-Bytes `0x4AB668`/`0x4AB670` fuer den Device-Pointer bereits
byte-verifiziert sind -- ein Original-Linker-Platzierungsfehler ist
unwahrscheinlich); (2) eine Diskrepanz spezifisch in der
Global-Daten-Platzierung dieser Rekompilierung.

**Repository:** `upgrade/selective-upstream`. Zweite Runde temporaerer,
vollstaendig zurueckgesetzter Instrumentierung in der gitignoreten
`recomp_0027.c` diese Runde; keine getrackten Dateien geaendert (nur
Dokumentation).

**Bewiesen (kumulativ, inkl. neu in Update 226):** Alle Eintraege aus
Updates 109-225 gueltig, MIT AUSNAHME von Update 225s "Nullpitch"-
Interpretation, die hiermit korrigiert wird: die Kopie selbst ist
fehlerfrei; die Ursache liegt in der Herkunft der Zielbasisadresse.

**Offen (Stand nach Update 226), priorisiert:**
1. `dst_desc+0x18`s Ursprung finden -- welcher Allokations-/Textur-
   Erstellungs-Aufruf setzt diesen Wert, mit welchen Eingaben?
2. Wenn gefunden: per Capstone/xemu vergleichen, ob echte Hardware
   dieselbe physische Adresse fuer diese Oberflaeche berechnet.
3. Semantisch korrekten Fix bestimmen (Allokator-Bug vs.
   Global-Platzierungs-Diskrepanz) und erst dann implementieren.
4. Den separat bestehenden "manche Aufrufe der Zeilenkopie schreiten nie
   fort"-Defekt (Update 224, andere Aufrufe derselben Funktion) fuer
   spaetere Nachverfolgung vermerkt.
5. Restliche niedrigprioritaere Punkte aus frueheren Updates unveraendert.

**Instrumentierung:** Keine aktiv (zweimal vollstaendig zurueckgesetzt
und rueckgebaut bestaetigt).

**Relevante neue Artefakte:** `tools/debug/append_update226.py`,
`tools/debug/append_update226_status.py`, `tools/debug/update_handoff_226.py`
(reine Doku-Skripte).

**Laufende Prozesse / IN PROGRESS:** Keine Testprozesse laufen. Upstream-
Stand unveraendert (`origin/main` bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
