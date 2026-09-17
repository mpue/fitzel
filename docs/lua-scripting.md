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

`key` ist ein GLFW-Keycode → benutze die `game.KEY_*`-Konstanten (§3.7).
`button`: `game.MOUSE_LEFT` (0), `MOUSE_RIGHT` (1), `MOUSE_MIDDLE` (2).

### 3.2 Kamera (Play-Modus)

| Aufruf | Rückgabe | Beschreibung |
|--------|----------|--------------|
| `game.cameraPos()` | `x, y, z` | Position der Spielerkamera |
| `game.cameraDir()` | `x, y, z` | normalisierte Blickrichtung |

```lua
local px, py, pz = game.cameraPos()
local dx, dy, dz = game.cameraDir()
```

### 3.3 Entities erzeugen & entfernen

| Aufruf | Rückgabe | Beschreibung |
|--------|----------|--------------|
| `game.spawn{ … }` | `id` (int) | Neues Objekt erzeugen (Parameter-Tabelle, §4) |
| `game.spawnPrefab(name, x, y, z [, yaw])` | `id` (int) | Prefab-Instanz erzeugen; gibt die **Wurzel-ID** zurück |
| `game.destroy(id)` | – | Objekt entfernen |
| `game.getPos(id)` | `x, y, z` oder `nil` | Weltposition; `nil` bei unbekannter ID |
| `game.setPos(id, x, y, z)` | – | Objekt an Position setzen |

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
| `game.applyImpulse(id, jx, jy, jz)` | – | Impuls anwenden |
| `game.setAngularVelocity(id, wx, wy, wz)` | – | Drehgeschwindigkeit (rad/s) setzen |

No-op bei unbekannten IDs oder Objekten ohne dynamischen Physik-Body.

### 3.5 Audio

| Aufruf | Beschreibung |
|--------|--------------|
| `game.playSound(name)` | One-shot-Sound aus dem `sounds/`-Ordner abspielen (z. B. `"shot.wav"`) |
| `game.playAudio(id)` | AudioSource-Komponente eines Objekts starten |
| `game.stopAudio(id)` | AudioSource-Komponente eines Objekts stoppen |

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

Um etwas an einer Stelle der Welt zu beschriften (Punkte über einem Gegner),
rechnet das Skript die Weltposition selbst durch die Kamera, die es gesetzt hat —
`shmup.lua` zeigt das in `project()`.

Score/HUD liegen im **Host** (nicht in der isolierten Skript-Umgebung), sind also
über alle Skripte hinweg geteilt.

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
| `game.setCameraPos(x, y, z)` | – | Spielerkamera setzen |
| `game.setCameraDir(x, y, z)` | – | Blickrichtung setzen (wird normalisiert) |
| `game.setCameraFov(grad)` | – | vertikaler Öffnungswinkel der freien Kamera; gilt bis Play endet |
| `game.setFocus(nah, fern)` | – | Tiefenschärfe der Skript-Kamera: scharf bis `nah`, ganz unscharf ab `fern` (Meter); `game.setFocus()` gibt die Einstellung der Ansicht zurück. Gilt bis Play endet |
| `game.setCamera(entityId)` | – | Auf die Camera-Komponente eines Objekts umschalten; `-1` = Spielerkamera |
| `game.screenSize()` | `w, h` | Viewport-Größe in Pixeln |
| `game.loadScene(name)` | – | Andere Szene des Projekts laden (am Frame-Ende, Play läuft weiter) |
| `game.saveData(slot, wert)` | bool | Spielstand speichern: Zahlen, Texte, Wahrheitswerte und (verschachtelte) Tabellen, als JSON pro Spiel im Benutzerordner (`%APPDATA%\fitzel\saves\<Projekt>\<slot>.json`); atomar geschrieben |
| `game.loadData(slot)` | Wert oder `nil` | Spielstand lesen; `nil`, wenn es noch keinen gibt |
| `game.log(...)` | – | Zeile auf die Konsole (stderr), beliebig viele Argumente wie `print` |

**Achtung Kamera:** solange eine Camera-Komponente aktiv ist (`game.setCamera(id)`
oder *Active on start*), überschreibt sie am Frame-Ende `setCameraPos`/`Dir`/`Fov`.
Mit `game.setCamera(-1)` gibt man dem Skript die Kontrolle zurück.
Ein Skript, das im Play `game.setCameraPos` aufruft, übernimmt das Auge ganz: der
Läufer (Start „zu Fuss") bewegt die Kamera danach nicht mehr — sonst stünde sie zu
Beginn jedes Frames wieder bei der Kapsel, und Terrain, Bäume und Gras würden dort
gestreamt statt unter dem Bild, das das Skript zeigt.

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

---

*Diese Referenz spiegelt den Stand von `ScriptSystem.cpp` / `ScriptHost.hpp`. Wenn
neue `game.*`-Funktionen dazukommen, hier ergänzen.*
