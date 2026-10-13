# -*- coding: utf-8 -*-
status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG. Dies ist wieder eine begrenzte Root-Cause-Suche
(Speicherkorruption), keine neue Subsystem-Frage -- Update 222s
gegenteilige Einschaetzung ist zurueckgezogen (siehe Update 223).

**Seit Update 222 -- Update 223 (diese Sitzung, KORREKTUR):** Update 222s
"kein Video-Backend verdrahtet"-Diagnose war unvollstaendig. Eine fruehere
Sitzung (siehe `projects/scda/CMakeLists.txt`s eigene Kommentare
2026-09-04/11, von Update 222 nicht gepueft) hatte bereits per xemu
bewiesen: echte Hardware hat an `sub_004A12B0` einen gueltigen
Geraete-Zeiger und das bestehende `NV2A_ACK[]`/`nv2a_ack_thread`-System
(inkl. exakt des Registers `0x100410`) beantwortet den Poll bereits
korrekt. Live-Breakpoints diese Runde bestaetigen: `sub_0049D210`
(Geraeteerstellung) laeuft erfolgreich und schreibt den korrekten Zeiger
`0x004AB670`; ein direkt folgender `sub_004A12B0`-Aufruf sieht diesen
korrekten Wert. Ein SPAETERER `sub_004A12B0`-Aufruf (anderer Call-Pfad:
`sub_0049B0C0 -> sub_004A1740 -> sub_004A1600`) sieht stattdessen Muell
(`0x7400087D`). **Zwischen diesen beiden Aufrufen wird `0x4AB668`
ueberschrieben -- ein echter Speicherkorruptions-Bug, kein fehlendes
Backend.** Ein erster Versuch, den Schreibzugriff per Hardware-Watchpoint
zu fangen, war nicht erfolgreich (Breakpoint funktioniert nachweislich,
aber der zweite Schreibzugriff wurde in zwei Beobachtungsfenstern nicht
gefangen -- braucht einen zweiten Versuch mit anderer Methode).

**Explizit NICHT empfohlen:** ein "Video-Backend-Stub", der `0x4AB668` auf
einen gefaelschten Wert zwingt. Das wurde in der Vorgeschichte dieser
Codebasis fuer genau diese Adresse bereits verworfen -- es wuerde den
echten Bug verdecken.

**Aktueller Blocker:** unbekannter Schreibzugriff, der einen gueltigen
D3D-Geraete-Zeiger bei `0x4AB668` zwischen zwei `sub_004A12B0`-Aufrufen
mit Muell ueberschreibt. Naechster Schritt: die Quelle dieses
Schreibzugriffs finden (z.B. breiterer Hardware-Watchpoint, Multi-Thread-
Synchronisation der Breakpoints pruefen, oder statische Suche nach
Code, der ueber einen falsch berechneten Zeiger in diese Adresse
schreiben koennte).

**Repository:** `upgrade/selective-upstream`. Nur Dokumentation und
Live-Debugging diese Runde (Update 223), kein Code geaendert.

**Bewiesen (kumulativ, inkl. neu in Update 223):** Alle Eintraege aus
Updates 109-221 unveraendert gueltig. Update 222s Diagnose ("fehlendes
Video-Backend") ist WIDERLEGT und durch Update 223 ersetzt: die
Geraeteerstellung und die NV2A-Bestaetigung funktionieren beide bereits;
das Problem ist eine Speicherkorruption von `0x4AB668` nach erfolgreicher
Geraeteerstellung.

**Offen (Stand nach Update 223), priorisiert:**
1. Den Schreibzugriff finden, der `0x4AB668` zwischen den beiden
   `sub_004A12B0`-Aufrufen korrumpiert (Root-Cause-Suche, wie alle
   bisherigen Bugs dieser Sitzung -- kein Scoping-Gespraech noetig, das
   ist regulaere Bug-Arbeit).
2. Restliche niedrigprioritaere Punkte aus frueheren Updates unveraendert.

**Instrumentierung:** Keine aktiv. Zwei cdb-Kommandoskripte im Scratchpad
(`cdb_devcreate_check.txt`, `cdb_write_watch_4ab668.txt`) fuer die naechste
Runde wiederverwendbar.

**Relevante neue Artefakte:** `tools/debug/append_update223.py`,
`tools/debug/append_update223_status.py`, `tools/debug/update_handoff_223.py`
(reine Doku-Skripte); `tools/debug/devcreate_check_out.log`,
`tools/debug/write_watch_4ab668_out.log` (Live-cdb-Logs, zur Referenz
behalten).

**Laufende Prozesse / IN PROGRESS:** Keine Testprozesse laufen. Upstream-
Stand unveraendert (`origin/main` bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
