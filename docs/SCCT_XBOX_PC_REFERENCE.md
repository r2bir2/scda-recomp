# Statische Referenzanalyse: Splinter Cell Chaos Theory Xbox / PC

Stand: 25. August 2026

## Zweck und Grenzen

Diese Analyse vergleicht statisch die Xbox- und PC-Binaries von **Tom Clancy's
Splinter Cell: Chaos Theory** (SCCT) und ordnet belastbare Gemeinsamkeiten zur
bereits untersuchten Original-Xbox-Version von **Splinter Cell: Double Agent**
(SCDA) ein.

Keine untersuchte EXE oder XBE wurde ausgeführt. Aus den Ergebnissen wurden
keine Änderungen an der SCDA-Runtime, am Recompiler oder an generiertem
Double-Agent-Code abgeleitet. Funktionsnamen der PC-Version sind nur dort
verwendet, wo sie als echte PE-Exporte oder eingebettete Strings vorliegen.
Die Xbox-XBEs sind weitgehend symbolbereinigt; es wird daher keine
Adressgleichheit zwischen Plattformen behauptet.

## Untersuchte Dateien und Hashes

| Plattform/Rolle | Datei | Größe | SHA-256 |
|---|---|---:|---|
| Xbox Retail-Launcher | `references/xboxversion/default.xbe` | 4.444.160 Bytes | `B0CE97C7201DF59E171B2396F6630CD815DF0F01EA0375863C64E4DABADC3155` |
| Xbox Hauptspiel | `references/xboxversion/sc3.xbe` | 5.718.016 Bytes | `C4EB3352CBF1815EA3369C5AA8961EAD8E862DA4FC58DB9119F2A3D38798D0C1` |
| PC, StarForce-geschützt | `references/pcversion/splintercell3 backup.exe` | 25.724.416 Bytes | `F3959BFFB35E3CE47C1D7D042CB5F730F75A39C8DAB1977EB43C757E1C0D0DDF` |
| PC, statisch auswertbarer Patch-Build | `references/pcversion/splintercell3.exe` | 9.867.264 Bytes | `D56403F99FB0A09802BC71DF8970F9A2F42FC1FC4017574CE86FDE286AEF2161` |

`sc3_adv.xbe` wurde wie beauftragt nicht in die Vergleichsanalyse einbezogen.

## Auswahl der PC-Datei

Die bevorzugte `splintercell3 backup.exe` wurde zuerst geprüft, ist aber für
einen direkten statischen Codevergleich ungeeignet:

- PE-Zeitstempel `0` und ungewöhnliche Linkerkennung `75.24`;
- Sektionen `.sforce3`, mehrere `.brick`, `.start` und eine rund 16-MiB große
  `.vdata`;
- Image Base `0x10900000`, aber Einstieg bei RVA `0x00A6A000` in einer
  ausführbaren `.idata`-Sektion;
- fast alle Sektionen sind gleichzeitig schreibbar und ausführbar;
- die Importtabelle enthält überwiegend nur je eine Stellvertreterfunktion pro
  DLL, während `LoadLibraryA`/`GetProcAddress` dynamische Auflösung erlauben.

Das ist ein klarer statischer Pack-/DRM-Befund, ohne dass der Schutz gestartet
werden musste. Die darin sichtbaren Bibliotheksnamen bestätigen zwar D3D9,
DirectInput, DirectSound/EAX, Bink und Winsock, der eigentliche Spielcode ist
aber nicht zuverlässig als normales PE abgrenzbar.

Für den inhaltlichen Vergleich wurde deshalb `splintercell3.exe` verwendet.
Sie besitzt normale `.text`, `.rdata`, `.data`, `.rsrc` und `.reloc`-Sektionen,
eine vollständige Importtabelle und 150 benannte Exporte. Der PE-Zeitstempel
ist 6. September 2005. Der eingebettete PDB-Pfad lautet:

```text
d:\build\splintercellx-pc\patch\SplinterCell2\Launch\Src\
Retail Static NoMassive\SplinterCell3.pdb
```

Damit ist sie ein späterer PC-Patch-Build und nicht derselbe Buildstand wie die
Xbox-Retail-XBEs vom Februar 2005. Sie ist dennoch die belastbare statische
Referenz für Engine- und Plattformabstraktionen.

## Rolle der beiden Xbox-XBEs

### `default.xbe`: Retail-Launcher/Dispatcher

Sicher bzw. hoch wahrscheinlich:

- Zertifikatstitel: `Splinter Cell-CT vs`
- Title ID: `0x55530041`
- Buildzeit: 22. Februar 2005, 01:04:01 UTC
- Debugpfad:
  `...\SplinterCell2\lib\xbox\retail_launcher\default.exe`
- eingebettete Startziele:
  `D:\dl.xbe`, `D:\sc3_adv.xbe` und `D:\sc3.xbe`
- eingebettete Launcherzustände wie `UnLaunchDataManager`, `DOWNLOADER` und
  `?MENUPAGE=%i`

`default.xbe` ist damit nicht die primäre Kampagnen-XBE, sondern ein
Retail-Einstieg, der anhand von Launchdaten beziehungsweise Menüauswahl in
Downloader, adversarial/Versus oder Hauptspiel verzweigt. Er ist mit rund
3,0 MiB `.text` trotzdem kein winziger Stub: Er enthält erhebliche gemeinsam
gelinkte Engine-, Audio-, Netzwerk- und Xbox-SDK-Anteile.

### `sc3.xbe`: Hauptspiel

Sicher bzw. hoch wahrscheinlich:

- Zertifikatstitel: `Splinter Cell-CT`
- Title ID: `0x55530038`
- Buildzeit: 22. Februar 2005, 01:00:09 UTC
- Debugpfad:
  `...\SplinterCell2\lib\xbox\retail\sc3.exe`
- `.text`: 4.053.800 Bytes gegenüber 3.010.628 Bytes im Launcher
- 156 Kernel-Imports gegenüber 146 im Launcher
- zusätzlich gelinkte C++-Runtime `LIBCPMT`
- wesentlich mehr erkannte Funktionen und Engine-Strings

`sc3.xbe` ist die eigentliche Retail-Spiel-XBE für den Haupttitel
einschließlich Singleplayer und der in dieser Codebasis enthaltenen
Coop-/gemeinsamen Netzwerkanteile. Der getrennte adversarial/Versus-Titel
`sc3_adv.xbe` bleibt außerhalb dieser Analyse.

## Reproduzierbare Strukturkennzahlen

Die Xbox-Dateien wurden mit dem lokalen XBE-Parser und Disassembler aus diesem
Repository analysiert. Für beide Disassemblies wurden `.text` und `XONLINE`
einbezogen, aber keine projektspezifischen Function Seeds verwendet.

| Merkmal | `default.xbe` | `sc3.xbe` | SCDA `default.xbe` zum Vergleich |
|---|---:|---:|---:|
| XDK | 5849 | 5849 | 5849 |
| XBE-Sektionen | 34 | 34 | 34 |
| Kernel-Imports | 146 | 156 | 157 |
| `.text`-Größe | 3.010.628 | 4.053.800 | 4.541.096 Bytes |
| Instruktionen mit `XONLINE` | 1.033.547 | 1.388.998 | 1.551.018 |
| heuristisch erkannte Funktionen | 13.468 | 19.550 | 28.438 mit SCDA-Seeds |
| Xrefs | 162.751 | 264.193 | 297.449 |
| erreichbare Instruktionen | 83,0 % | 83,2 % | 93,1 % mit SCDA-Seeds |

Die unterschiedlichen Funktionszahlen sind nicht direkt als Quellcodeumfang
zu lesen: SCDA verwendet einen stark erweiterten Seed-Satz, die SCCT-Zahlen
sind ungefütterte Vergleichsläufe.

## Sicher oder hoch wahrscheinlich identifizierte Zusammenhänge

### Gemeinsame Engine-Codebasis

Der Stringvergleich extrahierte ASCII- und UTF-16-Strings ab fünf Zeichen:

- `sc3.xbe`: 13.880 eindeutige Strings
- PC `splintercell3.exe`: 28.348 eindeutige Strings
- davon 5.987 exakt gemeinsam
- SCDA-XBE: 15.040 eindeutige Strings
- davon 10.804 exakt mit `sc3.xbe` gemeinsam

Die absoluten Zahlen enthalten auch CRT- und SDK-Texte. Die inhaltliche
Schnittmenge umfasst jedoch eindeutig projektspezifische Klassen und Systeme:

- Unreal-/Echelon-Schicht: `UGameEngine`, `UEngine`, `AEchelonGameInfo`,
  `AEchelonLevelInfo`, `EchelonAI`, `EchelonIngredient`, `EchelonMenus`;
- Rendering: `FD3DTexture`, `FD3DVertexStream`, `FD3DIndexBuffer`,
  `FStaticVolumeTexture`, `UPixelShaderMaterial`, `UseHWShadowMapping`;
- DARE-Audio: identische Fehlertexte, Ressourcentypen, Event-/Bank-/Map-Loader,
  Streaming-, Volumetric- und Sound3D-Texte;
- Physik: Havok-Welten, Constraints und Speicherdiagnosen;
- Eingabe: `PlayerInput`, `EInputKey`, Record-/Playback-Input, Controllerachsen;
- Dateisystem/Config: `FConfigCacheIni`, `FFileStream`, `SavePath`,
  `CachePath`, `%sMaps\%s.ini`, `Default.ini`, `User.ini`;
- Save/Profile/Menu und gemeinsame Netzwerk-/NetZ-Begriffe.

Mindestens acht eingebettete DARE-Quellpfade sind nach Entfernen des
plattformabhängigen Build-Präfixes identisch, darunter `snddata.c`,
`sndldbin.c`, `sndldbnk.c`, `sndldmap.c`, `sndthememanager.cpp`, `sound3d.c`
und `toolarch\managers\sndmanager.cpp`. Das belegt mehr als nur ähnliche
Konfigurationen: Xbox und PC enthalten Varianten derselben Audioquellen.

### PC-Exporte als semantischer Wegweiser

Der auswertbare PC-Build exportiert 150 Namen:

- 127 `SND_`-/`snd_`-Funktionen der DARE-Audioschicht;
- 20 klar benannte Engine-/Gameplayfunktionen;
- drei Thread-/Critical-Section-Helfer der Soundtools.

Besonders relevante Exporte sind:

```text
ULevel::HavokCreateWorld
UGameEngine::InitMultiplayerSession
UGameEngine::LocalInitAudio
UGameEngine::SEC_LoadGame
UGameEngine::SEC_LoadMap
UGameEngine::SEC_LoadMenus
UGameEngine::SEC_SaveGame
UDareAudioSubsystem::SEC_InitSound
SND_fn_bFileNameExist
SND_fn_hOpenFileReadSnd
SND_fn_pvMallocSnd
SND_fn_vInitThreadSnd
SND_fn_vResolveFileName
```

Diese Namen sind keine automatische Adresszuordnung zur Xbox-XBE, liefern
aber belastbare semantische Anker für spätere manuelle Cross-References über
gemeinsame Strings, Callgraphen und Datenstrukturen.

### Xbox-SCCT und Xbox-SCDA sind außergewöhnlich eng verwandt

`sc3.xbe` und die SCDA-XBE haben 156 Kernel-Importordinals gemeinsam. SCDA
ergänzt lediglich Ordinal 270 (`RtlCompareString`). Auch die häufigsten
statischen Kernel-Callstellen sind nahezu gleich verteilt, beispielsweise:

| Import | SCCT `sc3.xbe` | SCDA |
|---|---:|---:|
| `RtlLeaveCriticalSection` | 151 | 161 |
| `RtlEnterCriticalSection` | 34 | 33 |
| `NtClose` | 28 | 28 |
| `RtlInitAnsiString` | 16 | 16 |
| `NtOpenFile` | 12 | 12 |
| `NtQueryInformationFile` | 10 | 10 |
| `NtWriteFile` | 10 | 10 |
| `NtCreateFile` | 8 | 8 |

Beide verwenden XDK 5849 und dieselbe 34-Sektionen-Grundstruktur. Die
Sektionen `DOLBY`, `dsstdfx` und `.data1` sind zwischen SCCT und SCDA sogar
byteidentisch. Viele weitere Xbox-SDK-Sektionen haben identische oder fast
identische Größen, aber wegen Adressen, Linkreihenfolge oder kleiner
Versionsunterschiede andere Hashes.

Das ist ein sehr starker Beleg dafür, dass SCDA nicht nur konzeptionell,
sondern auch binär aus derselben Xbox-Engine- und Middleware-Linie hervorgeht.

## Plattformersetzungen nach Subsystem

Die folgende Tabelle trennt gemeinsam gebliebene Engine-Semantik von der
offiziell ersetzten Plattformimplementierung.

| Subsystem | Xbox-SCCT | PC-SCCT | Belastbare Interpretation |
|---|---|---|---|
| Rendering | statisch gelinktes `D3D8`, `D3DX8`, `XGRPH`; Xbox-NV2A-Strings; Xbox-spezifische D3D-Sektionen | `d3d9.dll!Direct3DCreate9`, zusätzlich `ddraw.dll`; normale Win32-Fenster-/GDI-APIs | Unreal/Echelon-Material-, Shader-, Texture- und Renderklassen bleiben, der Low-Level-Renderer ist D3D8/NV2A gegen D3D9/Win32 ausgetauscht. |
| Audio | statisch gelinktes Xbox `DSOUND`, `XVOICE`, `VOICMAIL`, `DOLBY`, `dsstdfx`; Xbox-DARE-Renderer und Controller-/Headsetfunktionen | `DSOUND.dll`, `EAX.DLL`, `WINMM.dll`; Bink DirectSound; umfangreiche EAX-Listener-/Bufferpfade | DARE-Ressourcen, Events, Banks, Streaming und 3D-Audio sind weitgehend gemeinsam; Geräteerzeugung, Effektpfad, Timer und Puffer-Backend sind plattformspezifisch. |
| Eingabe | Xbox-Controllerachsen, Controller-IDs, Xbox-Inputpfade; Xbox-APIs statisch gelinkt | `DINPUT8.dll!DirectInput8Create` plus USER32-Tastatur-, Maus-, Cursor- und Window-Message-APIs | `UInput`/`PlayerInput` und Bindings bleiben, das Gerätebackend wird von Xbox-Controllern auf DirectInput/Win32 ersetzt. |
| Dateisystem | `NtCreateFile`, `NtOpenFile`, `NtReadFile`, `NtWriteFile`, `NtQuery*`; Gerätepfade `\Device\Cdrom0`, `\Device\Harddisk0`, Laufwerke `D:`, `T:`, `U:`, `Y:`, `Z:` | `CreateFileA/W`, `ReadFile`, `WriteFile`, `SetFilePointer`, `FindFirst/NextFile`, `GetFileAttributes`, Verzeichnis- und Profil-APIs | Höhere `FFileStream`-, Config-, Cache-, Map-, Save- und DARE-Dateisemantik bleibt. Handle-/Pfad-/Objektattributlogik ist komplett an Win32 angepasst. |
| Speicher | `NtAllocateVirtualMemory`, `NtFreeVirtualMemory`, `NtQueryVirtualMemory`, `MmAllocateContiguousMemory*`, `MmQuery*` | `VirtualAlloc/Free/Query/Protect`, `HeapCreate/Alloc/ReAlloc/Free/Size`, `Global/LocalAlloc` | Engine-Allokatoren wie `CEvilMemoryManager` bleiben erkennbar, delegieren aber an andere OS-/Hardwareprimitive. Xbox-GPU-/contiguous-memory-Semantik hat keinen direkten PC-Gegenpart. |
| Threads/Synchronisation | `PsCreateSystemThreadEx`, `PsTerminateSystemThread`, `KeWait*`, `KeSetEvent`, DPC/Timer/IRQL und `Rtl*CriticalSection` | `CreateThread`, `ExitThread`, `CreateEvent`, `WaitForSingle/MultipleObjects`, Mutex, Interlocked, TLS und Win32 Critical Sections | Worker-, Streaming-, Sound- und Transport-Threads bleiben als Enginekonzept; Erzeugung, Warten, TLS und Prioritäten werden durch Win32 ersetzt. DPC/IRQL bleibt Xbox-only. |
| Netzwerk/Online | statisch gelinktes `XNET`, `XONLINE`, `XVOICE`; Xbox-Live-Services, Dash-/Title-Launchpfade und Xbox-Kryptoimporte | umfangreiche `WS2_32`-Winsock-Imports; keine Xbox-Kernel-/Dashboardsemantik | NetZ-/Session-/Lobby-Logik ist teilweise gemeinsam, Transport, Login, Voice, Dashboard und Plattformdienste sind ersetzt. |
| Prozess/Plattform | `HalReturnToFirmware`, `LaunchDataPage`, `XeLoadSection`, Dashboard-XBEs, Xbox-Zertifikat/Title ID | `CreateProcess`, `ExitProcess`, Registry, Shell, Windows-Verzeichnisse, Message Loop | Launcher, Patch-/Downloader- und Neustartverhalten benötigen auf PC eigenständige Prozess- und UI-Implementierungen. |
| Video | statische Xbox-Bink-Sektionen | `binkw32.dll` mit D3D9-/DirectSound-Funktionen | Bink bleibt Middleware, wird aber auf Xbox statisch und auf PC dynamisch/plattformspezifisch eingebunden. |

## Für das SCDA-Debugging wahrscheinlich nützliche Erkenntnisse

### Sicher bzw. hoch wahrscheinlich nützlich

1. **Kernel-Bridge-Priorisierung:** Die praktisch identischen 156
   Importordinals und sehr ähnlichen Callstellenzahlen zeigen, dass korrektes
   SCCT-Xbox-Verhalten für Datei-, Speicher-, Thread- und Synchronisationscalls
   eine starke Referenz für SCDA ist.
2. **Dateipfade als Plattformgrenze:** Xbox-Gerätepfade und Win32-Pfade sind
   nur Backenddetails unter gemeinsamen höheren Datei-/Config-/DARE-Systemen.
   Beim aktuellen SCDA-Stopp in `sub_0005355F` sollten daher zuerst der
   erzeugte ANSI-Pfad, `OBJECT_ATTRIBUTES`, Create-Disposition und die
   Rückkehrsemantik der `NtCreateFile`-Bridge geprüft werden.
3. **PC-Export `SND_fn_bFileNameExist`:** Der offizielle PC-Build bestätigt,
   dass mindestens die gemeinsame DARE-Schicht eine explizite
   Dateiexistenzabstraktion besitzt. Ihre Win32-Implementierung kann später als
   semantische Referenz dienen, ohne PC-Code in die Xbox-Runtime zu kopieren.
4. **DARE ist besonders gut kartierbar:** 127 benannte PC-Audioexporte,
   identische Quellpfade und viele gemeinsame Diagnose-Strings ermöglichen
   eine spätere, gezielte Zuordnung von Xbox-Audiofunktionen.
5. **Rendering nicht mit API-Gleichheit verwechseln:** Gemeinsame Unreal-/
   Echelon-Klassen bedeuten nicht, dass D3D9-Verhalten direkt auf Xbox-D3D8/
   NV2A übertragen werden darf. Nützlich sind Zustandsmaschinen und
   Ressourcenlebenszyklen, nicht rohe API-Aufrufe.

### Plausible Hypothesen

- `sub_0005355F` in SCDA könnte zu einer allgemeinen Xbox-Dateiexistenzprüfung
  gehören, die auf PC durch `CreateFile`/`GetFileAttributes` oder eine Funktion
  wie `SND_fn_bFileNameExist` umgesetzt wird. Die derzeitigen Daten beweisen
  keine konkrete Funktionsgleichheit.
- Die sehr ähnlichen Xbox-Kernel-Callprofile sprechen dafür, dass einige
  unbekannte SCDA-Funktionen durch Callgraph-/Stringvergleich mit `sc3.xbe`
  semantisch benannt werden können, auch wenn Compilerlayout und Adressen
  abweichen.
- Die PC-Exporte für `SEC_LoadMap`, `SEC_LoadGame`, `SEC_SaveGame` und
  `LocalInitAudio` könnten später Anker liefern, um große SCDA-Initialisierer
  in Enginephasen zu zerlegen.
- Die normalen Win32-Rückgabepfade können helfen, erwartete Erfolg-/Fehlerwerte
  einer Xbox-Bridge zu verstehen. Sie ersetzen aber keine Prüfung des Xbox-ABI
  und der Xbox-NTSTATUS-Konventionen.

## Ungeklärte Punkte

- Es besteht noch keine belastbare Adresszuordnung zwischen PC-Exporten und
  Funktionen in `sc3.xbe` oder SCDA.
- Der genaue Entscheidungsbaum des Launchers zwischen `dl.xbe`, `sc3.xbe` und
  `sc3_adv.xbe` wurde nicht vollständig disassembliert; seine Dispatcherrolle
  ist jedoch durch Buildpfad und Zielstrings gesichert.
- Die StarForce-EXE wurde nicht entpackt. Es ist nicht bewiesen, welcher
  ungepackte Codebestand exakt unter ihrer Schutzschicht liegt.
- PC-Patch-Build (September 2005), Xbox-Retail-Build (Februar 2005) und
  SCDA-Build (August 2006) unterscheiden sich zeitlich und wahrscheinlich in
  Optimierung, Bugfixes und Featureflags.
- Exakte Stringüberschneidungen enthalten neben Enginecode auch CRT,
  Middleware und Xbox-SDK. Einzelne Strings allein beweisen keine
  Funktionsgleichheit.
- Für robuste Funktionsmatches wären später normalisierte Kontrollflussgraphen,
  Konstanten-/String-Xrefs und Callgraph-Signaturen sinnvoll. Eine solche
  Zuordnung war nicht Teil dieser ersten statischen Referenzanalyse.

## Lokale Analyseartefakte

Reproduzierbare, ignorierte Zwischenstände liegen unter
`build/reference_analysis`:

- `scct_default_xbe.json`
- `scct_sc3_xbe.json`
- `disasm_default/`
- `disasm_sc3/`
- kleine Hilfsskripte für String-, Quellpfad- und Sektionshashvergleiche

Diese Artefakte enthalten keine Änderungen am Double-Agent-Projektcode.
