# Fitzel — Lua-Scripting-Referenz

Diese Doku beschreibt **exakt das, was aktuell im Code gebunden ist**
(`sandbox/src/ScriptSystem.cpp`, `ScriptHost.hpp`). Nichts hier ist geplant oder
erfunden — alles ist heute lauffähig.

Scripting läuft im **Play-Modus**. Jede Entity kann ein Lua-Skript tragen
(Inspector → *Script*, oder das Feld `script` beim `game.spawn`). Beim Start von
Play wird eine **frische Lua-VM** erzeugt; alle Skripte laden neu und `start()`
läuft erneut.

---

## 1. Skript-Aufbau

Ein Skript darf zwei Funktionen definieren — beide sind optional:

```lua
function start(e)          -- einmal, beim ersten Update nach „Play"
end

function update(e, dt, t)  -- jeden Frame, solange gespielt wird
end
```

- `e` — die **Entity-Tabelle** dieses Skripts (siehe §2)
- `dt` — Sekunden seit dem letzten Frame (Delta-Zeit)
- `t` — Sekunden seit Play-Start (Uhr)

Ein Objekt darf **mehrere Script-Komponenten** tragen — eine für das, was es tut,
eine für das, wie es aussieht. Sie laufen in der Reihenfolge, in der die
Komponenten angelegt wurden; schreiben zwei auf dasselbe Feld, gewinnt die letzte.

Jedes Skript läuft in seiner **eigenen Umgebung**: `local`- und globale Variablen
sind pro (Objekt, Datei) isoliert. Zwei Objekte mit demselben Skript teilen also
**keinen** Zustand — und zwei Skripte auf einem Objekt ebensowenig. Gemeinsamer
Zustand läuft über den Host (`game.addScore` / `game.getScore` / `game.setHud`).

Ein Laufzeitfehler wird **einmal** gemeldet (in die Konsole + Editor-UI) und
**deaktiviert nur dieses eine Skript** bis zum nächsten Play-Start.

Die **komplette Lua-Standardbibliothek** ist verfügbar (`math`, `string`, `table`,
`os`, `io`, …), da `luaL_openlibs` geladen wird.

---

## 2. Die Entity-Tabelle `e`

`e` ist eine schlichte Tabelle. Der Transform ist **lokal** (relativ zum Parent im
Szenegraph); für ein Wurzelobjekt ist lokal == Welt.

| Feld | Bedeutung | Rückschreibbar? |
|------|-----------|-----------------|
| `e.x`, `e.y`, `e.z` | Position (lokal) | **ja** — Schreiben bewegt das Objekt |
| `e.rx`, `e.ry`, `e.rz` | Rotation in **Grad** (Euler) | **ja** |
| `e.sx`, `e.sy`, `e.sz` | Halb-Ausdehnung (half extents) | **ja** |
| `e.name` | Name des Objekts | nein (nur lesen) |
| `e.id` | numerische Entity-ID | nein (nur lesen) |
| `e.type` | Entity-Typ (`game.BOX` …) | nein (nur lesen) |
| `e.parent` | ID des Eltern-Objekts (`-1` = Wurzel) | nein (nur lesen) |
| `e.active` | eigener Aktiv-Schalter | nein (nur lesen — setzen via `game.setActive`) |

**Wichtig:** Nach `update`/`start` werden **nur die numerischen Transform-Felder**
(`x/y/z`, `rx/ry/rz`, `sx/sy/sz`) zurück ins Objekt kopiert. `name`/`id` sind
faktisch read-only. Das direkte Setzen von `e.x = …` ist der einfachste Weg, ein
Objekt zu bewegen (Kinematik); für physikalische Bewegung siehe `game.setVelocity`
/ `game.applyImpulse`.

```lua
function update(e, dt, t)
    e.ry = e.ry + 45.0 * dt   -- 45°/Sekunde drehen
    e.y  = e.y + math.sin(t) * dt
end
```

---

## 3. Das globale `game`-Objekt

Alle Engine-Funktionen hängen an der globalen Tabelle `game`.

### 3.1 Eingabe

| Aufruf | Rückgabe | Beschreibung |
|--------|----------|--------------|
| `game.keyDown(key)` | bool | Taste ist **gerade gedrückt** (jeder Frame) |
| `game.keyPressed(key)` | bool | Taste **in diesem Frame** heruntergegangen (Flanke) |
| `game.mouseDown(button)` | bool | Maustaste gerade gedrückt (`button` default 0) |
| `game.mousePressed(button)` | bool | Maustaste in diesem Frame gedrückt (Flanke) |
| `game.mousePos()` | `x, y, über` | Zeiger in HUD-Leinwand-Einheiten (1080 hoch, Ursprung oben links, wie `game.hud*`); `über` = der Zeiger ist über der Ansicht |
| `game.mouseRay()` | `ox, oy, oz, dx, dy, dz` oder `nil` | Welt-Strahl von der Kamera durch den Zeiger — zum Anklicken von Dingen in der Szene |
| `game.mouseWheel()` | Zahl | Wie weit das Mausrad in diesem Frame gedreht wurde (Rasten, + = vom Körper weg); 0 außerhalb von Play |
| `game.showCursor(an)` | – | Mauszeiger im Play freigeben (`true`) oder wieder dem Läufer überlassen (`false`); gilt bis Play endet |
| `game.captureInput()` | bool | Tasten und Maustasten für dieses Skript festhalten — **jeden Frame** aufrufen, solange ein Menü offen ist; `true` = dieses Skript hält sie |
| `game.textInput()` | Text | Was seit dem letzten Aufruf getippt wurde (UTF-8, mit Umlauten; ohne Backspace/Enter — die sind Tasten). Für ein eigenes Eingabefeld, zusammen mit `game.captureInput()` |

`key` ist ein GLFW-Keycode → benutze die `game.KEY_*`-Konstanten (§3.7).
`button`: `game.MOUSE_LEFT` (0), `MOUSE_RIGHT` (1), `MOUSE_MIDDLE` (2).

**Ein Menü, das die Tasten für sich will** (Inventar, Karte): Solange ein Skript
in jedem Frame `game.captureInput()` aufruft, sehen **alle anderen** Skripte
keine Tasten und keine Maustasten — die Figur läuft nicht los, während man im
Inventar blättert. Esc beendet dann nicht Play, sondern kommt beim Skript an
(`game.keyDown(game.KEY_ESCAPE)`), und eine Orbit-Kamera hält still. Losgelassen
wird, indem das Skript nicht mehr fragt: ein Frame ohne Aufruf genügt. Ein
Skript, das mit einem Fehler ausfällt, kann das Spiel also nie taub hinterlassen.
Wer zuerst fragt, hält die Tasten, bis er aufhört.

### 3.2 Kamera (Play-Modus)

| Aufruf | Rückgabe | Beschreibung |
|--------|----------|--------------|
| `game.cameraPos()` | `x, y, z` | Position der Spielerkamera |
| `game.cameraDir()` | `x, y, z` | normalisierte Blickrichtung |

```lua
local px, py, pz = game.cameraPos()
local dx, dy, dz = game.cameraDir()
```

| Aufruf | Rückgabe | Beschreibung |
|--------|----------|--------------|
| `game.orbitFrame(gewicht, abstand, seite, höhe, fov)` | – | Die Orbit-Kamera **für diesen Frame** anders rahmen — Zielen über die Schulter: `gewicht` 0..1 blendet vom eingestellten Bild dahin, `abstand` m vom Drehpunkt, der Drehpunkt `seite` m nach rechts und `höhe` m nach oben, Sichtfeld `fov` Grad (0 = lassen). Die Maus dreht dabei um so viel langsamer, wie das Bild enger wird. Jeden Frame aufrufen; wer aufhört, gibt das Bild zurück |

### 3.3 Entities erzeugen & entfernen

| Aufruf | Rückgabe | Beschreibung |
|--------|----------|--------------|
| `game.spawn{ … }` | `id` (int) | Neues Objekt erzeugen (Parameter-Tabelle, §4) |
| `game.spawnPrefab(name, x, y, z [, yaw])` | `id` (int) | Prefab-Instanz erzeugen; gibt die **Wurzel-ID** zurück |
| `game.destroy(id)` | – | Objekt entfernen |
| `game.getPos(id)` | `x, y, z` oder `nil` | Weltposition; `nil` bei unbekannter ID |
| `game.setPos(id, x, y, z)` | – | Objekt an Position setzen |
| `game.clone(id [, name])` | `id` (int) | Kopie eines Objekts **samt Kindern und Komponenten** unter demselben Parent, an derselben Stelle; deferred wie `spawn`, `0` bei unbekannter ID |
| `game.setLocal(id, x, y, z [, rx, ry, rz])` | – | **Lokalen** Transform setzen (relativ zum Parent; bei einem Wurzelobjekt = Welt) — der Turm auf dem Rumpf, das Bein an der Hüfte |
| `game.setLocals(liste [, schritt])` | – | Viele lokale Transforms auf einmal: flache Liste `id, x, y, z, rx, ry, rz, id, …` (`schritt` 7, Standard) oder `id, x, y, z, …` (`schritt` 4, nur Position). Ein Aufruf pro Frame für eine ganze Armee statt einer pro Teil |

Ein mit `game.spawn{ type = game.LIGHT, r = …, g = …, b = … }` erzeugtes Objekt ist ein
echtes Punktlicht (Farbe aus `r/g/b`); Helligkeit und Reichweite stellt `game.setLight` ein.

**Deferral:** `game.spawn` gibt die neue ID **sofort** zurück, das Objekt erscheint
aber erst am **Ende des Frames** (die Tick-Schleife iteriert gerade die
Entity-Liste). `game.spawnPrefab` und `game.destroy` sind ebenfalls deferred.

**Prefabs:** `game.spawnPrefab` instanziiert ein im Editor gespeichertes Prefab
(`<Projekt>/prefabs/*.fprefab`) per **Name** (Groß-/Kleinschreibung egal) an der
Weltposition `x, y, z`, optional um `yaw` Grad um die Hochachse gedreht. Der ganze
Subtree (Wurzel + Kinder samt Components) wird erzeugt; jede Instanz-Entity trägt
eine `PrefabComponent`. Rückgabe ist die **ID der Instanz-Wurzel** (0, wenn kein
Prefab dieses Namens existiert oder kein Projekt offen ist). Das Prefab wird beim
ersten Aufruf von der Platte geladen (Modelle importiert) und danach **im Speicher
gecacht** — wiederholtes Spawnen ist billig. Funktioniert im Editor-Play **und** im
exportierten Spiel (der `prefabs/`-Ordner wird beim Export mitkopiert).

```lua
-- Fass-Stapel jede Sekunde vor dem Spieler fallen lassen
local t = 0
function update(dt)
    t = t + dt
    if t >= 1.0 then
        t = 0
        local x, y, z = game.cameraPos()
        game.spawnPrefab("Barrel Stack", x, game.terrainHeight(x, z), z, 90)
    end
end
```

### 3.4 Physik (auf dynamischen Bodies, per ID)

| Aufruf | Rückgabe | Beschreibung |
|--------|----------|--------------|
| `game.setVelocity(id, vx, vy, vz)` | – | Lineare Geschwindigkeit setzen |
| `game.getVelocity(id)` | `vx, vy, vz` oder `nil` | Aktuelle lineare Geschwindigkeit |
| `game.applyImpulse(id, jx, jy, jz [, px, py, pz])` | – | Impuls anwenden; mit `px, py, pz` dort, wo es getroffen hat (Welt). Ein Objekt mit **Swing**-Komponente (oder darunter Hängendes) schaukelt dann, statt geschoben zu werden – getroffen am Rand verdreht es sich auch |
| `game.setAngularVelocity(id, wx, wy, wz)` | – | Drehgeschwindigkeit (rad/s) setzen |

No-op bei unbekannten IDs oder Objekten ohne dynamischen Physik-Body.

#### 3.4.1 Figuren durch die Welt laufen lassen

| Aufruf | Rückgabe | Beschreibung |
|--------|----------|--------------|
| `game.moveCharacter(id, vx, vz [, dt])` | `x, y, z, amBoden, aufGelände` oder `nil` | Figur `id` als Kapsel durch die Physik-Welt laufen lassen: waagrecht mit `vx, vz` m/s, für `dt` Sekunden (Standard 1/60). Gibt zurück, wo ihre Füße jetzt stehen (Welt), ob sie auf etwas steht und ob das das Gelände ist |
| `game.removeCharacter(id)` | – | Die Kapsel der Figur wieder entfernen (etwa beim Einsteigen ins Auto) |
| `game.spawnVehicle(id)` | `true`/`false` | Das Fahrzeug (Objekt mit Vehicle-Komponente) in die Physik-Welt setzen, **ohne** es zu fahren: es steht geparkt mit angezogener Handbremse, Figuren laufen nicht mehr hindurch. Es gibt ein Physik-Auto pro Welt — ein zweites wird abgelehnt (`false`) |
| `game.driveVehicle(id)` | `true`/`false` | Die Steuerung des Spielers geht an dieses Auto (wie V mit dem nächsten): W/S, A/D, Leertaste, C für die Kameras daran. Setzt es bei Bedarf erst in die Welt |
| `game.leaveVehicle()` | – | Die Steuerung zurück; das Auto bremst, wo es ist, bis zum Stand |
| `game.drivenVehicle()` | `id, tempo, lenkung, gas` oder `nil` | Das gefahrene Auto: Tempo in m/s (+ vorwärts), Lenkung −1 links … 1 rechts, Gas −1 … 1 — für ein Lenkrad und Pedale, die sich bewegen. `nil`, wenn keins gefahren wird |
| `game.restart()` | – | Play noch einmal von vorn, so wie die Szene stand, als Play begann (wie der Restart-Knopf eines Menüs) — ohne die Datei neu zu lesen, ungespeicherte Änderungen im Editor bleiben |
| `game.quit()` | – | Das Spiel verlassen (der Beenden-Knopf eines Menüs): der Player schließt sich, der Editor verlässt Play. Wirkt am Ende des Frames |
| `game.castRay(ox, oy, oz, dx, dy, dz [, maxDist])` | `x, y, z, nx, ny, nz, id, dist` oder `nil` | Ein Strahl durch die Physik-Welt **und** das Gelände, wie es gezeichnet ist: das Erste, was er trifft (Standard bis 200 m). `id` ist das getroffene Objekt, `-1` die Welt selbst (Gelände, Straße, Brücke). Für Schüsse und Sichtlinien — genauer als `game.raycast`, der nur Auswahl-Kästen kennt |
| `game.groundHeight(x, y, z [, maxDist])` | `y` oder `nil` | Worauf etwas fiele, das man bei `x, y, z` loslässt: senkrecht nach unten (bis `maxDist`, Standard 50 m) die erste Straße, Brücke, der erste Boden — oder das Gelände, so wie es **gezeichnet** ist (nie darunter). Ausserhalb von Play nur das Gelände |

Beim ersten Aufruf bekommt das Objekt eine Kapsel (so hoch wie das Objekt, 0,3 m
Radius), die auf dem steht, was unter dem Objekt liegt – eine Figur auf einer Brücke
also auf der Brücke, nicht auf dem Boden darunter. Sie steht auf Gelände, Straßen,
Brücken, Mauern, Gebäuden und allem mit Physik-Komponente, bleibt an Wänden stehen,
steigt Stufen und Bordsteine bis 0,4 m hoch, hält sich beim Bergabgehen an den Boden und
fällt, wenn er aufhört. Bewegt wird das Objekt nicht: das Skript setzt es selbst an die
zurückgegebene Stelle. Figuren blockieren weder einander noch den Läufer. Nur im Play,
sonst `nil`.

**Füße aufs Gelände:** Die Physik tastet das Gelände nur alle 4 m ab, gezeichnet ist es
viel feiner. Steht die Figur auf Gelände (`aufGelände`), gehören die Füße deshalb auf
`game.terrainHeight(x, z)`, auf allem anderen (Straße, Brücke, Dach) auf `y`:

```lua
local x, y, z, ground, terrain = game.moveCharacter(self.id, vx, vz, dt)
if x then
    if terrain then y = game.terrainHeight(x, z) end
    self.x, self.y, self.z = x, y, z      -- Wurzelobjekt: lokal = Welt
end
```

### 3.5 Audio

| Aufruf | Beschreibung |
|--------|--------------|
| `game.playSound(name)` | One-shot-Sound aus dem `sounds/`-Ordner abspielen (z. B. `"shot.wav"`) |
| `game.sound(name [, laut [, tonhöhe [, x, y, z [, nah [, fern]]]]])` | One-shot mit Lautstärke und Tonhöhe; mit `x, y, z` **im Raum**: voll laut bis `nah` m (Standard 15), still ab `fern` m (Standard 400), von der Kamera aus gehört. Pro Datei spielen bis zu 6 Stimmen zugleich, die älteste wird abgeschnitten |
| `game.playAudio(id)` | AudioSource-Komponente eines Objekts starten |
| `game.stopAudio(id)` | AudioSource-Komponente eines Objekts stoppen |

#### 3.5.1 Synth: Noten, Regler, MIDI-Songs

Ein Objekt mit einer **Synth**-Komponente spielt einen Patch aus dem Synth-Panel
(`content/patches/<name>.json`). Es kann einen ganzen Song aus einer MIDI-Datei
spielen (`content/midi/<name>.mid`), Noten, die ein Skript schickt, oder – bei
einem Patch ohne `gate` – einfach durchklingen, während das Spiel an seinen
Reglern dreht (Motor, Wind, Alarm). Alle Aufrufe nehmen die **ID des Objekts**
wie `game.playAudio`. Der Synth wird beim ersten Aufruf gebaut; „Play on start“
in der Komponente ist dafür nicht nötig.

Eine Note ist eine Zahl (`60` = eingestrichenes C) oder ein Name: `"C4"`, `"F#3"`,
`"Bb2"`, `"A4"` (= 69). Die Lautstärke (velocity) geht von 0 bis 1. Werte über 1
werden als MIDI-Wert 0..127 gelesen.

| Aufruf | Rückgabe | Beschreibung |
|--------|----------|--------------|
| `synth.play(id)` | bool | Synth starten, dazu den Song der Komponente, falls einer eingetragen ist |
| `synth.stop(id)` | – | alles verstummt: Song stoppt, Noten werden losgelassen |
| `synth.noteOn(id, note [, vel])` | bool | Note anschlagen (`vel` Standard 0.8) |
| `synth.noteOff(id [, note])` | – | Note loslassen; ohne `note` alle |
| `synth.set(id, regler, wert)` | bool | einen Regler (Dial) des Patches setzen, z. B. `"rpm"` |
| `synth.playMidi(id [, datei [, loop]])` | bool | Song von vorn spielen; ohne `datei` den der Komponente, `loop` überschreibt „Loop song“ |
| `synth.stopMidi(id)` | – | Song anhalten (der Synth bleibt an) |
| `synth.isPlaying(id)` | bool | läuft der Song noch? |
| `synth.setTempo(id, faktor)` | bool | 1 = wie notiert, 2 = doppelt so schnell |
| `synth.note(name)` | int / nil | Notenname → MIDI-Nummer (`synth.note("A4")` → 69) |
| `synth.lastError()` | string | warum der letzte Aufruf `false` gab (Patch fehlt, Regler unbekannt …) |

Ein Patch für Noten braucht die Regler `pitch` (MIDI-Note) und `gate` (Taste
gedrückt), optional `velocity`. Jede gleichzeitig klingende Note bekommt eine
eigene Stimme. Wie viele es höchstens gibt, stellt „Voices“ in der Komponente
ein. Alle anderen Regler gelten für alle Stimmen zugleich. Kanal 10 eines
MIDI-Songs ist fast immer Schlagzeug und wird übersprungen, außer „Play drum
channel“ ist an.

```lua
-- Eine kleine Fanfare, wenn der Spieler das Ziel erreicht
local fanfare = { "C4", "E4", "G4", "C5" }
local step, clock = 0, 0

function update(e, dt, t)
    if game.keyPressed(game.KEY_F) then step, clock = 1, 0 end
    if step > 0 then
        clock = clock + dt
        if clock > 0.15 then
            synth.noteOff(e.id)
            if step <= #fanfare then synth.noteOn(e.id, fanfare[step], 0.9) end
            step, clock = step + 1, 0
            if step > #fanfare + 1 then step = 0 end
        end
    end
end
```

```lua
-- Musik: im Kampf schneller, am Ende eines Levels ein anderer Song
function update(e, dt, t)
    synth.setTempo(e.id, inBattle and 1.25 or 1.0)
    if levelDone and not switched then
        synth.playMidi(e.id, "finale.mid", false)
        switched = true
    end
end
```

#### 3.5.2 Musik: ein Song mit Uhr, Filter und Analyse

Die globale Tabelle `music` spielt **den einen Song** des Spiels – gedacht für
Rhythmusspiele und alles, was wissen muss, *wo* die Musik gerade ist. Anders als
eine AudioSource kann er bei jeder Sekunde starten, sagt seine Position (so, wie
sie aus den Lautsprechern kommt) und lässt sich während des Spielens filtern.
Namen werden aufgelöst wie bei `game.playSound` (Asset-Datenbank, dann
`content/sounds`); ein Song im `music/`-Ordner des Projekts wird am Dateinamen
gefunden. Beim Ende von Play verstummt er.

| Aufruf | Rückgabe | Beschreibung |
|--------|----------|--------------|
| `music.load(name)` | `true, sekunden` / `false, warum` | Song laden (ersetzt und stoppt den alten) |
| `music.play([ab])` | – | ab Sekunde `ab` spielen (Standard 0); **negativ** = so viele Sekunden Stille vorweg |
| `music.stop()` | – | anhalten |
| `music.pause()` / `music.resume()` | – | pausieren / weiterspielen |
| `music.fade([sek])` | – | über `sek` (Standard 1) ausblenden, dann stoppen |
| `music.time()` | Zahl | Position in Sekunden, wie gehört (Puffer abgezogen, geglättet, läuft nie rückwärts) |
| `music.duration()` | Zahl | Länge des Songs in Sekunden |
| `music.isPlaying()` / `music.isPaused()` | bool | Zustand |
| `music.setFilter(hz [, gain [, shelfDb [, glätten]]])` | – | Tiefpass bei `hz`, lineare Verstärkung `gain` (Standard 1), Bass-Shelf bei 120 Hz in dB (Standard 0); alles gleitet mit Zeitkonstante `glätten` (Standard 0.12 s, 0 = sofort) |
| `music.setVolume(v)` | – | Lautstärke des Skripts (unter dem Mixer-Pegel) |
| `music.sampleRate()` | int | Abtastrate des Songs |
| `music.spectrum()` | Array (512) | Spektrum 0..1 (FFT 1024, −100..−30 dB), **einmal pro Frame** aufrufen – die Glättung schreitet pro Aufruf fort |
| `music.waveform([n])` | Array | die letzten `n` (Standard 128, max. 1024) Samples, −1..1 |
| `music.analyze(name)` | Tabelle / `nil, warum` | Bänder-Analyse für die Beat-Erkennung (siehe unten); **blockiert**, während dekodiert wird |

`music.analyze` dekodiert den Song zu Mono mit 22 050 Hz und teilt ihn in Bass
(< 140 Hz), Mitten (um 1,8 kHz) und Höhen (> 7 kHz). Pro Stück von `hop` Samples
(256 ≈ 11,6 ms) gibt es je Band einen RMS-Wert und einen „Flux“ (wie stark die
Log-Energie gegenüber dem Stück davor stieg, normiert auf Mittelwert 1). Tempo,
Beats und Noten daraus zu machen, ist Sache des Spiels.

| Feld | Bedeutung |
|------|-----------|
| `duration`, `rate`, `hop`, `frames` | Länge (s), Abtastrate (22050), Samples pro Frame, Anzahl Frames |
| `lowFlux`, `midFlux`, `highFlux` | Arrays (`frames` lang): Energieanstieg je Band |
| `lowRms`, `midRms`, `highRms` | Arrays: Lautstärke je Band |

Frame `i` (1-basiert) liegt bei `(i - 1) * hop / rate` Sekunden.

```lua
-- Song laden, nach 2 s Vorlauf starten, bei Pause dumpf filtern
function start(e)
    local ok, len = music.load("track.ogg")
    if not ok then game.log("Musik: " .. tostring(len)) return end
    music.play(-2.0)
end

function update(e, dt, t)
    if game.keyPressed(game.KEY_P) then
        if music.isPaused() then music.resume(); music.setFilter(20000, 1, 0)
        else music.pause(); music.setFilter(400, 0.7, 3) end
    end
    game.setHud(string.format("%.2f / %.0f s", music.time(), music.duration()))
end
```

### 3.6 Punktestand & HUD (gemeinsamer Zustand)

| Aufruf | Rückgabe | Beschreibung |
|--------|----------|--------------|
| `game.addScore(n)` | – | Punktzahl erhöhen (`n` default 1) |
| `game.getScore()` | int | aktuelle Punktzahl |
| `game.setHud(text)` | – | HUD-Text im Play-Overlay setzen |

#### 3.6.1 Eigenes HUD zeichnen

Für ein richtiges Spiel-HUD (Balken, Symbole, Banner, Titelbildschirm) zeichnet
ein Skript selbst — mit einfachen 2D-Aufrufen auf eine **virtuelle Leinwand, die
immer 1080 Einheiten hoch** ist und so breit, wie das Seitenverhältnis der Ansicht
es ergibt (`game.hudSize()`). Ein einmal gebautes Layout sitzt so bei jeder
Fenstergrösse und auch im eingefügten Viewport des Editors richtig. Ursprung ist
oben links, y wächst nach unten. Farben sind vier Zahlen 0..1 (`a` optional, 1).

| Aufruf | Beschreibung |
|--------|--------------|
| `game.hudSize()` → `w, h` | Leinwandgrösse (`h` ist immer 1080) |
| `game.hudRect(x, y, w, h, r, g, b, a [, rund])` | gefülltes Rechteck, `rund` = Eckenradius |
| `game.hudGradient(x, y, w, h, r, g, b, a, r2, g2, b2, a2)` | Rechteck mit Verlauf oben → unten |
| `game.hudFrame(x, y, w, h, r, g, b, a [, dicke, rund])` | Rechteck-Umriss |
| `game.hudLine(x1, y1, x2, y2, r, g, b, a [, dicke])` | Linie |
| `game.hudCircle(x, y, radius, r, g, b, a [, dicke])` | Kreis — ohne `dicke` gefüllt |
| `game.hudTri(x1, y1, x2, y2, x3, y3, r, g, b, a)` | gefülltes Dreieck |
| `game.hudText(x, y, text, grösse, r, g, b, a [, align, bold])` | Text mit Schatten; `y` = Oberkante, `align` 0 links / 0.5 Mitte / 1 rechts |
| `game.hudTextSize(text, grösse [, bold])` → `w, h` | Textmasse in Leinwand-Einheiten |
| `game.hudImage(bild, x, y, w, h [, r, g, b, a [, u0, v0, u1, v1]])` | ein Bild (Texture-Asset, Dateiname oder GUID), getönt mit `r, g, b, a` (Standard weiss), auf den Ausschnitt `u0..u1, v0..v1` beschnitten (Standard ganz; `v` läuft nach unten) |
| `game.imageSize(bild)` → `w, h, u0, v0, u1, v1` | Bildgrösse in Pixeln (so wie das HUD es hält, lange Seite höchstens 1024) und der Kasten, den sein sichtbarer — nicht ganz durchsichtiger — Teil füllt; `nil` für ein unbekanntes Bild |
| `game.worldToHud(x, y, z)` → `hx, hy` | wo ein Weltpunkt auf der Leinwand liegt; `nil` hinter der Kamera |
| `game.setCrosshair(an)` | Fadenkreuz des Players ein/aus (ein Spiel mit eigenem HUD will es meist nicht) |

Gezeichnet wird, was das Skript **in diesem Frame** aufgerufen hat: die Liste wird
vor jedem Skript-Tick geleert — also jeden Frame neu zeichnen, wie bei ImGui.

```lua
function update(e, dt, t)
    local w, h = game.hudSize()
    game.hudRect(20, 20, 300, 18, 0.1, 0.1, 0.1, 0.7, 4)          -- Balken-Hintergrund
    game.hudRect(22, 22, 296 * leben, 14, 1.0, 0.3, 0.2, 1.0, 3)  -- Lebensbalken
    game.hudText(w * 0.5, 40, "SCORE " .. punkte, 36, 1, 1, 1, 1, 0.5, true)
end
```

Um etwas an einer Stelle der Welt zu beschriften (Punkte über einem Gegner, „E
Aufheben“ über einer Pistole), gibt `game.worldToHud` den Punkt auf der Leinwand.

Ein **Icon** ist meist mit Rand gerendert (eine Pistole mitten in einem
1920×1080-Bild). `game.imageSize` liefert den Kasten, den das Sichtbare füllt;
mit ihm als Ausschnitt füllt die Pistole ihr Feld:

```lua
local w, h, u0, v0, u1, v1 = game.imageSize("pistol.png")
local aspect = (u1 - u0) * w / ((v1 - v0) * h)      -- Seitenverhältnis des Sichtbaren
game.hudImage("pistol.png", x, y, 120, 120 / aspect, 1, 1, 1, 1, u0, v0, u1, v1)
```

Bilder werden beim ersten Aufruf geladen und für die Sitzung gehalten.

Score/HUD liegen im **Host** (nicht in der isolierten Skript-Umgebung), sind also
über alle Skripte hinweg geteilt.

#### 3.6.2 Skripte, die zusammenarbeiten: `shared`

Jedes Skript hat seine eigene Umgebung — zwei Skripte sehen ihre Variablen nicht.
Die eine Tür dazwischen ist die globale Tabelle **`shared`**: eine gewöhnliche
Tabelle, die alle Skripte sehen und die bei jedem Play-Start leer ist. Was
hindurchgeht, entscheiden die Skripte. Ein Steuerungs-Skript kann dort zum
Beispiel Funktionen für andere Skripte ablegen:

```lua
-- tp_controller.lua (auf der Figur)
function start(self)
    shared.figures = shared.figures or {}
    shared.figures[self.id] = { pickup = function(x, y, z) ... end }
end

-- inventory.lua (zweites Skript auf derselben Figur)
local fig = shared.figures and shared.figures[self.id]
if fig then fig.pickup(x, y, z) end
```

Felder **in** `shared` setzen (`shared.x = 1`), nie `shared` selbst neu zuweisen —
`shared = {}` legte nur eine eigene Variable dieses Skripts an.

### 3.7 Objekte finden, abfragen, umbauen

| Aufruf | Rückgabe | Beschreibung |
|--------|----------|--------------|
| `game.find(name)` | `id` oder `nil` | Erstes Objekt mit diesem Namen (exakt, sonst case-insensitiv) |
| `game.findAll(name)` | Array von IDs | Alle Objekte mit diesem Namen |
| `game.entities()` | Array von IDs | Alle Objekte der Szene |
| `game.entityInfo(id)` | Tabelle oder `nil` | Steckbrief (§3.7.1) |
| `game.getName(id)` / `game.setName(id, s)` | string / – | Anzeigename |
| `game.getRot(id)` | `rx, ry, rz` oder `nil` | **Welt**-Rotation in Grad |
| `game.setRot(id, rx, ry, rz)` | – | Welt-Rotation setzen (wird in den lokalen Transform zurückgerechnet) |
| `game.getScale(id)` | `sx, sy, sz` oder `nil` | Halb-Ausdehnung |
| `game.setScale(id, s)` / `(id, sx, sy, sz)` | – | Ein Wert = uniform |
| `game.setActive(id, bool)` | – | Objekt (samt Kindern) ein-/ausschalten |
| `game.isActive(id)` | bool oder `nil` | Effektiv sichtbar (Objekt **und** alle Eltern aktiv) |
| `game.setParent(id, parentId)` | – | Umhängen, **Welt-Transform bleibt stehen**; `-1` = an die Wurzel |
| `game.getParent(id)` | `id` oder `nil` | Eltern-ID |
| `game.children(id)` | Array von IDs | Direkte Kinder |

`setParent` verweigert Zyklen (ein Objekt kann nicht unter sein eigenes Kind).

#### 3.7.1 `game.entityInfo(id)`

| Feld | Typ | Bedeutung |
|------|-----|-----------|
| `id`, `parent` | int | IDs (`parent` = `-1` bei Wurzelobjekten) |
| `type` | int | Entity-Typ (`game.BOX` … `game.EMPTY`) |
| `name` | string | Anzeigename |
| `script` | string | Lua-Datei der Script-Komponente (`""` = keine) |
| `material` | string | GUID des zugewiesenen Materials (`""` = keins) |
| `model` | string | Quelldatei der Model-Komponente (`""` = keine) |
| `active` | bool | eigener Schalter |
| `activeInHierarchy` | bool | inklusive aller Eltern |
| `physics`, `dynamic` | bool | hat Physik-Komponente / ist dynamisch |

#### 3.7.2 Animation Graph (Zustandsautomat) steuern

Hat ein Objekt eine **Animation Graph**-Komponente, setzt das Skript deren
Parameter; die Übergänge im Graphen entscheiden, was daraus wird. Objekte ohne
Graph oder unbekannte Parameter werden stillschweigend übergangen.

Ein Zustand spielt einen Timeline-Clip, eine **Modell-Animation** (die Clips,
die ein geriggtes .glb mitbringt – Idle, Walk …) oder beides. Mit einer
Modell-Animation posiert der Graph das Skelett selbst; eine Animation-Komponente
braucht die Figur dafür nicht. Tempo 0 hält das erste Bild des Clips – so kommt
auch eine Figur, die nur einen Walk mitbringt, zu einem Idle. Ein Übergang kann
eine **Überblendung** (Fade, in Sekunden) haben: Der verlassene Zustand spielt
weiter und gibt seine Pose in dieser Zeit ab; 0 schneidet hart. Überblendet
werden nur Modell-Animationen, Timeline-Clips schalten sofort um. Das Tempo eines
Zustands darf negativ sein (der Clip läuft rückwärts) und kann mit einem
Zahl-Parameter multipliziert werden („Speed times“): `game.animNumber(id,
"walkSpeed", -0.5)` lässt den Walk halb so schnell rückwärts laufen, ohne
Sprung in der Pose.
`sandbox/scripts/walker.lua` ist ein vollständiges Beispiel: eine Figur mit WASD
steuern und dabei zwischen Idle und Walk umschalten. Mit `CAMERA_RELATIVE` und
einer Kamera als Kind der Figur (Modus *Follow parent*, **Orbit with mouse** an)
wird daraus eine Third-Person-Steuerung: Die Maus dreht die Kamera um die Figur,
die Tasten laufen relativ zur Blickrichtung (`game.cameraDir()`).

| Aufruf | Rückgabe | Beschreibung |
|--------|----------|--------------|
| `game.animTrigger(id, name)` | – | Trigger auslösen; bleibt gesetzt, bis ein Übergang ihn verbraucht |
| `game.animBool(id, name [, wert])` | – | Bool-Parameter setzen (`wert` Standard `true`) |
| `game.animNumber(id, name, wert)` | – | Zahl-Parameter setzen |
| `game.animState(id)` | string / `nil` | Name des aktuellen Zustands |

```lua
-- Tür: mit E öffnen, sobald sie zu ist
local door = game.find("Tuer")
function update(e, dt, t)
    if game.keyPressed(game.KEY_E) and game.animState(door) == "Closed" then
        game.animTrigger(door, "open")
    end
end
```

#### 3.7.3 Gegenstände tragen: Knochen einer Figur

Eine geriggte Figur hat Knochen (`CC_Base_R_Hand`, `mixamorig:Head` …). Ein
Objekt lässt sich an einen davon hängen und folgt ihm dann in jedem Frame — so,
wie die Figur gerade posiert ist, nach dem Skinning und vor dem Zeichnen. Das
Objekt bleibt ein eigenes Objekt (Wurzel, eigene Grösse); nur Ort und Drehung
kommen vom Knochen. Gilt bis `game.detach` oder bis Play endet.

| Aufruf | Rückgabe | Beschreibung |
|--------|----------|--------------|
| `game.bonePos(id, knochen)` | `x, y, z, rx, ry, rz` oder `nil` | Weltposition und -drehung eines Knochens, so wie die Figur zuletzt gezeichnet wurde |
| `game.bones(id)` | `{ namen }` | alle Knochen der Figur, in Skelett-Reihenfolge |
| `game.attach(objekt, figur, knochen [, x, y, z [, rx, ry, rz]])` | bool | `objekt` an den Knochen hängen. Mit Zahlen: so weit (m) und so gedreht (Grad) im Raum des Knochens; ohne: bleibt, wo es jetzt ist — relativ zum Knochen |
| `game.detach(objekt)` | – | wieder loslassen; das Objekt bleibt, wo es zuletzt war |
| `game.attach(…, rx, ry, rz, blend)` | bool | wie oben, aber in `blend` Sekunden vom jetzigen Sitz dorthin, statt zu springen (die Pistole dreht sich beim Anlegen in der Hand) |
| `game.toWorld(id, x, y, z)` | `wx, wy, wz` oder `nil` | ein Punkt im eigenen Raum des Objekts (Meter von seiner Mitte, mitgedreht, nicht skaliert) in der Welt — die Laufmündung einer Pistole in der Hand |
| `game.emit(id)` | – | den **Burst** der Particle-Komponente des Objekts dort auslösen, wo es gerade steht (Einschlag, Mündungsfeuer). Ein Effekt-Objekt reicht für viele Einschläge: hinsetzen, auslösen, weiter |
| `game.decal(x, y, z, nx, ny, nz [, größe [, material [, drehung]]])` | bool | Ein **Decal** dort, wo etwas getroffen hat: auf der Fläche bei `x, y, z` mit der Normale `nx, ny, nz` (wie `game.castRay` sie liefert), `größe` Meter breit (Standard 0,12), im Bibliotheks-Material `material` – ohne Namen das Einschussloch der Engine –, um `drehung` Grad gedreht. Geschnitten aus allem, was dort stillsteht (Objekte, Modelle, Gelände; lose Physik-Körper nicht); die letzten 256 bleiben, bis Play endet. `false`, wenn dort nichts ist |
| `game.shatter(id, x, y, z, dx, dy, dz [, stärke])` | bool | **Glas zerbricht** dort, wo ein Schuss es getroffen hat: Objekt `id` (wie `game.castRay` es meldet; `-1`, die Welt, sucht selbst das Glas an der Stelle – so kommt man an die Scheiben einer importierten Halle) am Punkt `x, y, z`, der Schuss unterwegs in Richtung `dx, dy, dz`, `stärke` 1 für eine Kugel. Glas ist ein Material mit „Glass“ oder eines, das durchsichtig ist (Deckkraft unter 1) und kein Cutout. Die getroffene Scheibe – nur sie, mit Rückseite und Kanten – reißt vom Einschuss aus in Scherben, kleine am Loch, größere am Rahmen; sie fliegen mit dem Schuss, kippeln, fallen und bleiben flach liegen, bis Play endet. Danach geht ein `game.castRay` durch das Loch. Ein ganzes Glas-Objekt (Plane, flache Box mit Physics) zerbricht als Ganzes. `false`, wenn dort kein Glas ist |
| `game.reach(id, seite, x, y, z [, gewicht])` | – | Eine **Hand** der Figur (`"left"`/`"right"`) — oder ein **Fuß** (`"leftFoot"`/`"rightFoot"`) — **für diesen Frame** an einen Punkt der Welt bringen (inverse Kinematik): Schulter und Ellbogen (Hüfte und Knie) beugen sich, Hand und Fuß behalten die Ausrichtung der Animation. `gewicht` 0..1 ist der Anteil des Wegs von dort, wo die Animation sie hat (zum Ein- und Ausblenden). Jeden Frame aufrufen; wer aufhört, gibt sie der Animation zurück. Türgriff, Lenkrad, die zweite Hand an der Pistole, die Füße im Fußraum eines Autos |

```lua
-- Die Pistole in die rechte Hand (CC-Rig: Y entlang der Finger, Z zum Daumen,
-- die Handfläche schaut nach -X; die Pistole liegt modelliert auf der Seite)
game.attach(pistole, self.id, "CC_Base_R_Hand", -0.031, 0.112, 0.063, -0.1, -57.5, -90.3)
-- Aufheben: im Moment des Greifens festhalten, wo sie liegt
game.attach(pistole, self.id, "CC_Base_L_Hand")
```

#### 3.7.4 Aufsammelbares: Collectibles und Inventar-Gegenstände

Die **Collectible**-Komponente ist auf zwei Arten ein Aufsammel-Ding. Ohne Haken
bei *Inventory item* wird es beim Hineinlaufen eingesammelt (Punkte, Klang,
weg). Mit Haken passiert beim Berühren nichts: Ein Skript (das der Figur) bietet
es im *Pickup radius* an, hebt es auf und legt es ins Inventar. Dazu trägt es,
was der Spieler darüber erfährt: Name, Icon (ein Texture-Asset), Kategorie,
eine Zeile Beschreibung, Anzahl.

| Aufruf | Rückgabe | Beschreibung |
|--------|----------|--------------|
| `game.collectibles()` | `{ id, ... }` | alle aktiven Objekte mit Collectible-Komponente |
| `game.collectible(id)` | Tabelle oder `nil` | `item` (Name, sonst der des Objekts), `icon`, `category` (`"misc"`, `"weapon"`, `"ammo"`, `"health"`, `"key"`, `"document"`), `description`, `count`, `inventory` (bool), `radius`, `points`, `sound` |

Ein aufgehobener Gegenstand wird am besten **deaktiviert** (`game.setActive(id,
false)`), nicht gelöscht: Beim Wegwerfen kommt dasselbe Objekt mit allen seinen
Komponenten zurück. `treetest/scripts/inventory.lua` ist ein vollständiges
Inventar (Hinweis über dem Gegenstand, Aufheben mit Animation, Fenster mit
Icons, Ausrüsten in die Hand, Wegwerfen).

### 3.8 Assets

Alles, was die Asset-Datenbank kennt (Texturen, Modelle, Sounds, Materialdateien) —
Engine-Assets **und** Projekt-Assets — ist per GUID adressierbar. Überall, wo ein
Asset erwartet wird, akzeptiert die API vier Schreibweisen: die 32-stellige **GUID**,
den **Dateinamen** (`"brick.png"`), den **relativen Pfad** (`"textures/brick.png"`)
oder den **Namensstamm** (`"brick"`). Exakte Dateinamen gewinnen vor Pfaden, Pfade
vor Stämmen.

| Aufruf | Rückgabe | Beschreibung |
|--------|----------|--------------|
| `game.assets([typ])` | Array von Tabellen | Alle Assets, optional gefiltert: `"Texture"`, `"Model"`, `"Sound"`, `"Material"` |
| `game.findAsset(name [,typ])` | GUID oder `nil` | Referenz auflösen |
| `game.assetInfo(ref)` | Tabelle oder `nil` | Einzelnes Asset |
| `game.assetPath(ref)` | string oder `nil` | Absoluter Pfad auf der Platte |
| `game.refreshAssets()` | – | Datenbank neu von der Platte einlesen (neue Dateien auftauchen lassen) |

Jede Asset-Tabelle hat: `id` (GUID), `name` (Dateiname), `path` (Pfad relativ zur
Quelle), `type` (`"Texture"`/`"Model"`/`"Sound"`/`"Material"`), `source`
(`"Engine"` oder `"Project"`).

```lua
for _, a in ipairs(game.assets("Texture")) do
    game.log(a.name, a.id, a.source)
end
```

### 3.9 Modelle

| Aufruf | Rückgabe | Beschreibung |
|--------|----------|--------------|
| `game.loadModel(ref)` | `modelId` oder `nil` | Model-Asset in die Model-Library importieren (bereits geladene werden wiederverwendet) |
| `game.modelInfo(modelId)` | Tabelle oder `nil` | `name`, `path`, `min`, `max`, `size` (je `{x,y,z}`), `meshes`, `animated` |

Zum **Platzieren** braucht es `loadModel` gar nicht — `game.spawn{model = …}` (§4)
importiert selbst und setzt die Größe aus der Bounding-Box des Modells.

```lua
local id = game.spawn{ model = "tree.glb", x = 10, y = game.terrainHeight(10, 4), z = 4,
                       scale = 1.5, physics = game.PHYSICS_STATIC }
```

### 3.10 Materialien

Materialien liegen in der Material-Bibliothek des Projekts und werden per GUID
referenziert; mehrere Objekte können sich eines teilen (eine Änderung wirkt dann
auf alle). Wo ein Material erwartet wird, geht auch sein **Name**.

| Aufruf | Rückgabe | Beschreibung |
|--------|----------|--------------|
| `game.materials()` | Array von Tabellen | Die ganze Bibliothek |
| `game.findMaterial(name)` | GUID oder `nil` | Material per Name suchen |
| `game.materialInfo(ref)` | Tabelle oder `nil` | Einzelnes Material |
| `game.createMaterial{…}` | GUID | Neues Material anlegen (Felder §3.10.1) |
| `game.setMaterialProps(ref, {…})` | bool | Vorhandenes Material ändern — **live**, alle Nutzer sehen es sofort |
| `game.setMaterial(entityId, ref)` | bool | Material einem Objekt zuweisen |
| `game.getMaterial(entityId)` | GUID oder `nil` | Zugewiesenes Material |
| `game.setColor(entityId, r, g, b)` | bool | Objekt einfärben: legt dem Objekt **sein eigenes** Material an (beim zweiten Aufruf wiederverwendet), färbt also nie andere mit ein |

#### 3.10.1 Material-Tabelle

Bei `createMaterial`/`setMaterialProps` werden **nur die angegebenen Felder**
geschrieben — man kann also einen einzelnen Wert ändern, ohne den Rest zu kennen.

| Feld | Typ | Bedeutung |
|------|-----|-----------|
| `name` | string | Anzeigename |
| `color` (oder `albedo`, oder `r`/`g`/`b`) | `{r,g,b}` oder Zahl | Grundfarbe (ohne Textur) |
| `reflectivity` | 0..1 | matt … Spiegel |
| `roughness` | 0..1 | Unschärfe der Spiegelung |
| `opacity` | 0..1 | Deckkraft |
| `glass` | bool | Fresnel-Alpha (klare Mitte, spiegelnder Rand) |
| `alphaMode` | int | `game.ALPHA_OPAQUE` / `ALPHA_CUTOUT` / `ALPHA_BLEND` |
| `cutoff` | 0..1 | Schwelle für `ALPHA_CUTOUT` |
| `emission` | `{r,g,b}` | Eigenleuchten |
| `emissionStrength` | Zahl | Skaliert das Leuchten (>1.5 blüht sichtbar) |
| `texture` | Asset-Ref | Basisfarben-Textur (`""` löscht den Slot) |
| `normalMap` | Asset-Ref | Normal-Map |
| `emissionMap` | Asset-Ref | Emissions-Map |

`materialInfo`/`materials` liefern dieselben Felder zurück (Farben als
`{x=,y=,z=}`-Tabellen, Maps als GUID-Strings) plus `id` und `fromModel`.

```lua
local m = game.createMaterial{ name = "Lava", texture = "lava.png",
                               emission = {1.0, 0.35, 0.05}, emissionStrength = 2.0 }
game.setMaterial(game.find("Boden"), m)
-- später, im update: pulsieren lassen
game.setMaterialProps(m, { emissionStrength = 1.5 + math.sin(t * 3) })
```

### 3.11 Licht

`game.setLight(id, {…})` ändert die Light-Komponente eines Objekts (nur die
angegebenen Felder), Rückgabe `true` wenn das Objekt eine hat.

| Feld | Typ | Bedeutung |
|------|-----|-----------|
| `color` | `{r,g,b}` | Lichtfarbe |
| `intensity` | Zahl | Helligkeit |
| `range` | Zahl | Reichweite in Metern |
| `type` | int | `game.LIGHT_POINT` (0) / `game.LIGHT_SPOT` (1) |
| `spotAngle`, `spotBlend` | Zahl | Kegel-Halbwinkel (Grad) / Kantenweichheit 0..1 |

```lua
game.setLight(e.id, { intensity = 6 + math.random() * 4 })  -- Flackern
```

### 3.12 Welt, Kamera, Debug

| Aufruf | Rückgabe | Beschreibung |
|--------|----------|--------------|
| `game.terrainHeight(x, z)` | Zahl | Geländehöhe an einer Weltposition |
| `game.raycast(ox,oy,oz, dx,dy,dz [,maxDist])` | `id, hx, hy, hz, dist` oder `nil` | Strahl gegen die Pick-Boxen der Objekte (achsen-parallel, Rotation wird ignoriert); `maxDist` default 1000 |
| `game.waterAt(x, z)` | `oberfläche, tiefe` oder `nil` | Wasser an einer Stelle: Oberfläche eines Bachs/Flusses oder des Sees, wenn das Gelände darunter liegt, und wie tief es dort ist; `nil`, wo es trocken ist |
| `game.trees(x0, z0, x1, z1)` | `{ x, z, größe, … }` | Alle Bäume des prozeduralen Waldes im Rechteck — hier und jetzt erzeugt, auch wo noch nichts gestreamt ist, und auf jedem Rechner mit derselben Szene dieselben (Wegfindung um Wälder) |
| `game.clearTrees(x, z, r)` | – | Der Wald wächst ab jetzt nicht mehr in dieser Scheibe (ein Gebäude wurde dort gebaut) |
| `game.setCameraPos(x, y, z)` | – | Spielerkamera setzen |
| `game.setCameraDir(x, y, z)` | – | Blickrichtung setzen (wird normalisiert) |
| `game.setCameraFov(grad)` | – | vertikaler Öffnungswinkel der freien Kamera; gilt bis Play endet |
| `game.setFocus(nah, fern)` | – | Tiefenschärfe der Skript-Kamera: scharf bis `nah`, ganz unscharf ab `fern` (Meter); `game.setFocus()` gibt die Einstellung der Ansicht zurück. Gilt bis Play endet |
| `game.setCamera(entityId)` | – | Auf die Camera-Komponente eines Objekts umschalten; `-1` = Spielerkamera |
| `game.screenSize()` | `w, h` | Viewport-Größe in Pixeln |
| `game.loadScene(name)` | – | Andere Szene des Projekts laden (am Frame-Ende, Play läuft weiter) |
| `game.timeOfDay()` | Zahl | Die Uhr der Szene in Stunden (0 bis unter 24) |
| `game.setTimeOfDay(stunden)` | – | Uhr stellen: Sonne, Himmel, Licht und Straßenlaternen folgen sofort |
| `game.dayLength()` | Zahl | Wie viele echte Sekunden ein ganzer Tag gerade dauert; `0` = die Uhr steht |
| `game.setDayLength(sekunden)` | – | Die Uhr läuft ab jetzt so schnell (ein Tag in `sekunden`), auch wenn die Szene sie angehalten hat; `0` hält sie an, `nil` lässt wieder die Szene entscheiden |
| `game.setStreetLamps(modus)` | – | Die Straßenlaternen der Städte von Hand: `"on"`/`"an"`/`true` an, `"off"`/`"aus"`/`false` aus, `"auto"`/`nil` wieder mit der Dämmerung. Scheinwerfer, Fenster und Straßenbahnen gehen weiter nach der Dämmerung |
| `game.streetLamps()` | `brennen, modus` | Ob die Laternen gerade leuchten, und der Modus (`"auto"`, `"on"`, `"off"`) |
| `game.saveData(slot, wert)` | bool | Spielstand speichern: Zahlen, Texte, Wahrheitswerte und (verschachtelte) Tabellen, als JSON pro Spiel im Benutzerordner (`%APPDATA%\fitzel\saves\<Projekt>\<slot>.json`); atomar geschrieben |
| `game.loadData(slot)` | Wert oder `nil` | Spielstand lesen; `nil`, wenn es noch keinen gibt |
| `game.log(...)` | – | Zeile auf die Konsole (stderr), beliebig viele Argumente wie `print` |
| `game.rest([fps])` | – | „In diesem Frame bewegt sich nichts von selbst": Play zeichnet dann höchstens `fps` Bilder pro Sekunde (Standard 10) und wartet sonst auf Eingaben. Jeden Frame neu aufrufen, sobald es ausbleibt, läuft wieder die volle Rate |

Uhr, Tageslänge und Laternen gehören dem Spiel: Endet Play, steht die Uhr wieder
dort, wo die Szene sie hatte, und die Laternen gehen wieder nach der Dämmerung.

**Achtung Kamera:** solange eine Camera-Komponente aktiv ist (`game.setCamera(id)`
oder *Active on start*), überschreibt sie am Frame-Ende `setCameraPos`/`Dir`/`Fov`.
Mit `game.setCamera(-1)` gibt man dem Skript die Kontrolle zurück.
Ein Skript, das im Play `game.setCameraPos` aufruft, übernimmt das Auge ganz: der
Läufer (Start „zu Fuss") bewegt die Kamera danach nicht mehr — sonst stünde sie zu
Beginn jedes Frames wieder bei der Kapsel, und Terrain, Bäume und Gras würden dort
gestreamt statt unter dem Bild, das das Skript zeigt.

**GPU schonen mit `game.rest`:** Play läuft sonst immer in voller Bildrate, auch wenn
ein Brettspiel nur auf den nächsten Klick wartet. Ein Skript, das weiss, dass gerade
nichts gleitet, dreht oder rechnet, ruft in diesem Frame `game.rest()` auf. Nach einer
Viertelsekunde ohne Unterbrechung (TAA, Bewegungsunschärfe und Belichtung beruhigen
sich) wartet die Hauptschleife auf Eingaben; jede Mausbewegung, jeder Klick und jede
Taste weckt sie sofort. Gehaltene Tasten muss das Skript selbst prüfen: eine gedrückte
Taste meldet sich nur einmal. Der Aufruf gilt für das ganze Spiel, nicht nur für das
eigene Objekt: Das rufende Skript muss wissen, dass sonst nichts läuft (Physik,
Animationen anderer Skripte). Rufen mehrere Skripte, gilt die höchste Rate.

### 3.12.1 Mehrspieler: die Tabelle `net`

Für Spiele, deren Regeln komplett im Skript laufen (Lockstep): Jeder Rechner rechnet
dieselbe Simulation aus demselben Startwert, über das Netz gehen nur die **Befehle**.
Der Relay-Server (`NetRelay`, eingebaut über `net.serve` oder eigenständig als
`fitzelserver.exe --port 27960`) verwaltet Räume und schickt alle 100 ms einen
nummerierten `TURN` mit den Befehlen aller Spieler an alle. Alles wird gepollt:
`net.poll()` einmal pro Frame ist die ganze Netzschleife. Ende von Play schließt alle
Verbindungen und einen gehosteten Server.

| Aufruf | Rückgabe | Beschreibung |
|--------|----------|--------------|
| `net.connect(host [, port])` | `true` / `false, warum` | Verbindung zum Relay aufbauen (Port Standard 27960); fertig, sobald `net.status()` `"connected"` meldet |
| `net.status()` | `zustand, fehler` | `"idle"`, `"connecting"`, `"connected"` oder `"closed"` |
| `net.send(text)` | bool | Eine Nachricht schicken (siehe Protokoll) |
| `net.poll()` | `{ text, … }` | Was seit dem letzten Aufruf ankam; treibt auch den Verbindungsaufbau und einen gehosteten Server |
| `net.close()` | – | Verbindung trennen |
| `net.serve([port [, turnMs]])` | `true, port` / `false, warum` | Den Relay in diesem Spiel starten (ein Spieler hostet) |
| `net.stopServer()` | – | Gehosteten Relay beenden |
| `net.serverInfo()` | `läuft, port, clients, räume` | Zustand des gehosteten Relays |
| `net.addresses()` | `{ ip, … }` | Die IPv4-Adressen dieses Rechners (was ein Host seinen Mitspielern sagt) |
| `net.clock()` | Zahl | Monotone Uhr in Sekunden |

Protokoll (Text, Felder durch Tab getrennt): Client → Server `HELLO name\tspiel\tversion`,
`LIST`, `CREATE max\tname\tconfig`, `JOIN name`, `LEAVE`, `SET daten`, `CONFIG text`
(nur Host), `START` (nur Host), `CMD befehl`, `HASH turn\twert`, `CHAT text`, `PING x`.
Server → Client `WELCOME id`, `ROOMS …`, `ROOM …`, `START seed\tturnMs\tslot\tanzahl\tconfig`
plus eine Zeile pro Spieler, `TURN n` plus `slot\tbefehl` pro Befehl, `LEFT slot\tname`
(wer geht, gibt auf: der Server legt ein `gg` als dessen Befehl in den nächsten Turn),
`DESYNC turn` (die `HASH`-Werte der Spieler weichen ab), `CHAT name\ttext`, `ERROR text`.
`steelwars.lua` (Projekt Steelwars) ist ein vollständiges Beispiel.

### 3.12.2 Module: `require`

Ein Spiel darf aus mehreren Dateien bestehen: `require("sw.units")` lädt
`scripts/sw/units.lua` des Projekts — auch aus dem Archiv eines exportierten Spiels.
Module laufen in der globalen Umgebung der VM (sie sehen `game`, `net`, `shared` …),
werden einmal pro Play geladen und geben üblicherweise eine Tabelle zurück.

### 3.12.3 Städte: Orte und Wege zu Fuß

Die Städte des Stadtgenerators als Ziel für Figuren. Abgeleitet aus dem, was die Stadt
ohnehin baut: die Gehwege rund um jeden Block, an den Blockecken über die Straße
verbunden (nur gerade hinüber, nie diagonal über eine Kreuzung, nie mitten im Block),
und die benannten Orte. Verkehr hält für Figuren, die mit `game.moveCharacter` laufen,
auch auf dem Überweg.

| Aufruf | Rückgabe | Beschreibung |
|--------|----------|--------------|
| `game.townPlaces([art])` | `{ ort, … }` | Alle Orte aller Städte, wahlweise nur einer Art. Ein Ort: `name` („Rathaus“, „Bushaltestelle Lindenstraße“, „Lindenstraße 12“), `kind`, `street`, `number` (Hausnummer, 0 = keine), `x, y, z` (auf dem Gehweg davor — dort bleibt eine Figur stehen), `ax, az` (der Ort selbst, zum Hinsehen), `town` (1-basiert) |
| `game.townPath(x0, z0, x1, z1)` | `{ {x=, y=, z=}, … }` oder `nil` | Fußweg vom Gehweg nächst Start zum Gehweg nächst Ziel, als Punkte in Gehreihenfolge; `nil`, wenn keine Verbindung besteht |
| `game.streetAt(x, z [, maxDist])` | `name, abstand` oder `nil` | Die nächste benannte Straße (Standard: höchstens 40 m entfernt) |

Arten (`kind`): `home` (Haus, Reihenhaus), `flat` (Wohnblock), `office` (Hochhaus),
`works` (Gewerbe), `park`, `stop` (Bushaltestelle) und die öffentlichen Gebäude
`townhall`, `school`, `kindergarten`, `church`, `police`, `firestation`, `hospital`,
`library`, `museum`, `theatre`, `pool`, `petrol`, `station`, `industry`, `powerplant`,
`landfill`, `supermarket`, `cinema`, `store` (Kaufhaus), `parking` (Parkhaus), `hotel`.
Hausnummern laufen jede Straße entlang ab ihrem Anfang, ungerade links,
gerade rechts -- dieselben, die auf den Hausnummernschildern an den Türen stehen. Gibt es eine Art mehrmals, steht die Straße im Namen
(„Schule (Goethestraße)“). Die Liste wird neu abgeleitet, sobald sich eine Stadt
ändert — einmal in `start` holen genügt.

```lua
local schulen = game.townPlaces("school")
local p = game.townPath(self.x, self.z, schulen[1].x, schulen[1].z)
-- p[1] … p[#p] ablaufen, z. B. mit game.moveCharacter
```

### 3.12.4 Sprachmodell: die Tabellen `llm` und `json`

Figuren, die selbst entscheiden und reden: ein Sprachmodell auf diesem Rechner über
die HTTP-API von [Ollama](https://ollama.com) (Standard `http://127.0.0.1:11434`,
Modell `qwen3:4b`). Ein Modell braucht Sekunden, ein Frame Millisekunden — deshalb
blockiert nichts: `llm.chat` stellt eine Anfrage in die Warteschlange und gibt sofort
ihre Nummer zurück, ein Hintergrund-Thread schickt die Anfragen der Reihe nach, und
`llm.poll()` liefert, was seitdem zurückkam. Ende von Play verwirft Warteschlange und
ausstehende Antworten.

| Aufruf | Rückgabe | Beschreibung |
|--------|----------|--------------|
| `llm.chat(anfrage)` | `id` | Anfrage einreihen (siehe unten) |
| `llm.poll()` | `{ antwort, … }` | Was seit dem letzten Aufruf fertig wurde. Eine Antwort: `id`, `ok`, `text` (ohne `<think>`-Teil), `data` (der Text als Lua-Wert, wenn `format` gesetzt war und gültiges JSON kam), `error`, `ms`, `tokens` |
| `llm.pending()` | Zahl | Anfragen in der Schlange plus die gerade laufende |
| `llm.clear()` | – | Wartende Anfragen verwerfen (die laufende nicht) |
| `llm.setHost(host [, port])` | – | Anderer Server, z. B. ein Rechner im LAN |
| `llm.setModel(name)` | – | Standardmodell für Anfragen ohne `model` |
| `llm.model()` | Text | Das Standardmodell |
| `json.encode(wert [, hübsch])` | Text | Lua-Wert als JSON (Regeln wie `game.saveData`) |
| `json.decode(text)` | Wert oder `nil, fehler` | JSON als Lua-Wert |

Die Anfrage ist eine Tabelle im Format von Ollamas `/api/chat`: `messages`
(`{ {role="user", content="…"}, … }`) oder kurz `prompt` (eine Nutzer-Nachricht),
dazu `system` (wird vorangestellt), `model`, `format` (`"json"` oder ein JSON-Schema als
Tabelle — dann kommt die Antwort auch als `data`), `options` (`{temperature=0.8,
num_predict=120}`), `priority = true` (vor alle wartenden Anfragen — für einen Spieler,
der auf eine Antwort wartet) und alles Weitere, das Ollama kennt. `think` steht auf `false`,
wenn nicht anders gesetzt: Ein denkendes Modell grübelt sonst Sekunden vor jedem Satz.

```lua
local frage
function update(self, dt)
    if not frage then
        frage = llm.chat{ system = "Du bist Bäckerin in einer Kleinstadt.",
                          prompt = "Was rufst du einem Kunden zu? Ein Satz.",
                          options = { temperature = 0.9 } }
    end
    for _, a in ipairs(llm.poll()) do
        if a.id == frage then game.log(a.ok and a.text or a.error) end
    end
end
```

Läuft kein Server, kommt die Antwort mit `ok = false` und einem `error`, der es sagt.
Der erste Aufruf nach dem Start von Ollama lädt das Modell (bei 4B rund 30 s); danach
dauert ein kurzer Satz etwa eine Sekunde. `town_agents.lua` (Projekt scaper) lässt
damit Leute durch die Stadt gehen, die selbst entscheiden, wohin, und miteinander reden.

### 3.13 Konstanten

**Entity-Typen** (für `game.spawn`s `type`):
`game.BOX` (0), `game.RAMP` (1), `game.CYLINDER` (2), `game.SPHERE` (3),
`game.LIGHT` (4), `game.SUN` (5), `game.MODEL` (6), `game.EMPTY` (7),
`game.PLANE` (8) — eine flache, beidseitige Fläche mit UV 0..1 über die ganze
Fläche: mit einem Textur-Material im Alpha-Modus `ALPHA_BLEND` ein Sprite
(Wolke, Feuerball, Insel von oben)

**Physik-Modi:** `game.PHYSICS_NONE` (0), `game.PHYSICS_STATIC` (1), `game.PHYSICS_DYNAMIC` (2)

**Alpha-Modi:** `game.ALPHA_OPAQUE` (0), `game.ALPHA_CUTOUT` (1), `game.ALPHA_BLEND` (2)

**Licht-Typen:** `game.LIGHT_POINT` (0), `game.LIGHT_SPOT` (1)

**Maustasten:** `game.MOUSE_LEFT` (0), `game.MOUSE_RIGHT` (1), `game.MOUSE_MIDDLE` (2)

**Mit der Maus spielen:** Im Play hält der Läufer den Zeiger gefangen (Maus-Blick).
Ein Spiel, das mit der Maus bedient wird, ruft in `start()` `game.showCursor(true)`
auf und übernimmt mit `game.setCameraPos` das Auge. `game.mousePos()` passt direkt
zu den HUD-Knöpfen, `game.mouseRay()` zum Zielen in die Welt — den Schnitt mit
einer Ebene oder einem Objekt rechnet das Skript selbst (`chess.lua` zeigt beides).
Klicks über anderen Editor-Fenstern melden `über = false`.

**Tasten (GLFW-Codes):**
`KEY_SPACE`, `KEY_ENTER`, `KEY_ESCAPE`, `KEY_TAB`, `KEY_BACKSPACE`, `KEY_DELETE`,
`KEY_LSHIFT`, `KEY_LCTRL`, `KEY_LALT`, `KEY_RSHIFT`, `KEY_RCTRL`, `KEY_RALT`,
`KEY_LEFT`, `KEY_RIGHT`, `KEY_UP`, `KEY_DOWN`,
`KEY_A` … `KEY_Z`, `KEY_0` … `KEY_9`, `KEY_F1` … `KEY_F12`

---

## 4. Parameter für `game.spawn`

`game.spawn` nimmt **eine Tabelle**. Alle Felder sind optional; die Defaults:

| Feld | Default | Bedeutung |
|------|---------|-----------|
| `type` | `3` (SPHERE) | Entity-Typ (`game.BOX` etc.) |
| `x`, `y`, `z` | `0` | Startposition |
| `size` | `0.5` | Kurzform: setzt `sx/sy/sz`, falls diese fehlen |
| `sx`, `sy`, `sz` | `size` | Halb-Ausdehnung pro Achse |
| `rx`, `ry`, `rz` | `0` | Startrotation (Grad) |
| `r`, `g`, `b` | `0.8` | Farbe (0..1) |
| `vx`, `vy`, `vz` | `0` | Anfangsgeschwindigkeit (dynamische Bodies) |
| `mass` | `1.0` | Masse |
| `physics` | `2` | `0` = keine, `1` = statisch, `2` = dynamisch |
| `name` | – | Anzeigename |
| `script` | – | Lua-Datei unter `scripts/` (z. B. `"bullet.lua"`) |
| `model` | – | Model-Asset (Name/Pfad/GUID) — macht daraus ein **Model**-Objekt, Größe aus der Bounding-Box |
| `scale` | `1.0` | Skalierung des Modells (nur mit `model`) |
| `material` | – | Material aus der Bibliothek (Name oder GUID) |
| `parent` | `-1` | Eltern-Objekt; dann sind `x/y/z` und `rx/ry/rz` **lokal** zum Parent |

```lua
local id = game.spawn{
    type = game.SPHERE,
    x = px, y = py, z = pz,
    size = 0.14,
    r = 0.95, g = 0.9, b = 0.3,
    mass = 0.5,
    vx = dx * 34, vy = dy * 34, vz = dz * 34,
    script = "bullet.lua",
}

-- Ein Baum aus dem Asset-Bestand, statisch, auf Geländehöhe:
local tree = game.spawn{
    model = "tree.glb", scale = 1.4,
    x = 12, y = game.terrainHeight(12, -5), z = -5,
    physics = game.PHYSICS_STATIC,
}
```

Ein `spawn` mit `model` gibt `0` zurück, wenn das Asset nicht gefunden wurde (die
Konsole nennt den Namen).

---

## 5. Skript an ein Objekt hängen

- **Im Editor:** Objekt selektieren → Inspector → Feld *Script* → Dateiname
  (z. B. `spin.lua`). Dateien liegen im `scripts/`-Ordner neben der Exe (wird beim
  Build aus `sandbox/scripts/` kopiert).
- **Per Code:** beim `game.spawn` das Feld `script = "…"` setzen.

Danach **Play** drücken. `start` läuft einmal, `update` jeden Frame.

### 5.1 Skript-Parameter (globale Variablen im Inspector)

Jede **globale Variable auf Modulebene** (also eine, die *ohne* `local`
angelegt wird) erscheint automatisch als **editierbares Feld im Inspector** unter
der Script-Komponente. Der Wert im Skript ist nur der **Default**; der im
Inspector eingestellte Wert **überschreibt** ihn beim Start von Play und wird
**mit der Szene gespeichert** (pro Objekt-Instanz — zwei Objekte mit demselben
Skript haben also eigene Werte).

```lua
-- Alles ohne `local` = ein Inspector-Feld:
speed   = 45.0          -- Zahl   -> Drag-Feld
bobbing = true          -- bool   -> Checkbox
label   = "Hallo"       -- string -> Textfeld
tint    = {1.0, 0.5, 0} -- 3 Zahlen -> Farb-/Vektor-Picker

local baseY = nil       -- `local` -> NICHT im Inspector (privater Zustand)

function start(e) baseY = e.y end
function update(e, dt, t)
    e.ry = e.ry + speed * dt            -- nutzt den Inspector-Wert
    if bobbing and baseY then
        e.y = baseY + math.sin(t * 2) * 0.4
    end
end
```

**Typ-Ableitung** (aus dem Default-Wert):

| Lua-Wert | Inspector-Widget |
|----------|------------------|
| Zahl | Drag-Feld |
| `true` / `false` | Checkbox |
| String | Textfeld |
| Tabelle `{x, y, z}` / `{r, g, b}` | Vektor bzw. **Farbe** (Farbe, wenn `r/g/b`-Schlüssel oder der Name nach Farbe klingt: `color`, `colour`, `tint`, `rgb`) |

**Fallstricke:**

- Nur `number` / `bool` / `string` / 3-Zahlen-Tabellen werden exponiert. Funktionen
  (`start`, `update`, Helfer), verschachtelte Tabellen usw. werden ignoriert.
- Namen, die mit `_` beginnen, sind **privat** und erscheinen nicht.
- Der Modul-Rumpf wird zum Einlesen der Defaults **einmal ausgeführt** (in einer
  Sandbox, `game.*` ist dabei ein No-op). Teure Arbeit gehört in `start()`, nicht
  auf Modulebene.
- Die Vektor-/Farb-Werte kommen im Skript als Tabelle an, lesbar als `t.x/t.y/t.z`,
  `t.r/t.g/t.b` **oder** `t[1]/t[2]/t[3]`.
- Ein Feld umbenennen/entfernen im Skript entfernt es (nach dem nächsten Blick in
  den Inspector) auch aus dem Objekt; „Reset to defaults" setzt alle Felder auf die
  Skript-Defaults zurück.

---

## 6. Komplettbeispiele (im Repo unter `sandbox/scripts/`)

### `spin.lua` — Objekt drehen & wippen
Zeigt zugleich **Skript-Parameter** (§5.1): `spinSpeed`, `bobHeight`, `bob` sind
globale Variablen und damit im Inspector einstellbar.
```lua
spinSpeed = 45.0   -- Grad/Sekunde (Inspector-Feld)
bobHeight = 0.4    -- Wipp-Amplitude in Metern
bob       = true   -- Wippen an/aus (Checkbox)

local baseY = nil
function start(e)  baseY = e.y  end
function update(e, dt, t)
    e.ry = e.ry + spinSpeed * dt
    if bob and baseY then e.y = baseY + math.sin(t * 2.0) * bobHeight end
end
```

### `assets.lua` — Asset-Tour
Skript auf ein beliebiges Objekt legen, Play drücken: `M` pflanzt das erste
Model-Asset vor die Kamera, `T` baut ein Material aus dem ersten Textur-Asset und
hängt es dran, `C` färbt das Objekt unterm Fadenkreuz (Raycast), `L` listet alle
Assets auf die Konsole. Zeigt `game.assets`, `game.spawn{model=…}`,
`game.createMaterial`, `game.setMaterial`, `game.setColor`, `game.raycast`,
`game.terrainHeight` und `game.log`.

### `scroller.lua` — Top-down Vertical-Scroller-Kamera
Skript auf ein **beliebiges** einzelnes Objekt legen (ein Empty reicht), Play
drücken: die Ansicht springt auf einen hohen, steilen Top-down-Winkel und scrollt
mit konstanter Geschwindigkeit nach vorn (+Z) — wie Raiden / 1942. Treibt die
freie Kamera jeden Frame über `game.setCameraPos`/`setCameraDir`/`setCameraFov`
(Scripts ticken **nach** der Spielerbewegung, gewinnen also). Wichtig: **keine**
Camera-Entity „active on start" markieren, sonst übernimmt die die Ansicht.
Tunables oben im Skript: `SPEED`, `HEIGHT`, `TILT` (kleiner = steiler; `> 0`
halten, exakt senkrecht gimbal-lockt eine Yaw/Pitch-Kamera), `FOV`. Kombiniert
mit `game.spawnPrefab` (§3.3) lassen sich Gegner/Hindernisse vorausscrollend
einsetzen.

### `orbit.lua` — Objekt auf einer Kreisbahn
Skript auf das Objekt legen, Play drücken: es läuft auf einem Kreis um die
Stelle, an der es beim Start **stand** — man stellt es also auf die Mitte, nicht
auf die Bahn. `radius` und `speed` sind Inspector-Felder, dazu `startAngle`
(mehrere Objekte gleichmässig auf einer Bahn verteilen), `clockwise` und
`faceForward` (Nase in Fahrtrichtung). `speed` ist eine **Bahn**geschwindigkeit
in m/s, nicht Grad pro Sekunde: ein grösserer Radius macht die Runde länger, nicht
das Objekt schneller. Weil der Transform lokal ist (§2), kreist ein Objekt mit
Parent im Raum des Parents und wandert mit ihm mit.

### `shooter.lua` + `bullet.lua` + `can.lua` — „Dosen schiessen"
Ein kleines Mini-Game: Skript auf ein beliebiges Objekt legen, Play drücken.
Linksklick schiesst eine Kugel in Blickrichtung, `R` stellt die Dosenreihe neu auf,
umgeworfene Dosen geben einen Punkt. Zeigt zusammen so ziemlich die ganze API:
Eingabe, Kamera, `spawn`/`destroy`, Physik-Velocity, Sound, Score/HUD und
skript-übergreifende Kommunikation. Siehe die Dateien direkt.

### `chess.lua` — Schach gegen eine Engine, mit der Maus

An die Wurzel eines Schachbretts hängen, unter der die Figuren als Kinder liegen.
Das Skript liest Feldgröße und Ausrichtung aus der Lage der beiden Könige, erkennt
die Figuren am Namen und klont fehlende (auch bei der Bauernumwandlung). Bedient
wird per Klick-Klick (`game.showCursor`, `game.mouseRay`, `game.mousePos`), die
Engine (Alpha-Beta mit Ruhesuche, ~1250 Elo geschätzt) rechnet in einer Coroutine
in Scheiben von ~10 ms, damit das Bild weiterläuft. Inspector-Felder: Farbe des
Menschen, Suchtiefe, Zufall, Patzer-Anteil, Denkzeit, Zug-Klang (`move.wav`, spielt
per `game.playSound`, wenn die gezogene Figur aufsetzt, bei beiden Seiten). Wartet das Spiel auf den
Menschen und steht alles still, ruft es `game.rest()` — die GPU zeichnet dann nicht
mehr in voller Rate dasselbe Bild.

### `inventory.lua` — ein Inventar für eine Figur

Als **zweites** Skript auf eine Figur hängen, die mit `tp_controller.lua` (Projekt
treetest) läuft. Aufheben lässt sich jedes Objekt mit **Collectible**-Komponente,
bei der *Inventory item* angehakt ist (§3.7.4). Im *Pickup radius* erscheint über
dem Gegenstand ein Hinweis („E Pistole aufheben“, `game.worldToHud`); auf `E` dreht
sich die Figur, tritt vor oder zurück, bis der Gegenstand dort liegt, wo ihre linke
Hand den Boden trifft, bückt sich (Graph-Zustand `pickup`) und nimmt ihn — ab dem
Griff hängt er an `CC_Base_L_Hand` (`game.attach`), dann wandert er in den Beutel.
Den Ablauf steuert der Controller; das Inventar fragt ihn über `shared.figures[id]`
(§3.6.2). Ohne Controller geht der Gegenstand sofort in den Beutel – ebenso, wenn er
nicht auf dem Boden liegt, auf dem die Figur steht (mehr als `FLOOR_REACH`, 0,3 m,
über oder unter ihren Füßen: Tisch, Regal, Sims).

`I` oder `Tab` öffnet das Inventar (`game.captureInput`: die Figur bleibt stehen,
Esc schließt nur das Fenster): ein Raster mit Icons (`game.hudImage`, auf den
sichtbaren Teil des Bildes zugeschnitten), daneben die Karte des gewählten Dings mit
Name, Kategorie, Beschreibung und den Knöpfen. Waffen lassen sich **ausrüsten** —
sie hängen dann mit `GRIP_POS`/`GRIP_ROT` in der rechten Hand —, alles lässt sich
**wegwerfen**: es fliegt im Bogen nach vorn und landet auf dem, was dort liegt
(`game.groundHeight`: Weg, Brücke, Boden), bereit zum Wiederaufheben. Maus, Pfeile
oder WASD wählen. Inspector-Felder: Tasten, Anzahl Plätze, Knochen, Griff, Akzentfarbe
und die Klänge. Die Beschriftungen stehen oben im Skript in `TEXT`. Munition stapelt
sich (eine zweite Schachtel kommt auf die erste), und ein weggeworfener Stapel behält,
was noch drin war.

### `weapon.lua` — Schießen mit der ausgerüsteten Waffe

Als **drittes** Skript auf dieselbe Figur, nach Controller und Inventar. Es arbeitet
mit dem, was das Inventar ausgerüstet hat, und mit dessen Munition
(`shared.inventories[id]`: `equipped`, `ammo`, `takeAmmo`).

- **Rechte Maustaste** zielt: Der Controller bleibt stehen und dreht die Figur mit der
  Kamera (`shared.figures[id].hold(yaw)`, Graph-Bool `aiming`, Zustand `aim`), die
  Orbit-Kamera kommt über die rechte Schulter (`game.orbitFrame`), die Waffe dreht sich
  in der Hand in den Anschlag (`game.attach` mit Überblendung), ein Fadenkreuz erscheint.
- **Linke Maustaste** schießt (Trigger `shoot`, Zustand `shoot` mit dem Rückstoß): ein
  Strahl vom Auge durch die Bildmitte (`game.castRay`), Staub und Funken am Einschlag,
  Mündungsfeuer, Rauch und ein Lichtblitz an der Mündung (`game.toWorld`,
  `game.emit` auf Effekt-Objekte, `game.setLight`), ein Stoß für lose Physik-Körper,
  eine Treffer-Markierung, der Einschlag ist mit Schallverzögerung zu hören.
- **R** lädt nach; ein leeres Magazin klickt und lädt beim nächsten Schuss selbst nach.
  Unten rechts zeigt eine Karte Waffe, Magazin (eine Marke je Patrone) und Vorrat.
- Jeder Treffer hinterlässt ein **Einschussloch** (`game.decal`) auf dem, was dort
  stillsteht – Wand, Boden, Modell, Gelände. `HOLE_SIZE` ist seine Größe,
  `HOLE_MATERIAL` ein eigenes Bibliotheks-Material (leer: das der Engine),
  `BULLET_HOLES = false` schaltet sie ab.
- **Glas** zerbricht, wo es getroffen wird (`game.shatter`), klirrt (`GLASS_SOUND`), und
  der Schuss geht durch das Loch weiter zu dem, was dahinter ist. `GLASS = false`: Glas
  bekommt Einschusslöcher wie eine Wand.

Inspector-Felder: Tasten, Magazin, Feuerrate, Nachladezeit, Reichweite, Stoß, Griff im
Anschlag, Mündung, die Schulterkamera (`AIM_DIST`, `AIM_SIDE`, `AIM_UP`, `AIM_FOV`),
die Namen der Effekt-Objekte und die Klänge.

### `sokoban.lua` — Sokoban, ein ganzes Spiel in einer Datei
Skript auf **ein** Objekt legen — am besten ein **Empty**, denn das Spielbrett
wird um dessen Position herum gebaut — und Play drücken. Pfeiltasten/WASD laufen
und schieben, `Z`/Backspace nimmt einen Zug zurück (bis zurück zum Levelanfang),
`R` startet das Level neu, `Q`/`E` drehen die Ansicht in 90°-Schritten (die
Steuerung dreht mit), `N`/`P` blättern durch die zwölf Level.

Sonst muss **nichts** in der Szene stehen: Boden, Wände, Kisten, Ziele und die
Spielfigur entstehen alle per `game.spawn` (mit `physics = game.PHYSICS_NONE` —
ein Schiebe-Puzzle will keine Physik) und verschwinden beim Levelwechsel wieder
per `game.destroy`. Eine Kiste, die auf einem Ziel steht, färbt `game.setColor`
grün. Die Kamera hängt an `setCameraPos`/`Dir`/`Fov` und rahmt das Brett anhand
seiner Grösse ein — also **keine** Camera-Entity „active on start" (§3.12).

Inspector-Felder: `startLevel`, `cellSize`, `stepTime`, `repeatDelay`,
`repeatRate`, `camPitch`, `camZoom`, `sound`, `snapToGround`.

Zwei Stellen darin sind es wert, nachgelesen zu werden: die Kisten bekommen ihre
Farbe **einen Frame später** (der `spawn` von eben existiert noch nicht, ein
`setColor` darauf wäre ein No-op — §3.3), und die Bewegungsrichtungen werden aus
der Ansichtsdrehung gerechnet statt fest verdrahtet, damit „hoch" nach dem Drehen
immer noch heisst, was der Spieler sieht.

### `shmup.lua` — SKYSTRIKE, ein Vertical-Shoot'em-up
Skript auf **ein** Empty legen, Play drücken, Enter. Vier Stages — Inselmeer,
Flotte, Sturmfront (Regen, Blitze) und Küste mit Festland — mit Staffeln in
Formationen auf Spline-Bahnen, Drohnenschwärmen, Kamikaze-Hornissen,
Laser-Drohnen, Minen, Kanonenbooten, Panzern, Bunkern, einem Zwischenboss und vier
Endbossen mit abschiessbaren Teilen und Angriffsphasen; danach beginnt die
nächste Runde schneller. Zwei Schiffe (Vulcan-Streuung oder durchschlagender
Laser, im Titel wählbar), fünf Waffenstufen, Bomben, Chain, Graze, Continue.
Abgeschossenes stürzt brennend ins Meer, Schiffe sinken, der Boden läuft über
Übergangskacheln (Küste, Wetterfront) von einer Landschaft in die nächste.

Zeigt, wie weit man mit Primitiven kommt: rund 1700 Objekte werden in `start()`
auf Vorrat gespawnt und nur noch per `setActive` geschaltet (§3.3 — ein Geschoss,
das einen Frame zu spät erscheint, trifft aus dem Nichts). Wolken, Inseln, Meer,
Feuer und Rauch sind texturierte `PLANE`-Sprites; das HUD mit Seitenleisten,
Bossbalken und Bannern ist mit `game.hud*` gezeichnet (§3.6.1).

Die Texturen erzeugt `tools/gen_shmup_textures.py` (braucht numpy). Die Sounds
schneidet `tools/import_shmup_sounds.py <Sample-Bibliothek> content/sounds` aus
aufgenommenen Explosionen und Foley (`skystrike_*.wav`, mehrere Varianten je
Zweck, zufällig gewählt); dahinter liegen die synthetischen aus
`tools/gen_shmup_sounds.py`. Ohne beides läuft das Spiel mit schlichten
Materialien und den Standard-Sounds der Engine.

**Über echter Landschaft.** Liegt das Skript in einer Szene mit Terrain (und
`landscape` ist an), fliegt SKYSTRIKE statt über das gemalte Meer über die Szene
selbst — Stage HIGH VALLEY. Der Ursprung der Flugebene wandert über die Welt
(`OZ`), die Flugebene reitet auf dem höchsten Gelände im Bild (`BY`), und alles
wird mit `SYS.LAND.scale` (5 m je Einheit) gezeichnet: Jäger, Panzer und Festung
haben die Grösse neben echten Bäumen. Bodengegner stehen per
`game.terrainHeight` auf dem Gelände. Der Träger (das Empty mit dem Skript) ist
der Start der Route, ein Empty namens **„Shmup Arena"** der Platz der Festung
(eine Lichtung — Wald wächst sonst durch sie hindurch). `nextScene` verbindet zwei
Szenen zu einer Runde: im SKYSTRIKE-Projekt geht es nach der Küste in
`hochland.fitzel` weiter und danach zurück aufs Meer; Punkte, Schiffe, Bomben und
Waffe reisen in einer kleinen Datei neben dem Highscore mit. `hochland.fitzel`
ist eine für den Blick von oben eingestellte Kopie der scaper-Landschaft
(Impostoren mit Draufsicht und Kartenschatten, keine Mesh-Schatten, kein
Fernterrain, Flüsse getönt) — im düsteren Abendlicht mit fester Belichtung. Über
dem Tal brennt es: Feuer mit Glut und Rauchfahnen, die über die Kronen abziehen,
jedes abgeschossene Bodenziel brennt weiter, und ab und zu wetterleuchtet es.

**Hangar und Einsätze.** Das Titelmenü bietet KAMPAGNE, EINSÄTZE und HANGAR (Pfeile
hoch/runter, Enter, Backspace zurück). Jede Runde zahlt Credits (ein Hundertstel
der Punkte, beim ersten Abschluss eines Einsatzes ein Bonus); im Hangar kaufen sie
Upgrades in drei Stufen — Feuerkraft, Feuerrate, Raketen, Schild, Triebwerke,
Bombenschacht, Reserveschiff, Bergung —, und das Schiff trägt, was gekauft ist. Ein
einmal geschaffter Einsatz lässt sich allein fliegen, auch über die Szenengrenze
(das Tal aus der Meer-Szene und zurück ins Einsatz-Menü). Das Pilotenprofil liegt
per `game.saveData("profile", …)` im Benutzerordner.

---

## 7. Gut zu wissen (Fallstricke)

- **Frische VM bei jedem Play:** kein Zustand überlebt einen Play-Stop.
- **`spawn`/`destroy` sind deferred** — die ID ist sofort gültig, das Objekt kommt
  aber erst nächsten Frame; verlasse dich im selben Frame nicht auf seine Existenz.
- **Isolierte Umgebungen:** globale Variablen gelten pro (Objekt, Skriptdatei) —
  nicht szenenweit, und auch nicht zwischen zwei Skripten auf demselben Objekt.
  Geteilter Zustand nur über den Host (Score/HUD) — oder ohnehin bewusst gehalten.
- **Fehler = Skript still deaktiviert** bis zum nächsten Play — nur dieses eine;
  weitere Skripte auf demselben Objekt laufen weiter. Konsole/Editor-UI zeigt die
  letzte Fehlermeldung. Ein Skript, das gar nichts tut, hat meist dort seinen
  Grund: nach dem ersten Fehler wird es nicht mehr aufgerufen.
- **Nur numerische Transform-Felder werden zurückgeschrieben** (`x/y/z`, `rx/ry/rz`,
  `sx/sy/sz`). `e.name`/`e.id`/`e.type`/`e.parent`/`e.active` schreiben wirkt nicht.
- **Material- und Asset-Änderungen im Play-Modus sind flüchtig.** Play macht vorher
  eine Kopie von Szene *und* Materialbibliothek und stellt sie beim Stoppen wieder
  her — per Skript angelegte Materialien (auch die von `game.setColor`) verschwinden
  also wieder und landen nie in der gespeicherten Projektdatei.
- **`game.loadModel` / `spawn{model=…}` lädt beim ersten Mal von der Platte** (GPU-Upload).
  Das kostet einen Frame-Hänger — besser in `start()` vorladen als mitten im Spiel.
- **`game.raycast` trifft Pick-Boxen, keine Dreiecke:** achsen-parallele Kästen um
  `center ± half`, Rotation wird ignoriert. Für Sichtlinien und „worauf zeige ich"
  reicht das, für exakte Treffer auf schrägen Modellen nicht.
- **`game.log` schreibt nach stderr** (Konsolenfenster des Editors), es gibt kein
  Log-Panel in der UI.
- **`game.setCameraFov` gilt erst seit 2026-09 wirklich.** Vorher setzte die freie
  Kamera jeden Frame den Öffnungswinkel des Editors zurück (meist 60°) — ein Skript,
  das damals mit einem anderen Wert „passend" eingestellt wurde, war in Wahrheit auf
  60° abgestimmt und rahmt jetzt enger oder weiter.
- **`game.loadScene` im Editor verwirft ungespeicherte Änderungen** der Szene, die
  man verlässt: Play wird gestoppt, die neue Szene geladen und Play neu gestartet.
  Wer ein Spiel mit Szenenwechsel im Editor testet, speichert vorher.
- **Unter `game.rest` läuft die Spielzeit langsamer.** `dt` ist auf 0,05 s begrenzt;
  bei 10 Bildern pro Sekunde vergeht pro Frame also nur die halbe Zeit. Alles, was
  nach Zeit läuft (Animationen, Timer, Physik, die noch ausrollt), gehört deshalb in
  Frames ohne `game.rest`.

---

*Diese Referenz spiegelt den Stand von `ScriptSystem.cpp` / `ScriptHost.hpp`. Wenn
neue `game.*`-Funktionen dazukommen, hier ergänzen.*
