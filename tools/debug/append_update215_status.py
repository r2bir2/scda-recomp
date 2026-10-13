path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

text = """

## Update 215 (2026-09-21): sub_000516B7 vollstaendig neu gelesen; die fehlerhafte Cursor-Fortschreibungsformel auf eine exakte Zeile eingegrenzt, strukturell nachweislich unfaehig, die echte Bytezahl zu nutzen -- selbst mit Update 186s bereits aktivem ehrlichen-Byte-Count-Fix -- das erklaert, warum der Fix vor vier Tagen nie vollstaendig geschlossen hat

**Ziel dieser Runde:** klaeren, welche der zwei `sub_000516B7`-Aufrufstellen
in `sub_00040821` der tatsaechliche Kurzlesevorgang von `common.lin`s
letztem Chunk ist (Update 214 fand Datei/Offset, aber nicht welcher
Aufruf), und praezise erklaeren, warum Update 186s bereits aktiver,
bereits verifizierter Kernel-seitiger Fix (ehrliches Byte-Count-Reporting)
die in Update 213/214 verfolgte Korruption nicht vollstaendig verhindert.

**Vollstaendiges Neu-Lesen von `sub_00040821`s relevanter Spanne
(`recomp_0001.c:68565-68790`, Guest `0x40821`-`0x40992`) in einem Durchgang,
ersetzt mehrere teilweise/falsche Annahmen dieser Sitzung:**
- `loc_0004087B` berechnet `restplatz_bis_pufferende = puffergroesse -
  cursor`. Wenn `restplatz_bis_pufferende >= 8` (Platz fuer einen vollen
  Datensatz-Header), geht es direkt zu `loc_00040909` (noch kein Aufruf).
  Ist er `< 8`, faellt der Code in `loc_0004089A`, das eine SEPARATE,
  kleinere Kompaktierung macht (verschiebt den `<8`-Rest), und ruft dann
  **Aufrufstelle 1** (`sub_000516B7`, Ruecksprungadresse `0x000408E1`) fuer
  einen Refill auf.
- **Aufrufstelle 1 (`0x408E1`) ist die, die tatsaechlich `common.lin`s
  letzten, kurzen (89.969-Byte-)Read ausfuehrt** -- bestaetigt durch die
  Arithmetik: die untenstehende fehlerhafte Formel erzeugt den in Updates
  213/214 erfassten `read_cursor=0x4001` nur, wenn der Vor-Read-
  `restplatz_bis_pufferende` gleich `7` war, und `0x4000 - 7 = 0x3FF9`
  passt exakt zu `0x4001 - 8` (die zwei bei `loc_00040909` konsumierten
  4-Byte-Header-Felder landen genau auf dem erfassten Cursor).
- Bei `loc_000408E1` (Rueckkehr von Aufrufstelle 1) macht `sub_00040821`
  `test eax,eax; jne loc_000408F0` -- ueberspringt die "Chunk verarbeitet"-
  Callback-Pruefung/den Dispatch komplett, wann immer `eax != 0`. Ein
  vollstaendiges Lesen von `sub_000516B7`s eigenem Koerper
  (`recomp_0002.c:45924-46153`, alle 100 Instruktionen, nicht nur das in
  Updates 211/212 gelesene Fragment) zeigt: es hat **zwei unterschiedliche
  Rueckkehrpfade mit unterschiedlichen `eax`-Werten**: ein sofortiger,
  vollstaendig synchroner Abschluss liefert `eax=1` ueber `loc_00051720`;
  ein Pfad, der ueber den generischen Kernel-Bridge-Wrapper `sub_00052A85`
  laeuft (fuer alles, was `STATUS_PENDING` war oder per `GetLastError`
  weitergereicht wird), liefert `eax=0` ueber `loc_0005179B`. Da unser
  Host-`ReadFile` synchron ist, schliesst ein kurzer-aber-nicht-leerer Read
  sofort ab und liefert `eax=1` -- was `sub_00040821` als "Callback
  ueberspringen" behandelt, nicht als Fehler. Das erklaert, warum an
  Aufrufstelle 1 selbst kein Fatal-Dispatch passiert: er wird per Design
  nicht erreicht bei einem einfachen synchronen Abschluss (der Callback
  existiert speziell, um asynchrone Abschluesse zu signalisieren) -- nicht
  weil an dieser Pruefung selbst etwas falsch waere.
- **Der eigentliche Bug, an der exakten Zeile bestaetigt, ist unbedingt und
  unabhaengig vom obigen `eax`-Zweig:** sowohl der `eax!=0`- (skip) als auch
  der `eax==0`- (callback-geprueft) Pfad treffen sich bei `loc_000408F0`,
  das unbedingt ausfuehrt:
  ```
  eax = MEM32(ebp-4);              // die ECHTE Anzahl Bytes, die sub_000516B7 tatsaechlich
                                    // in den Out-Parameter des Aufrufers geschrieben hat -- hier verfuegbar
  MEM32(ebx+0x4064) += eax;        // nutzt sie korrekt fuer diesen laufenden Zaehler
  eax = 0x4000 - MEM32(ebp-12);    // MEM32(ebp-12) ist restplatz_bis_pufferende, berechnet
                                    // VOR dem Read, bei loc_0004087B -- veraltet, nie mit der
                                    // echten uebertragenen Zahl aktualisiert
  MEM32(ebx+0x4060) = eax;         // ... aber DIESER -- der eigentliche Lese-/Schreib-Cursor --
                                    // nutzt den veralteten Vor-Read-Wert stattdessen
  ```
  Die echte Bytezahl liegt eine Zeile darueber in einem Register und wird
  fuer den Cursor schlicht nie benutzt. Das ist zeilengenau derselbe Defekt,
  den Update 173 bereits am 17.09.2026 diagnostiziert hat ("Diese Formel
  beruecksichtigt an KEINER Stelle die tatsaechlich gelesene Bytezahl"),
  jetzt an der exakten aktuellen Quellzeile mit einer konkreten, echten,
  erfassten Instanz bestaetigt.

**Warum Update 186s bereits aktiver Fix das nicht schliesst:**
`scda_disc_zero_short_read_tail` (`src/kernel/kernel_file.c`, unveraendert,
weiterhin aktiv) laesst `xbox_NtReadFile` die EHRLICHE uebertragene
Bytezahl ueber `IoStatusBlock->Information` melden -- und diese ehrliche
Zahl erreicht den Guest tatsaechlich als `MEM32(ebp-4)` bei
`loc_000408F0`. Aber die Cursor-Fortschreibungsformel liest `MEM32(ebp-4)`
strukturell nie fuer den Cursor -- nur fuer den unabhaengigen
`+0x4064`-Akkumulator. Keine noch so ehrliche gemeldete Bytezahl des
Kernels kann eine Formel reparieren, die sie gar nicht konsultiert. Das ist
ein echter Defekt im originalen, ausgelieferten Spielcode (bytegenau
identisch zur Retail-XBE bestaetigt, Updates 171/213) -- nicht behebbar
durch Anpassung der Kernel-Bridge-NTSTATUS-/Byte-Count-Semantik, und kein
Recompiler-Bug.

**Die eine verbleibende offene Frage, praezise eingegrenzt:** Update 185
hat bereits bewiesen, dass echte Hardware per `xemu` den IDENTISCHEN
Kurzlesevorgang erlebt (`want=0x20000 got=0x15F71`) an genau dieser Stelle.
Wenn echte Hardware dieselbe (bytegenau identische) fehlerhafte Formel
ausfuehrt und nicht abstuerzt, ist die einzige Erklaerung, dass der
EINGABEWERT der Formel (`MEM32(ebp-12)`, also `restplatz_bis_pufferende`,
berechnet einen Aufruf frueher, BEVOR Aufrufstelle 1 ueberhaupt feuert)
zwischen echter Hardware und recomp im Moment des Aufrufs von
Aufrufstelle 1 abweicht -- z.B. wenn echte Hardwares Cursor zufaellig exakt
auf einem Vielfachen der Chunk-Groesse sitzt, wenn sie diesen letzten
Stream-Read erreicht (`restplatz_bis_pufferende == 0`), degeneriert die
fehlerhafte Formel zu `cursor = 0x4000 - 0 = 0x4000` exakt, landet also
genau auf der Grenze, wo frische (echte, nicht-Muell) Daten beginnen --
vermeidet den fehlgeparsten Datensatz-Header durch Zufall, nicht durch
einen echten Fix. Das ist noch nicht live bestaetigt und ist der naechste
konkrete Pruefpunkt.

**Kein Patch implementiert, gemaess expliziter Standing Instruction fuer
diese Runde.** Der Mechanismus ist jetzt auf Zeilenebene verstanden und mit
der vier Tage frueheren Update-171-186-Untersuchung zusammengefuehrt; was
bleibt, ist per `xemu` zu bestaetigen, ob `restplatz_bis_pufferende` von
recomps `7` abweicht, wenn Aufrufstelle 1 fuer `common.lin`s letzten Chunk
feuert -- das wuerde die Real/Recomp-Abweichung vollstaendig erklaeren,
ohne dass ausser dem (echten, aber auf echter Hardware unsichtbaren)
Cursor-Formel-Defekt irgendetwas falsch sein muesste.

**Naechster Test (fuer STRONG):**
1. `tools/debug/xemu_object_refill_wantgot.py` (Update 185, weiterhin
   vorhanden und direkt wiederverwendbar) erweitern, um zusaetzlich
   `MEM32(ebx+0x4058)` und `MEM32(ebx+0x4060)` (bzw. die aequivalenten
   `ecx`/`eax`-Registerwerte unmittelbar vor dem Aufruf) am selben
   Breakpoint zu lesen, fuer genau den Treffer, der den
   `want=0x20000 got=0x15F71`-Kurzlesevorgang erzeugt, um echte Hardwares
   `restplatz_bis_pufferende` in genau diesem Moment zu erhalten.
2. Gegen recomps eigenen Wert (`7`, aus dieser Sitzung bereits bekannt)
   vergleichen. Falls unterschiedlich, ist die Abweichung vollstaendig
   erklaert und die Fix-Entscheidung wird: die Cursor-Formel korrigieren,
   um die echte Bytezahl zu nutzen (ein Von-Grund-auf-Bugfix, der der
   urspruenglichen Absicht entspricht, sicher, da er nur Verhalten auf dem
   bereits kaputten Kurzlese-Pfad aendert) vs. untersuchen, warum recomps
   eigenes vorgelagertes Bookkeeping ueberhaupt einen anderen
   `restplatz_bis_pufferende` erreicht als echte Hardware (eine echte,
   andere, noch unbekannte Recomp-seitige Zustandsabweichung).
3. Erst danach: Fix waehlen und implementieren, verifizieren, dann ALLE
   `SCDA-DISC-ERR-INSTRUMENTATION`-Marker, die
   `scda_capture_wraparound_once`-Infrastruktur, und den vergroesserten
   Ringpuffer gemaess Standing Instruction zuruecksetzen, vollstaendige
   Recomp-Pipeline, canonical Build durchlaufen, bis zum naechsten Blocker
   weiterarbeiten.

**Modell-Stufe:** unveraendert STRONG -- verbleibende Arbeit ist ein
begrenzter, skriptbasierter xemu-Feldvergleich unter Wiederverwendung
vorhandenen Update-185-Toolings.
"""

with open(path, "wb") as f:
    f.write(raw + text.replace("\n", "\r\n").encode("utf-8"))

print("done, new size", len(raw) + len(text.replace("\n", "\r\n").encode("utf-8")))
