# S.T.A.L.K.E.R.-Level nach fitzel holen

`stalkerimport` liest ein Level direkt aus den `.db`-Archiven eines installierten
S.T.A.L.K.E.R. und schreibt es als Modell und Szene in ein fitzel-Projekt. Nichts muss vorher
entpackt werden, und das Spiel bleibt unverändert.

Lesbar sind die Archive von Clear Sky, Call of Pripyat und den Enhanced Editions von 2025
(auch Shadow of Chornobyl EE). Die verschlüsselten Archive des originalen Shadow of Chernobyl
von 2007 kann das Werkzeug nicht lesen und sagt das auch. Lose Dateien unter `gamedata/` gehen
vor die Archive, wie im Spiel selbst; ein Mod wird also mit übernommen.

Die Spielinhalte gehören GSC Game World. Das Werkzeug ist dafür da, die eigene Kopie in fitzel
anzusehen und damit zu arbeiten. Die erzeugten Dateien bitte nicht weitergeben.

## Aufruf

Gebaut wird das Werkzeug mit dem Rest (`cmake --build build/linux --target stalkerimport`) und
liegt dann in `build/linux/bin`. Welche Level ein Spiel hat:

```bash
build/linux/bin/stalkerimport "/pfad/zu/STALKER Shadow of Chornobyl - EE" --list
```

Ein Level in ein Projekt (der Ordner wird angelegt, wenn es ihn noch nicht gibt):

```bash
build/linux/bin/stalkerimport "/pfad/zu/STALKER Shadow of Chornobyl - EE" l01_escape ~/Documents/Fitzel/stalker
```

Statt des Namens nimmt `all` alle Level. Ein Level braucht zwei bis fünf Sekunden, das ganze
Spiel gut 30 Sekunden und etwa 2,5 GB Platz. Danach das Projekt im Editor öffnen
(*File > Open Project*), die Szene des Levels laden und Play drücken. Damit die
Levelübergänge irgendwohin führen, alle Level konvertieren.

Optionen:

| Option | Wirkung |
|---|---|
| `--cell <m>` | Kantenlänge einer Kartenzelle in Metern, Standard 64. Eine Zelle ist ein Knoten des Modells und ein Objekt in der Hierarchy. |
| `--max-texture <px>` | Größere Texturen werden bis auf diese Kante halbiert, Standard 2048. |
| `--terrain <px/m>` | Texel pro Meter der gebackenen Terrain-Texturen, Standard 8. `0` nimmt die eine gestreckte Farbkarte des Spiels. |
| `--no-props` | Ohne die Requisiten aus `level.spawn`. |
| `--no-life` | Ohne Figuren, Anomalien, Sounds und Logik: nur die Kulisse. |
| `--no-lift` | Behält die Höhen des Spiels (siehe unten). |
| `--no-scene` | Nur Modelle und Texturen, keine Szene. |

Mit `--extract <präfix> <ordner>` entpackt das Werkzeug Dateien aus den Archiven, so wie sie
sind, etwa `--extract levels/l01_escape/ /tmp/escape` oder `--extract config/ /tmp/config`.
`--files [präfix]` listet nur, was es gibt (`--files sounds/anomaly/`).

## Was im Projekt landet

- `models/<level>.glb`: die Levelgeometrie, ein Knoten pro Kartenzelle, darin eine Primitive
  pro Material. Die `.meta` daneben behält ihre GUID, wenn man das Level noch einmal
  konvertiert. Szenen, die auf das Modell zeigen, finden es also weiter.
- `models/<level>_props.glb`: die Requisiten, die `level.spawn` aufstellt (Kisten, Fässer,
  Lampen, Radios, Fahrzeugwracks), ein Knoten pro Stück, schon an Ort und Stelle gedreht.
  Kreaturen, Waffen, Munition und Essen bleiben weg, die gehören zum Spiel und nicht zum Level.
  Ebenso der geskriptete Hubschrauber, der sonst in der Luft hinge.
- `models/<level>/`: die Texturen als JPG (deckend) oder PNG (mit Alpha), dazu
  `terrain_<x>_<z>.jpg`, die gebackene Terrain-Textur jeder Zelle.
- `<level>.fitzel`: die Szene mit Sonne, Player Start, den beiden Modellen als Gruppen und den
  Lichtern des Levels. Beide Gruppen tragen eine statische Physics-Komponente, die
  Levelgeometrie und die Requisiten kollidieren also als ihre Dreiecke. Ausgeschnittenes wie Laub
  und Zäune kollidiert nicht.
- `models/characters/`: die Figuren, ein GLB pro Modell des Spiels mit Skelett, Skin und
  Animationen (Texturen in `textures/` daneben). Alle Level teilen sie sich.
- `sounds/stalker/`: die Sounds, die die Level brauchen, als Ogg, so wie das Spiel sie hat.
  Die Namen sind flach (`xr_anomaly_electra_idle1.ogg`), weil fitzel einen Sound am
  Dateinamen findet.
- `scripts/stalker/`: die Logik. `levels/<level>.lua` hält, was die Daten des Levels sagen,
  die übrigen Dateien spielen es (siehe unten). Ein neuer Import schreibt beides neu; wer ein
  Skript ändern will, kopiert es besser unter anderem Namen.
- `game.json`, falls das Projekt noch keine hat.

Der Player Start steht im ersten Level am Spawn des Spielers (in Escape im Bunker des Händlers).
Die anderen Level haben keinen, dort steht er auf dem KI-Wegpunkt, der der Kartenmitte am
nächsten liegt. Die liegen immer auf begehbarem Boden.

## Das Leben des Levels

Die Szene bekommt eine Gruppe *Life* mit den Figuren, *Anomalies* mit den sichtbaren
Anomalien, *Sounds* mit den Klangquellen des Levels und ein Objekt *Zone*. Dessen Skript
`stalker/zone.lua` liest die Daten des Levels und spielt alles Weitere.

**Figuren.** Jeder Stalker, Händler und Mutant aus `level.spawn` steht als animierte Figur
dort, wo das Spiel ihn hinstellt, in dem Modell, das er im Spiel trägt (OGF mit Skelett, die
Animationen aus dem Modell oder den `.omf`-Bibliotheken). Für Menschen setzt der Konverter
Gehen und Rennen wie X-Ray aus Beinen und Oberkörper zusammen. Jede Figur trägt den
Animationsgraphen `xr_human` oder `xr_mutant`, den das Skript über die Zahl `mode` steuert.

**Was die Leute tun.** Das sagt die Custom Data des Spawns (`[logic]`, auch über `cfg =`
in eine Datei des Spiels), sonst das Smart Terrain, zu dem jemand gehört: dessen
Patrouillenwege aus `level.game` werden verteilt wie im Spiel nach ihren Namen
(`..._kamp`, `..._walk`, `..._sleep`, `..._guard`). So läuft einer seine Route ab und bleibt
an Punkten mit `a=guard` oder `t=...` stehen, andere sitzen oder knien ums Lagerfeuer und
blicken hinein (einer spielt Gitarre oder Mundharmonika, mit den Liedern aus dem Spiel),
stehen Wache, schlafen oder gehen um ihren Platz herum. Kommt der Spieler nah, bleiben sie
stehen, drehen sich zu ihm, grüßen und sagen etwas.

**Mutanten** streifen um ihr Revier, fressen, legen sich hin. Kommt der Spieler in ihre
Nähe, jagen sie ihn und beißen (Hunde schwächer, Wildschweine und Blutsauger stärker), bis
er tot oder weit genug weg ist. **Krähen** kreisen über ihrem Platz.

**Anomalien** sitzen, wo das Spiel sie hat, mit Partikeln je nach Art (Funken der Elektra,
Staub der Gravitationsanomalien, Flammen, grüner Flaum) und ihrem Summen und Knistern aus
dem Spiel. Wer hineingerät, löst eine Entladung aus: Knall, Partikelstoß, Schaden nach der
Stärke aus der Config. **Strahlung** füllt die Strahlungsanzeige und zehrt an der
Gesundheit, der Geigerzähler tickt schon kurz davor. **Minenfelder** explodieren.
Lagerfeuer brennen, flackern und verbrennen, wer hineintritt.

**Levelübergänge**: Wer in einem steht, liest „E - weiter nach Garbage“. `E` lädt die Szene
des anderen Levels und stellt den Spieler an den Punkt, an den der Übergang im Spiel führt,
mit seiner Gesundheit. Das geht über `game.saveData("stalker_travel")`.

**Sounds**: die Klangquellen des Levels (`level.snd_static`), die Umgebungsgeräusche, die
das Spiel zufällig um den Spieler herum abspielt (Hunde, Wind, Schüsse in der Ferne,
unterirdisch Atmen und Metall), die Musik der Tageszeit mit Pausen dazwischen, die Stimmen
der Leute und Mutanten.

**Spieler und Steuerung**: Gehen wie in fitzel. Linke Maustaste schießt mit der Pistole
(acht Schuss, R lädt nach), getroffen wird die Haut der Figur. Mutanten brauchen je nach Art
ein bis sechzehn Treffer, Menschen drei. Unten links stehen Gesundheit und Strahlung, unten
rechts die Munition. Wer stirbt, steht nach ein paar Sekunden dort wieder auf, wo er das
Level betreten hat.

Fern vom Spieler (80 m, Krähen 160 m) sind Figuren ausgeblendet und stehen still. In Escape
zeichnet fitzel mit Figuren und Logik ein Bild in 15 bis 16 ms auf einer RTX 4090.

## Wie die Shader übersetzt werden

| X-Ray-Shader | in fitzel |
|---|---|
| `default`, `def_vertex`, `def_objects_lod`, `flora\trunk_wave`, `models\model` | deckend |
| `def_aref`, `def_trans`, `flora\leaf_wave`, `models\transparent` | ausgeschnitten, beidseitig |
| `effects\wallmarkblend` (Schmutz, Risse, Schilder) | Decal, geblendet, ohne Schatten |
| `models\window` | Glas, geblendet, ohne Schatten |
| `effects\water*` | Wasser, geblendet, glänzend |
| `selflight` | leuchtend (die Textur ist die Emissionskarte) |
| `levels\*` (das Terrain) | gebacken, siehe unten |
| `effects\glow`, `effects\lightplanes`, `details\lod` | weggelassen (Effekte, keine Geometrie) |

Blenden kann fitzel aus glTF nicht übernehmen. Deshalb setzt die Szene diese Materialien in
ihren `modelMaterialOverrides` auf Blend.

Das Terrain zeichnet das Spiel aus einer Farbkarte über das ganze Level (in Escape 2048 Texel auf
1,4 km) und vier Detailtexturen, die eine Maske mischt. Die Detailtexturen stehen in
`shaders.xr`, die Kachelung in `textures.ltx`. fitzels Materialien haben eine Farbkarte, also wird
die Rechnung des Spiels (Farbe × gemischte Details × 2) pro Zelle in eine eigene Textur gebacken,
von oben aufgelegt.

## Der Höhenversatz

Eine Szene ohne fitzel-Terrain spielt trotzdem auf einem Boden: einem unsichtbaren flachen
Kollider bei y = 0. Die Level des Spiels reichen weit darunter (Escape fast 100 m), der Spieler
stünde sonst auf dieser Ebene statt im Bunker. Deshalb hebt der Konverter das ganze Level, bis
nichts mehr unter y = 0 liegt, und sagt, um wie viel: „lifted 98 m clear of fitzel's ground
(game y = scene y − 98)“. Mit `--no-lift` bleibt es bei den Höhen des Spiels.

## Texturen remastern

Die Texturen des Spiels sind meist 512 Texel groß. `tools/remaster-textures.py` rechnet die
Texturen eines Levels mit einem Upscale-Modell in ComfyUI hoch, Standard ist 4x-UltraSharpV2
(`4x-UltraSharpV2.safetensors` von huggingface.co/Kim2091/UltraSharpV2, Lizenz CC BY-NC-SA 4.0,
nach `ComfyUI/models/upscale_models/`). Außer Python mit numpy und Pillow braucht es nichts.

```bash
python3 tools/remaster-textures.py ~/Documents/Fitzel/stalker/models/l04_darkvalley /tmp/hd
```

Die Ergebnisse tragen dieselben Namen. Über die des Levels kopiert, bleiben `.meta` und GLB
gültig. Was das Skript dabei beachtet:

- **Größe:** doppelt so groß, Texturen bis 256 Texel viermal, höchstens 2048 (`--scale`,
  `--small`, `--max`).
- **Kacheln:** Pro Achse wird gemessen, ob die Textur kachelt. Auf diesen Achsen wird sie vor
  dem Hochrechnen ringsum fortgesetzt, das Modell sieht also über die Naht, und das Ergebnis
  kachelt weiter.
- **Korn:** Upscale-Modelle halten feines Korn (Beton, Rost, Erde) für Rauschen und glätten
  es weg, die Flächen wirken dann wächsern. `--grain` (Standard 0,9) holt das Korn des Originals
  zurück, in dem Band, das die Quelle selbst zeigen kann. Alles Feinere kommt vom Modell.
- **Alpha:** Harte Ausschnitte (Laub, Zäune, DXT1) werden glatt hochgerechnet und steil um 0,5
  geschnitten, weil das Modell um kleine Löcher Ringe zieht. Weiches Alpha (Blut, Schmutz) geht
  durchs Modell. Unter durchsichtigen Texeln füllt das Skript die Farbe aus den sichtbaren
  Nachbarn auf, ebenso die fast schwarzen Säume, die DXT1 neben Löchern hinterlässt. Sonst
  mischen Filter und Mipmaps Schwarz in die Ränder.

Die Terrain-Kacheln lässt das Skript aus, weil sie gebacken sind. Mehr Schärfe bekommen sie
beim Import mit `--terrain 16`. Ein neuer Import schreibt aber alle Texturen wieder aus dem
Spiel, also erst importieren, dann remastern. GLB und die übrigen Texturen kommen bei einem
neuen Import bitgleich heraus, man kann die neuen `terrain_*.jpg` also auch einzeln
herüberkopieren.

fitzel lädt Texturen unkomprimiert. Dark Valley braucht remastert etwa viermal so viel
Grafikspeicher wie vorher.

Bildmodelle taugen dafür nicht, das wurde ausprobiert: Z-Image Turbo erfindet beim Übermalen
Muster und Dinge (Blechnähte im Rost, Riffelblech statt Nummernschild), FLUX.2 Klein als
Restaurier-Edit gibt das Bild praktisch unverändert zurück.

## Was (noch) nicht mitkommt

- **Das gebackene Licht.** Lightmaps und die Himmelssicht pro Vertex bleiben im Spiel, fitzel
  beleuchtet selbst. Draußen sieht das gut aus, Innenräume sind aber zu hell. Das Light-Grid
  hilft bei großen Außenleveln wenig, weil es auf 128 Zellen pro Achse begrenzt ist. Die
  unterirdischen Level (ohne Terrain) bekommen eine Nachtszene ohne Sonne, ihr Licht kommt von
  den Lampen des Levels.
- **Normal Maps.** Die `_bump`-Texturen des Spiels sind ein eigenes Format und werden noch nicht
  umgerechnet.
- **Gras und Bodendetails** aus `level.details`.
- **Die Kollisionsgeometrie** aus `level.cform`. Kollidiert wird mit der sichtbaren Geometrie,
  die unsichtbaren Wände am Kartenrand gibt es also nicht.
- **Die Skripte des Spiels.** Quests, Dialoge, Handel, die A-Life-Simulation und die Kämpfe
  der Leute sind gegen die X-Ray-Engine geschrieben und laufen nicht. Was die Daten des
  Levels über Leute und Orte sagen, spielen die Skripte in `scripts/stalker/`. Die Leute
  tragen keine Waffen und schießen nicht zurück, Gegenstände liegen nicht herum.
- **Wegfindung.** Die Leute gehen ihre Wege in geraden Linien von Punkt zu Punkt. Ein
  Mutant, der den Spieler jagt, läuft gerade auf ihn zu und kann an einem Zaun hängen
  bleiben.
- **Poltergeister und der geskriptete Hubschrauber** bleiben weg.
