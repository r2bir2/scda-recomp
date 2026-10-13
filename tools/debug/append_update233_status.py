# -*- coding: utf-8 -*-
path = "PROJECT_STATUS.md"

with open(path, "rb") as f:
    raw = f.read()

entry = """
## Update 233 (2026-09-22): neuen Kernel-Call-Steady-State nach Update 232 charakterisiert -- ein echter Pool-Worker-Thread haengt dauerhaft in einer regulaeren Alertable-Wait-Retry-Schleife fest; dabei einen unabhaengigen toten Kernel-Bridge-Bug gefunden und behoben

Direkte Fortsetzung gemaess stehender Anweisung: nach Abschluss und
Bestaetigung von Update 232 den neuen Steady-State danach (wiederholte
Kernel-Aufrufe ueber Ordinale 99/234/246/277/294) charakterisieren, bevor
irgendetwas angefasst wird -- gleiche hierarchische Eingrenzungsmethodik.

### Schritt 1: Bedeutung der Ordinale und Form des Zyklus

Die Ordinal-Tabellen in `src/kernel/kernel_bridge.c` identifizieren die
fuenf: 99 = KeDelayExecutionThread, 234 = NtWaitForSingleObjectEx, 246 =
ObReferenceObjectByHandle, 250 = ObfDereferenceObject, 277 =
RtlEnterCriticalSection, 294 = RtlLeaveCriticalSection. Das bereits
vorhandene `scda_runtime.log` aus Update 232s eigenem 90-Sekunden-
Verifikationslauf (noch auf der Platte, kein Neulauf fuer diesen ersten
Schritt noetig) zeigte zwei unterschiedliche `esp`-Basen verschraenkt im
Schwanz nach dem Uebergang (`0x00F7B6xx` und `0x013F9Exx`) -- zwei Threads,
die unabhaengig voneinander durch dieselbe kleine Ordinal-Menge laufen,
danach ohne jede neue `[FILE]`/`[PATH]`/`[READ]`/`[ICALL]`-Aktivitaet.

Der Uebergang selbst: unmittelbar vor Beginn des Steady-State oeffnet das
Spiel `splintercell4xboxretail.umd` (das Haupt-Disc-Image, bereits einmal
zuvor geoeffnet) erneut und ruft Ordinal 37 (`FscSetCacheSize`) darauf auf,
das keine Bridge hat und 0 zurueckgibt. Eine Live-Aufrufzaehler-Diagnose
(hinzugefuegt und wieder entfernt im Dispatch-Fallback von
`kernel_bridge.c`, nicht committet) bewies, dass diese Warnung eine
Nebenspur ist: Ordinal 37 und die gesamte IoCompletion-/Fsc-Familie (35,
79, 90, 91, 191, 223, 227) werden je genau EINMAL ueber einen Lauf mit
mehreren hunderttausend Aufrufen aufgerufen, nie in einer Schleife -- mit
direkter Evidenz ausgeschlossen statt als "vermutlich harmlos" stehen
gelassen.

### Schritt 2: Das Render-Loop-Enter/LeaveCriticalSection-Rauschen ist eine vorbestehende, bereits dokumentierte Eigenschaft, nicht neu

Das Aufrufpaar mit dem hoechsten Volumen im Schwanz nach dem Uebergang
(`RtlEnterCriticalSection`/`RtlLeaveCriticalSection` um ein festes Lock an
Xbox-VA `0x6B27A0`, erreicht ueber einen Lazy-Init-Wrapper
`sub_000767ED`, aufgerufen aus einer Pro-Frame-Timer-Glaettungsroutine,
`sub_00039566` -> `sub_002E2F8B`/`sub_002E2F99`) laeuft laut einem
Live-Checkpoint bereits seit sehr frueh in der Bootsequenz kontinuierlich
-- nicht erst ab dem Uebergang. Sein rohes Aufrufvolumen (zehntausende ueber
einen Lauf) passt zu diesem Projekts eigenem, bereits am 2026-09-11
geschlossenen Befund, direkt im `RECOMP_KERNEL_HIST`-Kommentar von
`kernel_bridge.c` festgehalten: dieses Recomp taktet sich noch nicht auf
echtes Display-/Vsync-Timing ein, daher laeuft Pro-Frame-Housekeeping
weit schneller als 60 Hz -- Design-/bekannte Einschraenkung, kein Bug.
Weiteres Verfolgen haette einen bereits geschlossenen, unabhaengigen Befund
wiedereroeffnet; ausgeschlossen und fallengelassen.

### Schritt 3: das eigentliche Signal -- ObfDereferenceObject ist eine tote Bridge, und der Poll, zu dem es gehoert, endet nie

Ein Live-Zaehler pro Ordinal (temporaer, im "keine Bridge"-Fallback von
`kernel_thunk_dispatch`, nach Gebrauch entfernt) zeigte, dass Ordinal 250
(`ObfDereferenceObject`) im Steady-State unbegrenzt waechst (588, 603,
618, ... +15 alle 3 Sekunden), waehrend jedes andere Ordinal ohne Bridge
bei ein paar einmaligen Aufrufen stehen blieb. Ein Blick in
`kernel_bridge.c` fand `bridge_ObfDereferenceObject` vollstaendig
implementiert (ruft das echte `xbox_ObfDereferenceObject` auf), aber nie
in den Dispatch-Switch eingebunden -- `/* case 250:
bridge_ObfDereferenceObject */`, auskommentiert, zwei Zeilen unter den
bereits gefixten, bereits dokumentierten `NtSuspendThread`-/
`NtResumeThread`-Eintraegen, die GENAU diese Bug-Klasse beschreiben: "die
Implementierung war schon da und nur der Dispatch-Eintrag fehlte, was
schlimmer ist als ein reiner Stub". Wer 224/231 angebunden hat, hat
offenbar das direkt benachbarte 250 uebersehen (und 252/253, in diesem
Eintrag unangetastet gelassen als unabhaengige Netzwerk-Ordinale,
ausserhalb des Umfangs hier).

Ein Blick in `xbox_ObfDereferenceObject`s echten Koerper (`kernel_ob.c`)
zeigt, dass es selbst bewusst ein No-Op ist -- Win32-Handles verwalten ihre
eigene Lebensdauer, daher zaehlt dieses Projekt sie absichtlich nicht per
Referenz. Das Verdrahten des Dispatch-Eintrags ist daher ein echter,
sicherer, risikofreier Korrektheits-Fix (entspricht diesem Datei-eigenen
etablierten Fix-Muster fuer tote Bridges), hat aber -- durch Neulauf nach
dem Fix bestaetigt -- keine Wirkung auf den Steady-State, weil die echte
Bridge ebenfalls nichts Beobachtbares tut. Trotzdem in `kernel_bridge.c`
gefixt (Dispatch-Switch, Ordinal 250 -> `bridge_ObfDereferenceObject`), da
eine nachweislich tote, bereits praezedenzierte Bridge unverdrahtet zu
lassen keinem Zweck dient und die eigenen Kommentare der Datei genau diese
Bug-Klasse bereits als sofort zu fixen kennzeichnen.

### Schritt 4: der ObReferenceObjectByHandle-Poll loest korrekt auf -- der Thread, auf den gewartet wird, laeuft tatsaechlich noch

Ein Live-Checkpoint in `bridge_ObReferenceObjectByHandle` (temporaer, nach
Gebrauch entfernt) zeigte, dass das abgefragte Handle (`0x48000007`,
getaggt, aus `s_handle_kthread_va[7]`) bei allen 800+ Stichproben
konsistent und erfolgreich auf eine stabile, nicht-null Fake-KTHREAD-
Adresse (`0x013FB000`) aufloest -- nicht auf null haengengeblieben, nicht
flackernd. `bridge_mark_thread_done` (das das "fertig"-Byte dieser Struktur
umschaltet) wird fuer dieses konkrete kthread_va schlicht nie aufgerufen,
was bedeutet, dass der zugrundeliegende Worker-Thread tatsaechlich noch
nicht beendet ist, nicht dass der Aufloesungsmechanismus selbst kaputt
ist. Das schliesst direkt ein erneutes Auftreten des BEREITS GEFIXTEN
Update-169/170-Bugs ("loest immer auf null auf") aus -- andere
Fehlerform, mit direkter Evidenz bestaetigt, nicht als harmlos angenommen.

### Schritt 5: den eigentlichen Worker-Thread verfolgt -- eine regulaere Alertable-Wait-Retry-Schleife, die ihr Objekt nie signalisiert sieht

`PsCreateSystemThreadEx` erzeugt in diesem Lauf genau 4 Worker-Threads,
alle ueber dieselbe generische CRT-`_threadstartex`-foermige Trampolin-
Funktion (`sub_000524DA`), die nur `ctx1(ctx2)` aufruft -- das eigentliche
Pro-Thread-Verhalten steckt also in der jeweiligen `ctx1`-Adresse:
`0x00054701`, `0x004BBE90` (zweimal verwendet, mit unterschiedlichen
`ctx2`-Daten -- ein 2-Worker-Pool, der unterschiedliche Task-Objekte
verarbeitet), und `0x00036889`.

`sub_004BBE90` (die Pool-Routine) ist eine kurze, geradlinige, NICHT
schleifende Funktion: einmal auf ein Handle aus ihrem Task-Objekt warten,
dann eine kleine bedingte Aufraeumarbeit, dann zurueckkehren. Das
Zurueckverfolgen ihres Wait-Aufrufs (`sub_00052D2E` -> `sub_00052C18`)
findet eine manuelle Retry-Schleife:

```
loc_00052C2C:
    NtWaitForSingleObjectEx(handle, mode=1, alertable, timeout_ptr);   // Ordinal 234
    if (result < 0) goto error;
    if (timeout == 0) goto done;
    if (result == 0x101) goto loc_00052C2C;      // retry
    goto done;
```

`0x101` ist `STATUS_ALERTED` -- das ist lehrbuchmaessig korrekter
Alertable-Wait-Retry-Code (ein durch eine zugestellte APC unterbrochener
Wait, keine echte Fertigstellung, sollte einfach erneut warten), kein Bug
im Spiel. Die Aufrufstelle pusht genau 4 Argumente passend zu Ordinal 234s
echter Groesse von 16 Byte/4 Argumenten (`g_slot_arg_bytes[234]`), also
keine Wiederholung der bereits dokumentierten
`NtWaitForMultipleObjectsEx`-4-vs-6-Argumente-Stack-Korruptions-Bug-Klasse
ein paar hundert Zeilen weiter oben in derselben Datei -- direkt geprueft
statt angenommen, angesichts dessen, wie leicht diese Datei laut ihrer
eigenen Historie genau diesen Fehler in Schwester-Wait-Wrappern wiederholt
hat. Eine positionelle Auffaelligkeit wurde notiert (der gepushte Wert, der
im Bridge-Slot fuer `Alertable` landet, ist eigentlich der rohe
Timeout-Wert des Aufrufers, kein Boolean), aber `WaitForSingleObjectEx`s
`Alertable`-Parameter steuert nur, ob eine wartende APC den Wait vorzeitig
unterbrechen kann -- er entscheidet nicht, ob ein echtes Signal ihn
beendet -- erklaert also keine dauerhafte Blockade und wurde in diesem
Eintrag nicht als Fix-Ziel weiterverfolgt.

`xbox_NtWaitForSingleObjectEx` (`kernel_sync.c`) ruft das echte Win32
`WaitForSingleObjectEx` auf dem aufgeloesten nativen Handle auf. Der Wait
ist echt, nicht simuliert: wenn nichts in diesem Recomp jemals genau
dieses Handle signalisiert, blockiert der Worker-Thread hier fuer immer,
laut Design der zugrundeliegenden Win32-Primitive -- passt exakt zum
beobachteten Verhalten (der Worker kehrt nie zurueck,
`bridge_mark_thread_done` wird fuer ihn nie aufgerufen, und der Poll des
Hauptthreads ueber `ObReferenceObjectByHandle` nach "ist dieser Thread
fertig" sieht daher nie eine Fertigstellung).

### Markiert, aber in diesem Eintrag nicht weiterverfolgt

Der bedingte Aufraeumpfad von `sub_004BBE90` ruft `sub_004BBD90` auf, wenn
ein Task-Feld ungleich null ist -- `sub_004BBD90` wird direkt in der
GESCHLOSSENEN Bink-Worker-/Ringbuffer-Untersuchung frueherer Updates
genannt. Das ist eine strukturelle Nachbarschaft (dieselbe Funktions-
familie), nicht derselbe Mechanismus (der Fix jener Untersuchung war ein
4-vs-6-Argumente-Stack-Korruptions-Bug im MULTI-Objekt-Wait-Pfad,
`sub_00052D40` -> `sub_00052C5C` -> `NtWaitForMultipleObjectsEx`; der Befund
dieses Eintrags ist der Schwester-SINGLE-Objekt-Wait-Pfad, `sub_00052D2E`
-> `sub_00052C18` -> `NtWaitForSingleObjectEx`, und die Blockade tritt ein,
bevor `sub_004BBD90` je erreicht wird). Gemaess der stehenden Anweisung,
jenen geschlossenen Pfad ohne neue direkte Evidenz nicht wieder zu oeffnen,
und da dies eine andere Aufrufkette ist, wird dies hier als verwandter,
aber eigenstaendiger Bereich festgehalten statt in eine der beiden
Untersuchungen eingefaltet. Naechster konkreter Schritt: herausfinden, was
das Handle dieses konkreten Pool-Workers tatsaechlich ist (ein Event? ein
Semaphore? an welches Subsystem als Producer gebunden?) und ob der
Producer, der es signalisieren sollte, nicht implementiert, nicht
erreicht, oder selbst blockiert ist -- noch nicht erledigt.

### In diesem Eintrag angewendeter Fix

`src/kernel/kernel_bridge.c`: Ordinal 250 (`ObfDereferenceObject`) in den
Kernel-Dispatch-Switch eingebunden, exakt nach dem Vorbild des
`NtSuspendThread`-/`NtResumeThread`-Fixes ein paar Zeilen darueber. Per
Rebuild + Neulauf bestaetigt: die Warnung "no bridge for ordinal 250" ist
weg, keine neuen Warnungen oder Regressionen, und (wie aus dem Lesen der
echten Implementierung vorhergesagt) der Steady-State bleibt unveraendert,
da das echte `xbox_ObfDereferenceObject` selbst ein dokumentiertes No-Op
ist.

Kein Stub, kein Fake-Signal, kein Clamp, kein uebersprungener Pfad, und
kein Hand-Edit an einer generierten `.c`-Datei ueberlebt -- jede in diesem
Eintrag hinzugefuegte Diagnose (Zaehler fuer fehlende Bridges,
`ObReferenceObjectByHandle`-Handle-/Aufloesungs-Logger) war temporaer,
wurde gelesen und wieder entfernt; die einzige dauerhafte Aenderung ist der
eine echte, praezedenzierte, als sicher verifizierte Dispatch-Wiring-Fix.
"""

new_raw = raw + entry.replace("\n", "\r\n").encode("utf-8")
with open(path, "wb") as f:
    f.write(new_raw)

print("done")
