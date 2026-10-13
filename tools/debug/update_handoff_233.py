# -*- coding: utf-8 -*-
status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG war fuer die gesamte Update-233-Charakterisierung
ausreichend. Keine Eskalation auf MAXIMUM noetig -- jeder Schritt lieferte
eine eindeutige Live- oder Code-Evidenz (Aufrufzaehler, Handle-Aufloesung,
Argumentzahl-Abgleich gegen g_slot_arg_bytes), nie einen Widerspruch oder
mehrere gleichwertig plausible Erklaerungen.

**Update 233:** Der in Update 232 als naechstes offenes Ziel markierte
neue Kernel-Call-Steady-State (Ordinale 99/234/246/277/294) ist jetzt
charakterisiert, nicht mehr unbekannt. Kern-Befund: das Enter/Leave-
CriticalSection-Rauschen um Xbox-VA `0x6B27A0` (hoechstes Aufrufvolumen)
ist eine VORBESTEHENDE, bereits am 2026-09-11 dokumentierte Eigenschaft
(fehlendes Vsync-Pacing, siehe `RECOMP_KERNEL_HIST`-Kommentar in
`kernel_bridge.c`) und laeuft seit fruehester Bootzeit, nicht erst ab dem
Uebergang -- kein neuer Bug. Das eigentliche neue Signal war Ordinal 250
(`ObfDereferenceObject`), das im Steady-State unbegrenzt waechst: seine
Bridge existierte bereits vollstaendig implementiert, war aber (exakt wie
beim bereits gefixten `NtSuspendThread`/`NtResumeThread`-Paar direkt
daneben) nie in den Dispatch-Switch eingebunden. Gefixt (siehe unten);
die echte Implementierung ist selbst ein dokumentiertes No-Op, daher ohne
Wirkung auf den Steady-State, aber ein echter, sicherer Korrektheits-Fix.

**Eigentliche Ursache des Steady-State (charakterisiert, noch nicht
behoben):** Einer von zwei Pool-Worker-Threads (`PsCreateSystemThreadEx`,
Routine `sub_004BBE90`, zweimal mit unterschiedlichen Task-Kontexten
gestartet) haengt dauerhaft in einer regulaeren, lehrbuchmaessig korrekten
Alertable-Wait-Retry-Schleife (`sub_00052D2E` -> `sub_00052C18`, Ordinal
234 `NtWaitForSingleObjectEx`, Retry bei `STATUS_ALERTED`=0x101) fest, weil
das zugrundeliegende ECHTE Win32-Handle (via `WaitForSingleObjectEx`)
nie signalisiert wird. Der Hauptthread pollt korrekt und erfolgreich
(`ObReferenceObjectByHandle` loest das getaggte Handle stabil auf ein
gueltiges Fake-KTHREAD auf -- kein Wiederauftreten des bereits gefixten
Update-169/170-Null-Bugs), sieht aber nie "fertig", weil der Worker
tatsaechlich nicht fertig ist. Kein ABI-/Argumentzahl-Fehler gefunden (4
Argumente, 16 Byte, passt zu `g_slot_arg_bytes[234]`) -- anders als beim
bereits gefixten Schwester-Bug in `NtWaitForMultipleObjectsEx`
(4-vs-6-Argumente), der direkt geprueft und explizit ausgeschlossen wurde.

**Strukturelle Naehe zum geschlossenen Bink-Pfad, bewusst nicht
wiedereroeffnet:** `sub_004BBE90`s bedingter Aufraeumpfad kann
`sub_004BBD90` aufrufen (aus der GESCHLOSSENEN Bink-Worker-Untersuchung
bekannt), aber die hier gefundene Blockade tritt VOR diesem Aufruf im
SINGLE-Objekt-Wait-Pfad auf -- ein anderer Mechanismus als der bereits
gefixte MULTI-Objekt-Wait-Pfad (`sub_00052D40` -> `sub_00052C5C`). Gemaess
Anweisung als verwandt, aber eigenstaendig dokumentiert, nicht als
Wiedereroeffnung des geschlossenen Falls behandelt.

**Fix diese Runde:** `src/kernel/kernel_bridge.c` -- Ordinal 250
(`ObfDereferenceObject`) in den Kernel-Dispatch-Switch eingebunden (war
implementiert, aber unverdrahtet, exaktes Vorbild:
`NtSuspendThread`/`NtResumeThread`). Per Rebuild + Neulauf verifiziert:
Warnung weg, keine Regression, Steady-State wie erwartet unveraendert.

**Repository:** `upgrade/selective-upstream`. Geaenderte GETRACKTE Datei
diese Runde: `src/kernel/kernel_bridge.c` (der Fix). Alle waehrend der
Live-Bisektion hinzugefuegten Diagnosen (Aufrufzaehler fuer fehlende
Bridges, `ObReferenceObjectByHandle`-Logger) wurden nach Gebrauch wieder
entfernt -- `git diff` zeigt nur die eine Dispatch-Zeile plus Kommentar.
Keine Hand-Edits an generierten `.c`-Dateien ueberleben (gitignored,
temporaere Checkpoints in `recomp_0003.c`/`recomp_0015.c` waren nur fuer
diese Live-Sitzung und wieder entfernt).

**Bewiesen (kumulativ, inkl. neu in Update 233):** Alle Eintraege aus
Updates 109-232 gueltig. Neu: der Render-Loop-Lock-Churn ist bestaetigt
kein neuer Bug (vorbestehend, bereits erklaert). Ordinal-250-Dispatch-Luecke
gefunden und behoben. Die tatsaechliche Blockade-Ursache (Worker-Thread
wartet auf nie signalisiertes Handle) ist lokalisiert, aber die Frage
"was sollte dieses Handle signalisieren, und warum tut es das nicht" ist
noch offen.

**Offen (Stand nach Update 233), priorisiert:**
1. Identifizieren, welches konkrete Objekt (Event? Semaphore?) der
   blockierte Pool-Worker (`sub_004BBE90` -> `sub_00052D2E` ->
   `sub_00052C18`) tatsaechlich abwartet, und zu welchem Subsystem der
   erwartete Producer gehoert (welcher Task wird hier verarbeitet -- die
   beiden `ctx2`-Werte `0x010DD260`/`0x010DD2C0` sind der Einstiegspunkt).
   Naechster konkreter Schritt: Feld-Layout des Task-Objekts an `ctx2`
   nachvollziehen (`esi+4`, `esi+8`, `esi+0x14`, `esi+0x18` in
   `sub_004BBE90`) und herausfinden, wer dieses Handle erzeugt und wer es
   signalisieren sollte -- ob eine fehlende Kernel-Bridge, ein unerreichter
   Codepfad, oder eine spielerinput-abhaengige, fuer den automatisierten
   Testlauf schlicht erwartungsgemaess nie eintretende Bedingung.
2. 10 verbleibende benigne-aussehende `source=0x0005F701`-Treffer mit
   echten, nicht-null Zielen -- niedrige Prioritaet, unveraendert.
3. Ordinal 252/253 (`PhyGetLinkState`/`PhyInitialize`) haben laut Code
   ebenfalls implementierte, aber unverdrahtete Bridges (gleiche Klasse wie
   Ordinal 250) -- bewusst nicht in diesem Update mitgefixt (Netzwerk-
   Ordinale, ausserhalb des aktuellen Untersuchungsfokus), aber als
   niedrigpriorer Folgefix vermerkt.
4. Restliche niedrigprioritaere Punkte aus frueheren Updates unveraendert.

**Instrumentierung:** Keine aktiv (alle Live-Diagnosen aus Update 233
wieder entfernt, bestaetigt per `git diff`).

**Relevante neue Artefakte:** `tools/debug/append_update233.py`,
`tools/debug/append_update233_status.py`, `tools/debug/update_handoff_233.py`
(Doku-Skripte); `src/kernel/kernel_bridge.c` (der eigentliche Fix,
GETRACKT, nicht Doku).

**Laufende Prozesse / IN PROGRESS:** Kein scda_recomp.exe-Testprozess
aktiv (nach Verifikationslauf sauber beendet). xemu-Status vom Nutzer
zuletzt unveraendert -- fuer Update 233 nicht benoetigt, da die
Charakterisierung rein ueber Live-Recomp-Checkpoints und Code-Lesen
gelang. Upstream-Stand unveraendert (`origin/main` bei `766ecef`, Tags
`v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
