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

### Ein Schiff auf dem Meer, auf dessen Deck man steht

Das Schiff (das leere Model-Objekt, unter dem die Teile des Modells hängen) bekommt
*Add Component > Buoyancy* und eine **statische** *Physics* (Dynamic aus). Buoyancy ohne
dynamische Physics heißt: Es wird nicht geworfen, es **reitet die Wellen** -- hebt sich,
stampft und rollt mit dem Wasser unter seiner Wasserlinie, gemächlich wie ein großes
Schiff, und alles darauf fährt mit, auch die Spielfigur. Die statische Physics am
Modell-Objekt lässt jedes Teil als seine eigenen Dreiecke kollidieren: Man läuft auf dem
Deck, wie es gezeichnet ist. Das Schiff im Editor so hinstellen, wie es bei ruhiger See
im Wasser liegt (Wasserlinie auf Höhe des Wasserspiegels) -- die Wellen kommen im Play
dazu. Damit man an Deck beginnt: einen *Player start* (Add-Menü, *Gameplay*) als Kind des
Schiffs auf das Deck setzen. Ein Floß oder eine Kiste, die herumgeworfen werden sollen,
bekommen dagegen eine *dynamische* Physics zur Buoyancy.

### Segel und Flaggen im Wind flattern lassen

Das Teil des Modells auswählen, das Segeltuch ist (alle Segel in einem Teil sind in
Ordnung), *Add Component > Sail*. Im Play hält jedes Tuchstück dort fest, wo es eine Rah,
einen Mast, ein Stag oder ein Tau berührt; der Bauch atmet mit den Böen, und Wellen laufen
zur freien Kante -- stärker und schneller, je mehr Wind weht (über dem Meer der Wind aus
dem Wasser-Panel, an Land der Wind der Bäume). **Billow** ist, wie weit der Bauch atmet,
**Flutter**, wie weit die freien Kanten schlagen (beides in Metern bei frischem Sturm).
Segel kollidieren nicht. Weil man Segel von beiden Seiten sieht: im Panel *Materials* beim
Segelmaterial *Double sided* einschalten, sonst sind sie von hinten unsichtbar.

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
| Strg+E, Strg+Alt+E | gewählte Kanten als UV-Naht markieren / Naht lösen (rot gezeichnet) |
| Num . | Gewähltes ins Bild |

Tab verlässt den Modus wieder; dann gelten die normalen Editor-Tasten.
Beispiel ohne Ziehen: `G Z 0.5 Enter` hebt die Auswahl um genau einen halben Meter.

## Der UV-Editor

*View > Objects > UV editor* – wie Blenders UV-Editor, für Meshes, die in fitzel
bearbeitbar sind (importierte Modelle behalten die UVs ihres Autors). Die Textur der Fläche
liegt im Hintergrund (ohne Textur ein Raster), darüber die Flächen, die im Modelling-Modus
im Flächenmodus gewählt sind – alle, solange keine gewählt ist oder *View > All faces* an ist.
Mausrad zoomt, mittlere Maustaste verschiebt, Home zeigt alles, F die Auswahl.

| Taste | Wirkung |
|---|---|
| 1 / 2 / 3 / 4 | Ecken / Kanten / Flächen / ganze Inseln wählen |
| Klick, Shift+Klick | wählen, dazu/weg (oder *Add* an: ohne Shift) |
| B | Rahmen: eine Ecke klicken, dann die andere (Shift dazu, Strg weg); Ziehen geht auch |
| A, Alt+A, Strg+I | alles, nichts, umkehren; L Insel unter dem Zeiger, Strg+L verbundene |
| G / R / S | verschieben / drehen / skalieren: der Zeiger führt, Klick oder Enter setzt, Rechtsklick oder Esc bricht ab; X / Y sperrt eine Achse, Strg rastet (Texel, 5°, 0,1), eine Zahl tippen ist exakt |
| U | Unwrap-Menü |
| M | Gewähltes in seiner Mitte zusammenführen |
| P, Alt+P | anheften (bleibt beim nächsten Unwrap, wo es ist) / lösen |
| Strg+Z, Strg+Y | rückgängig / wiederholen |

*Sticky* (an): Ecken, die an derselben Stelle an derselben Mesh-Ecke hängen, gehen mit –
eine Insel bleibt ganz. Aus: nur das Gewählte bewegt sich, so löst man eine Fläche heraus.
Unter der Fläche stehen U und V der Auswahl zum Schritt-für-Schritt-Verschieben (ein Schritt =
ein Texel), dazu ±90°, Spiegeln und Anheften als Knöpfe.

Menü *UV*:
- **Unwrap**: schneidet das Mesh an den Nähten auf und wickelt es winkeltreu ab (konform, wie
  Blenders „Conformal“). Angeheftete Ecken halten die Teile, wo sie sind; die anderen werden
  in Metern gemessen und in die Kachel gepackt. Ein geschlossenes Teil ohne Naht (ein ganzer
  Kasten) meldet das – erst Nähte setzen.
- **Smart UV Project**: ohne Nähte; Flächen, die in dieselbe Richtung zeigen (Winkelgrenze),
  werden je eine Insel. **Cube / Cylinder / Sphere Projection**, **Project in metres** (die
  Textur in Metern entlang einer Achse – eine Ziegelreihe über alle Wände), **Reset** (jede
  Fläche füllt die Kachel).
- **Seams from islands**, **Seams from sharp edges** (Kanten schärfer als der Winkel),
  **Clear seams**.
- **Pack Islands** (Rand, drehen), **Average Islands Scale** (gleiche Texturdichte),
  **Mirror U/V**, **Rotate ±90**, **Align U/V**, **Merge at Center / by Distance**, **Snap to
  Pixels**, **Pin / Unpin**.

Die Operationen wirken auf die Flächen, die im UV-Editor ganz gewählt sind – sonst auf alle
gezeigten. *View > Stretch* färbt die Flächen nach Texturdichte (blau weniger, rot mehr
als der Durchschnitt). Nähte setzt man im Modelling-Modus im Kantenmodus (*Mark seam* /
*Clear seam*, Strg+E / Strg+Alt+E). Typischer Weg: Kanten wählen, Strg+E, im UV-Editor U >
Unwrap, dann Pack.

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

## Die Reihenfolge der Level festlegen (nächstes Level laden)

In *File > Game Settings...* unter *Scene order* mit *+ Add scene* die Level eintragen und
mit *Up*/*Down* sortieren; *x* nimmt eins wieder heraus. Ein Skript geht dann mit
`Game.LoadNextScene()` (C#) bzw. `game.loadNextScene()` (Lua) zum nächsten Level und fragt
mit `Game.GetCurrentScene()` bzw. `game.currentScene()`, welche Szene gerade läuft. Nach dem
letzten Level lädt `LoadNextScene` nichts — was dann kommt, entscheidet das Skript. Die
Szenen der Reihenfolge kommen immer mit in den Export.

## Exportieren

*File > Export Game...* baut das Spiel als eigenständigen Ordner mit `player.exe`
(ohne Editor); *File > Export for Web...* baut es für den Browser. Vorher die Startszene
in *File > Game Settings...* wählen. Details: README, Kapitel „Shipping a game“.

## Der Bildeditor (Image editor)

*View > Assets > Image editor* oder Rechtsklick auf eine Textur im Asset-Browser: ein
kleines Photoshop mit Photoshops Werkzeugen, Menüs und Tasten. Ebenen stehen rechts unter
*Layers*; *File > Save* schreibt die Datei, Materialien zeigen sie sofort neu. Eine `.psd`
behält Ebenen, Masken und Ebenenstile.

### Im Bildeditor ein Bild aus einem Prompt machen (ComfyUI)

Der Knopf *New image from prompt* in der Menüleiste des Bildeditors (auch *File*, oder
*Strg+Alt+N*) öffnet ein eigenes Fenster. Beschreiben, was das Bild zeigt, Größe wählen,
*Generate* (oder *Strg+Enter*). ComfyUI malt es; das Bild öffnet sich in einem neuen Tab,
benannt nach den ersten Wörtern des Prompts und noch nicht gespeichert (*Strg+S*). Mit
*Onto the open picture, as a layer* landet es stattdessen als Ebene im offenen Bild.
Während ComfyUI arbeitet, zeigt das Fenster den Schritt und eine Vorschau; der Editor
bleibt bedienbar, *Cancel* bricht ab.

Unter *Server* steht die Adresse (Standard `http://127.0.0.1:8188`, ein Rechner im LAN
geht auch, wenn ComfyUI mit `--listen` läuft); *Test* sagt, ob er antwortet, welche
Grafikkarte er hat und ob ihm Modelldateien des Workflows fehlen. Ein *Start command*
(z. B. `systemctl start comfyui`) wird ausgeführt, wenn der Server nicht antwortet.
Unter *Workflow*: eingebaut ist ComfyUIs Vorlage Z-Image Turbo (braucht
`z_image_turbo_bf16.safetensors`, `qwen_3_4b.safetensors`, `ae.safetensors`); ein eigener
Workflow wird in ComfyUI mit *File > Export (API)* gespeichert und hier mit *From a
file...* gewählt – wohin Prompt, Größe, Seed und Schritte gehen, steht darunter.

### Im Bildeditor ein Bild ins Projekt speichern (Save in assets)

*File > Save in assets...* (*Strg+Alt+S*, nur mit offenem Projekt) speichert ohne
Dateidialog in die Ordner des Projekts: Name, Ordner (`textures/`, wohin auch Importe
gehen, oder ein Ordner, in dem schon Bilder des Projekts liegen) und Format (PNG, JPG,
TGA – was Materialien lesen). Das Bild erscheint gleich unter *Assets*; der Tab ist danach
diese Datei, *Strg+S* speichert wieder dorthin. Gibt es den Namen schon, wird der Knopf
rot (*Replace*): Ersetzen ändert jedes Material, das die Datei benutzt. Ebenen werden in
die Datei zusammengefügt und bleiben im Editor erhalten.

### Im Bildeditor eine Normal Map erzeugen

*Image > Generate normal map...* liest die Helligkeit des Bildes als Höhe und legt die
Normal Map in einem eigenen Tab an (`<name>_normal`); das Bild selbst bleibt unverändert.
Während der Dialog offen ist, zeigt der Tab das Ergebnis sofort: *Strength* (wie steil),
*Smoothing* (Körnung und JPEG-Blöcke glätten), *Flatten light* (bei Fotos: großflächige
Schattierung herausnehmen, sonst wird der Lichtabfall zur Schräge), *Dark is high*
(Fugen und Risse, die hell sind; oder Beulen zu Dellen machen) und die Richtung von Grün:
*Up (OpenGL)* für fitzel, glTF, Blender, Unity, *Down (DirectX)* für Unreal. *OK* behält den
Tab, *Cancel* verwirft ihn. Mit *Strg+Alt+S* landet die Map danach im Ordner ihrer Textur.

### Im Bildeditor ein Material aus den Ebenen bauen

*Image > Build material from layers...* (mit offenem Projekt) macht aus einem Bild mit
Ebenen ein ganzes Material. Was eine Ebene ist, sagt das **letzte Wort ihres Namens**:

| letztes Wort | wird |
|---|---|
| Normal, nrm | Normal Map |
| Height, Bump, Displacement, Höhe | Normal Map (wenn es keine Normal-Ebene gibt) |
| Roughness, Rough, Rauheit | Rauheit |
| Gloss, Smoothness, Glanz | Rauheit, umgekehrt |
| Metal, Metallic, Metall | Metall |
| AO, Occlusion | Ambient Occlusion |
| Emission, Emissive, Glow, Leuchten | Leuchten |
| Opacity, Alpha, Cutout, Deckkraft | Transparenz (Cutout) |

„map“, „copy“ und Zahlen am Ende zählen nicht („Normal map“, „Roughness copy 2“),
CamelCase und Unterstriche gehen auch („brick_normal“, „WallRoughness“). Nur das letzte
Wort: „Metal plate“ ist eine Farbebene. **Alle anderen sichtbaren Ebenen** ergeben
zusammen die Farbtextur, so wie man sie sieht. Map-Ebenen zählen auch versteckt – man kann
sie ausblenden, während man an der Farbe malt; wo sie durchsichtig sind, gilt der neutrale
Wert (flach, mittleres Grau, kein Metall, keine Verdeckung, kein Leuchten, deckend).

Der Dialog zeigt für jede Ebene, was sie wird, und welche Dateien entstehen:
`<name>_color.png`, `_normal.png`, `_orm.png` (Rauheit, Metall und AO in einem Bild, wie
die Materialien es lesen), `_emission.png`, `_opacity.png`. *Build* schreibt sie und legt
in der Materialbibliothek das Material `<name>` an – oder aktualisiert es, wenn es das
schon gibt: Neu bauen ändert also das Material, das die Objekte schon tragen. Nur die Maps
richten sich nach den Ebenen; Glas, Tönung und die anderen Einstellungen bleiben. Die
Regler für Rauheit und Metall stehen danach auf dem Mittelwert ihrer Map. Gespeichert wird
das Material mit dem Projekt (*File > Save*).

### Im Bildeditor eine Ebenenmaske anlegen und bemalen

Der Maskenknopf unter der Ebenenliste (Rahmen mit Kreis) oder *Layer > Layer mask >
Reveal all / Hide all / Reveal selection / Hide selection*. Mit einer Auswahl wird sie zur
Maske. Danach malen alle Werkzeuge auf der Maske: Schwarz verbirgt, Weiß zeigt, Grau halb;
Verlauf, Filter und *Strg+I* (Umkehren) gehen auch. Die Maskenminiatur neben der Ebene:
Klick = Maske bemalen, Klick aufs Ebenenbild = wieder die Ebene bemalen (oder *Strg+\\*),
Shift+Klick = Maske aus/an, Alt+Klick = Maske allein anzeigen, Strg+Klick = Maske als
Auswahl. *Layer > Layer mask > Apply* rechnet sie in die Transparenz ein, *Delete* wirft sie weg.

### Im Bildeditor einen Schlagschatten, eine Kontur oder einen Schein geben (Ebenenstil)

Der *fx*-Knopf unter der Ebenenliste, Doppelklick aufs Ebenenbild oder *Layer > Layer
style*: Schlagschatten (*Drop shadow*), Kontur (*Stroke*), Schein nach außen/innen (*Outer
glow*, *Inner glow*), Schatten nach innen (*Inner shadow*), abgeflachte Kante und Relief
(*Bevel & emboss*), Farb- und Verlaufsüberlagerung. Links Effekte an- und abhaken, rechts
ihre Werte; das Bild zeigt es sofort, *OK* behält es als einen Schritt, *Cancel* nimmt es
zurück. Die Effekte folgen der Form der Ebene, auch wenn man weitermalt. *Fill* (rechts
neben dem Schloss) blendet die Ebene selbst aus, die Effekte bleiben. Unter *Blending
options* gibt es fertige Stile (Sticker, Raised, Glow, Pressed in); *Layer > Layer style*
kann Stile kopieren, einfügen und löschen.

### Im Bildeditor nach Farbe auswählen (Color range)

*Select > Color range...* öffnet rechts ein Panel. Ins Bild klicken nimmt die Farbe dort
auf (Shift+Klick fügt eine hinzu, Alt+Klick nimmt eine weg); *Fuzziness* bestimmt, wie
ähnlich eine Farbe sein muss – es läuft weich aus. Statt Farben zu klicken: *Select* auf
*Reds*, *Blues*, *Highlights*, *Shadows*, *Skin tones* usw. Die kleine Vorschau zeigt die
Auswahl (weiß = ausgewählt) oder das Bild, *Invert* kehrt um, die vier Knöpfe ersetzen die
alte Auswahl oder addieren dazu. *OK* (Enter) übernimmt, *Cancel* (Esc) verwirft.
Für eine zusammenhängende Fläche einer Farbe ist der Zauberstab (*W*) schneller.

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
| Objects | Modeling / Mesh paint / UV editor / Procedural | Mesh bearbeiten / bemalen / UVs abwickeln / prozedurale Objekte |
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
