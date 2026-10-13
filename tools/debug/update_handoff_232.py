# -*- coding: utf-8 -*-
status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG war fuer die gesamte Update-232-Bisektion und den
Fix ausreichend. Keine Eskalation auf MAXIMUM noetig -- die Bisektion
lieferte an jedem Schritt einen eindeutigen, live gemessenen Uebergang,
nie einen Widerspruch zwischen xemu und Recomp oder mehrere verbleibende
Erklaerungen.

**Update 232 -- GROSSER MEILENSTEIN:** Der Menu-Tree-Node-Pointer-Bug aus
Updates 230/231 (`ebx=0x00390037`) ist vollstaendig ursachenbehoben,
nicht nur workaroundet. Die Ursache war ein Codegenerator-Bug in
`_fixup_icall_esp_save()` (`tools/recomp/translator.py`): bei
`sub_0026940F`s allererstem indirekten Aufruf wurde der
`_icall_esp`-Ruecksetzpunkt vor statt nach den einmaligen
`esi`/`edi`-Registersicherungen erfasst, weil (a) die FPO-Prolog-
Erkennung zwei fuehrende Setup-Zeilen (einen vtable-Fetch) nicht
ueberspringt und (b) die Push/Pop-Zaehl-Heuristik `esi`/`edi` nicht als
Sicherung erkennt, da beide separat als wiederholtes Konstanten-Argument
in 9 weiteren Aufrufen derselben Funktion wiederverwendet werden. Da das
Call-Ziel dieses einen Aufrufs (`0x000460C5`, mitten in einer anderen
Funktion `sub_000460BB` liegend, kein registrierter Funktionseinstieg)
nicht aufloesbar ist, loest `RECOMP_ICALL_SAFE` seinen -- an sich
korrekten -- Fallback aus (`g_esp = saved_esp`), verwirft dabei aber
faelschlich die 8 Byte der `esi`/`edi`-Sicherungen dauerhaft, wodurch der
Rest von `sub_0026940F`s und danach `sub_001D8842`s eigener Stack-Zeiger
um 8 Byte verschoben bleibt -- deren jeweilige Epiloge lesen daraufhin
Datenmuell statt der echten gesicherten Register, was sich als
`ebx=0x00390037` viele Aufrufe spaeter in `sub_00062237` manifestiert.

**Fix:** `_fixup_icall_esp_save()` ueberspringt jetzt fuehrende reine
Setup-Zeilen (keine `esp`/`PUSH32`/`POP32`/`RECOMP_`/Kontrollfluss-
Wirkung) vor der FPO-Sicherungs-Erkennung. Vierter Regressionstest
`test_fpo_save_after_vtable_fetch_setup` hinzugefuegt (alle 4 Tests in
`tools/recomp/test_icall_esp_save.py` bestehen). Vollstaendige
Pipeline-Regenerierung (28159/28162 Funktionen, 0 fehlgeschlagen,
identischer Umfang) und sauberer Rebuild durchgefuehrt und verifiziert.

**Verifiziert per Live-Retest (~90s, 179.000+ Kernel-Calls):**
`0x00390037` kommt im gesamten `scda_runtime.log` kein einziges Mal mehr
vor; die alte Null-Vtable-Call-Signatur (`source=0x0005F701,
target=0x00000000`) ist weg. Ein neuer, ANDERER Steady-State (Kernel-
Aufruf-Zyklus ueber Ordinale 99/234/246/277/294) wird erreicht -- das ist
der naechste, noch unerforschte Blocker.

**Contig-Alias-Fix aus Update 229 bleibt bestaetigt und abgeschlossen**
-- vollstaendig unabhaengig von Update 232 (andere Adressklasse, anderer
Mechanismus, andere Codeschicht: Laufzeit-Speicherlayout vs.
Codegenerator).

**Repository:** `upgrade/selective-upstream`. Geaenderte GETRACKTE
Dateien diese Runde: `tools/recomp/translator.py` (der Fix),
`tools/recomp/test_icall_esp_save.py` (neuer Test). Keine Hand-Edits an
generierten `.c`-Dateien ueberleben -- alle temporaere Instrumentierung
wurde durch die finale vollstaendige Pipeline-Regenerierung ersetzt;
`git status`/`git diff` zeigen fuer die generierten Dateien keine
getrackten Aenderungen (gitignored).

**Bewiesen (kumulativ, inkl. neu in Update 232):** Alle Eintraege aus
Updates 109-231 gueltig, mit einer Praezisierung: der in Updates 230/231
gefundene Bug ist JETZT vollstaendig erklaert und behoben, nicht mehr nur
lokalisiert.

**Offen (Stand nach Update 232), priorisiert:**
1. Neuer Steady-State nach dem Fix: ein sich wiederholender Kernel-Call-
   Zyklus (Ordinale 99, 234, 246, 277, 294) -- noch nicht untersucht, ob
   echter neuer Blocker oder erwartetes Verhalten (z.B. regulaeres
   Input-/Audio-/Timer-Polling). Naechster konkreter Schritt: identifizieren,
   was diese Ordinale bedeuten und ob/wie die Schleife normalerweise
   endet (gleiche Methodik wie Update 230's Vorgehen fuer den
   Bink-Worker-Verdacht: erst pruefen ob Testumgebung vollstaendig ist,
   dann ggf. xemu-Ground-Truth-Vergleich).
2. 10 verbleibende benigne-aussehende `source=0x0005F701`-Treffer mit
   echten, nicht-null Zielen -- niedrige Prioritaet, kein bekannter
   Registererhaltungs-Schaden, nicht weiter verfolgt.
3. Restliche niedrigprioritaere Punkte aus frueheren Updates unveraendert.

**Instrumentierung:** Keine aktiv (vollstaendig zurueckgesetzt durch
komplette Pipeline-Regenerierung, bestaetigt).

**Relevante neue Artefakte:** `tools/debug/append_update232.py`,
`tools/debug/append_update232_status.py`, `tools/debug/update_handoff_232.py`
(Doku-Skripte); `tools/recomp/translator.py` und
`tools/recomp/test_icall_esp_save.py` (der eigentliche Fix + Test,
GETRACKT, nicht Doku).

**Laufende Prozesse / IN PROGRESS:** Kein scda_recomp.exe-Testprozess
aktiv (nach Verifikationslauf sauber beendet). xemu-Status vom Nutzer
zuletzt unveraendert. Upstream-Stand unveraendert (`origin/main` bei
`766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
