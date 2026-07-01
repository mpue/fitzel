# fitzel — Refactoring-Notizen

*Für den Tag, an dem der Kopf wieder frei ist. Kein Druck, das läuft nicht weg.*

---

## Ausgangslage (Stand Anschauen)

- **Engine (~2.900 LOC): Finger weg.** Rule-of-five über jede GL-besitzende Klasse
  lehrbuchkorrekt (`std::exchange`-Move, guarded Destruktor, copy deleted). Sauber
  gekapselt, modular. Das ist der stabile Teil — hier gibt's nichts zu sägen.
- **`sandbox/src/main.cpp`: 2.625 Zeilen, `main()` läuft von Zeile 206 bis 2625.**
  237 rohe `gl*`-Aufrufe. Jedes Szenen-Subsystem lebt als lokales Lambda + nackte
  Buffer-Handles im Rumpf.
- Handles von Hand balanciert (17 `glGen` / 17 `glDelete`). Leakt *nur* deshalb
  nicht, weil `main()` keinen Early-Return / keine Exception hat, an dem der Cleanup
  übersprungen würde. Fragil, aber aktuell korrekt.

**Wichtig:** Das ist eine Sandbox. Sie *darf* wuchern. Das hier ist Genussarbeit,
kein Feuerwehreinsatz. Die Engine/Sandbox-Trennung selbst ist die richtige
Architektur — es geht nur darum, gereifte Sandbox-Teile in die Engine zu heben.

---

## Die Kernidee: eine `InstancedField`-Abstraktion

Fünf Subsysteme teilen **dasselbe Muster**: instanziertes Base-Mesh + Instance-Buffer
+ Regenerier-Lambda beim Chunk-Wechsel. Die schreien nach *einer* gemeinsamen
Abstraktion, **nicht** nach fünf Einzelklassen.

| Subsystem   | ca. Zeile in main.cpp | Base-Mesh          |
|-------------|-----------------------|--------------------|
| Grass       | ~451                  | Blade              |
| Trees       | ~562                  | Trunk + 3 Cones    |
| Flowers     | ~819                  | Bloom-Quad         |
| Birds       | ~926                  | Flapping Billboard |
| Fireflies   | ~972                  | Point (gl_VertexID)|

Gemeinsames Interface, grob:

```
class InstancedField {
    // Base-Mesh (VAO/VBO) — via bestehende Mesh-RAII, nicht roh
    // Instance-VBO — gehört in einen RAII-Wrapper, kein nacktes glGen
    // regenerate(chunkCoord, terrainQuery, ...) -> füllt Instance-Daten
    // draw(view, proj, shader) -> ein Draw-Call, instanziert
};
```

Damit fallen die rohen `glGenBuffers`/`glGenVertexArrays` für diese fünf weg und
landen hinter der gleichen sauberen RAII wie der Rest der Engine.

---

## Was sonst noch in `main()` klebt (niedrigere Priorität)

Diese sind nicht das Instancing-Muster, aber auch Kandidaten fürs Auslagern:

- **Roads / Paths** (~668) — inkl. `roadPickTerrain`, `buildRoad`
- **Testauto** (~776) — `placeCar`
- **Audio-Setup** (~1049) — wetterabhängige Layer
- **Kamera-Pfad** (~35, `CamKey` / Catmull-Rom) — Recorder/Player, könnte ein
  eigenes `CameraPath`-Utility werden
- **Preset-System** (~1147) — `addF`/`addB`/`addI`, save/load/delete

Die sind eigenständiger und weniger dringend als die fünf Vegetation-Systeme.

---

## Der Name-vs-Inhalt-Punkt

README nennt es **"vegetation rendering engine"** — aber die Vegetation ist der eine
Teil, der **nicht** in der Engine ist, sondern in der Sandbox-`main()` versackt.
Für den Privatgebrauch wurscht. Sobald es das Instancing-Refactoring gibt, hält das
Repo aber endlich, was der Name verspricht: dann *ist* die Vegetation Teil der Engine.

---

## Nicht vergessen

- Kein `glGetError` / KHR_debug-Callback im Frame. Bei GL 3.3 Core hobby-mäßig ok,
  aber falls beim Refactoring was schwarz wird: ein Debug-Callback spart Nerven.
- Nach dem Rausziehen: mal unter ASan/TSan halten. Die Instance-Buffer-Uploads über
  Worker-Threads (Terrain macht das schon) sind der wahrscheinlichste Ort für eine
  Race, wenn die Vegetation denselben Weg geht.

---

*Bis dahin steht ein Renderer, der Sonnenuntergänge macht. Das reicht.*
