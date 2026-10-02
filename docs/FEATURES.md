# Fitzel — Feature-Übersicht

Stand: 2026-09-30. Die Details stehen in der [README](../README.md), die Lua-API in
[lua-scripting.md](lua-scripting.md).

Fitzel ist eine C++20/OpenGL-3.3-Engine mit Editor (`sandbox.exe`) und einem Player ohne
Editor (`player.exe`). Im Editor baut man ganze Welten (Landschaft, Straßen, Städte, Himmel
und Wetter), modelliert und animiert Objekte und schreibt das Spiel dazu in Lua;
„Export Game“ macht daraus ein eigenständiges Spiel. Die Beispiele reichen vom Rennspiel
über Shoot'em-up, Flipper und Sokoban bis zum Rhythmusspiel. Dazu kommen ein Pfadtracer
für Standbilder und ein Synthesizer für eigene Musik.

## Landschaft

- **Unendliches Terrain**, prozedural erzeugt und gestreamt. Es gibt Presets (Hügel,
  Alpen, Canyon, Mesa, Archipel, Fjorde) und Regler für Täler, Gipfel und Relief.
- **Sculpt-Pinsel**: heben, senken, glätten, ebnen, thermische Erosion, Regen-Erosion,
  Stempel (Kuppel, Krater, Gebirgszug …) und Carve für Täler.
- **Terrain-Texturen**: vier PBR-Layer, triplanar gemappt und nach Höhe und Hang
  gemischt. Dazu kommt ein Pinsel zum Bemalen von Hand.
- **Ferngebirge** bis zum Horizont.
- **Wasser**: ein See mit Gerstner-Wellen, Spiegelung, Refraktion und Schaum. Bäche und
  Flüsse entstehen aus Splines, samt Wasserfällen.
- **Vegetation**: Gras per GPU-Scatter in Tiles, Wälder nach Ökologie-Regeln bis zum
  Horizont, Impostors für die Ferne, Blumen, ein Pinsel zum Verstreuen von Objekten und
  Baumteile mit eigenen Materialien.
- **Baumgenerator**: baut Bäume und speichert sie als `.glb` im Projekt.
- **Belebte Landschaft**: Wind, Wildtiere, Herden, Vögel, Fische, Pollen und passende
  Umgebungsgeräusche.

## Straßen und Strecken

- **Straßen als Splines**, mit Kreuzungen, Brücken, Tunneln, Loopings, Querneigung,
  Unterführungen und Decals.
- **Straßenrand wird automatisch abgeleitet**: Leitplanken, Bordsteine und Pfosten.
- **Weitere Spline-Geometrie**: Zaun, Mauer, Gleis. Dazu ein Designer für freistehende
  Brücken.
- **Level-Generator**: baut aus einem Seed eine ganze Rennszene (Landschaft, Rundkurs,
  Renn-Logik, Dekoration).

## Städte

- **Stadtgenerator** mit den Presets Dorf, Kleinstadt, Stadt, Metropole und Vorort.
  - Die Straßen werden echte Roads, die man danach weiter bearbeiten kann.
  - Die Blöcke sind in Zonen eingeteilt: vom Hochhauskern bis zum Einfamilienhaus, dazu
    ein Gewerbegebiet, Parks und öffentliche Gebäude (Rathaus, Kirche, Schule, Feuerwehr,
    Krankenhaus, Bahnhof, ein Kraftwerk mit Strommasten).
- **Häuserschluchten** entlang der Strecken, abgeleitet aus Biom-Regeln.
- **Straßendetails**: Gehwege, Straßenschilder in DIN 1451 (auch mit echten Frankfurter
  Namen), Ampeln nach deutschem Ablauf, Bushaltestellen und Laternen, die bei Dämmerung
  angehen.
- **Verkehr**: Autos fahren nach dem Intelligent Driver Model, halten an Ampeln, Busse
  halten an den Stationen, Fußgänger laufen als Prefabs. Nahe beim Spieler bekommen die
  Autos echte Physik, es gibt Unfälle, und Wracks blockieren die Spur.

## Himmel, Wetter, Licht

- **Tag und Nacht**, volumetrische Wolken, ein Wolken-Layer-Stack (eine Ebene pro
  Wolkentyp) und Wolkenschatten.
- **Ein einziger Wetterwert von 0 bis 1** steuert Wolken, Nebel, Wellen, Blitze, Regen
  und die Wetter-Sounds. Dazu gibt es Wetter-Presets.
- **Nebel**: Höhennebel mit Luftperspektive, Dunst, der sich nach der Bewölkung färbt,
  und platzierbare Nebel-Volumen mit Lichtstrahlen.
- **Licht**: Sonne mit Kaskadenschatten, Punktlichter (16 aktiv, davon 4 mit Schatten),
  HDRI-Beleuchtung und ein gebackenes Lichtproben-Gitter.

## Rendering

- **Nachbearbeitung und Effekte**: HDR, Bloom, God Rays, Lens Flare, ACES und AgX, Color
  Grading, SSAO, SSR, TAA, GGX, Kontaktschatten.
- **Ansichten**: orthografisch per Num 5 und mehrere Shading-Modi.
- **Offline-Pfadtracer**: Tiefenschärfe, weiche Sonne, Speichern als EXR, Wälder mit über
  100.000 Bäumen per Instancing. Zusätzlich eine Vorschau mit dem GPU-Tracer im
  Viewport.
- **Performance**: Frustum-Culling, Occlusion Queries, Profiler mit Frame-Trace auf F4
  und GPU-Timer.

## Modellieren (Blender-Stil)

- **Modelling-Modus mit Tab**, Blender-Tasten und den Ebenen Vertex, Edge und Face.
- **Werkzeuge**: Extrude, Bevel, Inset, Loop Cut, Merge/Weld, Dissolve, Spin,
  Duplizieren (in einer Reihe oder entlang eines Splines) und ein UV-Panel.
- **Materialien pro Fläche**. Alle Grundformen lassen sich bearbeiten.
- **Modifier-Stack**: Subdivision Surface, Decimate, Wireframe, Array, Solidify.
- **Mesh-Paint**: Terrain-Layer auf Objekte malen. Dazu 3D-Cursor und Snapping.
- **Prozedurale Objekte (Mini-Houdini)**: View > Objects > Procedural. Ein Knoten-Graph
  aus Formen (Box, Tube/Kegel, Kugel, Torus, Grid), Kopien (radial, in Reihe, auf
  Punkte), Kombinieren (Merge, Transform, Mirror), Details (Panels, Extrude, Lattice,
  Thicken, Subdivide, Delete faces) und Material pro Flächenauswahl wird bei jeder
  Änderung in ein normales Mesh gekocht. Vorlagen „Ring station“ und „Modular
  station“, „Fuel depot“. Dazu 2D-Formen und Kurven (Circle, Rectangle, Curve mit
  Punktliste, Resample), Sweep (Profil entlang Kurve: Rohre, Gänge), Revolve (Drehkörper:
  Tanks, Kuppeln) sowie Punktauswahl als Filterkette (Select points nach Box, Kugel,
  Richtung, jedem n-ten, Zufall; ersetzen/hinzufügen/abziehen/schneiden; Delete points);
  die Auswahl steuert Copy onto points, Transform und den Flächenfilter. Kurven und
  ausgewählte Punkte werden im Viewport eingezeichnet.
  Prefab-Knoten setzen Prefabs des Projekts als echte Objekte unter das prozedurale
  Objekt (verschieb-, kopier- und auf Punkte setzbar; Undo schließt sie ein).
  Knoten wählt man per Klick, Shift+Klick oder Rahmen (A alle, Alt+A keine) und
  verschiebt sie gemeinsam; Entf löscht Knoten, nie Szenen-Objekte; Shift+A bzw.
  Rechtsklick öffnet das Add-Menü mit Suche an der Mausposition, Rechtsklick auf einen
  Knoten dessen Menü; mittlere Maustaste schwenkt.
  Verdrahtet wird von Hand (Ziehen von Punkt zu Punkt oder zwei Klicks);
  die Knoten ordnen sich selbst an, bis man einen verschiebt (Ziehen mit Raster oder
  „Move“ + Klick), „Arrange“ ordnet neu. Zahlen per Stepper oder getippt.

## Animation und Kamera

- **Timeline**: jedes Feld im Inspector kann Keyframes bekommen.
- **Animations-Graph** (Zustandsautomat): spielt Modell-Clips, blendet zwischen ihnen
  über, kennt negatives Tempo und wird per Lua gesteuert.
- **Kameras**: Verfolger, Cockpit, Kamera-Wechsler, Kamerapfade, Split-Screen.
- **Multishot-Kamera** für Werbe- und Replay-Shots, darunter Dolly Zoom und Fly-by.
- **Orbit mit der Maus** und `walker.lua` als Third-Person-Steuerung.

## Gameplay

- **Physik** mit Jolt, dazu Stoff und Softbodies (Vorhang, Flagge).
- **Figuren mit Kollision**: `game.moveCharacter` lässt eine Figur als Kapsel durch die
  Welt laufen – auf Straßen und Brücken statt durch sie hindurch, vor Wänden stehen
  bleibend, Bordsteine hinauf und über Kanten hinab. Auf Gelände stehen die Füße auf
  dem gezeichneten Boden. `walker.lua` nutzt das.
- **Fahrzeuge**: ein Auto und ein Gleiter mit festem 120-Hz-Takt, Reifenspuren,
  Kondensstreifen und Gischt.
- **Renn-Baukasten**: Gegner-KI, Startaufstellung, Checkpoints, Boost-Pads, HUD,
  Bestenliste, Schwierigkeitsstufen, Showroom als Startmenü.
- **Waffen**: Lock-on-Raketen.
- **Fertige Komponenten**: Trigger, Szenenwechsel, Mover, Spawner, Lift, Tür,
  Sammelobjekte, Partikel.
- **Lua-Scripting**: mehrere Skripte pro Objekt, bei jedem Play eine frische VM. Dazu ein
  Skript-Editor mit Autovervollständigung, ein UI-Overlay (Text, Buttons, Bilder) und
  Spielstände.
- **Beispielspiele**: SKYSTRIKE (Shoot'em-up mit Hangar und High Valley), Sokoban,
  Arkanoid, Invaders, Pinball, Dosenschießen, Rhythmicker-Port.
- **Schach gegen eine Engine** (`chess.lua`): Brett und Figuren aus der Szene gelesen,
  per Klick gespielt, Engine um 1250 Elo (geschätzt); dafür neu in Lua: `game.mousePos`,
  `game.mouseRay`, `game.showCursor`, `game.clone`, `game.rest` (wartet das Spiel nur
  auf den nächsten Klick, zeichnet Play höchstens 10 Bilder pro Sekunde statt voller Rate).

## Audio

- **Grundlage**: 3D-Soundquellen, ein Mixer, Motorsounds für Auto und Gleiter,
  Wetter-Sounds.
- **Modularer Synthesizer** mit MIDI-Player und `synth.*` in Lua.
- **Musik-API** mit Songuhr, Filtern und Analyse (für Rhythmusspiele).

## Editor und Assets

- **Panels**: Hierarchie, Inspector, Asset-Browser mit Drag & Drop in die Szene,
  Materialien, Prefabs (Stufe 1, Vorlagen).
- **Import**: Unity-FBX und Videos.
- **Hierarchie im Gizmo**: Kinder folgen dem Elternteil beim Verschieben, Drehen und
  Skalieren -- ein importiertes Modell wächst an seiner Wurzel als Ganzes.
- **Komfort**: Undo, Autosave und eine Toolbar mit Piktogrammen.
- **Bedienung ohne ruhige Hand**: alles lässt sich auch klicken und eintippen statt
  ziehen.

## Auslieferung

- **„Export Game“** erzeugt einen spielbaren Ordner: `player.exe`, verschlüsselten
  Content im `.fpak`, Ladebildschirm, eingebautes Icon, Start-Modus (zu Fuß, Auto,
  Gleiter, Attract-Screen) und optional einen Inno-Setup-Installer.
- **Absicherung**: rund 50 Prüfprogramme ohne Fenster, `check-all.bat`, ein ASan-Build
  und der Shot-Harness.

## Offen oder angefangen

- Prefabs Stufe 2 (verknüpfte Instanzen, Overrides).
- GPU-Tracer: HDRI und Tiefenschärfe fehlen noch.
- Schatten: die ferne Kaskade nur jeden zweiten Frame rendern.
- Dunst und Himmel bei Nacht.
- Abhängigkeiten auf feste Versionen pinnen.
- GitHub Pages für `docs/index.html` einschalten.
