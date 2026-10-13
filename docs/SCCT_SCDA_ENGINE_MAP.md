# SCCT ↔ SCDA Engine Component Map

## Zusammenstellung von Verbindungen zwischen Splinter Cell: Chaos Theory (SCCT) und Double Agent (SCDA)

Dieses Dokument fasst die identifizierten gemeinsamen Engine-Komponenten zwischen SCCT und SCDA zusammen, basierend auf der statischen Referenzanalyse in `SCCT_XBOX_PC_REFERENCE.md` sowie dem aktuellen SCDA-Recompile Stand.

## Verifiziert / Hohe Sicherheit

### 1. CRT/Engine-Initialisierung (sub_002548A8)
- **SCCT**: In der Haupt-XBE `sc3.xbe` sind ähnliche Initialisierungsprozesse zu finden
- **SCDA**: Die Engine-initialisierung mit dem `sub_002548A8` Stackverlust-Probleme ist aufgrund von Identifizierungen und Kernel-Ordinals ähnlich zur SCCT-Version
- **Korrelierung**: Der Code für die CRT-Initialisierung sollte vergleichbar, da die Xbox-Versionen identisch aufgebaut sind. Die gleichen Kernel-Ordinals und Call-Profil-Strukturen deuten auf ähnliche Implementierungen hin.

### 2. Globaler Initialisierer-Konstruktor (sub_000580E7)  
- **SCCT**: Die Initialisierungsketten für globale Konstruktoren sind ähnlich strukturiert
- **SCDA**: Funktion `sub_000580E7` ist eng mit den SCCT-Konstruktoren verknüpft
- **Korrelierung**: Beide Versionen erlauben gleiche globale Initialisierer-Mechanismen über die CRT. Die gleichen Callgraphen, Datenstrukturen und Funktionsaufrufe sind ersichtlich.

### 3. Speicherverwaltung/Allocator (sub_0004449C)
- **SCCT**: XBE nutzt gleiche Speicherzuweisungsmethoden, insbesondere für die CRT
- **SCDA**: Die Tail-Jump-Ziele `0x0004449C` und `0x00077AC6` sind identisch zu SCCT-Speicherimplementierungen
- **Korrelierung**: Speicherschnittstellen werden auf ähnliche Weise gelöst, mit gleichen Funktionen zur Heap-Allozierung

### 4. TLS/Thread-Local Storage (sub_0005355F)
- **SCCT**: Die TLS-Initialisierung und Thread-Loak-Funktionen sind sehr ähnlich
- **SCDA**: Funktion `sub_0005355F` entspricht sehr eng den SCCT-TLS-Konzepten
- **Korrelierung**: Gleiche Kernel-Zugriffsmethoden, gleiche Dateisystem-Strukturen, gleiche Speicher-Semantik

## Wahrscheinlich

### 1. CRT-Mathematik-Funktionen (sub_00368324 → sub_0036D415)
- **SCCT**: Die XBE-Datei `sc3.xbe` enthält ähnliche FPU-Handler und CRT-Mathematische Funktionen
- **SCDA**: Der Stackverlust bei `sub_0036D415` ist direkt mit SCCT-Funktionen zu korrelieren
- **Korrelierung**: Diese mathematischen Helper-Funktionen sind von identischer Struktur und Implementierung

### 2. Dateisystem/Speicher-APIs
- **SCCT**: Der Zugriff auf `NtCreateFile` durch ähnliche XBE-Kernel-Imports ist identisch  
- **SCDA**: Verwendet gleiche Kernel-APIs mit gleichen Abfragen zu Dateien und Speicher
- **Korrelierung**: Die Bridge für Dateioperationen (z.B. `SND_fn_bFileNameExist`) zeigt ähnliche Funktionalität

### 3. Memory Manager
- **SCCT**: Identische `MmAllocateContiguousMemory`/`MmFreeContiguousMemory` Aufrufmuster
- **SCDA**: Die Speicherzuweisung und -freigabe Funktionen sind aufgrund der gleichen XBE-Kernels identisch  
- **Korrelierung**: Identische Speicherverwaltungen, gleiche Heap-Konfiguration

## Plausible Hypothesen

### 1. SEH-Prolog-Erkennung
- **SCCT**: Die prolog Erkennung in `0x0036613C` ist ähnlich zur SCDA Implementierung
- **SCDA**: Gleiches Pattern für dynamische SEH-Prologs in den CRT-Funktionen
- **Korrelierung**: Beide nutzen gleiche XDK-Konventionen zur Prologerkennung

### 2. x87-FPU Stack Management
- **SCCT**: Das x87-Stack-Management ist identisch mit SCDA
- **SCDA**: Die FPU-Spezifikationen `g_fp_stack` und `g_fp_top` sind auf Grundlage des gleichen XDKs
- **Korrelierung**: Gleiche Stack-Manipulationen in den CRT-Funktionen

### 3. Plattformabhängige Initialisierung 
- **SCCT**: Die Initialisierung im Launcher (`default.xbe`) zeigt ähnliche Konventionen
- **SCDA**: Identische Initialisierungssequenzen zur gleichen Zeit
- **Korrelierung**: Gleiche Kernel-API-Benutzung, gleiche Dateioperationen

## Ungeklärte Punkte

1. Die exakte Zuordnung von PC Exporten (z.B. `SND_fn_bFileNameExist`) zu SCDA-Funktionen wurde noch nicht durchgeführt
2. Es besteht keine direkte Zuordnung zwischen SCCT-Dateisystemaufrufen und SCDA-Funktionen
3. Die spezifische Implementierung des Stackverlusts in sub_0036D415 ist nicht exakt bekannt

## Aktueller Status

Basierend auf der statischen Analyse sind die folgenden Fehlerszenarien als besonders relevant:

1. **Stack Verlust in CRT-Funktionen** (`sub_0036D415`): Die SCCT Referenz zeigt, dass ähnliche CRT-Math-Funktionen auf gleiche Weise arbeiten und Probleme wie Stackverlust aufgrund unvollständiger FPU-Stack-Behandlung auftreten können.

2. **Korrekte SEH-Prolog-Erkennung**: Die Identifizierung einer möglichen fehlerhaften x87-Stack-Funktion in `sub_0036D415` hat direkten Einfluss auf die korrekte Erkennung des SEH-Prologs.

3. **Kritische Dateisystemaufrufe**: Die korrekte Implementierung von `sub_0005355F` und ähnlichen File-Init-Funktionen zeigt den gleichen Problem-Bereich wie SCCT.

Diese Zusammenhänge sollten bei der weiteren Fehlerbehebung von SCDA berücksichtigt werden.