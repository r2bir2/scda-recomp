# -*- coding: utf-8 -*-
status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG. Der konkrete Writer ist gefunden; der naechste
Schritt (Herkunft der Zieladresse zurueckverfolgen, x86 vs. generiert
vergleichen) ist klassische, begrenzte Faithful-Translation-Arbeit --
genau das, was dieses Projekt schon oft mit STRONG geloest hat.

**Seit Update 224 -- Update 225 (diese Sitzung, WRITER GEFUNDEN):** Nach
zwei zurueckgezogenen falschen Spuren (RAM-Mirror-Aliasing trifft nicht
direkt zu, da `XBOX_PTR` kein Wraparound macht; eine vermeintliche
Stack-Pointer-Korruption war ein `cdb .frame`-Methodik-Artefakt, per `dv`
widerlegt) wurde per einer Runde echter, minimaler, One-Shot-
Quellcode-Instrumentierung (laut eigener Nutzer-Genehmigung als Plan-B-
Eskalation) der konkrete Schreibzugriff gefangen:

`sub_004AED5D`s Zeilenkopier-Schleife kopiert 2560 Bytes von Tiled-
Aperture-Guest-Adresse `0xF0253400` nach `0xF04AB400` (physisch
`[0x004AB400, 0x004ABE00)`). **Der Device-Pointer bei `0x004AB668` liegt
exakt in diesem Zielbereich (Offset 0x268 hinein).** Das erklaert auch
restlos, warum der Update-224-Hardware-Watchpoint nie ausloeste: der
Schreibzugriff geht ueber die separate Host-VA der Tiled-Aperture-View
(`g_tiled_view`), nicht ueber die Basis-RAM-View, auf die der Watchpoint
scharf war -- beide aliasen dieselben physischen Bytes ueber dieselbe
Datei-Mapping-Section, aber ein CPU-Daten-Breakpoint vergleicht lineare
Adressen, nicht physische Seiten.

**Der korrumpierte Wert `0x7400087D` ist keine "Muell"-Daten** -- es sind
echte Pixel-/Oberflaechen-Bytes aus der Quelle
`[0x00253400, 0x00253E00)`, die an den Device-Pointer-Offset geschrieben
wurden.

**Noch nicht getan (naechster Schritt, laut eigener Nutzer-Anweisung):**
zurueckverfolgen, woher die Zieladresse `edi=0xF04AB400` und die Bytezahl
`ebx=0xA00` tatsaechlich kommen (welcher Oberflaechen-/Textur-Deskriptor
`sub_004AED5D` diese Werte liefert), und per Capstone gegen
`default.xbe` pruefen, ob die Basisadresse/Pitch-Berechnung dieses
Deskriptors korrekt uebersetzt ist oder ob dort der eigentliche Bug (ein
Overrun) sitzt.

**Repository:** `upgrade/selective-upstream`. Ein temporaerer,
vollstaendig zurueckgesetzter Instrumentierungs-Durchlauf in der
gitignoreten `recomp_0027.c` diese Runde; keine getrackten Dateien
geaendert (nur Dokumentation).

**Bewiesen (kumulativ, inkl. neu in Update 225):** Alle Eintraege aus
Updates 109-224 unveraendert gueltig, mit einer Praezisierung: der
korrumpierende Schreibzugriff ist jetzt namentlich identifiziert (der
Dword-`memcpy` in `sub_004AED5D`s Zeilenkopier-Schleife, ueber die Tiled
Aperture), nicht mehr nur eingegrenzt.

**Offen (Stand nach Update 225), priorisiert:**
1. Den Ursprung von `edi`/`ebx` (Ziel-Oberflaechen-Deskriptor) bis zu
   seiner Quelle zurueckverfolgen.
2. Per Capstone gegen `default.xbe` pruefen: ist die Basisadress-/
   Pitch-Berechnung dieses Deskriptors in der Rekompilierung korrekt
   uebersetzt?
3. Semantisch korrekten Fix bestimmen (Overrun in der Deskriptor-
   Berechnung vs. Platzierungs-Diskrepanz des Device-Pointer-Globals) und
   erst dann implementieren -- kein Clamp, kein Workaround.
4. Den separat gefundenen "Zeilenkopier-Schleife schreitet manchmal nie
   fort"-Defekt (andere Aufrufe derselben Funktion, Pitch liest sich als
   Null) fuer spaetere Nachverfolgung vermerkt.
5. Restliche niedrigprioritaere Punkte aus frueheren Updates unveraendert.

**Instrumentierung:** Keine aktiv (vollstaendig zurueckgesetzt und
rueckgebaut bestaetigt).

**Relevante neue Artefakte:** `tools/debug/append_update225.py`,
`tools/debug/append_update225_status.py`, `tools/debug/update_handoff_225.py`
(reine Doku-Skripte).

**Laufende Prozesse / IN PROGRESS:** Keine Testprozesse laufen. Upstream-
Stand unveraendert (`origin/main` bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
