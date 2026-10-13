status_path = "PROJECT_STATUS.md"
worklog_path = "docs/SCDA_WORKLOG.md"

status_text = """
## Update 207 (2026-09-21): sub_0003FCA5 vollstaendig identifiziert -- es ist der lokalisierte "Disc-Fehler"-Dialog, kein generischer Crash-Handler; erklaert Downstream-Verhalten, aber nicht die Root Cause

**Ausloeser:** Eine gestaffelte Einzelprobe (Update-206-Technik, feineres
`0.02s`-Poll-Intervall, Schwelle `kb>150000`) traf erstmals einen Zeitpunkt,
an dem `sub_0003FCA5` selbst noch AKTIV Code ausfuehrt (nicht nur in der
finalen Idle-Schleife) -- ein `k 10`-Stack zeigte `sub_0003FCA5+0xb80`
aufrufend in eine bisher unbekannte Kette (`sub_00072989` ->
`sub_00052D5A` -> `sub_00052CDC` -> `kernel_thunk_dispatch` ->
`bridge_KeDelayExecutionThread` -> `xbox_KeDelayExecutionThread` ->
`SleepEx`/`NtDelayExecution`). Das zeigte erstmals, dass `sub_0003FCA5`
selbst substantielle Arbeit verrichtet, nicht nur ein `jmp $`-Rumpf ist.

**Vollstaendige Lektuere von `sub_0003FCA5` (459 Bytes/145 Instruktionen
Guest-Code, komplett gelesen):** Die Funktion ist ein klassischer
"zeige eine Fehlermeldung und haenge fuer immer"-Handler:
1. Lazy-Init-Check (`MEM32(0x5E76AC)`), bei Bedarf zwei Icalls zur
   Interface-Initialisierung.
2. Baut eine 640x480-Anzeigeflaeche auf (`sub_0049D320`, `sub_0049D210`,
   `MEM32(ebp-108)=0x280`, `MEM32(ebp-104)=0x1E0`).
3. Prueft ein globales Objekt (`MEM32(0x6500FC)`) und wartet ggf. in einer
   Schleife (`sub_000523F7`, dann `sub_00072989` -- DAS ist die Quelle des
   oben gefundenen Delay-Aufrufs) -- vermutlich ein Audiosystem-Shutdown-
   oder Nachrichtenschleifen-Wartevorgang.
4. Ermittelt einen Sprachindex (0-4) ueber `sub_000522B2` und waehlt damit
   eines von FUENF Nachrichtenpaaren aus String-Konstanten.
5. Rendert zwei Textzeilen an festen Bildschirmkoordinaten
   (`sub_000587B0`, zweimal, an (0x30,0x32) und (0x50,0x32)).
6. Flusht das Ergebnis per NV2A-Pushbuffer-Kick (`sub_0049CC50`, bereits in
   Update 206 gelesen).
7. `goto loc_0003FE6E;` -- die bekannte Endlosschleife (Selbst-`jmp`).

**Die fuenf String-Paare wurden per Capstone/Python direkt aus der XBE
gelesen (UTF-16LE, nicht ASCII):** Alle fuenf sind DIESELBE Meldung in
FUENF SPRACHEN (Code 0=Deutsch, 1=Franzoesisch, 2=Spanisch, 3=Italienisch,
4=Englisch): **"Bei der benutzten CD ist ein Problem aufgetreten.
Moeglicherweise ist sie verschmutzt oder beschaedigt."** / "There's a
problem with the disc you're using. It may be dirty or damaged." Der
"Fehlercode" 0-4 ist also KEIN Fehlertyp-Selektor, sondern ein
Sprachindex -- **`sub_0003FCA5` ist die lokalisierte Standard-Xbox-
"Disc-Fehler"-Anzeige, kein generischer Crash-Handler.**

**Bedeutung fuer die Root-Cause-Suche:** `sub_0003FCA5` wird von
`sub_00040821` (unsere Refill-Funktion) genau dann aufgerufen, wenn deren
eigener Disc-Lesevorgang fehlschlaegt. Da das Objekt laut Update 196 SCHON
VOR diesem Lesevorgang genullt ist, ist die "Disc-Fehler"-Anzeige ein
DOWNSTREAM-SYMPTOM: die Refill-Funktion versucht, mit einem bereits
kaputten/genullten Handle/Zeiger einen Lesevorgang auszufuehren, dieser
schlaegt (aus Sicht des generischen Fehlerbehandlungscodes wie ein
"Disc-Problem" aussehend) fehl, und das Spiel zeigt reflexartig die
Standardmeldung -- **nicht, weil tatsaechlich ein Disc-I/O-Problem vorliegt
(das Diskimage im Emulator ist einwandfrei), sondern weil ein ungueltiger
Handle-Wert von der generischen Fehlerbehandlung als Disc-Problem
fehlinterpretiert wird.** Das bestaetigt und praezisiert Update 196s Fund,
liefert aber weiterhin NICHT den konkreten Schreiber.

**Damit vollstaendig ausgeschlossen (kein weiterer Untersuchungsbedarf):**
`sub_0003FCA5` selbst und seine gesamte interne Aufrufkette
(`sub_000677E4`, `sub_0049D320`, `sub_0049D210`, `sub_000523F7`,
`sub_00072989`, `sub_00052D5A`, `sub_00052CDC`, `sub_00051673`,
`sub_0003B7B3`, `sub_002536EB`, `sub_00255615`, `sub_000587B0` x2,
`sub_0049CC50`+Unterfunktionen) -- alles generische, objektunabhaengige
Anzeige-/Rendering-/Wartelogik fuer den Fehlerdialog, kein Bulk-Zero.

**Naechster Test:** Die eigentliche Ursache liegt weiterhin VOR dem
Refill-Aufruf. Empfehlung: den GENAUEN Rueckgabewert/Statuscode
untersuchen, den `sub_00040821` unmittelbar vor dem Aufruf von
`sub_0003FCA5` prueft (welche Bedingung genau loest den Sprung zum
Fehlerdialog aus?) -- das koennte den Fehlertyp praezisieren (ungueltiges
Handle vs. falsche Groesse vs. Timeout) und den Suchraum weiter
eingrenzen, ohne erneut die ganze Root-Cause-Suche zu wiederholen.

**Model-Tier:** STRONG bleibt angemessen.
"""

worklog_text = """
## Update 207 (2026-09-21): sub_0003FCA5 fully identified -- it is the localized "disc error" dialog, not a generic crash handler; explains downstream behavior but not the root cause

**Trigger:** a staggered single sample (Update 206 technique, finer `0.02s`
poll interval, `kb>150000` threshold) for the first time landed at a moment
where `sub_0003FCA5` itself was still actively executing code (not yet in
the final idle loop) -- a `k 10` stack showed `sub_0003FCA5+0xb80` calling
into a previously unknown chain (`sub_00072989` -> `sub_00052D5A` ->
`sub_00052CDC` -> `kernel_thunk_dispatch` -> `bridge_KeDelayExecutionThread`
-> `xbox_KeDelayExecutionThread` -> `SleepEx`/`NtDelayExecution`). This
showed for the first time that `sub_0003FCA5` itself does substantial work,
not just a `jmp $` body.

**Full read of `sub_0003FCA5` (459 bytes / 145 guest instructions, read in
full):** the function is a classic "show an error message and hang forever"
handler:
1. Lazy-init check (`MEM32(0x5E76AC)`), two interface-init icalls if needed.
2. Builds a 640x480 display surface (`sub_0049D320`, `sub_0049D210`,
   `MEM32(ebp-108)=0x280`, `MEM32(ebp-104)=0x1E0`).
3. Checks a global object (`MEM32(0x6500FC)`) and, if present, waits in a
   loop (`sub_000523F7`, then `sub_00072989` -- THIS is the source of the
   delay call found above) -- presumably an audio-system shutdown or
   message-pump wait.
4. Determines a language index (0-4) via `sub_000522B2` and uses it to pick
   one of FIVE message pairs from string constants.
5. Renders two lines of text at fixed screen coordinates (`sub_000587B0`,
   called twice, at (0x30,0x32) and (0x50,0x32)).
6. Flushes the result via an NV2A pushbuffer kick (`sub_0049CC50`, already
   read in Update 206).
7. `goto loc_0003FE6E;` -- the known infinite loop (self-`jmp`).

**The five string pairs were read directly from the XBE via Capstone/Python
(UTF-16LE, not ASCII):** all five are the SAME message in FIVE LANGUAGES
(code 0=German, 1=French, 2=Spanish, 3=Italian, 4=English): "There's a
problem with the disc you're using. It may be dirty or damaged." The
"error code" 0-4 is therefore NOT an error-type selector but a language
index -- **`sub_0003FCA5` is the standard, localized Xbox "disc error"
display, not a generic crash handler.**

**Significance for the root-cause search:** `sub_0003FCA5` is called by
`sub_00040821` (our refill function) precisely when its own disc read
fails. Since the object is already zeroed BEFORE that read (per Update
196), the "disc error" screen is a DOWNSTREAM SYMPTOM: the refill function
tries to perform a read using an already-broken/zeroed handle or pointer,
that read fails (looking, to the generic error-handling code, like a "disc
problem"), and the game reflexively shows the standard message -- **not
because there is an actual disc I/O problem (the disc image under emulation
is fine), but because an invalid handle value gets misclassified by the
generic error path as a disc problem.** This confirms and sharpens Update
196's finding, but still does not identify the concrete writer.

**Now fully excluded (no further investigation needed):** `sub_0003FCA5`
itself and its entire internal call chain (`sub_000677E4`, `sub_0049D320`,
`sub_0049D210`, `sub_000523F7`, `sub_00072989`, `sub_00052D5A`,
`sub_00052CDC`, `sub_00051673`, `sub_0003B7B3`, `sub_002536EB`,
`sub_00255615`, `sub_000587B0` x2, `sub_0049CC50`+subfunctions) -- all
generic, object-independent display/rendering/wait logic for the error
dialog, no bulk zero.

**Next test:** the actual root cause remains upstream of the refill call.
Recommendation: examine the EXACT return value/status code that
`sub_00040821` checks immediately before calling `sub_0003FCA5` (what
precise condition triggers the jump to the error dialog?) -- this could
sharpen the error type (invalid handle vs. wrong size vs. timeout) and
narrow the search further, without repeating the whole root-cause search.

**Model tier:** STRONG remains appropriate.
"""

with open(status_path, "rb") as f:
    raw = f.read()
raw += status_text.replace("\n", "\r\n").encode("utf-8")
with open(status_path, "wb") as f:
    f.write(raw)

with open(worklog_path, "rb") as f:
    raw = f.read()
raw += worklog_text.replace("\n", "\r\n").encode("utf-8")
with open(worklog_path, "wb") as f:
    f.write(raw)

print("done")
