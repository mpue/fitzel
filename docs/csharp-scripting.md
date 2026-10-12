# Fitzel — C#-Scripting

Neben Lua kann ein Spiel seine Skripte in **C#** schreiben. Fitzel bettet dafür
**.NET 10** ein (die installierte Laufzeit, über `hostfxr`) und legt im Projekt eine
**Visual-Studio-Solution** an — wie Unity. Diese Doku beschreibt, was heute
gebunden ist (`sandbox/src/CSharpHost.cpp`, `sandbox/dotnet/Fitzel/`).

Voraussetzung: **.NET 10 SDK** (zum Bauen der Skripte) und Windows. Ein
exportiertes Spiel braucht auf dem Zielrechner die **.NET 10 Runtime**.

---

## 1. Ein C#-Projekt anlegen

*File → New Project…* → unter **Scripting** „C# (.NET, Visual Studio)“ wählen.
Ein bestehendes Projekt stellt man unter *File → Game Settings → Scripting* um
(„Save“ legt die Dateien an).

Im Projektordner entsteht:

| Datei | Zweck |
|---|---|
| `<Projekt>.sln` | die Solution für Visual Studio |
| `Assembly-CSharp.csproj` | übersetzt **jede `.cs` unter `scripts/`** in eine Assembly |
| `Directory.Build.props` | legt die Build-Zwischendateien nach `Library/obj` |
| `Library/Fitzel.dll` + `.xml` | die Fitzel-API (vom Editor aktuell gehalten, mit IntelliSense-Doku) |
| `Library/ScriptAssemblies/Assembly-CSharp.dll` | das Ergebnis, das Play lädt |
| `scripts/Rotator.cs` | ein erstes Skript zum Anfangen |
| `.gitignore` | `Library/`, `.vs/` bleiben draußen |

Die `.csproj` schreibt Fitzel nur einmal; danach darf man sie in Visual Studio
frei ändern (NuGet-Pakete, weitere Dateien …).

## 2. Arbeitsablauf

1. Skripte im eingebauten **C# Editor** schreiben (*Assets → C# Editor*, oder
   „Edit“ an der Script-Karte) — oder in Visual Studio (*File → Open in Visual
   Studio* im C# Editor, *Assets → C# Scripts → Open in Visual Studio*).
2. Skript schreiben und speichern (Strg+S). **Der Editor baut selbst** (`dotnet build` im
   Hintergrund), sobald sich eine `.cs` ändert — Visual Studio muss dafür nicht
   bauen. Baut man trotzdem in VS, landet das Ergebnis an derselben Stelle.
3. Fehler erscheinen im Fenster **C# Scripts** (öffnet sich bei einem
   fehlgeschlagenen Build von selbst). Ein Klick auf die Zeile öffnet die Datei im
   C# Editor an dieser Stelle.
4. **Play** speichert, was im C# Editor ungespeichert ist, wartet auf den Build und
   lädt dann die neueste Assembly — frisch bei jedem Play, ohne den Editor neu zu
   starten.

Neue Skripte: im C# Editor *File → New Script…*, oder *Assets → Scripts → File →
New…*, Sprache „C#“.

### Der C# Editor

Ein kleines VS Code im Editor. Den Code versteht Roslyn — der Compiler aus dem
installierten .NET-SDK, als eigener Hintergrundprozess (`Fitzel.LanguageServer`),
also genau das C#, mit dem die Skripte auch gebaut werden. Er kennt alle Skripte
des Projekts, die Fitzel-API (mit ihrer Doku) und das .NET-Framework.

| Taste | |
| --- | --- |
| Tippen, `.` oder Strg+Leertaste | Completion; Tab oder Enter übernimmt, `(` `.` `;` auch |
| `"` in `Entity.Find(` / `FindAll(` | die Objektnamen der offenen Szene (mit Typ) |
| `(` und `,` / Strg+Umschalt+Leertaste | Signaturhilfe; ↑/↓ wechselt Überladungen |
| Maus auf einem Namen | Quick Info (Typ, Doku) und die Fehler an der Stelle |
| F12 / Strg+Klick | zur Definition (aus der API: was es ist) |
| Umschalt+F12 | alle Verwendungen (unten unter *References*) |
| F2 | umbenennen, in allen Dateien; ein Strg+Z macht es rückgängig |
| Umschalt+Alt+F | Dokument formatieren |
| Strg+F / Strg+G | suchen / zu Zeile |
| Strg+/ (deutsch: Strg+#) | Zeilen aus- und einkommentieren |
| Strg+S, Strg+W | speichern, Tab schließen |

Fehler und Warnungen erscheinen schon beim Tippen als Wellenlinien und unter
*Problems*; dort stehen auch die Fehler des letzten Builds für Dateien, die nicht
offen sind. Ändert Visual Studio eine offene, ungeänderte Datei, folgt der Tab.

`Debug.Log` und Ausnahmen stehen im Fenster *Inspect → Log* (mit `[C#]`).

**Debuggen:** In Visual Studio *Debug → An Prozess anhängen* → `sandbox.exe`,
Codetyp „Managed (.NET Core, .NET 5+)“. Haltepunkte in den Skripten greifen dann
im Play-Modus.

## 3. Skript-Aufbau

Eine Klasse, die von `Behaviour` erbt, in einer Datei gleichen Namens
(`Rotator.cs` → `class Rotator`). Der Engine ruft die Methoden auf, die sie
findet — Sichtbarkeit egal, wie in Unity:

```csharp
using Fitzel;
using System.Numerics;

public class Rotator : Behaviour
{
    public float Speed = 45f;          // erscheint auf der Script-Karte

    void Start()     { Debug.Log($"{Name} startet"); }   // einmal, vor dem ersten Update
    void Update()    { Rotation += new Vector3(0, Speed * Time.DeltaTime, 0); }
    void OnDestroy() { }               // Objekt entfernt, oder Play endet
}
```

An ein Objekt: Inspector → *Add Component → Script* → die `.cs` wählen. Ein
Objekt darf mehrere Skripte tragen, auch Lua und C# gemischt — die Dateiendung
entscheidet, welches Backend es ausführt.

**Felder auf der Script-Karte:** öffentliche Felder (und private mit
`[SerializeField]`) vom Typ `float`/`double`/`int`, `bool`, `string`, `Vector3`
und `Color`. `[HideInInspector]` blendet ein öffentliches Feld aus. Der Wert auf
der Karte überschreibt den Initialwert im Code, bevor `Start` läuft.

**Fehler:** Wirft ein Skript eine Ausnahme, hört **nur dieses Skript** auf (bis
zum nächsten Play); die Meldung steht an der Script-Karte, der Stacktrace mit
Datei und Zeile im Log.

## 4. API

C# kann **alles, was Lua kann**. Häufige Aufrufe (Transform, Eingabe, HUD-Formen,
Raycast, Kamera, Sound) gehen direkt in die Engine; alles Weitere läuft über
**dieselben C-Funktionen, die Lua aufruft** (`LuaApi`), verhält sich also genau
gleich. `using Fitzel;` und `using System.Numerics;` sind im Projekt gesetzt
(`Vector3` ist `System.Numerics.Vector3`). Eine Lua-Fehlermeldung (falsches
Argument) kommt als `InvalidOperationException` an.

### Lua → C#

| Lua | C# |
|---|---|
| `e.x/y/z`, `e.rx/ry/rz`, `e.sx/sy/sz` | `Position`, `Rotation`, `HalfExtents` (lokal) |
| `e.name`, `e.id`, `e.type`, `e.parent`, `e.active` | `Name`, `Entity.Id`, `Entity.Type`, `Entity.Parent`, `Entity.Active` |
| `game.keyDown/keyPressed(k)` | `Input.GetKey(Key.W)` / `Input.GetKeyDown(…)` |
| `game.mouseDown/mousePressed(b)` | `Input.GetMouseButton(…)` / `Input.GetMouseButtonDown(…)` |
| `game.mousePos()`, `mouseWheel()`, `mouseRay()`, `showCursor(b)` | `Input.MousePosition` / `MouseOverView`, `MouseWheel`, `MouseRay`, `ShowCursor(b)` |
| `game.textInput()`, `game.captureInput()` | `Input.TextInput()`, `Input.Capture()` |
| `game.cameraPos/Dir()`, `setCameraPos/Dir/Fov` | `Camera.Position`, `Camera.Direction`, `Camera.FieldOfView` |
| `game.setCamera(id)`, `setFocus(n, f)`, `screenSize()`, `orbitFrame(…)` | `Camera.Use(e)`, `Camera.SetFocus(n, f)` / `ResetFocus()`, `Camera.ScreenSize`, `Camera.OrbitFrame(…)` |
| `game.spawn{…}`, `spawnPrefab`, `clone`, `destroy` | `Entity.Spawn(new SpawnInfo{…})`, `Entity.SpawnPrefab(…)`, `e.Clone()`, `e.Destroy()` |
| `game.find`, `findAll`, `entities`, `entityInfo` | `Entity.Find`, `Entity.FindAll`, `Entity.All()`, `e.Info` |
| `game.getPos/setPos`, `getRot/setRot`, `getScale/setScale` | `e.WorldPosition`, `e.WorldRotation`, `e.HalfExtents` |
| `game.setLocal`, `setLocals` | `e.SetLocal(pos, rot)`, `Entity.SetLocals(liste)` |
| `game.getName/setName`, `setActive`, `isActive` | `e.Name`, `e.Active`, `e.ActiveInHierarchy` |
| `game.setParent`, `getParent`, `children` | `e.Parent`, `e.Children` |
| `game.toWorld(id, x, y, z)` | `e.ToWorld(lokal)` |
| `game.setVelocity`, `getVelocity`, `setAngularVelocity`, `applyImpulse` | `e.Velocity`, `e.AngularVelocity`, `e.AddImpulse(…)` |
| `game.castRay`, `game.raycast` | `Physics.Raycast(…)`, `Physics.PickBoxes(…)` |
| `game.moveCharacter`, `removeCharacter` | `e.MoveCharacter(vx, vz, dt)`, `e.RemoveCharacter()` |
| `game.spawnVehicle`, `driveVehicle`, `leaveVehicle`, `drivenVehicle` | `e.SpawnVehicle()`, `e.DriveVehicle()`, `Vehicle.Leave()`, `Vehicle.Driven` |
| `game.animTrigger/animBool/animNumber`, `animState` | `e.AnimTrigger`, `e.AnimSetBool`, `e.AnimSetNumber`, `e.AnimState` |
| `game.bones`, `bonePos`, `attach`, `detach`, `boneScale`, `rayFigure`, `reach` | `e.Bones`, `e.BonePose(b)`, `obj.AttachTo(figur, b, …)`, `obj.Detach()`, `e.BoneScale`, `e.RayFigure`, `e.Reach(seite, ziel, w)` |
| `game.emit`, `game.door`, `game.decal`, `game.shatter` | `e.Emit()`, `e.Door(offen?)`, `World.Decal(…)`, `World.Shatter(…)` |
| `game.collectibles`, `collectible` | `Entity.Collectibles()`, `e.Collectible` |
| `game.setLight`, `getLight` | `e.Light = new LightSettings{…}` / `e.Light` |
| `game.setMaterial`, `getMaterial`, `setColor` | `e.Material`, `e.SetColor(farbe)` |
| `game.materials`, `findMaterial`, `materialInfo`, `createMaterial`, `setMaterialProps` | `Materials.All()`, `.Find`, `.Info`, `.Create(new MaterialEdit{…})`, `.Set(ref, …)` |
| `game.assets`, `findAsset`, `assetInfo`, `assetPath`, `refreshAssets` | `Assets.List(typ)`, `.Find`, `.Info`, `.Path`, `.Refresh()` |
| `game.loadModel`, `modelInfo` | `Models.Load(ref)`, `Models.Info(id)` |
| `game.sound`, `playSound`, `playAudio`, `stopAudio` | `Audio.Play/PlayAt(…[, channel])`, `e.PlayAudio()`, `e.StopAudio()` |
| `game.addScore`, `getScore`, `setHud`, `setCrosshair` | `Game.AddScore(n)`, `Game.Score`, `Hud.Message`, `Hud.Crosshair` |
| `game.hudRect/Gradient/Frame/Line/Circle/Tri/Text` | `Hud.DrawRect/DrawGradient/DrawFrame/DrawLine/DrawCircle/DrawTriangle/DrawText` |
| `game.hudSize`, `hudTextSize`, `hudImage`, `imageSize`, `worldToHud` | `Hud.Size`, `Hud.TextSize`, `Hud.DrawImage`, `Hud.ImageSize`, `Hud.WorldToHud` |
| `game.terrainHeight`, `waterAt`, `groundHeight` | `World.TerrainHeight`, `World.WaterAt`, `World.GroundHeight` |
| `game.trees`, `clearTrees`, `fellTree`, `burnTree` | `World.Trees`, `.ClearTrees`, `.FellTree`, `.BurnTree` |
| `game.timeOfDay/setTimeOfDay`, `dayLength/setDayLength` | `World.TimeOfDay`, `World.DayLength`, `World.ReleaseDayLength()` |
| `game.streetLamps`, `setStreetLamps` | `World.StreetLampMode`, `World.StreetLampsLit` |
| `game.townPlaces`, `townPath`, `streetAt` | `Town.Places(art)`, `Town.Path(von, nach)`, `Town.StreetAt(p)` |
| `game.loadScene`, `restart`, `quit`, `rest` | `Game.LoadScene`, `Game.Restart()`, `Game.Quit()`, `Game.Rest(fps)` |
| `game.saveData`, `loadData` | `Game.SaveData(slot, wert)`, `Game.LoadData<T>(slot)` |
| `game.log(…)` | `Debug.Log(…)` |
| `synth.*` | `Synth.Play/Stop/NoteOn/NoteOff/Set/PlayMidi/StopMidi/IsPlaying/SetTempo/Note/LastError` |
| `music.*` | `Music.Load/Play/Stop/Pause/Resume/Fade/Time/Duration/IsPlaying/IsPaused/SetFilter/SetVolume/SampleRate/Spectrum/Waveform/Analyze` |
| `net.*` | `Net.Connect/Status/Send/Poll/Close/Serve/StopServer/ServerInfo/Addresses/Clock/PageHost` |
| `llm.*` | `Llm.Chat(anfrage)`, `Llm.Poll()`, `Pending`, `Clear()`, `SetHost`, `Model` |
| `json.encode/decode` | `System.Text.Json` (in .NET eingebaut) |
| `shared` | `Shared.Get<T>(key)` / `Shared.Set(key, wert)`: derselbe Topf wie in Lua; C#-Skripte untereinander nehmen einfach statische Felder |
| `require("modul")` | nicht nötig: alle `.cs` sind eine Assembly, Klassen sehen sich direkt |
| `game.KEY_*`, `MOUSE_*`, `BOX`…, `PHYSICS_*`, `ALPHA_*`, `LIGHT_*` | `Key`, `MouseButton`, `EntityType`, `PhysicsMode`, `AlphaMode`, `LightType` |

Was noch nicht typisiert ist (oder ein künftiges Lua-Feature), geht immer roh:

```csharp
var r = LuaApi.Call("game", "waterAt", x, z);       // wie game.waterAt(x, z)
if (!LuaApi.IsNil(r, 0)) depth = r[1].GetSingle();
```

Argumente gehen als Lua-Werte (Zahlen, Strings, Bools, Listen, Dictionaries oder
anonyme Objekte als Tabellen, `Vector3`/`Color` als `{x, y, z}`, `Entity` als ID),
jede Lua-Rückgabe kommt als ein `JsonElement`.

**Tasten festhalten** (`Input.Capture()`) gilt über beide Sprachen hinweg: Hält ein
C#-Menü die Tasten, sehen Lua- und andere C#-Skripte keine, und umgekehrt.

## 5. Export

*File → Export Game* packt **nur die übersetzte `Assembly-CSharp.dll`** ins Spiel
(keine Quellen, keine Solution), legt `dotnet/Fitzel.dll` neben die exe und meldet
im Status, dass der Zielrechner die .NET 10 Runtime braucht. Im **Web-Export**
laufen C#-Skripte nicht (nur Lua).

## 6. Gut zu wissen

- Die Brücke (`LuaApi`) kostet pro Aufruf ein paar Mikrosekunden (die Werte gehen
  als JSON hinüber). Für Dutzende Aufrufe pro Frame egal; wer Tausende Objekte
  bewegt, nimmt `Entity.SetLocals` (ein Aufruf für alle), wie in Lua.
- `Game.LoadData<T>` liest JSON: eigene Klassen mit öffentlichen Feldern oder
  Eigenschaften gehen direkt (`Game.LoadData<Spielstand>("slot1")`).
- `Debug.Log` und Ausnahmen stehen im Log-Fenster.

Prüfprogramm: `build/release/bin/csharpcheck.exe` legt ein Projekt an, baut es und
führt Skripte gegen echte Entities und eine echte Lua-VM aus: Grundlagen und eine
Stichprobe der ganzen API (Exit-Code 0 = alles in Ordnung).
