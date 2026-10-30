# Fitzel — den Editor bedienen

Wie man im Editor arbeitet: Bildschirm, Kamera, Auswahl, Verschieben, Objekte anlegen,
Hierarchy und Inspector, der Modelling-Modus, Play, Projekte, Szenen und Export.
Was die einzelnen Funktionen können, steht in [FEATURES.md](FEATURES.md) und in der
README. Menüs und Knöpfe heißen im Editor englisch und sind hier genau so geschrieben.
Alle Panels liegen im Menü *View*: Schreibt ein Handbuch *Assets → Scripts* oder
*Construct > Roads*, ist *View > Assets > Scripts* bzw. *View > Construct > Roads* gemeint.

## Auf einen Blick

- Kamera: rechte Maustaste halten = umsehen, dabei W A S D fliegen, Q/E runter/hoch;
  mittlere Maustaste ziehen = verschieben (Pan); Mausrad = heranfahren, Strg+Rad = Brennweite.
  F = Auswahl ins Bild holen. Num 1/3/7 = vorne/rechts/oben (mit Strg: hinten/links/unten),
  Num 5 = orthografisch; die Zifferntasten oben gehen auch.
- Auswahl: Linksklick im Viewport oder in der Hierarchy; Strg+Klick nimmt dazu/weg,
  Strg+Ziehen im Viewport = Rahmen. Esc = Werkzeug weg, zweites Esc = Auswahl weg.
- Gizmo: Q verschieben, W drehen, E skalieren, X Welt/Lokal; Strg beim Ziehen = in Schritten.
- Anlegen: Formen-Knöpfe in der Toolbar, Rechtsklick (oder Shift+A) im Viewport = Add-Menü,
  Rechtsklick in der Hierarchy = Add child. Shift+Rechtsklick setzt den 3D-Cursor, Shift+S = Snap-Menü.
- Entf = löschen, Strg+J = Meshes vereinen, Strg+Z / Strg+Y = Rückgängig/Wiederholen,
  Strg+S = Projekt speichern.
- Tab = Modelling-Modus (Ecken/Kanten/Flächen, Blender-Tasten).
- Play/Stop oben rechts in der Menüleiste; Esc beendet Play. Shift+F = zu Fuß gehen,
  V = Fahrzeug fahren, G = Gleiter fliegen, B = Boot steuern.
- Panels: alle im Menü View, nach Aufgabe gruppiert (World, Planting, Construct, Objects,
  Assets, Presentation, Inspect). Strg+Leertaste = Bereich auf ganzen Bildschirm.

## Der Bildschirm

Oben die **Menüleiste**: *File*, *Scene*, *Edit*, *View*, *Help* und rechts daneben
**|> Play** (im Spiel **[ Stop ]**). Darunter die **Toolbar** mit Speichern, Select/Create,
den Grundformen, Terrain, Kamera-Knöpfen, den Gizmo-Modi und den häufigsten Werkzeug-Panels.

Die Fläche darunter ist in **Bereiche** (Areas) geteilt, wie in Blender. Standard:
links die **Hierarchy** (alle Objekte der Szene als Baum), in der Mitte der **3D-Viewport**
(Fenster *Scene*), rechts der **Inspector** über einem Werkzeugbereich. Jeder Bereich hat
oben eine schmale Kopfzeile: ein Aufklappmenü wählt, welches Panel er zeigt, Knöpfe teilen
ihn nebeneinander oder übereinander und fügen ihn wieder zusammen. **Strg+Leertaste** gibt
dem Bereich unter der Maus den ganzen Bildschirm und wieder zurück.

*View > Reset layout* stellt die Standardaufteilung wieder her, *View > Close all panels*
schließt alle Werkzeug-Panels. *View > Interface* stellt Schriftgröße und Schriftart ein.
Bis zu vier 3D-Viewports gleichzeitig: *View > General > 3D viewport 2/3/4*.

## Die Kamera im Viewport

| Eingabe | Wirkung |
|---|---|
| rechte Maustaste halten + Maus | umsehen |
| rechte Maustaste halten + W A S D | vor, zurück, links, rechts fliegen |
| rechte Maustaste halten + E / Q | hoch / runter |
| mittlere Maustaste ziehen | Ansicht verschieben (Pan): was man greift, bleibt unter dem Zeiger |
| Mausrad | heranfahren / zurückfahren (Dolly) |
| Strg + Mausrad | Brennweite (Field of View) enger/weiter |
| F | die Auswahl ins Bild holen (Frame) |
| Num 1 / Strg+Num 1 | Ansicht von vorne / hinten |
| Num 3 / Strg+Num 3 | von rechts / links |
| Num 7 / Strg+Num 7 | von oben / unten |
| Num 5 | perspektivisch ↔ orthografisch |

Ohne Ziffernblock gehen die Zifferntasten der oberen Reihe genauso. Dieselben Ansichten
stehen im Menü *View > Viewpoint* — dafür muss man nichts gedrückt halten.
W/A/S/D/Q/E fliegen nur, solange die rechte Maustaste gehalten wird; sonst sind Q/W/E die
Gizmo-Tasten.

## Auswählen

- **Linksklick** auf ein Objekt im Viewport wählt es aus. Liegen mehrere übereinander,
  wählt ein weiterer Klick an derselben Stelle das nächste.
- **Strg+Klick** nimmt ein Objekt zur Auswahl dazu oder wieder heraus;
  **Strg+Ziehen** zieht einen Rahmen und nimmt alles darin dazu.
- In der **Hierarchy**: Klick wählt, Strg+Klick nimmt dazu/weg.
- **Esc** legt zuerst das Transform-Werkzeug weg, ein zweites Esc hebt die Auswahl auf.
- Was ausgewählt ist, zeigt der Inspector; bei mehreren das zuletzt angeklickte.

## Verschieben, Drehen, Skalieren (Gizmo)

Am ausgewählten Objekt sitzt das **Gizmo**. Man zieht an seinen Pfeilen, Ringen oder Würfeln.

| Taste | Gizmo |
|---|---|
| Q | Move — verschieben (Strg: am Raster einrasten) |
| W | Rotate — drehen (Strg: in Schritten) |
| E | Scale — skalieren (Strg: in Schritten) |
| X | Welt- oder Objekt-Achsen (World/Local) |

Dieselben Modi sind Knöpfe in der Toolbar. Genaue Zahlen tippt man im **Inspector** ein
(Position, Rotation, Scale). Skalieren mit dem Gizmo nimmt Kinder in der Hierarchie mit.
*View > Objects > Grid* schaltet das Konstruktionsraster, *View > Objects > 3D cursor*
den 3D-Cursor ein.

### Den 3D-Cursor setzen

Der 3D-Cursor ist ein Punkt in der Szene, an dem neue Objekte erscheinen.
**Shift+Rechtsklick** im Viewport setzt den Cursor dorthin, wo man klickt (auf den Boden
oder ein Objekt). **Shift+S** öffnet das Snap-Menü: *Cursor to World Origin*,
*Cursor to Grid*, *Cursor to Terrain*, *Cursor to Selection* setzen den Cursor;
*Selection to Cursor* und *Selection to Grid* bewegen die Auswahl dorthin.
Genaue Koordinaten tippt man im Panel *3D cursor* (*View > Objects > 3D cursor*) ein.

## Objekte anlegen

- **Toolbar, Grundformen:** Box, Ramp, Cylinder, Sphere, Plane, Light, Empty. Im Modus
  *Select* legt ein Klick auf den Knopf die Form sofort am 3D-Cursor (oder vor der Kamera) an.
  Im Modus *Create* wählt der Knopf nur die Form, und jeder Klick auf freien Boden im
  Viewport setzt eine. Esc geht zurück zu *Select*.
- **Terrain:** der Terrain-Knopf der Toolbar legt den Boden an (eine Szene hat einen).
- **Add-Menü:** Rechtsklick (ohne Ziehen) oder **Shift+A** im Viewport öffnet es an der
  Mauszeiger-Stelle. Es hat Gruppen und ein Suchfeld: *Shapes*, *Lights* (Point, Spot …),
  *Cameras* (Camera, Main camera, Follow camera, Camera switcher …), *Gameplay*
  (Player start, Trigger, Scene trigger, Spawner, Collectible, Door, Door opener, Lift, Mover,
  Moving platform, Turntable, Seesaw, Script), *Physics* (Physics box/ball, Static collider, Floating box, Wave maker),
  *Audio* (Audio source, Sound zone, Reverb zone, Synth), *Effects* (Particles,
  Volumetric fog, Decal), *Racing* (Start / finish, Checkpoint, Grid position, Boost pad),
  *World* (Terrain …).
- **Kameras:** *Camera from view* (**Strg+Alt+C**) stellt eine Kamera dorthin, wo man gerade
  steht; *Align to view* (**Strg+Alt+0**) bewegt die ausgewählte Kamera (oder ein Objekt)
  dorthin. Beide auch als Toolbar-Knöpfe.
- **Modelle:** aus dem Panel *Assets* in den Viewport ziehen — sie landen auf dem Boden.
  Eine Textur auf ein Objekt gezogen gibt ihm ein neues Material mit dieser Textur.
- **Prefabs:** im Panel *Prefabs* eins wählen und *Add* (oder Rechtsklick > *Add to scene*).
- Eigene Dateien (Modelle, Texturen, Sounds) holt *File > Import Assets...* ins Projekt.

### Bewegte Plattformen, Lifts und Drehscheiben

- *Moving platform* (Add-Menü, *Gameplay*) ist ein Boden mit **Mover** und statischer
  **Physics**: Er fährt im Play von seinem Platz um *Offset* und zurück und nimmt mit, was
  darauf steht. *Duration* ist die Zeit für hin und zurück, *Pause* die Wartezeit an jedem
  Ende (ein Lift, der an den Stockwerken hält; ein Stempel zwischen zwei Stößen), *Start
  phase* (0 bis 1) verschiebt den Takt, damit mehrere nacheinander arbeiten.
- *Turntable* ist eine Scheibe mit **Spinner**: Sie dreht sich mit *Speed* Grad pro Sekunde
  um ihre *Axis*. Ein Spinner mit *Rock* über 0 wippt stattdessen hin und her (*Seesaw*).
  Um einen anderen Punkt drehen: das Objekt unter ein Empty an diesem Punkt hängen und
  dem Empty den Spinner geben.
- Ohne Physics-Komponente bewegt sich nur das Bild. Kinder mit eigener statischer Physics
  bewegen sich als Kollider mit (Kehrarme, Geländer auf einer Plattform).

## Hierarchy

Der Baum aller Objekte. Ziehen und Fallenlassen auf ein anderes Objekt macht es zum Kind
(es bewegt sich dann mit), auf freie Fläche im Baum wieder zur Wurzel.
**F2** oder Doppelklick benennt um. **Rechtsklick auf eine Zeile:** *Rename*, *Duplicate*,
*Save as Prefab...*, *Unpack Prefab*, *Create Empty Parent*, *Create Empty Child*,
*Add child* (das ganze Add-Menü unter diesem Objekt), *Delete*; bei Kameras zusätzlich
*Set as Main Camera* und *Align to view*, bei Fahrzeugen *Add headlights*.
Rechtsklick auf freie Fläche: das Add-Menü für ein neues Objekt an der Wurzel.

## Inspector

Zeigt das ausgewählte Objekt: Name, aktiv, Transform (Position, Rotation, Scale), Material
und seine **Komponenten** — jede aufklappbar, mit ihren Eigenschaften. **Add Component**
unten fügt eine hinzu (Script, Physics, Audio Source, Particles, Vehicle …); der Knopf
*Remove this component* in ihrer Kopfzeile entfernt sie wieder.

Alle Zahlenfelder kann man ziehen oder per Doppelklick eintippen — für ruhige Arbeit ohne
präzises Ziehen ist Eintippen immer möglich.

### Ein Skript an ein Objekt hängen

1. Das Objekt auswählen (Viewport oder Hierarchy).
2. Im Inspector **Add Component > Script** wählen.
3. In der Script-Komponente aus der Liste die Skriptdatei wählen — dort stehen alle
   `.lua`- (und `.cs`-)Dateien aus dem Ordner `scripts/` des Projekts. *Edit* öffnet sie im
   Skript-Editor.
4. Die Parameter des Skripts (seine globalen Variablen oben in der Datei) erscheinen darunter
   unter *Script parameters* und lassen sich pro Objekt einstellen.
5. **Play** drücken: Das Skript läuft. Fehler zeigt die Komponente rot und das Panel *Log*.

Ein neues Skript schreibt man im Panel *Scripts* (*View > Assets > Scripts*); der Assistent
kann eins schreiben und anhängen (`write_script`, `attach_script`).

### Sounds eines Skripts ändern

Ist ein Skript-Parameter ein Sound (seine Vorgabe ist eine Datei wie `"kristall.wav"`), zeigt
die Script-Karte statt eines Textfelds die **Sound-Auswahl** über die Dateien in `sounds/`,
daneben **Preview** zum Anhören. Preview spielt mit den Parametern gleichen Namens:
zu `GemSound` gehören `GemVolume` (Lautstärke) und `GemPitch` (Tonhöhe, 1 = unverändert,
2 = eine Oktave höher). Eine neue Sounddatei in `sounds/` legen, dann steht sie in der Liste.

### Haare, Schmuck oder Brust einer Figur nachschwingen lassen

Die animierte Figur auswählen, *Add Component > Spring Bones*. Unter **Bones** die Namen
der Knochen eintragen, die schwingen sollen, mit Komma getrennt (zum Beispiel
`lPectoral, rPectoral` bei einer Daz-Figur, oder die Knochen eines Zopfs) -- so, wie sie
im Modell heißen (in Blender: die Knochen der Armature). **Frequency** (Hz) macht
die Feder straffer (höher) oder weicher (niedriger), **Damping** lässt sie schneller
ausschwingen (1 = gar kein Nachwippen), **Amount** ist die Stärke (0 = aus, 2 = doppelt),
**Max angle** der größte Ausschlag. Zu sehen ist es im Play, wenn sich die Figur dreht
oder geht.

## Bearbeiten, Rückgängig, Speichern

- **Strg+Z** / **Strg+Y** (oder Strg+Shift+Z): Rückgängig / Wiederholen, auch über *Edit*.
  Jede Änderung an der Szene ist ein Schritt.
- **Entf**: löscht die Auswahl (nicht, solange ein Textfeld aktiv ist).
- *Edit > Duplicate*, *Edit > Delete*, *Edit > Join meshes* (**Strg+J**: ausgewählte Meshes
  werden ein Mesh), *Edit > Save as Prefab...*, *Edit > Clear objects* (alles außer der Sonne).
- **Strg+S** speichert das Projekt (wie *File > Save Project* und der erste Toolbar-Knopf);
  nicht während Play. Stürzt der Editor ab, bietet er beim nächsten Start die automatische
  Sicherung zum Wiederherstellen an.

## Der Modelling-Modus (Tab)

**Tab** im Viewport bei einem ausgewählten Mesh oder einer Grundform schaltet in den
Modelling-Modus (wie Blenders Edit Mode); eine Grundform wird dabei zum editierbaren Mesh.
Das Panel *Modeling* (*View > Objects > Modeling*) hat jede Operation auch als Knopf mit
Zahlenfeld. Im Modus gelten Blenders Tasten:

| Taste | Wirkung |
|---|---|
| 1 / 2 / 3 | Ecken / Kanten / Flächen wählen |
| Klick, Shift+Klick, Ziehen | wählen, dazu/weg, Rahmen; Alt+Klick = Ring (Loop) |
| A, Alt+A, Strg+I | alles, nichts, umkehren; L / Strg+L = verbundene |
| G / R / S | mit der Maus verschieben / drehen / skalieren; X Y Z sperrt eine Achse, eine Zahl tippen, Enter bestätigt, Esc bricht ab |
| E, I, Strg+B | Extrudieren, Inset, Fase (Mausrad: Segmente) |
| Strg+R | Loop Cut |
| Shift+D | duplizieren und verschieben |
| F, J | Fläche füllen, verbinden |
| M, X/Entf, Alt+N | Zusammenführen-, Löschen-, Normalen-Menü; Shift+N Normalen nach außen |
| Num . | Gewähltes ins Bild |

Tab verlässt den Modus wieder; dann gelten die normalen Editor-Tasten.
Beispiel ohne Ziehen: `G Z 0.5 Enter` hebt die Auswahl um genau einen halben Meter.

## Play

**|> Play** in der Menüleiste startet die Szene als Spiel: Skripte laufen, Physik läuft,
man steht am *Player start* (oder an der Kamera). **[ Stop ]** oder **Esc** beendet es und
stellt die Szene genau so wieder her, wie sie vor Play war — was im Spiel passiert, wird
nicht gespeichert. Im Play: F8 nimmt auf (Panel *Recorder*), F9 öffnet die Grafikeinstellungen.

Zum Ausprobieren auch im Editor:

| Taste | Wirkung |
|---|---|
| Shift+F | zu Fuß gehen (Ego-Sicht, Maus schaut) — nochmal: zurück |
| V | das nächstgelegene Fahrzeug fahren (W A S D, Leertaste bremst) — nochmal: aussteigen |
| G | den nächstgelegenen Gleiter fliegen — nochmal: aussteigen |
| B | Boot steuern bzw. aussteigen |
| C | beim Fahren: Kameras des Fahrzeugs durchschalten |
| F11 | Vollbild ohne Oberfläche |
| F3 | Performance-Fenster |

Fehler aus Skripten stehen im Panel *Log* (*View > Inspect > Log*).

## Projekte und Szenen

- **Projekt** = ein Ordner mit Szenen (`.fitzel`), Skripten (`scripts/`), Materialien und
  Assets. *File > New Project...* (Name, Ort, Skriptsprache Lua oder C#),
  *File > Open Project* (zuletzt benutzte oder *Browse folder...*), *File > Save Project*,
  *File > Save Project As...*.
- **Szenen** im Menü *Scene*: *New Scene...*, *Save Scene*, *Rename Scene...*,
  *Delete Scene...*, und darunter die Szenen des Projekts zum Umschalten.
  *Reload Models from Disk* lädt in Blender geänderte Modelle neu.
- *File > Game Settings...* legt die Startszene und weitere Spiel-Einstellungen fest.

## Exportieren

*File > Export Game...* baut das Spiel als eigenständigen Ordner mit `player.exe`
(ohne Editor); *File > Export for Web...* baut es für den Browser. Vorher die Startszene
in *File > Game Settings...* wählen. Details: README, Kapitel „Shipping a game“.

## Welches Panel wofür

Alle Panels öffnet das Menü **View**; die häufigsten auch die Toolbar.

| Gruppe | Panel | wofür |
|---|---|---|
| World | Terrain | Gelände anlegen und erzeugen (Größe, Höhe, Rauschen) |
| World | Terrain sculpt | Gelände formen: anheben, senken, glätten, Erosion (Pinsel) |
| World | Terrain paint | Bodenschichten malen (Gras, Fels, Sand …) |
| World | Water / Rivers & brooks | Seen und Meer / Flüsse und Bäche |
| World | Sky & atmosphere | Wolkenschichten, Dunst |
| World | Weather & audio | Wetter-Presets, Regen, Sturm, Umgebungsklang |
| World | Environment | Tageszeit, Sonne, Himmelsbild |
| World | Post processing / Shadows & reflections | Bildlook, Schatten- und Spiegelqualität |
| Planting | Vegetation / Scatter / Tree generator | Bäume, Wälder, Gras / Objekte streuen / eigene Bäume |
| Construct | Roads | Straßen: Toolbar-Knopf Road editor, Klick = Punkt, Ziehen = verschieben, Strg+Ziehen = Höhe, Entf = Punkt löschen, dann Build |
| Construct | Splines & bridges | Zäune, Mauern, Gleise, Brücken entlang einer Kurve |
| Construct | City / Town generator / Houses / Buildings | Städte und Gebäude erzeugen |
| Construct | Level generator / Stairs / Street signs | Levels, Treppen, Straßenschilder |
| Construct | Vehicle / Glider | Fahrzeug und Gleiter einstellen |
| Objects | Modeling / Mesh paint / UV / Procedural | Mesh bearbeiten / bemalen / UVs / prozedurale Objekte |
| Objects | 3D cursor / Grid | 3D-Cursor und Raster |
| Assets | Materials / Models / Prefabs / Assets | Materialien, Modelle, Prefabs, alle Dateien des Projekts |
| Assets | Scripts / C# Editor | Lua- bzw. C#-Skripte schreiben |
| Assets | Image editor / Wave editor / Synth | Texturen, Sounds, Synthesizer |
| Assets | Retarget animations / Hold Pose Studio | Animationen auf andere Figuren übertragen / Haltepose |
| Assets | Assistant (AI) | dieser Assistent |
| Presentation | Camera / Camera path / Timeline / Sequencer | Kameras, Kamerafahrten, Animation, Schnitt |
| Presentation | Animation graph | Zustandsautomat für Figuren-Animationen |
| Presentation | Render | Offline-Bild mit dem Pathtracer |
| Presentation | Mixer / Recorder / UI Overlay | Mischpult, Aufnahme, Spiel-Oberfläche |
| Inspect | Performance (F3) / Stats / Log | Framezeit, Zahlen, Ausgaben und Fehler |
