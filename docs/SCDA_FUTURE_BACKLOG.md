# SCDA: vom Nutzer vorgemerkte spätere Baustellen

Stand: 2026-10-03. Diese Liste dokumentiert Wünsche und Beobachtungen des
Nutzers, **keine verifizierten Ursachen oder zugesagten Implementierungen**.
Noch keine Untersuchung oder Änderung zu diesen Punkten beginnen; zuerst den
laufenden F7-Lichttest auswerten. Reihenfolge und Machbarkeit sind offen.

- Missions-BIK beim ersten Laden: Video stockt stark, während Leveldaten
  erstmals in den HDD-Cache gelangen; nach erneutem Laden mit vorhandenen
  Cachedaten läuft es nach Nutzerbeobachtung normal.
- Intro-Videos überspringbar machen.
- Ersten Interaktionsbildschirm (sinngemäß „Drücke Start“ und anschließender
  Spiel-Intro-Trailer) überspringbar machen. Laut Nutzer geht das derzeit erst,
  wenn das Spiel mindestens einmal den Cache geschrieben hat; Zusammenhang
  später prüfen.
- Render-Distanz erhöhen; derzeit ist sichtbarer Nebel vorhanden.
- Bloom deaktivieren und nach Möglichkeit durch einen ShaderToggler-ähnlichen
  Effekt ersetzen. Lizenzlage prüfen; nur rechtlich passende Open-Source-
  Komponenten verwenden.
- Pre-Release-Intro-Musik aus vorhandener Pre-Release-Xbox-Version einbauen;
  alternativ eine vom Nutzer bereitgestellte WAV statt der bisherigen Musik
  abspielen. Rechte und technische Einbindung später klären.
- Höher aufgelöste Texturen für SC-20K und FN Five-seveN prüfen. Vom Nutzer
  genannte Referenz: <https://www.nexusmods.com/splintercellchaostheory/mods/25?tab=description>.
  Kompatibilität und Nutzungsrechte später prüfen.
- BIK-Videos durch KI-hochskalierte Varianten ersetzen; Nutzer verweist auf
  eine entsprechende Möglichkeit beim PC-Port. Format, Rechte und Einbindung
  sind noch ungeprüft.
- First-Person-Modus nur beim Zielen; laut Nutzer derzeit in
  `DynamicXbox.umd` aktiviert.
- Fly-Mod; laut Nutzer derzeit ebenfalls in `DynamicXbox.umd` aktiviert.
- Musikstottern im Othosk-Level kurz vor dem Angriff einer Wache untersuchen.
- Koop-Modus:
  - 60 FPS auch im Koop: der bekannte IPS-Wechsel von `2` auf `1` greift
    nach Nutzerbeobachtung zumindest in xemu nicht.
  - Splitscreen nach Möglichkeit in zwei Fenster aufteilen.
  - Zweiten Controller unterstützen; derzeit nach Nutzerbeobachtung nicht
    funktionsfähig. Perspektive: zwei Controller, zwei Fenster/Monitore an
    einem PC.
- Xbox-Surround-Ausgabe über S/PDIF: klären, ob sich vergleichbare
  Surround-Ausgabe unter Windows über Toslink bereitstellen lässt.

Aktuell maßgeblicher Test und dessen Befunde stehen im `CURRENT HANDOFF` von
`PROJECT_STATUS.md`.

## JBA (x05_hq2_r): Papierzettel am Boden mit weissen Flecken (Nutzerbericht 05.10.2026)

Screenshots: `docs/bug_screenshots/jba_paper_flicker_1.png`, `_2.png`.
Papierstuecke auf dem Boden bekommen an einer Stelle (oft Ecke) einen
weissen Fleck, der sich mit Blickwinkel/Bewegung aendert, im Stillstand
meist verschwindet, selten auch im Stillstand bleibt. Tritt auch ohne
VK-Fastpaths auf (A/B Update ~615). Noch NICHT bearbeitet.
Vermutungen (ungeprueft): (1) Glanzlicht/Specular falsch berechnet
(Vertex-Programm LIT/Saettigung oder Final-Combiner Specular-Addition),
(2) Textur-Wrap/Clamp oder Mip-Auswahl bei Zettel-Decals (Atlas-Nachbar
wird gesampelt), (3) Z-Fighting/Depth-Bias zwischen Decal und Boden mit
additivem Blending. Erster Schritt: gleiche Stelle in xemu als Referenz,
dann Draw-Call des Zettels (Combiner/Textur-State) recomp vs. xemu diffen.
- (2026-10-09) Koop/Splitscreen: schaltet Spieler links die Nachtsicht ein, wird
  das Bild rechts heller; ausserdem leichte helle Umrandung um die Koepfe der
  Spieler. Moeglicherweise auch auf Originalhardware so; vermutlich Bloom-/
  Helligkeitsanpassung, die beide Bildhaelften teilen. Nutzer hat dafuer einen
  Koop-Spielstand angelegt (vermutlich Slot 1) -- nicht veraendern.
- (2026-10-09) Grafikfehler "Zacken aus der Wand": an einer Wand (HQ-Karte, Naehe Rohr/
  Halterung oben) ragt ein dunkler Zacken/Stab aus der Wand. Nutzer-Screenshot liegt in
  `scratchpad/bloom_20261009/` (neuestes Bild vom 09.10. abends). Nach dem Bloom-Thema ansehen.
