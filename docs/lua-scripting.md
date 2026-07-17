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

Jedes Skript läuft in seiner **eigenen Umgebung**: `local`- und globale Variablen
im Skript sind pro Entity isoliert. Zwei Objekte mit demselben Skript teilen also
**keinen** Zustand. Gemeinsamer Zustand läuft über den Host (`game.addScore` /
`game.getScore` / `game.setHud`).

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
| `game.destroy(id)` | – | Objekt entfernen |
| `game.getPos(id)` | `x, y, z` oder `nil` | Weltposition; `nil` bei unbekannter ID |
| `game.setPos(id, x, y, z)` | – | Objekt an Position setzen |

**Deferral:** `game.spawn` gibt die neue ID **sofort** zurück, das Objekt erscheint
aber erst am **Ende des Frames** (die Tick-Schleife iteriert gerade die
Entity-Liste). `game.destroy` ist ebenfalls deferred.

### 3.4 Physik (auf dynamischen Bodies, per ID)

| Aufruf | Beschreibung |
|--------|--------------|
| `game.setVelocity(id, vx, vy, vz)` | Lineare Geschwindigkeit setzen |
| `game.applyImpulse(id, jx, jy, jz)` | Impuls anwenden |

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

Score/HUD liegen im **Host** (nicht in der isolierten Skript-Umgebung), sind also
über alle Skripte hinweg geteilt.

### 3.7 Konstanten

**Entity-Typen** (für `game.spawn`s `type`):
`game.BOX` (0), `game.RAMP` (1), `game.CYLINDER` (2), `game.SPHERE` (3)

**Maustasten:** `game.MOUSE_LEFT` (0), `game.MOUSE_RIGHT` (1), `game.MOUSE_MIDDLE` (2)

**Tasten (GLFW-Codes):**
`KEY_SPACE`, `KEY_ENTER`, `KEY_ESCAPE`, `KEY_LSHIFT`, `KEY_LCTRL`,
`KEY_LEFT`, `KEY_RIGHT`, `KEY_UP`, `KEY_DOWN`,
`KEY_A` … `KEY_Z`, `KEY_0` … `KEY_9`

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
```

---

## 5. Skript an ein Objekt hängen

- **Im Editor:** Objekt selektieren → Inspector → Feld *Script* → Dateiname
  (z. B. `spin.lua`). Dateien liegen im `scripts/`-Ordner neben der Exe (wird beim
  Build aus `sandbox/scripts/` kopiert).
- **Per Code:** beim `game.spawn` das Feld `script = "…"` setzen.

Danach **Play** drücken. `start` läuft einmal, `update` jeden Frame.

---

## 6. Komplettbeispiele (im Repo unter `sandbox/scripts/`)

### `spin.lua` — Objekt drehen & wippen
```lua
local baseY = nil
function start(e)  baseY = e.y  end
function update(e, dt, t)
    e.ry = e.ry + 45.0 * dt
    if baseY then e.y = baseY + math.sin(t * 2.0) * 0.4 end
end
```

### `shooter.lua` + `bullet.lua` + `can.lua` — „Dosen schiessen"
Ein kleines Mini-Game: Skript auf ein beliebiges Objekt legen, Play drücken.
Linksklick schiesst eine Kugel in Blickrichtung, `R` stellt die Dosenreihe neu auf,
umgeworfene Dosen geben einen Punkt. Zeigt zusammen so ziemlich die ganze API:
Eingabe, Kamera, `spawn`/`destroy`, Physik-Velocity, Sound, Score/HUD und
skript-übergreifende Kommunikation. Siehe die Dateien direkt.

---

## 7. Gut zu wissen (Fallstricke)

- **Frische VM bei jedem Play:** kein Zustand überlebt einen Play-Stop.
- **`spawn`/`destroy` sind deferred** — die ID ist sofort gültig, das Objekt kommt
  aber erst nächsten Frame; verlasse dich im selben Frame nicht auf seine Existenz.
- **Isolierte Umgebungen:** globale Variablen sind pro Entity, nicht global über die
  ganze Szene. Geteilter Zustand nur über den Host (Score/HUD) — oder ohnehin
  bewusst gehalten.
- **Fehler = Skript still deaktiviert** bis zum nächsten Play. Konsole/Editor-UI
  zeigt die letzte Fehlermeldung.
- **Nur numerische Transform-Felder werden zurückgeschrieben** (`x/y/z`, `rx/ry/rz`,
  `sx/sy/sz`). `e.name`/`e.id` schreiben wirkt nicht.

---

*Diese Referenz spiegelt den Stand von `ScriptSystem.cpp` / `ScriptHost.hpp`. Wenn
neue `game.*`-Funktionen dazukommen, hier ergänzen.*
