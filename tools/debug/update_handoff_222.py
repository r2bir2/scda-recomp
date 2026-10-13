# -*- coding: utf-8 -*-
status_path = "PROJECT_STATUS.md"

with open(status_path, "rb") as f:
    raw = f.read()

marker_start = b"## CURRENT HANDOFF"
marker_end = b"## Ergebnis in vier Kategorien"
start_idx = raw.index(marker_start)
end_idx = raw.index(marker_end)

new_handoff = """## CURRENT HANDOFF

**MODEL TIER:** STRONG fuer Diagnosearbeit. Das tatsaechliche Implementieren
eines Video-Backends (NV2A/D3D-Geraet) ist ein neues, grosses Arbeitspaket
und braucht eine eigene Scoping-Diskussion mit dem Nutzer, bevor begonnen
wird -- nicht einfach "weitermachen".

**MODEL RECOMMENDATION:** Vor jeder weiteren Arbeit am Video-Backend: mit
dem Nutzer klaeren, ob/wann dieses grosse neue Arbeitspaket (NV2A-/D3D-
Geraeteemulation von Grund auf) angegangen werden soll, da es qualitativ
anders ist als die bisherige bug-fokussierte Arbeit dieser Sitzung.

**Seit Update 221 -- Update 222 (diese Sitzung, NEUER BLOCKER
DIAGNOSTIZIERT):** `sub_004A12B0`s Poll-Loop wurde vollstaendig
diagnostiziert: es ist ein Hardware-Register-Polling
(`MEM32(0x4AB668)` -> `+0x1C28` -> `+0x100410`, ein PFIFO-/PGRAPH-artiges
"Kick"-Bit setzen und auf dessen Ruecksetzung warten), das ewig dreht, weil
`MEM32(0x4AB668)` (live bestaetigt: `0x7400087D`) weit ausserhalb des
64-MB-Guest-RAM liegt -- kein echtes, initialisiertes Geraet. Das
bestaetigt und praezisiert die bereits im Gedaechtnis stehende
"kein Video-Backend verdrahtet"-Luecke: die Boot-Sequenz erreicht jetzt
(dank des Updates-220/221-Fixes) erstmals den Punkt, an dem sie
TATSAECHLICH versucht, das Grafikgeraet zu nutzen.

**`sub_00040A88`s bestehender Kommentar deckt dies NICHT ab** -- jene
fruehere Untersuchung loeste ein anderes Symptom (UMOD-Reader-Seek-Bug),
nicht dieses Hardware-Polling ohne Backing-Geraet.

**Kein Fix versucht** -- dies ist eine Diagnose- und Umfangs-Entscheidung.
Ein echtes Video-Backend zu bauen ist ein substantielles neues Subsystem,
kein begrenzter Bugfix.

**Repository:** `upgrade/selective-upstream`. Nur Dokumentation (Update 222)
diese Runde, kein Code geaendert.

**Aktueller Blocker:** fehlendes NV2A-/D3D-Video-Backend -- `0x4AB668`
muss ein echtes Geraet werden, mit mindestens einem Stub-PFIFO-/PGRAPH-
Register-Modell, das Kicks quittiert. Umfang: gross, eigene
Scoping-Diskussion mit dem Nutzer empfohlen.

**Bewiesen (kumulativ, inkl. neu in Update 222):** Alle Eintraege aus
Updates 109-221 unveraendert gueltig. **NEU: der `sub_00040821`-Blocker ist
vollstaendig und dauerhaft behoben (219-221); der NAECHSTE Blocker ist
identifiziert und lokalisiert als fehlendes Video-Backend, nicht als
weiterer versteckter Logikbug (222).**

**Offen (Stand nach Update 222), priorisiert:**
1. Mit dem Nutzer klaeren, ob/wie das Video-Backend-Arbeitspaket angegangen
   werden soll (Umfang, Prioritaet, gewuenschter Detailgrad -- z.B. nur ein
   minimaler Stub, der Boot durchlaesst, vs. echtes Rendering).
2. Restliche niedrigprioritaere Punkte aus frueheren Updates unveraendert.

**Instrumentierung:** Keine aktiv.

**Relevante neue Artefakte:** `tools/debug/append_update222.py`,
`tools/debug/append_update222_status.py`, `tools/debug/update_handoff_222.py`
(reine Doku-Skripte).

**Laufende Prozesse / IN PROGRESS:** Keine Testprozesse laufen. Upstream-
Stand unveraendert (`origin/main` bei `766ecef`, Tags `v0.10.0`/`v0.11.0`).

"""

new_raw = raw[:start_idx] + new_handoff.replace("\n", "\r\n").encode("utf-8") + raw[end_idx:]
with open(status_path, "wb") as f:
    f.write(new_raw)

print("done")
