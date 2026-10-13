# SCDA: statische Voruntersuchung fuer Koop per System Link

Stand: 09.10.2026, Branch upgrade/selective-upstream, CURRENT HANDOFF Update 652.
Nur Quellcode, Disassembly und entpackte Konfiguration gelesen. Kein Netzwerk-
oder Spieltest, kein Build und keine Aenderung an Runtime, XBE oder Bloom-Pfad.

## Gesicherte Befunde

1. Das Spiel trennt im Menue Xbox Live, System Link und Split Screen.
   SC4_xbe_hack/dynamic/DataXb/System/Localization/pregamemenus.int:63-65
   enthaelt MainTitleLan=System Link und MainTitleSplitscreen=Split Screen;
   Zeile 81 definiert NetworkInitFail=Network Not Available. Der sichtbare
   Fehlertext belegt allein noch nicht, welche Pruefung fehlgeschlagen ist.
   ProfileDefaultsXBOX.ini:32-48 listet eigene Koop-Karten; die erste
   x01_coop_ice_a.ini:11 setzt Multiplayer=true. XboxLiveContent=0 dort ist
   kein Beweis fuer oder gegen System-Link-Netzwerkfunktion.

2. Die XBE enthaelt umfangreiche eigene Netzwerkbibliotheken:
   tools/disasm/output/scda_v060/asm/XNET.asm (433 erkannte Funktionen,
   VA 0x00465AC0..0x004789C0) und XONLINE.asm (711 erkannte Funktionen,
   VA 0x004789C0..0x0049A1E4). Die Erkennung zaehlt statische Funktionen,
   nicht zur Laufzeit ausgefuehrte Pfade.

3. Ein konkreter Spiel-Startpfad ist nachweisbar: sub_002560DC ruft bei
   0x00256254 sub_003C5170. Diese Routine startet ueber sub_00465FE2
   den XNET-Pfad, ueber sub_00478F54 den XONLINE-Pfad und fragt ueber
   sub_00465FF9 Zustand ab (text.asm:955668,1561835-1561882).
   sub_001D64C9 fragt bei 0x001D64E8 sub_00050488 und speichert das
   Link-Bit in Objekt+0x48 (text.asm:741371-741389).
   sub_00050488 ruft PhyInitialize und PhyGetLinkState; das Ergebnis wird
   auf Bit 0 reduziert (text.asm:120963-121000). In XONLINE.asm:7006-7020
   bricht sub_0047D31B bei fehlendem Link-Bit frueh mit 0x80151000 ab.
   Ob genau dieser Pfad den vom Nutzer gesehenen Text ausloest, ist ohne
   Laufzeitbeleg offen.

4. Die Runtime-Funktionen existieren: src/kernel/kernel_xbox.c:290-301
   liefert fuer PhyGetLinkState konstant 0x01 und fuer PhyInitialize
   STATUS_SUCCESS. Die Thunk-Tabelle kennt Ordinals 252/253
   (src/kernel/kernel_thunks.c:291-292), ebenso ihre Argumentgroessen
   (kernel_bridge.c:5862-5863). In bridge_for_ordinal sind die beiden
   Eintraege jedoch auskommentiert (kernel_bridge.c:6207-6208).
   Ein nicht gebridgter Kernelaufruf liefert im Dispatcher 0
   (kernel_bridge.c:6395ff). Damit koennen die XBE-Aufrufe den vorhandenen
   Link-aktiv-Stub auf dem synthetischen Bridge-Pfad derzeit nicht erreichen.
   Dies ist ein statisch belegter Fehlerkandidat, nicht schon der ganze
   LAN-Fix. Auch ein verdrahteter 0x01-Rueckgabewert beweist keine IP-Adresse
   und uebertraegt keine Pakete.

5. XNET spricht die Xbox-NIC direkt an. In XNET.asm finden sich 63
   Textstellen mit 0xFEF0-Registeradressen; sub_0046CB5B initialisiert
   die PHY, sub_0046C033 liest den Link-Status, und der Startpfad schreibt
   NIC-Register und registriert einen Interrupt (XNET.asm:12455ff).
   src/kernel/xbox_memory_layout.c:2670ff bildet 0xFEF00000 derzeit
   als einfachen RAM ohne NIC-Registersemantik ab. Im SCDA-CMake-/Kernel-
   Quellpfad wurde kein Winsock-/Host-Netzwerkbackend gefunden.
   Folgerung: Die kompilierte XNET-Logik kann daraus nicht einfach eine
   funktionierende Verbindung zwischen zwei PC-Prozessen erhalten.

## Was fuer zwei eigenstaendige Spielbilder benoetigt wuerde

- Zwei getrennte SCDA-Prozesse auf zwei PCs (oder isolierte Instanzen),
  jeder mit eigenem Renderer, Eingabegeraet, Profil und Save-/Cache-Pfad.
  Es ist kein weiterer Splitscreen-Renderer notwendig.
- Korrekte Link-, Adapter- und Adresszustands-Semantik statt nur eines
  konstanten Ready-Bits. Zuerst den ersten echten Abbruch im System-Link-
  Menue belegen; nicht den Fehlertext pauschal wegpatchen.
- Ein Host-Netzwerktransport fuer Discovery, Session-Host/Join und
  Spielpakete. Zwei moegliche Architekturen: (a) die erwartete Xbox-NIC
  mit DMA/Interrupt und virtueller Ethernet-Anbindung nachbilden, damit
  XNET weitgehend unveraendert laeuft; oder (b) die verwendeten XNET-
  Grenzfunktionen gezielt auf PC-Sockets abbilden, einschliesslich der
  Xbox-spezifischen Adress-, Session- und Sicherheitssemantik. Welche
  Variante kleiner ist, haengt vom tatsaechlich genutzten API-Pfad ab.
- Fuer standortuebergreifendes Spiel ist kein dedizierter Zwischenserver
  als Spielhost zwingend aus diesem Befund ableitbar; ein Prozess kann die
  Sitzung hosten. Ein VPN allein garantiert aber keine sichtbaren
  Sitzungen: Discovery-Verfahren, Broadcast/Multicast-Verhalten, Firewall,
  Adressierung und Latenz muessen anhand echter Pakete geprueft werden.

## Offene Punkte und billigster spaeterer Test

OFFEN: Ob der vom Nutzer beobachtete Fehler bereits an Ordinal 252/253,
an XNET/NIC-Initialisierung, IP-Konfiguration oder einer spaeteren
Session-Suche entsteht. Ebenfalls offen: konkrete Paketformate/Ports,
ob Xbox Live fuer den gewaelten System-Link-Pfad ueberhaupt gebraucht wird,
und ob zwei Recomp-Instanzen denselben Save-/Cache-Bestand vertragen.

Bei Wiederaufnahme zuerst einen eng begrenzten, opt-in Zustands-Trace fuer
Ordinal 252/253, Rueckgaben von sub_003C5170 und sub_00478F54 sowie
sub_001D64C9 Objekt+0x48 im System-Link-Menue erfassen. Danach erst
einen isolierten Link-Bridge-Versuch; falls die Suche weiter scheitert,
die erste NIC-Register-/Paket-Anforderung belegen. Kein Ready-Bit- oder
Menue-Symptompatch ohne diesen Nachweis.
