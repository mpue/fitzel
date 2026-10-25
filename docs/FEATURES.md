# Fitzel — Feature-Übersicht

Stand: 2026-10-16. Die Details stehen in der [README](../README.md), die Lua-API in
[lua-scripting.md](lua-scripting.md), die C#-API in [csharp-scripting.md](csharp-scripting.md).

Fitzel ist eine C++20/OpenGL-3.3-Engine mit Editor (`sandbox.exe`) und einem Player ohne
Editor (`player.exe`). Im Editor baut man ganze Welten (Landschaft, Straßen, Städte, Himmel
und Wetter, offenes Meer), modelliert und animiert Objekte und schreibt das Spiel dazu in Lua
oder C#; „Export Game“ macht daraus ein eigenständiges Spiel für Windows oder Linux, „Export
for Web“ eines für den Browser. Die Beispiele reichen vom Rennspiel
über Shoot'em-up, Flipper und Sokoban bis zum Rhythmusspiel, einem Doom-Klon (DARKNESS) und
einem Echtzeitstrategiespiel (STEELWARS). Dazu kommen ein Pfadtracer
für Standbilder und ein Synthesizer für eigene Musik.

## Landschaft

- **Unendliches Terrain**, prozedural erzeugt und gestreamt. Es gibt Presets (Hügel,
  Alpen, Canyon, Mesa, Archipel, Fjorde) und Regler für Täler, Gipfel und Relief.
- **Sculpt-Pinsel**: heben, senken, glätten, ebnen, thermische Erosion, Regen-Erosion,
  Stempel (Kuppel, Krater, Gebirgszug …) und Carve für Täler.
- **Terrain-Texturen**: vier PBR-Layer, triplanar gemappt und nach Höhe und Hang
  gemischt. Dazu kommt ein Pinsel zum Bemalen von Hand.
- **Ferngebirge** bis zum Horizont.
- **Offenes Meer**: ein FFT-Ozean nach Tessendorf aus einem JONSWAP-Spektrum, eingestellt
  über Windgeschwindigkeit (mit Beaufort), Richtung und Fetch, dazu Dünung und
  Kabbeligkeit. Drei Kaskaden (953/117/15 m) rechnet ein Worker-Thread auf der CPU – was
  schwimmt, liest genau die gezeichnete Oberfläche, auch im Browser. Ringgitter bis 20 km,
  Normalen pro Pixel mit GGX-Glanz, Durchleuchten der Kämme, Schaum, wo Kämme brechen,
  Spiegelung und Brechung in einem Bruchteil der Bildgröße.
  - **Brandung**: eine Uferkarte aus dem Gelände lässt Wellen auflaufen und bei etwa 0,8
    der Tiefe brechen; sie laufen als Film den Strand hinauf und hinterlassen weißes Wasser.
  - **Wellen von Körpern**: eine gedämpfte Wellengleichung um das Auge – Kielwasser, Ringe,
    das V eines Boots.
  - **Komponenten**: *Buoyancy* (schwimmt auf den Ecken, stampft und rollt), *Wave Maker*,
    *Boat* (B übernimmt das Ruder, W/S Gas, A/D Ruder, Schraube, Kiel, Verfolgerkamera).
    Die Spielfigur schwimmt im tiefen Wasser (Strg taucht, Leertaste taucht auf).
  - **Gischt** von Brechern, Sturmschaum, Spritzer beim Aufprall, Bugspritzer – beleuchtet.
  - **Regen auf dem Wasser** aus jeder Entfernung: Tropfenringe nah, Dellen dahinter, in der
    Ferne ein matterer Spiegel mit wandernden Schauerbändern.
  - **Wasser-Panel** mit Presets (stiller Teich bis Sturm), Kompass und Wellenhöhe als Zahl;
    alte Szenen übersetzen ihre Wellenhöhe in einen Wind. Geprüft mit `oceancheck` und
    `watercheck`.
- **Flüsse und Bäche** entstehen aus Splines, samt Wasserfällen.
- **Vegetation**: Gras per GPU-Scatter in Tiles, Wälder nach Ökologie-Regeln bis zum
  Horizont, Impostors für die Ferne, Blumen, ein Pinsel zum Verstreuen von Objekten und
  Baumteile mit eigenen Materialien.
- **Baumgenerator**: baut Bäume und speichert sie als `.glb` im Projekt.
- **Bäume fallen und brennen** (`game.fellTree`, `game.burnTree`): ein umgefahrener Baum
  kippt auf seinem Fuß um, ein brennender bräunt, glüht, verliert das Laub und bleibt als
  schwarzer Stumpf stehen. Lichtungen im Spiel nehmen nur die Bäume aus den geladenen
  Kacheln, statt den Wald neu zu erzeugen (`treefellcheck`).
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
    Krankenhaus, Bahnhof, ein Kraftwerk mit Strommasten) – und zum Einkaufen und Ausgehen
    Supermarkt (Parkplatz mit Autos, Einkaufswagen, Pylon mit P), Kino (Leuchtschrift,
    Vordach, Schaukästen), Kaufhaus, Parkhaus und Hotel.
  - Befestigte Grundstücke (Tankstelle, Parkplätze, Schulhöfe, Hochhausvorplätze) bleiben
    frei von Gras und Blumen; die Gärten der Einfamilienhäuser behalten ihres.
- **Häuserschluchten** entlang der Strecken, abgeleitet aus Biom-Regeln.
- **Treppengenerator** (View > Construct > Stairs): gerade, viertel- und halbgewendelt (mit
  Podest oder Wendelstufen), Spindel- und Bogentreppe, links- oder rechtsläufig. Höhe
  eingeben, Steigungen und Auftritt ergeben sich aus der Schrittmaßregel; massiv, auf
  Laufplatte, Wangen, Mittelholm oder als Kragarmstufen aus der Wand; Geländer mit
  Pfosten, Stäben, Glas oder Brüstung, Wände innen/außen, sieben Materiallooks und zwölf
  Vorlagen vom Keller bis zur Prunktreppe. Grundriss und Abwicklung zeichnen sich live,
  dazu die Kennzahlen (2R + G, Steigung, Lauflänge, Kopfhöhe bei der Spindel). Die Treppe
  ist begehbar (Collider), lässt sich später wieder öffnen und als Prefab speichern;
  geprüft mit `staircheck`, das Panel mit `stairpanelcheck`.
- **Straßendetails**: Gehwege, Straßenschilder in DIN 1451 (auch mit echten Frankfurter
  Namen), Ampeln nach deutschem Ablauf, Bushaltestellen und Laternen, die bei Dämmerung
  angehen. Dazu:
  - **Hausnummern** an jeder Haustür (blaues Emailleschild), je Straße hochgezählt,
    ungerade auf der einen, gerade auf der anderen Seite – dieselben Adressen, die die
    Stadtbewohner (`game.townPlaces`) benutzen. Die Haustüren sind jetzt geschlossen.
  - **Ladenschilder** über den Erdgeschossen im Zentrum und an der Hauptstraße
    („Bäckerei Schulz“, „Apotheke“ …), nachts leuchtend.
  - **Verkehrszeichen**: Vorfahrtstraße / Vorfahrt gewähren / Stop an Kreuzungen ohne
    Ampel (Alleen und Hauptstraße haben Vorfahrt, im Wohngebiet rechts vor links),
    Ortstafel mit dem Stadtnamen an jeder Ortseinfahrt (Rückseite: Ortsende),
    Zebrastreifen mit Schildern vor Schulen und Kindergärten.
  - **Werbung**: Litfaßsäulen an Ecken, Plakatwände auf leeren Grundstücken.
  - **Mülleimer** an Laternen und Bushaltestellen, **Gullideckel** auf der Fahrbahn und
    Straßenabläufe am Bordstein.
  Alles einzeln abschaltbar (Town generator > Look), geprüft mit `towncheck` („Street
  details“); `towncheck --where <szene>` sagt, wo was steht.
- **Verkehr**: Autos fahren nach dem Intelligent Driver Model, halten an Ampeln, Busse
  halten an den Stationen, Fußgänger laufen als Prefabs. Nahe beim Spieler bekommen die
  Autos echte Physik, es gibt Unfälle, und Wracks blockieren die Spur.
- **Nachts in der Stadt**: Standard-Straßenlaternen an jeder gepflasterten Straße, auch
  ohne Laternen-Prefab (Mast, Ausleger, Leuchte; warmes Licht ab der Dämmerung, die
  nächsten werfen Schatten). Die Autos des Stadtverkehrs haben Scheinwerfer und Rücklichter
  (nachts an), Bremslichter (beim Bremsen und im Stand) und Blinker vor dem Abbiegen –
  Platzhalter wie Prefab-Autos, deren Lampen auf der echten Front und dem Heck sitzen. Die
  Häuser haben nachts leicht beleuchtete Fenster, warm oder gedämpft, pro Haus anders
  verteilt (Anteil: windowLit der Stadt); die Fenster der Hochhäuser, Blocks und
  öffentlichen Gebäude leuchten erst mit der Dunkelheit richtig.
- **Straßenbahn**: Ein Spline mit dem Preset „Tram track“ wird in die Straßen eingelassen –
  wo er auf einer Straße liegt, liegen die Rillenschienen bündig in einem Pflasterband
  auf dem Asphalt (in Straßenhöhe, auch über Brücken), ohne Schwellen; daneben ein
  Gleis auf Schwellen. Punkte rasten auf die Straßenmitte ein. Doppelgleis, das an jedem
  offenen Ende zu einem Stumpfgleis zusammenläuft, Oberleitung an Masten am Bordstein,
  Haltestellenschilder. Darauf fahren Straßenbahnen (drei Glieder, Führerstand an beiden
  Enden): halten an jeder Haltestelle, wechseln an der Endstelle den Führerstand, bremsen
  in Kurven, vor Autos, Leuten und der Bahn davor; die Autos bremsen für die Bahn und
  reihen sich hinter ihr ein. Die Bahn hat Vorrang: der Gleisabschnitt vor ihr ist für die
  Autos gesperrt (sie warten an der Haltelinie, statt vor ihr einzubiegen), und wer schon
  darin steht, räumt ihn. Autos fahren außerdem nur in eine Kreuzung, wenn dahinter Platz
  ist – niemand bleibt mehr auf Kreuzung oder Gleis stehen. Anzahl, Tempo,
  Haltestellenabstand und Haltezeit im Spline-Panel. Geprüft mit `tramcheck` und
  `trafficcheck` („Trams among the traffic“: zehn Minuten Bahn und Stadtverkehr zusammen).
- **In der Straßenbahn**: Die Wagen sind innen hohl – Niederflurboden, Sitzbänke in
  Vis-à-vis-Gruppen mit freiem Mittelgang, gelbe Haltestangen, Lichtleisten (nachts
  leuchtend, dazu ein Licht pro Wagen der nächsten Bahn), Fenster zum Durchsehen,
  Faltenbalg-Übergang zum nächsten Wagen und ein abgeschlossener Führerstand.
  Schiebetüren öffnen an der Haltestelle auf der Seite, an der das Schild steht, und
  schließen vor der Abfahrt; wer in der Tür steht, hält sie offen. Im Play ist jeder
  Wagen eine bewegte Plattform in der Physik (Boden, Wände, Sitze, Türflügel): die
  Spielfigur steigt an der Haltestelle ein, geht durch den Gang bis in den nächsten
  Wagen und fährt mit – Figuren auf jedem kinematischen Körper werden mitgenommen
  und mitgedreht (`game.moveCharacter` liefert die Drehung als sechsten Wert).
  Stadtbewohner nehmen die Bahn: wer an einer Haltestelle mit Gehweg in der Nähe
  vorbeikommt, wartet manchmal am Bahnsteig, steigt ein, fährt ein paar Stationen
  stehend mit und steigt an einer Haltestelle mit Gehweg wieder aus (an der Endstelle
  alle); Autos halten für Leute, die zwischen Gehweg und Bahn die Straße queren.
  Geprüft mit `tramcheck` („Doors“, „Getting on“: echte Kapsel in echten Wagenkörpern)
  und `trafficcheck` („People riding the trams“).
- **Zu Fuß durch die Stadt**: Die Gehwege aller Blöcke sind an den Ecken über Überwege
  zu einem Netz verbunden (immer gerade über eine Straße, nie diagonal über eine
  Kreuzung). Jeder Ort hat einen Namen: öffentliche Gebäude, Parks, Bushaltestellen und
  jedes Haus mit Straße und Hausnummer (ungerade links, gerade rechts). In Lua:
  `game.townPlaces`, `game.townPath`, `game.streetAt`.

## Himmel, Wetter, Licht

- **Tag und Nacht**, volumetrische Wolken, ein Wolken-Layer-Stack (eine Ebene pro
  Wolkentyp) und Wolkenschatten.
- **Ein einziger Wetterwert von 0 bis 1** steuert Wolken, Nebel, Wellen, Blitze, Regen
  und die Wetter-Sounds. Dazu gibt es Wetter-Presets.
  Skripte lassen das Wetter per `game.weather` langsam in ein Preset übergehen, die Uhr
  läuft dabei vorwärts mit (aus einer Regennacht durch die Dämmerung in einen sonnigen Tag).
- **Auflösen**: `game.dissolve` lässt Objekte in einem Rauschmuster mit glühenden Rändern
  zerfallen, ihre Schatten zerfallen mit.
- **Nebel**: Höhennebel mit Luftperspektive, Dunst, der sich nach der Bewölkung färbt,
  und platzierbare Nebel-Volumen mit Lichtstrahlen.
- **Modelle aus Blender abgleichen**: als Hierarchie importierte Modelle (ein Objekt pro Teil)
  finden ihre Teile über den Namen. Nach erneutem Export aus Blender übernimmt die Szene
  beim Laden (oder per *Scene → Reload Models from Disk*) verschobene, skalierte, neue und
  gelöschte Objekte; was im Editor eingestellt wurde, bleibt (Dreiwege-Abgleich über die
  zuletzt gelesene Lage jedes Teils).
- **Licht**: Sonne mit Kaskadenschatten, Punktlichter (16 aktiv, davon 8 mit Schatten),
  HDRI-Beleuchtung und ein gebackenes Lichtproben-Gitter. Punktlicht-Schatten sind gecacht:
  was stillsteht, zeichnet eine Lampe einmal; pro Frame kommt nur hinzu, was sich bewegt
  (laufende Figuren, Türen). Wechselt eine Lampe ihren Schattenplatz, blendet ihr Schatten
  vorher weich aus.

## Rendering

- **Nachbearbeitung und Effekte**: HDR, Bloom, God Rays, Lens Flare, ACES und AgX, Color
  Grading, SSAO, SSR, TAA, GGX, Kontaktschatten – alles in einem Panel *Post processing*,
  jeder Abschnitt mit eigenem Schalter, mit der Szene gespeichert; der Post-FX-Knopf in der
  Ansicht schaltet ihn für die schlichten Modi ab.
- **Comic-Look** als Post-Effekt: Tusche aus der Tiefe (Falten, Silhouetten), Cel-Bänder,
  ein Punktraster im Dunkeln; das HUD bleibt unberührt, Skripte schalten ihn mit
  `game.setComic`.
- **Flüssigkeit auf der Linse** (`game.lensSplash`): Spritzer, durch die man die Welt
  gebrochen und unscharf sieht, Tropfen, die ruckweise die Scheibe hinunterlaufen und
  Spuren ziehen, nasses trocknet zu Flecken, die Linse wird immer dreckiger; Farbe frei
  (Blut, Wasser, Schlamm). Ein **Scheibenwischer** (`game.lensWipe`) räumt den Sektor frei,
  den sein Blatt überstreicht – Frame für Frame, wie über eine Autoscheibe.
- **Grafikeinstellungen des Spielers** (F9) gelten nur im Play; der Editor zeigt die
  Szene, wie sie gebaut ist.
- **Viele Lampen**: über 16 Punktlichter zeichnet der Renderer die für die Kamera
  wichtigsten, weich überblendet; acht werfen Schatten aus einem Cache.
- **Nichts flimmert in Bewegung**: stabile Kaskaden, SSAO-Normalen über drei Texel,
  Kontaktschatten und SSR aus der Pose des letzten Frames.
- **Ansichten**: orthografisch per Num 5, mehrere Shading-Modi und bis zu vier 3D-Ansichten
  nebeneinander, jede mit eigener Kamera, Shading und Post FX.
- **Offline-Pfadtracer**: Tiefenschärfe, weiche Sonne, Speichern als EXR, Wälder mit über
  100.000 Bäumen per Instancing. Zusätzlich eine Vorschau mit dem GPU-Tracer im
  Viewport.
- **Performance**: Frustum-Culling, Occlusion Queries, Profiler mit Frame-Trace auf F4
  und GPU-Timer. Uniforms werden nur hochgeladen, wenn sie sich ändern, Programme nur
  gewechselt, wenn nötig (Roads-Szene: 41 → 62 fps); Terrain-Chunks teilen sich ihre
  Indexpuffer.

## Modellieren (Blender-Stil)

- **Modelling-Modus mit Tab**, Blender-Tasten und den Ebenen Vertex, Edge und Face.
- **Werkzeuge**: Extrude, Bevel, Inset, Loop Cut, Merge/Weld, Dissolve, Spin,
  Duplizieren (in einer Reihe oder entlang eines Splines) und ein UV-Panel. Dazu
  **Join** (Strg+J: gewählte Meshes und Grundformen werden eines, Materialien inklusive),
  **Connect path** (ein gerader Schnitt durch alle Flächen zwischen zwei Ecken) und
  **Merge by distance** auf dem ganzen Mesh.
- **Materialien pro Fläche**. Alle Grundformen lassen sich bearbeiten.
- **Modifier-Stack**: Subdivision Surface, Decimate, Wireframe, Array, Solidify. Auf
  einer Grundform (Kasten, Kugel, Zylinder) wirkt er sofort – sie wird im selben Schritt
  zu einem Mesh genau ihrer Größe.
- **Mesh-Paint**: Terrain-Layer auf Objekte malen. Dazu 3D-Cursor und Snapping.
- **Prozedurale Objekte (Mini-Houdini)**: View > Objects > Procedural. Ein Knoten-Graph
  aus Formen (Box, Zylinder, Tube/Kegel, Kugel, Torus, Grid), Kopien (radial, in Reihe, auf
  Punkte), Kombinieren (Merge, Transform, Mirror), Details (Panels, Extrude, Lattice,
  Thicken, Subdivide, Delete faces) und Material pro Flächenauswahl wird bei jeder
  Änderung in ein normales Mesh gekocht. Vorlagen „Ring station“ und „Modular
  station“, „Fuel depot“. Dazu 2D-Formen und Kurven (Circle, Rectangle, Curve mit
  Punktliste, Resample), Sweep (Profil entlang Kurve: Rohre, Gänge), Revolve (Drehkörper:
  Tanks, Kuppeln), Spin wie in Blender (Flächen werden Körper, Linien Flächen, um eine
  Achse durch ein frei wählbares Zentrum in Schritten gedreht; mit Rise eine Schraube oder
  Wendelrampe, volle Drehung ohne Rise schließt sich ohne Enden), Extrude auch für Linien (offene Linie wird Wand, geschlossene ein Mantel; Achse wählbar) sowie Punktauswahl als Filterkette (Select points nach Box, Kugel,
  Richtung, jedem n-ten, Zufall; ersetzen/hinzufügen/abziehen/schneiden; Box und Kugel stehen sichtbar
  in der Szene, mit Handles für Mitte, Flächen bzw. Radius -- Shift zieht die Gegenseite mit; Delete points;
  Mesh to points macht aus Flächen lose Punkte: Ecken, Flächenmitten oder gleichmäßig
  über die Fläche gestreut, jeweils mit der Flächennormale zum Aufstellen der Kopien);
  die Auswahl steuert Copy onto points, Transform und den Flächenfilter. Kurven und
  ausgewählte Punkte werden im Viewport eingezeichnet. Die Punkte einer Curve haben
  dort Griffe: ziehen (in der Ebene, in der die Kurve liegt, mit Strg senkrecht dazu,
  auf 0,1 m gerastert), „+“ auf einem Teilstück setzt einen Punkt dazwischen, ziehfrei
  schieben Pfeiltasten und Bild↑/↓ den gewählten Punkt, Entf nimmt ihn heraus.
  Prefab-Knoten setzen Prefabs des Projekts als echte Objekte unter das prozedurale
  Objekt (verschieb-, kopier- und auf Punkte setzbar; Undo schließt sie ein).
  **Realize** schmilzt sie stattdessen in das Mesh ein (ein Objekt statt vieler, jede Fläche
  im Material ihres Teils, gespiegelte Kopien richtig gewendet); Prefabs mit Modell, Licht
  oder Skript bleiben Objekte. **Bake to mesh** (im Fenster und auf der Inspector-Karte)
  fixiert das Objekt: es behält sein Mesh, der Graph geht, gesetzte Prefabs bleiben normale
  Objekte – ein Undo-Schritt.
  Knoten wählt man per Klick, Shift+Klick oder Rahmen (A alle, Alt+A keine) und
  verschiebt sie gemeinsam; ein freier Knoten mit einem Eingang, über einem Draht
  abgelegt (gezogen, per Move oder aus dem Add-Menü), wird genau dazwischen verdrahtet
  (der Draht leuchtet vorher auf); Entf löscht Knoten, nie Szenen-Objekte; Shift+A bzw.
  Rechtsklick öffnet das Add-Menü mit Suche an der Mausposition, Rechtsklick auf einen
  Knoten dessen Menü; mittlere Maustaste schwenkt.
  **Parameter und Ausdrücke (wie Houdinis Type Properties):** ein Objekt hat eigene
  Regler – Float, Integer, Toggle, Vector, Menü, Text, Material, Prefab, dazu Überschriften
  und Trennlinien, in Ordnern, mit Bereich, Schrittweite, Tooltip und „Hide/Grey when“
  (Ausdruck wie `!beacons_on`). Sie stehen auf der Seite **Parameters** des Fensters
  und auf der Inspector-Karte des Objekts; „Edit interface“ ordnet, benennt um (Ausdrücke
  ziehen mit), ändert die Art und zeigt, wer einem Parameter folgt. Ein Klick auf den
  Namen einer Knoten-Einstellung: **Promote to parameter**, einem Parameter folgen,
  einen Ausdruck schreiben oder den Wert behalten. Ausdrücke rechnen mit Parametern
  und Einstellungen anderer Knoten (`ring_radius - ring_width / 2 - hub.radius`,
  `ch("../hub/length")`, Vektoren als `.x/.y/.z`), Operatoren inkl. Vergleich, `&&`,
  `?:`, Funktionen wie `sin` (Grad), `fit`, `clamp`, `rand`, `if`; Texte füllen `$name`
  bzw. `${formel}` ein (ein Material für viele Knoten: `$hull`). Werte halten sich an den
  Bereich der Einstellung, Fehler und Schleifen stehen am Knoten, das Feld wird grün
  bzw. rot. Ein gelöschter Parameter hinterlässt seinen Wert. Die „Ring station“ zeigt
  es: Ringradius, -breite, Speichenzahl, Leuchten und Rumpffarbe als Regler, die Speichen
  reichen per Ausdruck immer von der Nabe bis zum Ring.
  **Gebäude und Brücken:** Facade teilt Wände in Geschosse und Achsen, setzt Fenster mit
  Laibung und Rahmen zurück, im Erdgeschoss wahlweise Tür, Schaufenster oder nichts, dazu
  Gesimse je Geschoss (eigene Materialien für Glas, Rahmen, Türen, Gesimse; Rücksprünge
  oben bekommen keine Türen). Roof setzt Flach- (mit Attika), Sattel-, Walm-, Zelt- oder
  Pultdach mit Neigung und Dachüberstand auf die Deckflächen (Walm für jeden konvexen
  Grundriss). Arch ist eine Wand mit Bogenöffnung (rund, elliptisch, spitz, Parabel) für
  Arkaden, Viadukte, Tore, Brückenpylone; Arch curve dieselbe Form als Linie, mit
  negativem Stich ein durchhängendes Tragseil. Offset versetzt Linien seitlich (Deckkanten,
  zweites Seil), Drop lines lotet von Punkten auf darunterliegende Flächen oder eine Höhe
  (Pfeiler, Stützen, Hänger), Railing baut Geländer (Pfosten, Holme, Stäbe oder Paneele),
  Truss Fachwerkträger (Warren, Pratt, Howe, X; zweiseitig mit Querträgern oder einseitig
  als Dachbinder). Vorlagen „Town house“, „Office tower“, „Arch bridge“, „Suspension
  bridge“, „Truss bridge“. Konkave Flächen (L-Grundriss) gehen trianguliert an den Renderer.
  Verdrahtet wird von Hand (Ziehen von Punkt zu Punkt oder zwei Klicks);
  die Knoten ordnen sich selbst an, bis man einen verschiebt (Ziehen mit Raster oder
  „Move“ + Klick), „Arrange“ ordnet neu. Zahlen per Stepper oder getippt.

## Animation und Kamera

- **Timeline**: jedes Feld im Inspector kann Keyframes bekommen.
- **Animations-Graph** (Zustandsautomat): spielt Modell-Clips, blendet zwischen ihnen
  über, kennt negatives Tempo und wird per Lua gesteuert. Ein **Fade-Editor** zeigt jeden
  Übergang auf einer Zeitleiste (beide Zustände, Übergabekurve, Loop-Punkte); Ausgang,
  Ende und Einstieg in den neuen Clip lassen sich ziehen oder tippen, die Kurve wählen
  (linear, weich, ease in/out), und außerhalb von Play spielt die Vorschau die Blende auf
  der Figur ab – auch halb oder viertel so schnell.
- **Retargeting** (Assets → Retarget animations): Bewegungen eines fremden Skeletts
  auf eine eigene Figur übertragen – Mixamo, ActorCore/Character Creator, Daz Genesis,
  Unreal, Rigify, VRM, BVH. Die Knochen werden automatisch einer Humanoid-Vorlage
  zugeordnet (Namen plus Aufbau des Skeletts), jede Zuordnung lässt sich per Liste
  ändern. Eine Vorschau zeigt Quelle (bei reinen Bewegungsdateien als Gliederpuppe)
  und Figur nebeneinander, mit Knochen, Schatten und Bild-für-Bild-Steuerung. Clips
  lassen sich zuschneiden und „auf der Stelle“ machen; neue kommen per Dateidialog,
  Drag & Drop oder Klick in einer Bewegungs-Bibliothek dazu – oder von einer anderen
  Figur kopiert: deren Animationen ankreuzen, vorher ansehen, übernehmen. Bei Figuren
  derselben Rig-Familie wandern auch die übrigen Knochen (Twist, Augen, Kiefer) nach
  Namen mit. FBX, BVH und `.blend`
  liest ein installiertes Blender im Hintergrund, das der Editor selbst findet – unter
  Windows, Linux und macOS.
  Geschrieben wird in die `.glb` der Figur; das Original bleibt als `.orig`, das
  Rezept als `.retarget` daneben. Daz-Exporte werden dabei repariert (Kleidung auf
  dem Körperskelett, Haare und Wimpern am Kopf).
- **Kameras**: Verfolger, Cockpit, Kamera-Wechsler, Kamerapfade, Split-Screen.
- **Multishot-Kamera** für Werbe- und Replay-Shots, darunter Dolly Zoom und Fly-by.
- **Regen auf Glasscheiben** (Glass-drops-Komponente): Perlen landen auf einer Scheibe,
  manche wachsen zu Tropfen und laufen herunter, der Film trocknet wieder; das Glas bricht
  das Dahinterliegende durch jeden Tropfen. Gilt für Glas-Materialien, auch auf bewegten
  Objekten (die Fenster einer fahrenden Straßenbahn).
- **Zwischensequenzen aus Lua mitten im Level**: ein Skript führt die Kamera
  (`game.setCameraPos`/`Dir`/`Fov`) und gibt sie danach mit `game.releaseCamera()` an den
  laufenden Spieler zurück, dessen Kapsel die ganze Zeit stehen blieb.
- **Sequencer** (View > Presentation > Sequencer): Kamera-Shots, Animationen und Ton auf
  einer Uhr. Shots schneiden sich selbst, eine obere Kameraspur übersteuert eine untere;
  Animationsstreifen mit Start, Tempo und Loop; Tonspuren wie in einer DAW (Wellenform,
  Trimmen, S-Kurven-Blenden, Pegel in dB, Mute/Solo, Mixer-Kanal). Abgespielt vom Player
  und von Skripten (`game.playSequence`, C# `Sequence.Play`). *Camera from view*
  (Strg+Alt+C) und *Align to view* (Strg+Alt+0) setzen Kameras dorthin, wo der Viewport
  steht.
- **Orbit mit der Maus** und `walker.lua` als Third-Person-Steuerung.

## Gameplay

- **Zwei Skriptsprachen: Lua oder C#.** Beim Anlegen eines Projekts wählbar (oder
  später in den Game Settings). C# läuft auf eingebettetem .NET 10, unter Windows und
  Linux (dort mit VS Code, VSCodium oder Rider als IDE); das Projekt
  bekommt wie bei Unity eine Visual-Studio-Solution (`Assembly-CSharp.csproj`
  über alle `.cs` in `scripts/`). Der Editor baut im Hintergrund, sobald eine
  Datei sich ändert, zeigt Fehler im Fenster „C# Scripts“ und lädt bei jedem Play
  die neue Assembly. Skripte sind `Behaviour`-Klassen mit `Start`/`Update`/
  `OnDestroy`, öffentliche Felder stehen auf der Script-Karte. Lua und C# dürfen
  auf demselben Objekt nebeneinander laufen; C# kann dabei alles, was Lua kann
  (dieselben Engine-Funktionen dahinter). Export liefert nur die übersetzte
  DLL aus. API und Grenzen: [csharp-scripting.md](csharp-scripting.md).
- **C#-Editor eingebaut** (*Assets → C# Editor*), wie ein kleines VS Code: Tabs,
  Completion beim Tippen mit Doku zum markierten Eintrag, Signaturhilfe in
  Aufrufen, in `Entity.Find("…")` die Objektnamen der offenen Szene, Quick Info beim Hovern, Fehler und Warnungen als Wellenlinien schon
  beim Tippen (plus Problemliste), semantische Farben, F12/Strg+Klick zur
  Definition, Umschalt+F12 Referenzen, F2 Umbenennen über Dateien hinweg,
  Umschalt+Alt+F Formatieren, Strg+F Suchen, Strg+G Zeile, Strg+/ Kommentar,
  Klammern und Anführungszeichen schließen sich selbst. Dahinter steckt Roslyn
  aus dem installierten .NET-SDK (als eigener Prozess), also derselbe Compiler,
  der die Skripte baut. Visual Studio bleibt einen Menüpunkt entfernt.
- **Physik** mit Jolt, dazu Stoff und Softbodies (Vorhang, Flagge).
- **Figuren mit Kollision**: `game.moveCharacter` lässt eine Figur als Kapsel durch die
  Welt laufen – auf Straßen und Brücken statt durch sie hindurch, vor Wänden stehen
  bleibend, Bordsteine hinauf und über Kanten hinab. Auf Gelände stehen die Füße auf
  dem gezeichneten Boden. `walker.lua` nutzt das.
- **Inverse Kinematik**: Komponente „Inverse Kinematics“ stellt die Füße einer animierten
  Figur auf den Boden unter ihnen – Bordstein, Treppe, Hang –, senkt den Körper so weit
  wie der tiefere Fuß braucht und kippt einen stehenden Fuß mit der Neigung. Hände per
  `game.reach` an einen Punkt (Türgriff, Lenkrad, zweite Hand an der Waffe). Die Beine
  und Arme werden am Skelett selbst gefunden (Character Creator, Mixamo, Unreal,
  Blender, Daz).
- **Mesh-Kollider für Modelle**: ein statisches Modell kollidiert als seine eigenen
  Dreiecke statt als Hülle – Türen, Durchbrüche und Treppen sind begehbar, Laub, Gras
  und Decals bleiben außen vor. Physik auf dem Root eines importierten Modells macht
  alle Teile darunter fest (eine ganze Halle mit einem Klick); jede Kopie eines Modells
  teilt sich ein Mesh.
- **Fahrzeuge**: ein Auto und ein Gleiter mit festem 120-Hz-Takt, Reifenspuren,
  Kondensstreifen und Gischt.
- **Ein- und Aussteigen** (`car_driver.lua`): eine Figur geht auf `F` hinter das Ende
  der Fahrertür, fasst den Griff, zieht sie auf und steigt mit einer Mocap-Animation
  ein; innen zieht sie die Tür an der Verkleidung zu, setzt die Füße in den Fußraum
  und die Hände ans Lenkrad, das sich mit einschlägt – und fährt das Auto. Wieder `F`:
  anhalten, Tür von innen aufstoßen, aussteigen, am Griff zudrücken, zu Fuß weiter.
  Was vom Körper zwischen Tür und Auto ist, drückt die Tür auf statt durch sie zu
  gehen; Schnapp- und Zuschlag-Geräusch kommen von der Tür. Ein stehendes Auto ohne
  Gas hält mit der Handbremse; der Stadtverkehr hält für Figuren zu Fuß auf der
  Fahrbahn. Neu in Lua dafür: `game.spawnVehicle`/`driveVehicle`/`leaveVehicle`/
  `drivenVehicle`, Füße in `game.reach`, `takeover` im Figuren-Controller.
- **Renn-Baukasten**: Gegner-KI, Startaufstellung, Checkpoints, Boost-Pads, HUD,
  Bestenliste, Schwierigkeitsstufen, Showroom als Startmenü.
- **Waffen**: Lock-on-Raketen für die Fahrzeuge; für Figuren eine Pistole (siehe Schießen).
- **Fertige Komponenten**: Trigger, Szenenwechsel, Mover, Spawner, Lift, Tür,
  Sammelobjekte, Partikel.
- **Gegner markieren** (Enemy-Komponente): Name und Farbe, sonst keine Funktion – der
  Editor zeigt die Bounding Box des Objekts halbtransparent in dieser Farbe, damit die
  Gegner eines Levels zwischen der Kulisse auffallen. Nur im Editor: nicht im Play,
  nicht im Player, nicht im Bake.
- **Nässe** (Wet-Komponente): Wasserfilm auf einem Modell – dunkler, glänzend, mit
  Tropfen, die über die UVs auf der Haut bleiben, auch wenn die Figur sich bewegt. Ein
  Emitter mit „Wets objects“ (die Dusche) macht ein Objekt mit „Gets wet“ nass, solange
  sein Wasser in dessen Box fällt; danach trocknet es über die eingestellte Zeit.
- **Objekte mit Lebensdauer**: `game.spawn{…, lifetime = s}`, `game.spawnPrefab(…, s)` und
  `game.destroy(id, s)` räumen Hülsen, Funken und Trümmer selbst weg; ein Stoß direkt nach
  dem Spawn erreicht das neue Objekt (C#: `SpawnInfo.Lifetime`, `e.Destroy(seconds)`).
- **Inventar** (`inventory.lua`): Gegenstände in der Welt (Collectible mit „Inventory
  item“: Name, Icon, Kategorie, Beschreibung, Anzahl) zeigen im Radius einen Hinweis;
  auf `E` stellt sich die Figur hin, bückt sich mit einer Mocap-Animation und greift
  den Gegenstand mit der Hand. `I` öffnet ein Inventar-Fenster mit Icons, Karte und
  Knöpfen: Waffen ausrüsten (in der rechten Hand), alles wegwerfen (fliegt vor die
  Füße, auf Weg oder Brücke). Neu in Lua dafür: `game.hudImage`/`imageSize`,
  `game.worldToHud`, `game.captureInput` (ein Menü hält die Tasten, Esc schließt es
  statt Play), `game.attach`/`detach`/`bonePos`/`bones` (Dinge an Knochen hängen),
  `game.collectibles`/`collectible`, `game.groundHeight` und die Tabelle `shared`
  für Skripte, die zusammenarbeiten.
- **Schießen** (`weapon.lua`): mit der ausgerüsteten Pistole über die Schulter zielen
  (rechte Maustaste), schießen (linke), nachladen (R) mit Munition aus dem Inventar –
  Mocap-Anschlag und -Rückstoß, Mündungsfeuer, Rauch und Lichtblitz, Staub und Funken
  am Einschlag, Stoß für Physik-Körper, Fadenkreuz mit Treffer-Markierung, Magazin-
  Anzeige. Neu in Lua: `game.orbitFrame` (Schulterkamera), `game.castRay` (Physik und
  gezeichnetes Gelände), `game.emit` (Partikel-Burst auf Kommando), `game.toWorld`,
  `game.attach` mit Überblendung.
- **Lua-Scripting**: mehrere Skripte pro Objekt, bei jedem Play eine frische VM. Dazu ein
  Skript-Editor mit Autovervollständigung, ein UI-Overlay (Text, Buttons, Bilder) und
  Spielstände.
- **Beispielspiele**: SKYSTRIKE (Shoot'em-up mit Hangar und High Valley), Sokoban,
  Arkanoid, Invaders, Pinball, Dosenschießen, Rhythmicker-Port.
- **DARKNESS – moderner Doom-Klon** (Projekt `doom`, spielbare Demo auf pueski.de): E1M1 aus der Blend-Datei des Nutzers,
  per Blender-Export (Maßstab Doom → Meter, Türen als eigene Knoten, Himmel offen) und
  Szenen-Generator gebaut. Pistole und Schrotflinte als 3D-Waffen vor dem Auge, besessene
  Soldaten, Schrotflinten-Sergeants und Imps mit Feuerbällen (Sicht per Strahl, Aufwachen
  durch Schüsse, Kopftreffer), explodierende Fässer mit Kettenreaktion, Türen per `E`,
  Exit-Schalter mit Aufzug, Geheimräume, Nukage-Schaden, Automap (Tab), HUD mit
  Treffer-Richtung, Titel, Tod und Level-Statistik; Sounds aus echten Aufnahmen und eigene
  Industrial-Metal-Musik. Gegner, die man nicht sieht, werden ausgeschaltet (24 geskinnte
  Figuren kosteten sonst ~20 ms). Neu dafür in der Engine: `game.door`, *Step height* am
  PlayerStart (Doom-Stufen von 0,63 m), `game.captureInput` hält auch den Läufer an,
  `game.setLight{shadows=…}`, Lampen streuen im Volumennebel (*Lamps* im Fog-Panel:
  Lichthöfe und Kegel im Dunst) und `--bake-grid <dichte>` backt das Light-Grid von der
  Kommandozeile. Dazu *Run speed* am PlayerStart (Shift rennt), `game.getLight`, und bei mehr
  als 16 Punktlichtern zeichnet der Renderer die für die Kamera wichtigsten (weich überblendet)
  statt der ersten in Szenen-Reihenfolge; der Light-Grid-Bake hört alle.
- **Schach gegen eine Engine** (`chess.lua`): Brett und Figuren aus der Szene gelesen,
  per Klick gespielt, Engine um 1250 Elo (geschätzt); dafür neu in Lua: `game.mousePos`,
  `game.mouseRay`, `game.showCursor`, `game.clone`, `game.rest` (wartet das Spiel nur
  auf den nächsten Klick, zeichnet Play höchstens 10 Bilder pro Sekunde statt voller Rate).

- **STEELWARS** (Projekt `D:\fitzel_projects\steelwars`): ein Sci-Fi-Echtzeitstrategiespiel
  mit Roboterarmeen – Baufahrzeug baut die Basis, Harvester ernten Energicum, Raffinerie,
  Kraftwerk und Strom, 10 Einheiten (u. a. fliegender Terminator, Hypercopter als Transporter,
  Infiltrator übernimmt Gebäude und Panzer), 12 Gebäude mit Verstärkung/Erweiterung aus dem
  Forschungslabor, Radar-Minimap, Nebel des Krieges, KI-Gegner in drei Stufen. Die Simulation
  ist deterministisch und läuft im **Lockstep**: Mehrspieler über einen Relay-Server
  (`fitzelserver` oder im Spiel gehostet; der Relay ist für einen öffentlichen Linux-Server
  gehärtet: Verbindungslimit, Zeitlimit bis HELLO, Rückstau-Grenze, sauberes Beenden unter
  systemd, `--probe` zum Testen). Dafür neu in Lua: `require` für mehrere
  Skriptdateien, die Tabelle `net`, `game.setLocals` (eine Armee pro Frame in einem Aufruf),
  `game.sound` (positionierte One-Shots), `game.mouseWheel`, `game.waterAt`, `game.trees`
  und `game.clearTrees` (Wald als Hindernis, Lichtung beim Bauen).
  **Agenten als Kommandanten** (`sw/agent.lua`): Jede Seite kann ein Sprachmodell führen
  (Ollama, `qwen3:8b`), als kleiner Stab mit Befehlskette – das Oberkommando beurteilt die
  Lage und befiehlt den Unterkommandanten Wirtschaft (Bauliste, Harvester, Türme, Forschung)
  und Heer (Truppenmix, Einsatz, Ziel), die zurückmelden. Antworten kommen als JSON-Schema,
  dessen Auswahl Lua festlegt (kein Angriff ohne Fabrik und Heer, kein zweites Radar, kein
  Kraftwerk bei Stromüberschuss); ausgeführt wird mit dem Gehirn der KI, Befehle laufen
  durch die Lockstep-Session. Im Gefecht-Menü: deine Seite Mensch/Agent, Gegner KI/Agenten/
  gemischt – also Agent allein gegen die KI, Agent gegen Agent (auch zwei Modelle
  gegeneinander) oder du gegen Agenten; F2 zeigt die Gedanken der Stäbe.

- **Leute mit eigenem Kopf** (`town_agents.lua`): Figuren, die ein Sprachmodell auf
  diesem Rechner steuert (Ollama, im Skript `qwen3:8b`). Jede hat Namen, Alter, Beruf,
  Arbeitszeiten und eine Wohnung in der Stadt. Sie entscheidet selbst, wohin sie geht und
  was sie dort tut, und läuft über Gehwege und Überwege hin. Wer länger in einem Gebäude
  bleibt, geht hinein. Begegnen sich zwei, bleiben sie vielleicht stehen und reden; das
  Gespräch erscheint in Sprechblasen, und beide erinnern sich daran. `H` zeigt die Liste
  aller Leute. **Chat** (`T`): Fragen an die Stadt (ein Erzähler, der weiß, wer gerade wo
  ist) oder an eine Person (`Tab`, `@Name`) – wer nah steht, bleibt stehen und antwortet
  laut, wer weiter weg ist, wie per Handy; um einen Ort gebeten, geht sie vielleicht hin.
  **Markierungen** (`M`): farbiger Pfeil mit Nummer, Name und Entfernung über jeder
  Person, am Bildrand ein Pfeil in Richtung derer, die nicht im Bild sind. **Freie
  Kamera** (`K`): fliegen mit WASD/Leertaste/Strg, umsehen mit Pfeiltasten oder rechter
  Maustaste, `1`–`8` fliegt zu einer Person und folgt ihr, `0` zur eigenen Figur.
  **Verhalten für alle**: der Skript-Parameter `CONTEXT` oder im Chat `/alle <Text>`
  („Heute ist Stadtfest im Stadtpark“) steht in jedem Prompt; alle überlegen sofort neu,
  und dort genannte Orte stehen jedem zur Wahl.
  **Befehle an die Stadt**: im Chat `/zeit 21:30` (oder `/zeit 7`, `/zeit +2`),
  `/tempo <Spielminuten pro Sekunde>`, `/laternen an|aus|auto`, `/befehle` – oder in
  eigenen Worten an „Stadt“ („mach mal Abend“, „Laternen aus!“): der Erzähler ist auch
  die Stadtverwaltung. Die Engine folgt sofort (Sonne, Himmel, Licht, Laternen), und die
  Stadt weiß davon: Uhrzeit und Laternen stehen in jedem Prompt, die Änderung als
  Ereignis in der Erinnerung aller, und alle planen neu. Die Uhr der Stadt ist die der
  Szene (`START_HOUR = -1` übernimmt sie beim Start). Neu in Lua dafür:
  `game.timeOfDay/setTimeOfDay`, `game.dayLength/setDayLength`,
  `game.streetLamps/setStreetLamps`; nach Play ist alles wieder wie vorher.
  Ohne Ollama laufen sie zu zufälligen Zielen. Dafür neu in Lua: die Tabellen `llm`
  (Anfragen an das Modell, ohne den Frame aufzuhalten; Antworten als JSON nach Schema;
  `priority` für Wartende) und `json`, dazu `game.textInput` für eigene Eingabefelder.
  Geprüft mit `llmcheck` und `agentcheck`.
- **Mehrere Figuren desselben Modells** bewegen sich unabhängig: Die erste skinnt wie
  bisher in die Meshes des Modells, jede weitere in eine eigene Kopie (`SkinCopies`).
  Vorher übernahmen alle die Pose der zuletzt gerechneten.

## Audio

- **Grundlage**: 3D-Soundquellen, Motorsounds für Auto und Gleiter, Wetter-Sounds.
- **Mischpult wie in einer DAW** (Fenster *Mixer*, mit der Szene gespeichert): beliebig
  viele **Channels**, jede Klangquelle hängt an genau einem – Audio Sources wählen ihren
  im Inspector, `game.sound(..., channel)` pro Aufruf, Wetter/Zonen spielen auf *Ambient*,
  Schüsse und Fahrzeuge auf *SFX*, Songs und Synths auf *Music*. **Aux-Busse** mit
  **Sends** pro Channel (Pegel, vor oder nach dem Fader), **Insert-Effekte** auf Channels,
  Bussen und Master (EQ, Filter, Kompressor, Delay, Reverb, Chorus, Drive), Pan,
  Mute/Solo (Busse sind solo-sicher) und Stereo-Meter mit echten Pegeln. Ältere Szenen
  bekommen das Standardpult mit ihren drei alten Fadern. Bedienbar ohne präzises Ziehen:
  Klick setzt, Mausrad in Schritten, Doppelklick auf den Standard, −/+ an jedem
  Effekt-Parameter. **Global (all scenes)**: ein Pult für alle Szenen des Projekts
  (`mixer.json`), es bleibt beim Szenenwechsel stehen. Geprüft mit `mixercheck` (echter
  Testton durch Channel, Send, Bus und Master; Rundlauf des globalen Pults).
- **Modularer Synthesizer** mit MIDI-Player und `synth.*` in Lua.
- **Musik-API** mit Songuhr, Filtern und Analyse (für Rhythmusspiele).

## Editor und Assets

- **Bereiche wie in Blender**: der Bildschirm ist ein Baum aus Bereichen, jeder mit einer
  Kopfzeile (welcher Editor, teilen, zusammenlegen, Griff zum Ziehen); Strg+Leertaste holt
  einen Bereich auf den ganzen Schirm. Bis zu vier 3D-Ansichten mit eigener Kamera.
  Layout in `imgui.ini`, „Reset layout“ stellt die alte Anordnung her (`areacheck`).
- **Panels**: Hierarchie, Inspector, Asset-Browser mit Drag & Drop in die Szene,
  Materialien, Prefabs (Stufe 1, Vorlagen) – auch als `.zip` zwischen Projekten, mit
  allem, was sie brauchen (Modelle, Materialien, Skripte, Klänge, Animationsgraphen);
  der Import zeigt vorher Datei für Datei, was er tun würde.
- **Add-Menü**: die Basisobjekte nach Kategorie (Formen, Lichter, Kameras, Gameplay,
  Physik, Audio, Effekte, Rennen, Welt) – jedes eine Entität mit der passenden Komponente,
  sinnvoll vorbelegt (Spot hoch oben und nach unten, Tür in Türgröße). Rechtsklick (ohne
  Umsehen) oder Shift+A im 3D-Viewport setzt es auf den Boden unter dem Mauszeiger,
  Rechtsklick in der Hierarchie als Wurzel, auf eine Zeile als Kind. Suchfeld mit
  Fokus, Enter nimmt den ersten Treffer; geprüft mit `addmenucheck`.
- **Asset-Browser**: Filter nach Name, Typ und Quelle; Raster, Liste oder Tabelle (nach
  jeder Spalte sortiert, mit Ordner, Quelle, Größe, Datum); Rechtsklickmenü, Doppelklick
  setzt ein Modell. **Import** per Dateidialog (Datei-Menü und Assets-Panel), mehrere
  Dateien auf einmal.
- **Materialvorschau**: das gewählte Material auf Kugel, Würfel oder Kachel, im Studiolicht.
- **Log-Fenster** (View > Inspect > Log): stdout und stderr mit Filter, Fehler und Warnungen
  farbig; das Terminal sieht weiterhin alles.
- **Import**: Unity-FBX und Videos.
- **Bildeditor** (Assets > Image editor, oder Rechtsklick auf eine Textur im
  Asset-Browser): ein kleines Photoshop für die Texturen eines Projekts – auch so
  aufgebaut: Werkzeugleiste mit Icons links (jedes mit Tooltip: Name, Taste, Erklärung;
  Rechtsklick zeigt die Varianten), Optionsleiste des Werkzeugs oben, rechts Farbe /
  Farbfelder und Ebenen / Protokoll, Menüs Datei, Bearbeiten, Bild, Ebene, Auswahl, Filter,
  Ansicht – mit Photoshops Tastenkürzeln (V M L W C I B S E G O T U H Z, Strg+T, Strg+J,
  Strg+L, Strg+M, Strg+U, Alt+Rück, Strg+F, Ziffern für die Deckkraft …). Mehrere Bilder
  in Tabs; Ebenen mit Vorschaubild, Deckkraft, Mischmodi (Multiplizieren, Negativ
  multiplizieren, Ineinanderkopieren, Weiches Licht, Addieren, Differenz, Farbe …),
  „Transparente Pixel fixieren“, Umsortieren per Ziehen, Ebene durch Kopie / Ausschneiden,
  Sichtbare zusammenfügen. Werkzeuge: Verschieben (mit automatischer Ebenenwahl), Auswahl
  Rechteck/Ellipse, Lasso und Polygon-Lasso, Zauberstab, Freistellen mit Griffen,
  Pipette, Pinsel, Kopierstempel (Alt+Klick setzt die Quelle), Radierer, Verlauf
  linear/radial, Füllen, Weichzeichner/Scharfzeichner, Abwedler/Nachbelichter/Schwamm,
  Text (Systemschriften), Formen, Hand, Zoom. Frei transformieren (Strg+T): skalieren,
  drehen, spiegeln mit Griffen oder Zahlen. Auswahl addieren/abziehen/schneiden (auch
  mit Shift/Alt), Rand verschieben, erneut auswählen, Auswahl verändern (Erweitern,
  Verkleinern, Weiche Kante, Rand, Abrunden), Kontur füllen, Fläche füllen.
  Korrekturen: Gradationskurven, Tonwerte, Helligkeit/Kontrast, Belichtung, Dynamik,
  Farbton/Sättigung, Farbbalance, Fotofilter, Umkehren, Tontrennung, Schwellenwert,
  Verlaufsumsetzung, Entsättigen, Auto-Farbton, Auto-Kontrast. Filter: Gaußscher und
  Bewegungsunschärfe, Unscharf maskieren, Rauschen, Median, Mosaik, Wolken, Relief,
  Konturen finden, Vignette, Hochpass, Verschiebungseffekt – und für Spiele: „Make
  tileable“ (nahtlos kachelbar, mit Kachelvorschau) und „Normal map from height“.
  Bildgröße, Arbeitsfläche, Freistellen, Zuschneiden, Drehen, Spiegeln; Zwischenablage
  auch mit anderen Programmen (Screenshot einfügen); Datei als Ebene platzieren.
  Speichern als PNG/JPG/TGA/BMP oder als PSD mit allen Ebenen (Photoshop öffnet sie, PSDs
  aus Photoshop öffnen sich hier mit ihren Ebenen) – Materialien, die das Bild benutzen,
  zeigen es sofort neu. Ohne ruhige Hand bedienbar: ein Strich schreibt absolut (noch mal
  drübermalen dunkelt nicht nach), der Pinsel hängt an einem Faden (Stabilisator), Linien
  von Klick zu Klick, Auswahl/Formen/Verläufe/Freistellen per Klick – Klick, das
  Polygon-Lasso Ecke für Ecke, jeder Wert ein Stepper, Raster zum Einrasten, jeder Schritt
  im Protokoll zurücknehmbar.
- **Wave-Editor** (Assets > Wave editor, Doppel- oder Rechtsklick auf einen Sound im
  Asset-Browser, oder Datei aufs Fenster ziehen): wav, ogg, mp3, flac öffnen, mehrere in
  Tabs. Wellenform je Kanal (Spitze über RMS, eingezoomt bis zu den einzelnen Samples),
  Zeitlineal, Übersicht als Scrollleiste, Mausrad-Zoom um den Mauszeiger, vertikaler
  Zoom. **Vorhören** über die Engine: ab Cursor, nur die Auswahl oder im Loop – genau so,
  wie das Spiel ihn spielt; verschobene Loop-Punkte hört man beim nächsten Durchlauf.
  Pegelanzeige mit Peak-Hold, Lautstärke, Tempo. **Loop-Punkte** als Marker (ziehen oder
  als Zahl), aus Auswahl/Cursor, auf Nulldurchgänge einrasten, „Find best end“ sucht das
  Ende, nach dem es am ähnlichsten weitergeht wie nach dem Start, „Crossfade seam“
  blendet die Naht weg, „Audition seam“ spielt den Übergang in Schleife. Gespeichert im
  smpl-Chunk der WAV – ein loopender Sound im Spiel (AudioSource, Wetter, Autos …)
  spielt dann den Anfang einmal und wiederholt nur den Loop. **Bearbeiten**: Ausschneiden,
  Kopieren, Einfügen (passt Kanäle und Abtastrate an), Löschen, Zuschneiden, Stille,
  Stille einfügen, Ein-/Ausblenden mit fünf Kurven, Gain, Normalisieren, Umkehren,
  Invertieren, DC entfernen, Stille abschneiden, Mono/Stereo, Resampling – auf beiden
  Kanälen oder einem, jeder Schritt rückgängig zu machen. **Analyse** der Auswahl: Peak,
  RMS, DC, Clipping, Spektrum. Speichern als WAV mit 16 (gedithert), 24 oder 32 Bit
  Float. Bedienbar ohne ruhige Hand: Klick setzt den Cursor, Shift+Klick das andere Ende
  der Auswahl, „Snap“ legt alles auf Nulldurchgänge; geprüft mit `wavecheck`.
- **Schaukeln** (Komponente „Swing“): Hängendes – Haken an der Kette, Lampe am Kabel,
  Schild, Sandsack – pendelt um seinen Aufhängepunkt, wenn es getroffen wird
  (`game.applyImpulse` mit Trefferpunkt; `weapon.lua` tut das), die Schwerkraft holt es
  zurück, die Dämpfung lässt es ausschwingen. Was in der Hierarchie darunter hängt,
  schwingt mit; bewegte Kollider gehen mit, damit der nächste Schuss es wieder trifft.
- **Zerbrechendes Glas**: Eine getroffene Scheibe (Material mit „Glass“ oder durchsichtig,
  nicht Cutout) reißt vom Einschuss aus in Scherben – klein am Loch, groß am Rahmen –, die
  mit dem Schuss fliegen, fallen, abprallen und flach liegen bleiben (`game.shatter`;
  `weapon.lua` tut das, der Schuss geht durchs Loch weiter). Die Scheibe wird in den
  Dreiecken gefunden, nicht eingerichtet: ein Fenster aus zwanzig Scheiben in einem
  importierten Modell verliert genau die getroffene, nur diese eine Kopie des Modells.
- **Decals**: Komponente „Decal“ – der Kasten des Objekts legt das Bild seines Materials
  auf alles, was darin liegt: modellierte Objekte, importierte Modelle, Gelände (Plakat
  an der Wand, Riss im Boden, Moos, Brandfleck). Echte Geometrie, aus den Dreiecken
  darunter geschnitten – beleuchtet, beschattet, im Spiel und im Pathtracer. Dazu
  `game.decal` für Einschusslöcher zur Laufzeit; `weapon.lua` setzt sie.
- **Kamera auf Textur**: ein Material zeigt live, was eine Kamera der Szene sieht
  (Monitor, Überwachungsbildschirm, Spiegel) -- Kamera, Bildgröße und "leuchtet wie
  ein Bildschirm" im Material-Panel; im Editor, im Play und im exportierten Spiel.
- **Maus fangen im Browser**: Der Web-Player holt sich den Mauszeiger (Pointer-Lock) bei jedem
  Klick oder Tastendruck, solange das Spiel ihn gefangen haben will – der Browser gibt ihn nur
  innerhalb einer Nutzeraktion her. Ego-Shooter drehen so die Sicht statt den Zeiger wandern zu
  lassen; Esc gibt ihn frei und beendet im Browser nicht mehr das Spiel.
- **Gebackene Lichter** („Baked only“ an der Light-Komponente): werden nicht live gezeichnet,
  nur ins Licht-Grid gebacken – direktes Licht an jeder Sonde (mit Sichtprüfung) und das
  Streulicht. Kosten zur Laufzeit nichts und zählen nicht gegen die 16 Live-Lichter: weiches
  Grundlicht für Räume, wie die Sektor-Helligkeit im alten Doom. Dazu: Licht-Grid-Bakes sehen
  die Lampen-Symbole des Editors nicht mehr (vorher steckte jede Lampe in ihrem eigenen Würfel
  und kam nie im Grid an), und Flächen lesen das Grid eine halbe Zelle vor sich statt auf sich
  (kein Abdunkeln durch Sonden unter dem Boden).
- **Beidseitige Materialien**: Häkchen „Double sided“ im Material-Panel (und
  `doubleSided` in `game.setMaterialProps`) – die Fläche wird von beiden Seiten gezeichnet,
  beschattet und wirft von beiden Seiten Schatten; die Rückseite ist beleuchtet wie die
  Vorderseite. Für Blätter, Gitter, Zäune, Stoff und Wände aus einer einzigen Fläche. Der
  glTF-Import übernimmt Blenders Einstellung bei Materialien mit Transparenz.
- **Leuchten nach Tageslicht, Fresnel-Rand, Schattenwurf abschaltbar**: „Glows“ im
  Material-Panel lässt die Emission immer, nur im Dunkeln (Nachtseite eines Planeten,
  Stadtlichter; auch im Schatten) oder nur im Sonnenlicht leuchten. „Rim“ sammelt Glühen
  und – bei Blend – Deckkraft an der Silhouette (Atmosphäre, Kraftfeld, Hologramm), „Rim
  soft edge“ blendet den äußersten Saum weich aus. „Casts shadows“ aus nimmt reine
  Leuchtkörper (Sonnenscheibe, Sternkuppel um die Szene) aus allen Schattenpässen.
- **Hierarchie im Gizmo**: Kinder folgen dem Elternteil beim Verschieben, Drehen und
  Skalieren -- ein importiertes Modell wächst an seiner Wurzel als Ganzes.
- **Komfort**: Undo, Autosave und eine Toolbar mit Piktogrammen.
- **Bedienung ohne ruhige Hand**: alles lässt sich auch klicken und eintippen statt
  ziehen.

## Auslieferung

- **„Export Game“** erzeugt einen spielbaren Ordner: `player.exe`, verschlüsselten
  Content im `.fpak`, Ladebildschirm, eingebautes Icon, Start-Modus (zu Fuß, Auto,
  Gleiter, Attract-Screen) und optional einen Inno-Setup-Installer. Der Ladebildschirm kann
  pro Szene ein eigenes Bild zeigen (`game.json` → `loading.sceneBackgrounds`, Szene → Bild):
  eine Karte pro Level beim Levelwechsel. Ein Pack verliert keine
  Datei: eine unlesbare bricht den Export ab, das Archiv wird erst unter einem Zwischennamen
  geschrieben.
- **Linux**: nativer Build mit `build-linux.sh` (Paket mit `build-deb.sh`), dedizierte GPU
  auf Hybrid-Laptops, Dateidialoge über zenity/kdialog, Wayland-Vollbild auf dem
  richtigen Schirm. Ein exportiertes Spiel bringt `<spiel>.png`, eine `.desktop`-Vorlage und
  `install-desktop-entry.sh` mit (Menüeintrag und Icon; `--remove` nimmt sie wieder heraus).
- **„Export for Web“** macht dasselbe Spiel zu einem Ordner für einen Webserver:
  `index.html`, der Browser-Player (`player.js`/`player.wasm`, WebAssembly + WebGL 2,
  gebaut mit `web\build-web.bat`) und `game.fpak`. Ein Player für alle Spiele, wie
  `player.exe`. Texturen werden auf ein einstellbares Maß verkleinert (Game Settings,
  Standard 2048), lange Sound-Schleifen der Engine auf eine Minute gekürzt; Spielstände
  bleiben im Browser (IndexedDB). Läuft auch auf Hostern ohne eigene Header: ein
  mitgelieferter Service Worker macht die Seite für Threads cross-origin-isoliert.
  Kopflos: `sandbox.exe --open <projekt> --export-web <ordner>` (ebenso `--export`).
  Im Browser fehlen Bildschirmraum-Reflexionen und Kontaktschatten. Punktlicht-Schatten
  laufen (wie am Desktop) über ein Textur-Array (`cubefacecheck` hält den Lookup gegen den
  Treiber).
  Shader prüft `python web/shadercheck.py` offline als GLSL ES inklusive Sampler-Budget.
- **Absicherung**: 74 Prüfprogramme ohne Fenster, `check-all.bat`, ein ASan-Build
  und der Shot-Harness.
- **Engine-Audit (Oktober 2026)**: alle Engine-Quellen durchgesehen, 160 Befunde nach
  Schwere. Alle kritischen und hohen sind behoben, von den mittleren 35 ganz und 4
  teilweise, fast jeder Fix mit einem Regressionstest: präparierte glTF-, Pak- und
  Patch-Dateien werden abgewiesen, Physik-Handles sind sicher gegen doppeltes Entfernen,
  NaN legt keinen Synth mehr still. Bericht und Stand: [engine-audit-2026-10.md](engine-audit-2026-10.md).

## Offen oder angefangen

- Aus dem Audit, weil sie Aussehen oder Verhalten ändern: sRGB-Texturen, fester
  Physik-Zeitschritt, indizierte Modelle, ein gemeinsames Material-Mapping für assimp und
  glTF, Hall mit Raumgröße, Sortierung nach Render-Zustand, Pak mit MAC.
- Prefabs Stufe 2 (verknüpfte Instanzen, Overrides).
- GPU-Tracer: HDRI und Tiefenschärfe fehlen noch.
- Schatten: die ferne Kaskade nur jeden zweiten Frame rendern.
- Dunst und Himmel bei Nacht.
- Abhängigkeiten auf feste Versionen pinnen.
- GitHub Pages für `docs/index.html` einschalten.

## Nice to have

- Retargeting: ein Knopf „Replace…“ neben „Add it to the character“ – die angesehene
  Bewegung ersetzt eine gewählte Animation der Figur, Name und Einstellungen bleiben.
  Bisher: alten Eintrag entfernen, neue Bewegung hinzufügen, auf den alten Namen umbenennen.
