# Idee fuer spaeter: Schnellstart und Level-Cache

Stand: 09.10.2026. Nur vorgemerkt; nichts implementiert.

- Ein zweiter Starter soll automatisch den zuletzt gespeicherten Spielstand
  laden und Menue/Intros umgehen. Die vorhandene Diagnose-Technik mit
  launch_data.bin kann als Ausgangspunkt dienen. Vor Umsetzung festlegen,
  welcher Save bei mehreren Profilen der letzte ist und wie bei fehlendem
  oder ungueltigem Save sicher ins normale Menue zurueckgefallen wird.
- Der Recomp implementiert gegenwaertig keine eigene Erststart-Kopie der
  Spieldateien in den Xbox-Titelcache. src/kernel/kernel_path.c bildet
  Z:-Dateizugriffe auf den Spielordner ab, weil die Xbox-Dashboard-Kopie
  von Disc nach Z: fehlt. Das Spiel selbst fuellt spaeter seinen Cache
  nach Bedarf; der aktuelle recomp_saves/Cache/Summary.txt nennt globale
  Sounddateien und LMaps fuer x05_hq2_r, nicht alle Level.
- Moegliche spaetere Erweiterung: alle Level im Cache vorhalten. Zuvor
  Cache-Manifest/Spiel-Invalidierung, Platzbedarf, Profilbindung und
  geaenderte Spieldateien pruefen. Dateien nicht einfach ungeprueft in
  den Cache kopieren: das Spiel verwaltet Summary.txt und seinen
  Level-spezifischen Inhalt selbst.

Diese Notiz aendert keinen Starter, Cache oder Spielstand.
