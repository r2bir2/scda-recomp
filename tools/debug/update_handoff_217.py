# -*- coding: utf-8 -*-
status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG (seit Update 213, in Updates 214/215/216/217 bestaetigt)

**MODEL RECOMMENDATION:** Normale Arbeitsstufe. Naechster Schritt ist eine
einzelne, klar spezifizierte Speicher-Erfassung (ein zusaetzliches MEM32-Read
an einer bereits bekannten Stelle), kein offenes Reverse-Engineering mehr.

**Seit Update 216 -- Update 217 (diese Sitzung, DEN TATSAECHLICHEN AUFRUFER
VON sub_00040821 GEFUNDEN, SCHLEIFENMECHANISMUS VOLLSTAENDIG VERSTANDEN):**
Statt die drei Update-177-Kandidaten (`sub_0007DE25`/`sub_0007F412`/
`sub_00064B00`) blind weiterzulesen, wurde per Grep die tatsaechliche,
einzige Aufrufstelle von `sub_00040821` gefunden: `sub_00042F00`/
`sub_000430A5` (eine logische Funktion, an einem Sprungziel aufgeteilt).
`sub_0007DE25` wurde vollstaendig gelesen und als generische
Objekt-Abbauroutine ausgeschlossen (keine Terminator-Logik).

**Drei grosse neue Erkenntnisse:**
1. Das Ringpuffer-Objekt ist KEINE Heap-/Arena-Allokation, sondern per
   `alloca(0x40B0)` (16560 Bytes) direkt auf dem Stack-Frame der
   Loader-Funktion reserviert (`this = ebp - 16560`). "Vor dem Objekt"
   bedeutet hier den eigenen Stack-Frame der Funktion, keine groessere
   Heap-Arena.
2. Die tatsaechliche Lese-/Parse-Schleife (`loc_000431AE`..`loc_000431DA`)
   wurde vollstaendig gelesen: eine simple, bedingungslose bytegezaehlte
   Schleife (`while (MEM32(ebp-8) != 0)`), OHNE jeglichen Tag-/Terminator-
   Vergleich. Das bestaetigt Erklaerung 2 aus Update 216 konkret und macht
   die verbleibenden zwei Kandidatenfunktionen als Terminator-Pruefer
   unwahrscheinlich (sie liegen ausserhalb dieser Schleife).
3. `MEM32(ebp-8)` (das Gesamtbudget der Schleife) wird direkt aus den
   letzten 4 Bytes von `common.lin` gelesen (Seek zu EOF-4, echter
   4-Byte-Disk-Read) -- ein Datei-eigener Trailer-Wert. Daneben existiert
   ein ZWEITER, unabhaengiger Wert: die echte physische Dateigroesse via
   `sub_00051B8B`->`sub_00051B4D` (bestaetigter `NtQueryInformationFile`/
   `FileNetworkOpenInformation`-Wrapper, live abgefragt, kein Bug in
   `kernel_file.c` gefunden). Alle drei `common.lin`-Build-Output-Kopien
   (1.924.977 Bytes) stimmen exakt mit Retail ueberein -- Datei-Groessen-
   Diskrepanz als Ursache ausgeschlossen.

**Vereinheitlichte Hypothese (noch nicht empirisch bestaetigt):** da der
Trailer-Wert bytegenau identisch auf beiden Plattformen sein muss, aber
xemu nach dem identischen Kurzlesevorgang aufhoert und recomp nicht,
vermutet Update 217: xemus aeusserer Schleifenzaehler erreicht in derselben
Iteration null wie der Kurzlesevorgang (Schleife stoppt von selbst, bevor
der latente Cursor-Formel-Defekt aus Update 215 je erneut ausgeloest wird);
recomps Zaehler ist an dieser Stelle vermutlich noch ungleich null (ein
weiterer Refill-Aufruf folgt, der in den bereits gestoerten Zustand laeuft).
Das wuerde Erklaerung 2 und 3 aus Update 216 zu EINEM Mechanismus
vereinen, statt zu konkurrierenden Alternativen.

**Durchgefuehrte Housekeeping-Aktion:** keine in dieser Runde -- reine
Lese-/Analysearbeit, keine neue Instrumentierung hinzugefuegt oder entfernt.

**Repository:** `upgrade/selective-upstream`. Kein Fix committet -- wie
in Update 217 begruendet, die Hypothese ist noch nicht empirisch bestaetigt.

**Aktueller Blocker:** die vereinheitlichte Hypothese aus Update 217 ist
plausibel, aber unbestaetigt. Der naechste Schritt ist eine einzelne neue
One-Shot-Erfassung von `MEM32(objekt_this + 0x40B0 - 8)` am bereits
bekannten Korruptions-memcpy-Erfassungspunkt in `sub_00040821`, um zu pruefen,
ob der aeussere Schleifenzaehler zu diesem Zeitpunkt schon null ist oder
nicht.

**Bewiesen (kumulativ, inkl. neu in Update 217):** Alle Eintraege aus
Updates 109-216 unveraendert gueltig; **NEU: der tatsaechliche Aufrufer und
die Lese-/Parse-Schleife von sub_00040821 sind vollstaendig identifiziert
und gelesen (sub_00042F00/sub_000430A5); das Ringpuffer-Objekt ist
stack-alloziert (alloca 0x40B0), keine Heap-Arena; die Schleife hat
NACHWEISLICH keine Terminator-/Tag-Pruefung, sondern zaehlt einen aus der
Datei selbst (letzte 4 Bytes) gelesenen Bytewert herunter; eine zweite,
unabhaengige, live abgefragte physische Dateigroesse existiert daneben und
ist als Fehlerquelle ausgeschlossen (alle Build-Outputs matchen Retail
exakt) (217).**

**Offen (Stand nach Update 217), priorisiert fuer STRONG:**
1. Eine neue, kleine One-Shot-Erfassung von `MEM32(objekt_this + 0x40B0 - 8)`
   am bestehenden Korruptions-Erfassungspunkt in `sub_00040821` hinzufuegen,
   Lauf durchfuehren, Wert lesen (null oder ungleich null?).
2. Bei Bestaetigung der vereinheitlichten Hypothese: die Herkunft der
   Byte-Diskrepanz pro Iteration zwischen recomp und xemu weiter eingrenzen
   (warum verbraucht recomp pro Zyklus effektiv andere Byteanzahl als
   xemu, obwohl derselbe Code laeuft?).
3. Danach erst Fixstrategie waehlen (siehe Update 216 fuer die drei
   urspruenglichen Kandidaten, jetzt praeziser als EIN vereinheitlichter
   Mechanismus verstanden).
4. Restliche niedrigprioritaere Punkte aus frueheren Updates unveraendert.

**Naechster Test (Prioritaet, fuer STRONG):** siehe Update 217, letzter
Abschnitt "Noch nicht erledigt". Kurz: EINEN zusaetzlichen MEM32-Read an
der bekannten Korruptionsstelle ergaenzen (`this + 0x40B0 - 8`), Lauf,
Wert vergleichen mit "sollte null sein, wenn Schleife korrekt beendet
haette".

**Instrumentierung:** Aktuell KEINE aktiv (voller Revert bereits in Update
216 erfolgt). Fuer den naechsten Test wird eine neue, minimale, gezielt
dafuer passende One-Shot-Erfassung noetig sein.

**Relevante neue Artefakte:** `tools/debug/append_update217.py`,
`tools/debug/append_update217_status.py`, `tools/debug/update_handoff_217.py`
(alle reine Doku-Skripte, keine Instrumentierung).

**Laufende Prozesse / IN PROGRESS:** Keine Testprozesse laufen. Upstream-Stand
unveraendert (`origin/main` bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
