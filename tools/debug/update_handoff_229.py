# -*- coding: utf-8 -*-
status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG war ausreichend fuer Implementierung, Build und
Testlauf-Verifikation dieses Fixes.

**Seit Update 228 -- Update 229 (diese Sitzung, FIX IMPLEMENTIERT):** Auf
explizite Nutzer-Anweisung wurde die von Update 228 bestaetigte
architektonische Luecke direkt gefixt statt mit einem Workaround zu
umgehen. `xbox_ContiguousAlloc`s Bump-Allokator schreibt jetzt in einen
zweiten `MapViewOfFileEx`-View derselben File-Mapping-Section wie das
Basis-RAM, an einem physischen Offset (`max(g_memory_size,
XBOX_CONTIG_SIZE)`), der garantiert ausserhalb des vom Image/Stack/Heap
bereits beanspruchten Basis-RAM-Bereichs UND ausserhalb des Legacy-Contig-
Fensters liegt. Die Tiled Aperture wurde entsprechend erweitert, um auch
diesen neuen Bereich zu erreichen. Ein Startup-Selbsttest beweist den
Contig<->Tiled-Round-Trip statt ihn anzunehmen; bei jedem Fehlschlag faellt
der Code auf das alte, nicht-aliasierte Verhalten zurueck statt den Start
zu verweigern. Details, inklusive eines waehrend der eigenen Verifikation
gefundenen und behobenen Mirror-/Adresskollisions-Bugs, in Update 229 oben
und in `docs/SCDA_WORKLOG.md`.

**Bewusste Einschraenkung:** kein echter Drei-Wege-Alias (Basis-RAM sieht
den neuen Bereich absichtlich nicht) -- Basis-RAM dorthin zu erweitern
haette entweder die Mirror-Wraparound-Grenze verschoben (Regressionsrisiko
fuer andere Titel, z.B. RenderWare) oder eine Verkleinerung der Heap-
Obergrenze erfordert. Der implementierte Zwei-Wege-Alias (Contig<->Tiled)
reicht aus, um die Kollision zu beseitigen, da dynamische Contig-
Allokationen jetzt nie mehr denselben physischen Offset wie ein Basis-RAM-
Global teilen.

**Verifikation:** vollstaendiger Pipeline-Rebuild sauber (nur bereits
vorher vorhandene Warnungen). Zwei Testlaeufe (~20 s, ~100 s) je
180.000+ Kernel-Calls: Selbsttest bestanden, null Vorkommen der alten
Korruptions-/Crash-Signatur (`7400087D`, `004A12B0`, `[CONTIG] arena
exhausted`, Access Violation) in beiden vollstaendigen Logs. Beide Laeufe
stabilisieren sich unabhaengig von der Laufzeit auf denselben Steady-State
(Bink-Worker-Critical-Section-Polling) -- ob das der naechste echte
Blocker ist oder ein Umgebungsartefakt (keine echten Disc-Daten), ist noch
offen.

**Repository:** `upgrade/selective-upstream`. Quelltextaenderung diese
Runde: `src/kernel/xbox_memory_layout.c` (262 Zeilen hinzugefuegt, 15
entfernt/ersetzt -- siehe `git diff` fuer den vollstaendigen Diff).

**Bewiesen (kumulativ, inkl. neu in Update 229):** Alle Eintraege aus
Updates 109-228 gueltig. NEU: die 0x4AB668-Korruption ist behoben (Fix
implementiert, per Selbsttest und Testlauf verifiziert, keine formale
Live-Rueckbestaetigung an der urspruenglichen Korruptionsstelle unter
`cdb` -- als optionaler, strikt gruendlicherer Schritt offen, siehe unten).

**Offen (Stand nach Update 229), priorisiert:**
1. Optional: Live-Rueckbestaetigung an der urspruenglichen Korruptionsstelle
   (`sub_004AED5D`/`sub_004B1377`) unter `cdb`, um direkt zuzusehen, dass
   0x004AB668 dort nicht mehr korrumpiert wird -- nicht zwingend, die
   bestehende Evidenz (Selbsttest + sauberer Langlauf) gilt als
   ausreichend, aber strikt gruendlicher.
2. Klaeren, ob der in Update 229 beobachtete Steady-State (Bink-Worker-
   Critical-Section-Polling, Ordinale 277/246/294) der naechste echte
   Blocker ist oder ein Testumgebungsartefakt (fehlende echte Disc-Daten).
   Falls echter Blocker: mit derselben Bisektionsmethodik wie zuvor
   angehen.
3. Restliche niedrigprioritaere Punkte aus frueheren Updates unveraendert
   (u.a. der separate "manche Aufrufe der Zeilenkopie schreiten nie
   fort"-Defekt aus Update 224).

**Instrumentierung:** Keine aktiv. Der Fix selbst ist dauerhafter,
getrackter Quelltext (kein Diagnose-Code, kein Revert noetig).

**Relevante neue Artefakte:** `tools/debug/append_update229.py`,
`tools/debug/append_update229_status.py`, `tools/debug/update_handoff_229.py`
(Doku-Skripte); `src/kernel/xbox_memory_layout.c` (Fix).

**Laufende Prozesse / IN PROGRESS:** Keine Testprozesse laufen. Upstream-
Stand unveraendert (`origin/main` bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
