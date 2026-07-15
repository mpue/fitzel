# fitzel — Refactoring-Notizen

*Stand: 15.07.2026. Vorherige Fassung: 09.07.2026 — die beschrieb eine `main.cpp` mit
2.625 Zeilen. Die gibt es nicht mehr, siehe unten.*

---

## Ausgangslage

- **Engine (~2.900 LOC): Finger weg.** Rule-of-five über jede GL-besitzende Klasse
  lehrbuchkorrekt (`std::exchange`-Move, guarded Destruktor, copy deleted). Sauber
  gekapselt, modular. Das ist der stabile Teil — hier gibt's nichts zu sägen.
  (Unverändert gültig.)
- **`sandbox/src/main.cpp`: 7.668 Zeilen** — 57 % des gesamten Sandbox-Codes (13.352).
  Der Loop läuft von 2672 bis 7656, also **4.985 Zeilen in einer Schleife**.
- **Zwei Build-Targets aus derselben Datei:** `sandbox` (Editor) und `player`
  (`FITZEL_PLAYER`, das exportierte Spiel). Das ist die wichtigste Struktureigenschaft
  der Datei und stand bisher nirgends.

### Zahlen, die sich seit der letzten Fassung bewegt haben

| | 09.07. | 15.07. (Bestandsaufnahme) | 15.07. (nach Schritt 1–3) |
|---|---|---|---|
| `main.cpp` | 2.625 | 7.668 | **7.507** |
| rohe `gl*`-Aufrufe | 237 | 94 | **44** |
| `glGen` / `glDelete` von Hand balanciert | 17 / 17 | 4 / 4 | **0 / 0** |

**Die Fragilitätsklage ist geschlossen.** „Leakt *nur* deshalb nicht, weil `main()`
keinen Early-Return hat" gilt nicht mehr: Es gibt keine rohen Handles in `main.cpp`
mehr, die letzten vier (`rainVAO`/`rainVBO`, `sprayVAO`/`sprayVBO`) liegen jetzt
hinter RAII in `RainRenderer` und `SpraySystem`. Prüfbar mit
`grep -c glGen sandbox/src/main.cpp` → `0`. **Diese Null bitte nicht wieder aufgeben.**

**Aber die Datei ist fast dreimal so groß wie beim letzten Anschauen.** Der alte Satz
„Das ist eine Sandbox. Sie *darf* wuchern, kein Feuerwehreinsatz" hat sich damit still
überholt. Die Engine/Sandbox-Trennung ist weiterhin die richtige Architektur — aber
`main.cpp` ist kein Wuchern mehr, sondern der Ort, an dem der halbe Editor wohnt.

---

## Die Kernidee von damals: **erledigt**

Die vorgeschlagene `InstancedField`-Abstraktion für Gras/Bäume/Blumen/Vögel/Glühwürmchen
ist gebaut und ausgeliefert — als `VegetationSystem` (1.222 Zeilen), in vier Commits:

    548924c  Extract grass into VegetationSystem
    97b2c90  Extract trees into VegetationSystem
    915a5a5  Extract flowers into VegetationSystem -- meadow split complete
    13a311d  Extract birds + fireflies into VegetationSystem

**Mit einer Einschränkung, die den „Name-vs-Inhalt"-Punkt nur halb einlöst:**
`VegetationSystem` liegt in `sandbox/src/`, **nicht** in der Engine. Die Vegetation hat
`main()` verlassen, aber die Engine nie erreicht. Das README verspricht weiterhin eine
„vegetation rendering engine", in der die Vegetation nicht liegt. Der Umzug
`sandbox/src/VegetationSystem.*` → `engine/` ist ein eigener, kleiner Schritt — und
erst er löst den Punkt ein.

Ebenfalls erledigt aus der alten Liste: **Kamera-Pfad** (`CameraPath.cpp`),
**Audio-Setup** (`CarAudio.cpp`), **Roads/Paths** teilweise (`RoadSystem.cpp` 400 Zeilen
+ `RoadBridge.cpp` 307 — aber Panel und Serialisierung kleben weiter in `main.cpp`).
Noch offen: **Testauto** (`placeCar`), **Preset-System** (`addF`/`addB`/`addI`).

---

## Wo die Zeilen heute wirklich liegen

Der Loop (2672–7656) zerfällt in vier Teile. Die Aufteilung ist der eigentliche Befund:

| Teil | Zeilen | Anteil |
|---|---|---|
| **Editor-UI** — 3855–6809, **ein einziger `#ifndef FITZEL_PLAYER`-Block** | **2.955** | 59 % |
| Eingabe/Interaktion — Shortcuts, Physik-Auto (201), Arcade-Auto (96), FPS (116), Freikamera (57) | ~1.150 | 23 % |
| Simulation — Wetter (71), Tag/Nacht (52), Physikschritt (70), Skripte+Verhalten (322) | ~700 | 14 % |
| **Rendering** — 6847–7542, plus Rain/Spray-Deklarationen 538–610 | ~850 | 17 % |

Zwei Dinge folgen daraus, die man leicht falsch herum angeht:

**1. Die Editor-UI ist der große Hebel — und sie ist bereits vorgeschnitten.**
2.955 Zeilen, ein zusammenhängender Präprozessor-Block. Das Repo hat ein erprobtes
Muster dafür: freie Funktionen in einem `xxxui::`-Namespace über ein `PanelState`-Struct
aus Referenzen (`terrainui::`, `scatterui::`, `paintui::`, `sculptui::`, `vehicleui::`,
`roadbridge::` — sechsmal im Einsatz). Keine GL-Reihenfolgefallen; ein Fehler zeigt sich
als fehlendes Panel, nicht als falsches Pixel. `main.cpp` → ~4.750.

**2. Der Render-Block ist klein, aber er ist der Code, der ausgeliefert wird.**
Die 2.955 Panel-Zeilen sind aus `player.exe` herausgeschnitten. Der Render-Loop ist die
geteilte Laufzeit — der Code, der Struktur am dringendsten verdient und am ehesten in
die Engine graduiert. Nur ist er nicht die Antwort auf „`main.cpp` ist zu groß" (−11 %).

---

## Warum „eine abstrakte Renderer-Basis, von der alles erbt" nicht aufgeht

Eine Basisklasse kauft genau eins: über `vector<unique_ptr<Base>>` iterieren zu können.
**Die Reihenfolge im Frame verbietet Iteration.** Sie lautet: Terrain → Straße → Skids →
Autos → Entities → Env-Probe → Reflexion (*nur eine Teilmenge*, mit `glCullFace`-Flip) →
Refraktion → Hauptpass → Gras → Wasser → Regen → Spray → Glühwürmchen → SSAO → Composite
→ FXAA. Jeder Übergang hat eine Bedingung, die im Code als Kommentar steht (Env-Probe vor
den lit-Pässen; Reflexion/Refraktion linear, damit Wasser einmal tonemappt; SSAO braucht
die HDR-Tiefe). Das wird nie `for (auto& r : renderers) r->draw()`.

Eine Basis wäre also eine **Namenskonvention, kein Dispatch-Mechanismus.** Ehrlich
angewandt bleiben drei Erben:

| | erbt? | warum nicht |
|---|---|---|
| Rain, Spray, Water | **ja** | gleiche Form: eigener Shader, kein Queue, kein Reflexionspass |
| Road, Skids, Entities, Autos | nein | `fitzel::Renderer::submit()` *ist* bereits ihre Abstraktion |
| Sky | nein | 4×/Frame mit anderer View + Tonemap-Flag |
| Vegetation | nein | 8 Einsprungpunkte, 1.222 Zeilen |
| SSAO/Composite/FXAA | nein | geordnete Pipeline über Rendertargets, keine Szenenschicht |

Und: **von Code, der noch keine Objekte sind, kann nichts erben.** Wasser, Regen, Spray,
Sky, SSAO, Composite, FXAA sind heute Inline-GL gegen `main()`s Locals. Erst Klassen,
dann — vielleicht — eine Basis. Ihre richtige Form ist dann offensichtlich statt geraten.

---

## Reihenfolge

Jeder Schritt steht für sich; nach jedem kann Schluss sein.

1. ~~**`FrameContext`**~~ — **erledigt.** `sandbox/src/FrameRender.hpp`: `FrameContext`
   (aus `VegDrawContext` hochgezogen) + `makeFrameContext()`. Ersetzt die zwei
   handgebauten Kopien pro Frame. `sandbox_core` **zurückgestellt**: von den
   Runtime-Quellen hängt nur `RoadBridge.cpp` neben `main.cpp` an `FITZEL_PLAYER`, eine
   gemeinsame Bibliothek bräuchte also erst einen Panel-Split — und die Prüfstände
   kompilieren die `.cpp` bisher problemlos direkt mit. Wenn das beim dritten Mal nervt,
   hat sich die Bibliothek verdient.
2. ~~**`RainRenderer` + `SpraySystem`**~~ — **erledigt.** `SpraySystem` ist in `SprayPool`
   (reines CPU, prüfstandfähig) + GL-Besitzer geteilt; Emission bleibt beim Fahrzeug,
   wo sie hingehört. `glGen` in `main.cpp` = 0.
3. ~~**`RendererBase`**~~ — **erledigt**, in `FrameRender.hpp`. Eine pure virtual
   `draw(const FrameContext&)`; `RainRenderer` und `SpraySystem` erben. Name `*Base` nach
   `ComponentBase`; eine Sandbox-Klasse namens `Renderer` scheidet aus, weil `main.cpp`
   `fitzel::Renderer` unqualifiziert benutzt. **Wie vorhergesagt ist es ein Vertrag, kein
   Dispatch:** es gibt keine Liste zum Iterieren, der Loop ruft beide beim Namen. Was es
   bringt, ist `override` gegen abdriftende Signaturen und ein benannter Typ für
   „zeichnet sich selbst".
4. **Hier stehen wir.** Basisklasse und Panel-Block liegen gemeinsam auf dem Tisch.
5. Später: `WaterRenderer`, `SkyRenderer`, `PostChain` (~240 Zeilen). Vorbereitet, keine
   Vorbedingung. Der größere Hebel bleibt der Panel-Block (2.955 Zeilen).

**Kein `Pass`-Enum.** Der Reflexionspass zieht als einzigen Nicht-Queue-Teilnehmer
`veg.drawTrees(rctx)` — ein Enum für eine Teilmenge mit einem Mitglied. Zwei explizite
Aufrufe reichen. Erst wieder anfassen, wenn ≥2 Schichten in ≥2 Pässen aus einer
dynamischen Liste müssen.

---

## Wie geprüft wird

- **Beide Targets bauen** (`build-release.bat`). **`player` ist der Kanarienvogel:** zieht
  sich ein Renderer versehentlich eine Editor-Abhängigkeit, scheitert `player` am Linken.
  Kostenlose Architekturdurchsetzung — bewusst nutzen.
- **`grep -c "glGen\|glDelete" sandbox/src/main.cpp`** → muss bei Schritt 2 auf 0 und dort bleiben.
- **`wc -l sandbox/src/main.cpp`** vor jedem „fertig". Genau das nicht getan zu haben,
  ist der Grund für die Zahl oben.
- **Prüfstände** gegen `sandbox_core` für alles ohne GL (`makeFrameContext`, `SprayPool`).
  Genau dafür ist der Pool/GL-Schnitt da.
- **Optisch A/B**, da jeder Schritt ein reiner Umzug ist: Tag/Nacht anhalten, Wetterdrift
  aus, Kamera über den `CameraPathRecorder` reproduzierbar fahren, Screenshot vorher/nachher.

---

## Nicht vergessen

- Kein `glGetError` / KHR_debug-Callback im Frame. Bei GL 3.3 Core hobby-mäßig ok, aber
  falls beim Refactoring was schwarz wird: ein Debug-Callback spart Nerven. (Unverändert.)
- **`Renderer::end()` (`Renderer.hpp:130`) ist tote API** — wird nirgends aufgerufen,
  `main()` treibt `prepareShadows` + `renderScene` von Hand. Entweder benutzen oder löschen.
- **`sandbox/src/VoxelSystem.{hpp,cpp}` (181 Zeilen) ist tot** — nicht in `CMakeLists.txt`,
  nirgends referenziert, wird nie kompiliert (WIP aus `4f0168d`). Löschen oder wieder
  einhängen; unkompiliert liegen lassen ist die einzige falsche Antwort, weil nicht
  kompilierter Code lautlos verrottet.
- Nach dem Rausziehen: mal unter ASan/TSan halten. (Unverändert.)

---

*Bis dahin steht ein Renderer, der Sonnenuntergänge macht. Das reicht.*
