# Engine-Audit fitzel – Oktober 2026

**Umfang:** `engine/` (≈ 19 300 Zeilen C++20 in 9 Subsystemen), Stand `master` @ `c1c83d5` plus uncommittete Änderungen an `Physics.cpp/.hpp`.
**Methode:** Statische Durchsicht aller Engine-Quellen in sechs parallelen Teil-Reviews. Das Verhalten der Drittbibliotheken (cgltf 1.14, miniaudio 0.11.21, Jolt, stb_image, ImGui) wurde in ihren Quellen unter `build/linux/_deps` gegengeprüft. Es wurde nichts gebaut und nichts geändert.
**Verifikation:** Jedes Finding verweist auf konkrete Codezeilen. Die mit ✔ markierten Findings habe ich zusätzlich selbst am Code nachgeprüft. Mit „plausibel“ markierte Punkte sind am Code belegt, ihre Wirkung zur Laufzeit aber nicht nachgewiesen.

Schweregrade: **K** = Kritisch · **H** = Hoch · **M** = Mittel · **N** = Niedrig · **I** = Info

---

## 0. Umsetzungsstand (16. Oktober 2026)

Abgearbeitet auf dem Branch `fitzel-audit-rework`, nach `master` gemergt (Commits `c7f7287` bis `0fbf735`). Behobene Findings sind in den Tabellen unten mit **behoben** markiert, teilweise behobene mit **teilweise**. Zu fast jedem Fix gibt es einen Regressionstest in `sandbox/tools/*check`.

| Schwere | gesamt | behoben | teilweise | offen |
|---|---|---|---|---|
| K | 3 | 3 | – | – |
| H | 16 | 16 | – | – |
| M | 47 | 35 | 4 | 8 |
| N | 76 | 2 (GFX-10, TER-09, nebenbei) | – | 74 |
| I | 18 | – | – | 18 |

**Teilweise behoben:**
- GFX-04: Der Prefilter wählt die Mip-Stufe über das PDF, kopiert bei Roughness 0 und braucht 256 statt 1024 Samples. Die Irradiance liest Mip 4 mit dem halben Raster. Offen sind SH9 und ein asynchroner Bake.
- AST-04: Ein Index, den die Einträge nicht genau ausfüllen, wird abgelehnt. Damit wird ein falscher Schlüssel praktisch immer erkannt. Offen sind ein verschlüsseltes Magic und ein MAC; beides ist eine Formatänderung.
- AST-09: Kanonische Pfade werden pro Scan gecacht, Löschungen in einem Durchgang erledigt. Offen sind inotify bzw. `ReadDirectoryChangesW`.
- AUD-P1: Keine Neuberechnung bei unveränderter Frequenz, `pow` nur bei neuer Note. Offen sind ein Phasen-Rotator und die Auflösung pro Block.

**Offen, weil sie Aussehen, Verhalten oder Format ändern (Entscheidung ausstehend):**
- GFX-08: sRGB-Texturen
- REN-02: Sortierung nach Zustand
- MOD-08 und MOD-17: Animation auf Nicht-Joint-Nodes, Rig ohne Clips
- MOD-16: gemeinsames Material-Mapping für assimp und glTF
- MOD-25: indizierte Modelle
- AUD-10: Hall mit Raumgröße und Stereo
- PHY-06: fester Physik-Zeitschritt

**Messwert REN-01:** In der Szene `roads` (3440×1440, RTX 3080) fiel die Frame-Zeit von 24,3 auf 16,2 ms (41 → 62 fps). Die CPU-Submit-Zeit der Szene fiel von 19,5 auf 4,4 ms.

---

## 1. Zusammenfassung

Die Engine ist insgesamt sauber, kompakt und außergewöhnlich gut kommentiert. Fast jede nicht offensichtliche Entscheidung ist begründet. Es gibt keine TODO- oder FIXME-Reste, kaum rohes `new`/`delete`, und die RAII-Wrapper in `graphics/` sind korrekt umgesetzt (Kopie gelöscht, Move per `std::exchange`). Im Audio-Teil sind die Echtzeit-Regeln erkennbar beachtet.

Die Probleme häufen sich an **fünf wiederkehrenden Stellen**:

1. **Untrusted Eingaben werden nicht validiert.** Betroffen sind glTF (kein `cgltf_validate`), Pak-Header, Video-/EXR-Größen, Terrain-Settings, `.meta`- und `.patch`-JSON sowie Audio-Parameter (NaN). Die Folgen reichen von Out-of-bounds-Reads über `std::terminate` bis zu Endlosschleifen.
2. **Lebensdauer an API-Grenzen.** Betroffen sind Physics-`removeBody` und Fahrzeug-Constraints, der ImGui-Kontext und Shadow-Pool, die `Input`-Registrierung bei GLFW, `Renderer` ohne Destruktor und Audio-Stimmen gegenüber `Audio`.
3. **Globaler, versteckter Zustand.** Dazu gehören das Flip-Flag von stb_image (thread-local und global gemischt), GL-State-Lecks (FBO 0, Viewport, aktive Texture-Unit, Clear-Farbe) und die unsynchronisierten VFS-Globals.
4. **Thread-Synchronisation in Randfällen.** Beispiele sind der Lost-Wakeup im `TerrainStreamer`-Destruktor, das Null-Fenster in `Mixer::setInserts`, `SynthVoice::play` gegen den Audio-Thread und die Note-Queue, die nur einen Produzenten verträgt.
5. **CPU-Kosten im Render-Hot-Path.** Pro Draw und Pass werden etwa 200 Einzel-Uniforms hochgeladen, jeder Upload mit eigenem `glUseProgram`. Es gibt keine Sortierung nach Zustand und keine UBOs.

Im heutigen Einsatz (ein Fenster, eine GUI, feste Init-Reihenfolge in `sandbox/main.cpp`, vertrauenswürdige Assets) bleiben viele Punkte latent. Für eine Engine, die gepackte Spiele ausliefert, Nutzer-Assets importiert und von Tools mehrfach instanziiert wird, sind die Punkte aus Abschnitt 2 jedoch reale Absturz- und Sicherheitspfade.

### Verteilung

| Subsystem | K | H | M | N | I | Σ |
|---|---|---|---|---|---|---|
| Core / UI / Scene | – | – | 3 | 16 | 5 | 24 |
| Graphics | – | 1 | 7 | 13 | 3 | 24 |
| Renderer | – | 1 | 5 | 7 | 1 | 14 |
| Terrain | – | 2 | 3 | 5 | 2 | 12 |
| Model (glTF/assimp) | 1 | 4 | 12 | 9 | 2 | 28 |
| Voxel | – | – | – | 2 | 1 | 3 |
| Audio | 1 | 3 | 8 | 15 | 3 | 30 |
| Asset / Pak / VFS | – | 3 | 5 | 7 | – | 15 |
| Physics | 1 | 2 | 4 | 2 | 1 | 10 |
| **Summe** | **3** | **16** | **47** | **76** | **18** | **160** |

---

## 2. Top-Prioritäten (zuerst beheben)

| # | ID | Schwere | Befund | Ort |
|---|---|---|---|---|
| 1 | PHY-01 ✔ · **behoben** | K | `removeBody` ruft `DestroyBody` auch für unbekannte oder schon entfernte IDs auf. Jolt löscht dann einen Müllzeiger oder einen fremden Body. | [Physics.cpp:748](../engine/src/physics/Physics.cpp:748) |
| 2 | AUD-01 ✔ · **behoben** | K | `Mixer::setInserts` lässt `nullptr`-Effekte in `s->fx`, während der Lock freigegeben ist. Der Audio-Callback dereferenziert sie dann. | [Mixer.cpp:580](../engine/src/audio/Mixer.cpp:580) |
| 3 | MOD-01 ✔ · **behoben** | K | Kein `cgltf_validate`. Präparierte glTF-Dateien erzeugen Heap-Out-of-bounds-Reads (Indizes, Accessoren, BufferViews) sowie Endlosschleifen oder Stack-Overflows bei Zyklen. | [Model.cpp:743](../engine/src/world/Model.cpp:743) |
| 4 | MOD-02 ✔ · **behoben** | H | Der VFS-Read-Hook `cgltfRead` ignoriert die angeforderte Größe. Ist die `.bin`-Datei kürzer, meldet er trotzdem Erfolg, und es folgt ein OOB-Read. | [Model.cpp:118](../engine/src/world/Model.cpp:118) |
| 5 | AST-01/02 ✔ · **behoben** | H | Pak-Header: `count`, Index sowie Offset und Größe der Einträge werden nicht gegen die Dateigröße geprüft. Eine Datei von 40 Byte reicht für `bad_alloc` und `terminate`. | [Pak.cpp:75](../engine/src/asset/Pak.cpp:75) |
| 6 | AUD-02/03/04 ✔ · **behoben** | H | Delay-Lines lesen `m_buffer[n]` (float-Rundung). Der Fold-Drive terminiert ab etwa \|x\| ≥ 1e8 bzw. bei Inf nicht. NaN-Parameter führen über einen `int`-Cast zum Segfault. | [SynthDsp.cpp:275](../engine/src/audio/SynthDsp.cpp:275), [SynthDsp.cpp:315](../engine/src/audio/SynthDsp.cpp:315) |
| 7 | PHY-02/03 · **behoben** | H | Gescheiterte Creates liefern `0xFFFFFFFF` statt 0. Fahrzeug-Constraint und StepListener überleben ihren Body (Use-after-free). | [Physics.cpp:236](../engine/src/physics/Physics.cpp:236), [Physics.cpp:469](../engine/src/physics/Physics.cpp:469) |
| 8 | GFX-01 ✔ · **behoben** | H | `decodeThumbnail` setzt auf dem Render-Thread dauerhaft den thread-lokalen stb-Flip. Danach laden geflippte Texturen und HDR-Panoramen kopfüber. | [Texture.cpp:307](../engine/src/graphics/Texture.cpp:307) |
| 9 | TER-01 ✔ · **behoben** | H | Die Erosion in `rain()` klemmt mit `min(h[i], …)`. Gelände unter y = 0 wird auf 0 angehoben, das Sediment wird negativ. | [Terrain.cpp:561](../engine/src/world/Terrain.cpp:561) |
| 10 | TER-03 ✔ · **behoben** | M | Lost Wakeup im Destruktor von `TerrainStreamer`: Das Stop-Flag wird ohne Mutex gesetzt, `join()` kann hängen. | [Terrain.cpp:892](../engine/src/world/Terrain.cpp:892) |
| 11 | TER-02 · **behoben** | H | Terrain-Settings aus Szenendateien werden nicht validiert. Das führt zu NaN, `length_error` auf dem Worker-Thread und `std::terminate`. | [Terrain.cpp:746](../engine/src/world/Terrain.cpp:746) |
| 12 | AST-03 ✔ · **behoben** | H | Der Pak-Schlüssel liegt im Repo, und der dokumentierte Override `FITZEL_PAK_KEY_HEX` ist in keiner CMake-Datei verdrahtet. | [PakKey.hpp:18](../engine/src/asset/PakKey.hpp:18) |
| 13 | REN-01 / GFX-07 ✔ · **behoben** | H | Etwa 200 Uniform-Uploads pro Draw, jeder Setter mit eigenem `glUseProgram`. Das ist der größte Performance-Hebel. | [Renderer.cpp:953](../engine/src/render/Renderer.cpp:953), [Shader.cpp:303](../engine/src/graphics/Shader.cpp:303) |
| 14 | MOD-05 · **behoben** | H | Die glTF-Interpolation (STEP/CUBICSPLINE) wird ignoriert. CUBICSPLINE liest Tangenten als Werte. | [Model.cpp:809](../engine/src/world/Model.cpp:809) |
| 15 | CORE-13/14 · **behoben** | M | ImGui: Der globale Shadow-Pool überlebt den Kontext (Use-after-free beim zweiten `Gui`), und der Kontext wird nicht pro Instanz gehalten. | [Gui.cpp:396](../engine/src/ui/Gui.cpp:396) |

---

## 3. Übergreifende Themen und empfohlene Maßnahmen

### 3.1 Eingabevalidierung als Engine-Prinzip
- **Asset-Loader:** `cgltf_validate` sowie Größen- und Count-Limits vor jeder Allokation. Bildgrößen vor dem Decode per `stbi_info_from_memory` begrenzen oder `STBI_MAX_DIMENSIONS` setzen. Für assimp `aiProcess_ValidateDataStructure` aktivieren.
- **Container:** Den Pak-Header gegen die Dateigröße prüfen, in überlauffreier Form (`offset <= end - size`).
- **Daten aus JSON (Szene, `.meta`, `.patch`):** Eine zentrale `sanitized()`-Funktion je Settings-Struct in der Engine, nicht nur UI-Clamps in der Sandbox. nlohmann-Zugriffe mit Typprüfung oder in try/catch.
- **Numerische Setter (Audio, Kamera, Physics):** `std::isfinite` vor `std::clamp` prüfen, denn `std::clamp` lässt NaN durch.

### 3.2 Lebensdauer und Ownership
- Ein einheitliches Muster für Handles: `0` ist ungültig, IDs werden zentral konvertiert, Removal ist idempotent (PHY-01/02/03).
- Klassen mit rohen GL-Namen oder Callback-Registrierung (`Renderer`, `Input`, `Gui`) bekommen einen expliziten Destruktor und gelöschte oder korrekte Moves.
- Lebensdauer-Abhängigkeiten (Audio-Stimmen gegenüber `Audio`, GL-Objekte gegenüber dem Kontext) im Header dokumentieren oder per Token absichern.

### 3.3 Globaler Zustand
- **stb_image:** Nie über stb flippen, sondern nach dem Decode selbst, wie es der EXR-Pfad schon tut. Alternativ ausschließlich die `_thread`-Variante verwenden (GFX-01, MOD-09, CORE-23).
- **GL-State-Konvention:** Create- und Update-Funktionen sichern die Bindings und stellen sie wieder her. `begin()`/`end()` merken sich den vorherigen FBO und Viewport. Ein gemeinsamer `resetPixelStore()`-Helper (GFX-15/17, REN-05).
- **VFS:** Mount/Unmount nur vor dem Start von Threads, oder per `atomic<shared_ptr>` (AST-12).

### 3.4 Render-Performance
1. Frame- und Pass-Konstanten (Licht, Fog, Kaskaden, Point/Spot/Shadow, IBL) in std140-UBOs legen.
2. Das gebundene Programm cachen und `glUseProgram` aus den Settern entfernen.
3. Die opake Liste nach Programm, Material und VAO sortieren.
4. Die Welt-AABB einmal pro Submit berechnen und die Listen als Member wiederverwenden.
5. Gleiche Terrain-Indexpuffer pro `(n, stride)` teilen.
6. Indizierte Geometrie für Modelle und CPU-Skinning verwenden (heute etwa Faktor 6 mehr Vertices).

### 3.5 Konsistenz zwischen Header und Code
Mehrere Header versprechen etwas anderes, als der Code tut, z. B. `removeBody` „safe“, „Returns 0 on failure“, „looped over the clip duration“, „radial dead-zone“, „Notes (any thread)“, „row 0 = up“, „OpenGL 3.3“ und die Format-Liste in `AssetTypes`. Bei jedem Fix den Kommentar mitziehen. In `docs/invariants.md` lohnt sich ein Abschnitt zu den Thread-Anforderungen pro API.

---

## 4. Findings nach Subsystem

### 4.1 Core / UI / Scene

| ID | S | Kat. | Ort | Befund | Empfehlung |
|---|---|---|---|---|---|
| CORE-01 | N | Bug | [Window.cpp:220](../engine/src/core/Window.cpp:220) | Move übernimmt nur `m_handle`, nicht `m_fullscreen` und die gemerkte Fenstergröße. Ein verschobenes Vollbild-Fenster kann nicht mehr zurückschalten. | Alle Member per `std::exchange` übernehmen |
| CORE-02 | N | Inkons. | [Window.cpp:306](../engine/src/core/Window.cpp:306) | `swapBuffers`, `framebufferSize` und `aspectRatio` prüfen `m_handle` nicht, andere Methoden schon. | Einheitlicher Null-Guard |
| CORE-03 | N | Bug | [Window.cpp:139](../engine/src/core/Window.cpp:139) | GLFW-Hints sind prozessweit. Ein zweites Fenster erbt die `appId` des ersten. | `glfwDefaultWindowHints()` am Anfang |
| CORE-04 | N | Inkons. | [Window.hpp:28](../engine/include/fitzel/core/Window.hpp:28) | Der Kommentar nennt „OpenGL 3.3“, angefordert wird 4.3 mit 3.3 als Fallback. | Kommentar korrigieren |
| CORE-05 · **behoben** | M | Bug (Win) | [LogCapture.cpp:179](../engine/src/core/LogCapture.cpp:179) | `_IOLBF` bedeutet in der MSVC-CRT Vollpufferung. Logs erscheinen erst nach 4 KB. | Unter `_WIN32` `_IONBF` oder `fflush` pro Frame |
| CORE-06 | N | Ressource | [LogCapture.cpp:122](../engine/src/core/LogCapture.cpp:122) | `partial` wächst ohne `'\n'` unbegrenzt (z. B. bei `\r`-Fortschritt). | `\r` als Zeilenende werten, Längenlimit |
| CORE-07 | N | Race/Leak | [LogCapture.cpp:193](../engine/src/core/LogCapture.cpp:193) | `saved`-fd wird nie geschlossen. Ein erneutes `start()` bei laufendem Reader erzeugt einen Race. Kein `O_CLOEXEC`, Kindprozesse erben die Pipes. | `pipe2(O_CLOEXEC)`, Restart verweigern |
| CORE-08 | I | Inkons. | [LogCapture.cpp:81](../engine/src/core/LogCapture.cpp:81) | `stripAnsi` entfernt nur CSI, keine OSC-Sequenzen. Unter Windows umgehen Win32-Handles den Capture (plausibel). | OSC/ESC-x ergänzen |
| CORE-09 | N | Inkons. | [GlCaps.cpp:11](../engine/src/core/GlCaps.cpp:11) | Liest die globale Variable `GLAD_GL_VERSION_4_3` statt des aktuellen Kontexts. Widerspricht dem Header. | Aus `GL_MAJOR/MINOR_VERSION` ableiten |
| CORE-10 | N | Lifetime | [Input.cpp:108](../engine/src/core/Input.cpp:108) | `glfwSetWindowUserPointer(this)` ohne Abmeldung, implizit kopierbar. Ist der Scroll-Callback nach `~Input` noch aktiv, folgt ein Use-after-free. | Copy/Move löschen, Destruktor deregistriert |
| CORE-11 | N | Inkons. | [Input.cpp:162](../engine/src/core/Input.cpp:162) | Die „radiale“ Deadzone ist axial. Bei `deadzone >= 1` folgt eine Division durch 0. | 2D-Variante, Deadzone klemmen |
| CORE-12 | I | Inkons. | [Input.cpp:171](../engine/src/core/Input.cpp:171) | Eine ungültige Trigger-Achse liefert 0.5 („halb gedrückt“). | Bereich prüfen |
| CORE-13 · **behoben** | M | UAF | [Gui.cpp:396](../engine/src/ui/Gui.cpp:396) | Der globale `g_shadowPool` hält `ImDrawList`s mit `_Data` des zerstörten Kontexts. Ein zweites `Gui` greift auf freigegebenen Speicher zu. Die Listen werden nie freigegeben. | Pool als Member, im Destruktor freigeben |
| CORE-14 · **behoben** | M | Bug | [Gui.cpp:244](../engine/src/ui/Gui.cpp:244) | Der `ImGuiContext*` wird nicht gespeichert. Destruktor und Move-Zuweisung zerstören den *aktuellen* Kontext. `beginFrame` auf einem moved-from-Objekt ruft ImGui ohne Kontext auf. | `m_ctx` halten, `SetCurrentContext`, Guards |
| CORE-15 | N | Perf | [Gui.cpp:449](../engine/src/ui/Gui.cpp:449) | `CmdLists = out` verursacht free und malloc pro Frame. Lineare Fenstersuche pro DrawList. `CmdListsCount` bleibt veraltet. | `swap`, Map, Count nachführen |
| CORE-16 | N | Portab. | [Gui.cpp:178](../engine/src/ui/Gui.cpp:178) | Die Font-Pfade sind Debian-spezifisch. Auf Fedora und Arch gibt es keine Fonts, `boldFont`/`monoFont` sind dann null. | fontconfig oder mehrere Suchwurzeln |
| CORE-17 | N | Fehlerbeh. | [Gui.cpp:287](../engine/src/ui/Gui.cpp:287) | Die Rückgaben der ImGui-Backend-Inits werden ignoriert. | Prüfen und werfen |
| CORE-18 | I | Wartb. | [Gui.cpp:260](../engine/src/ui/Gui.cpp:260) | Die DPI-Skala wird nur einmal bestimmt. Es gibt keinen Content-Scale-Callback. | Callback registrieren |
| CORE-19 | N | Bug | [Camera.hpp:78](../engine/include/fitzel/scene/Camera.hpp:78) | `setBasis` schreibt `m_front`, bevor der Parallel-Check fehlschlägt. Die Basis ist dann nicht mehr orthonormal. | Erst lokal berechnen |
| CORE-20 | N | Wartb. | [Camera.hpp:95](../engine/include/fitzel/scene/Camera.hpp:95) | Ein `const`-Member löscht die Zuweisung von `Camera`. | `static constexpr kWorldUp` |
| CORE-21 | N | Inkons. | [Camera.cpp:60](../engine/src/scene/Camera.cpp:60) | Scroll klemmt das FOV auf [1, 90], Skripte setzen bis 140. `setFov` und `setFarPlane` sind ungeprüft. | Gemeinsamer Bereich, `far > near` |
| CORE-22 | I | Robust. | [Camera.cpp:50](../engine/src/scene/Camera.cpp:50) | `m_yaw` wächst unbegrenzt. | `fmod(…, 360)` |
| CORE-23 | N | Bug (plaus.) | [Window.cpp:185](../engine/src/core/Window.cpp:185) | Das Icon wird mit dem globalen stb-Flip geladen, der von früheren Texturen gesetzt sein kann. | Flip explizit setzen (siehe 3.3) |
| CORE-24 | I | Div. | [Window.cpp:199](../engine/src/core/Window.cpp:199) | `glViewport` im Resize-Callback läuft auf dem aktuellen Kontext. `printf("%s", glGetString)` ohne Null-Check. Der 4.3-Fehlversuch erscheint als GLFW-Fehler. | Aufräumen |

### 4.2 Graphics

| ID | S | Kat. | Ort | Befund | Empfehlung |
|---|---|---|---|---|---|
| GFX-01 ✔ · **behoben** | H | Bug/Thread | [Texture.cpp:307](../engine/src/graphics/Texture.cpp:307) | stb ist mit `STBI_THREAD_LOCAL` gebaut. `decodeThumbnail` setzt auf dem Render-Thread die thread-lokale Überschreibung, danach sind `fromFile(…, flip=true)` und das HDR-Panorama nicht mehr geflippt. `m_bottomUp` stimmt dann nicht mehr. Der Kommentar in VideoTexture nennt den globalen Setter „thread-local“. | Nach dem Decode selbst flippen, sonst nur die `_thread`-API |
| GFX-02 · **behoben** | M | Sicherheit | [VideoTexture.cpp:63](../engine/src/graphics/VideoTexture.cpp:63) | Breite und Höhe aus der Datei sind unbegrenzt, `blank()` allokiert w·h·4 (Gigabytes). fps NaN/Inf führt zu UB im `int`-Cast. | Größen- und fps-Limits, `isfinite` |
| GFX-03 · **behoben** | M | Bug (GLSL) | [EnvironmentIBL.cpp:121](../engine/src/graphics/EnvironmentIBL.cpp:121) | `texture()` mit impliziter LOD in nicht-uniformem `if`. Keine PDF-basierte Mip-Wahl, dadurch Fireflies. | `textureLod` mit PDF-LOD |
| GFX-04 · **teilweise** | M | Perf | [EnvironmentIBL.cpp:113](../engine/src/graphics/EnvironmentIBL.cpp:113) | Prefilter-Mip 0 mit roughness 0 macht 1024 identische Taps pro Texel, rund 75 % der Prefilter-Kosten. Irradiance kostet ≈ 98 M Taps, synchron. | Mip 0 per Blit, 64–256 Samples, SH9 |
| GFX-05 · **behoben** | M | Fehlerbeh. | [EnvironmentIBL.cpp:409](../engine/src/graphics/EnvironmentIBL.cpp:409) | `m_valid = true` ohne Shader-, FBO- und Max-Size-Prüfung. Der GL-State wird nicht wie kommentiert wiederhergestellt (FBO 0 statt des vorherigen, Programm, Scissor). | Validieren, State sichern |
| GFX-06 · **behoben** | M | Bug/Perf | [Mesh.cpp:199](../engine/src/graphics/Mesh.cpp:199) | `update(vertices)` auf ein indiziertes Mesh behält das alte EBO, Folge ist ein GPU-OOB. Bei Views wird der VBO der Basis überschrieben. `glBufferData` pro Frame. | Assert und orphan-and-grow über `stream()` |
| GFX-07 ✔ · **behoben** | M | Perf | [Shader.cpp:303](../engine/src/graphics/Shader.cpp:303) | Jeder Uniform-Setter ruft `glUseProgram`. | Programm-Cache (siehe REN-01) |
| GFX-08 | M | Inkons. | [Texture.cpp:145](../engine/src/graphics/Texture.cpp:145) | Farbtexturen als `RGBA8` mit `pow(2.2)` im Shader. Mipmaps und Filterung arbeiten im Gamma-Raum. EXR-Farbmaps werden doppelt linearisiert (plausibel). | `GL_SRGB8_ALPHA8` für Farbe |
| GFX-09 | N | Bug | [CascadedShadowMap.cpp:172](../engine/src/graphics/CascadedShadowMap.cpp:172) | Vorzeichen von near/far vertauscht. Bei r < 1,22 m wird die Kaskade abgeschnitten, sonst wird Tiefenpräzision verschenkt (Spanne 20r). | `near = 1 - r - margin`, `far = 1 + r` |
| GFX-10 · **behoben** | N | Bug | [CascadedShadowMap.cpp:45](../engine/src/graphics/CascadedShadowMap.cpp:45) | Mehr als 4 Kaskaden führen zu OOB auf `uCascadeSplits[4]` im Shader. | Auf [1, 4] klemmen |
| GFX-11 | N | Sicherheit | [EnvironmentIBL.cpp:152](../engine/src/graphics/EnvironmentIBL.cpp:152) | `malloc` ungeprüft, `int`-Index läuft bei großen EXRs über. | `std::vector`, `size_t` |
| GFX-12 | N | Bug | [Texture.cpp:61](../engine/src/graphics/Texture.cpp:61) | `channels` wird nicht validiert. Bei 0 oder > 4 folgt ein OOB-Read. `ImagePixels::valid()` prüft die Größe nicht. | Prüfen |
| GFX-13 | N | Fehlerbeh. | [RenderTarget.cpp:59](../engine/src/graphics/RenderTarget.cpp:59) | FBO-Completeness wird pro Klasse anders behandelt (nur Log, nur Kaskade 0 jeden Frame, oder gar nicht). Kein `GL_MAX_TEXTURE_SIZE`-Clamp. | Einheitliches `isValid()` |
| GFX-14 | N | Bug | [Texture.cpp:385](../engine/src/graphics/Texture.cpp:385) | `readback()` bindet `TEXTURE_2D` auch bei Array-Texturen und liefert Nullen als „gültig“. | Abbrechen oder per FBO lesen |
| GFX-15 | N | Bug | [Texture3D.cpp:70](../engine/src/graphics/Texture3D.cpp:70) | `update()` und der Panorama-Upload setzen ROW_LENGTH/IMAGE_HEIGHT nicht zurück (bekanntes Leck aus ImGui). | `resetPixelStore()` |
| GFX-16 | N | UB | [Texture.cpp:95](../engine/src/graphics/Texture.cpp:95) | `std::tolower(char)` mit UTF-8-Bytes ≥ 0x80. | Cast auf `unsigned char` |
| GFX-17 | N | Inkons. | [ShadowMap.cpp:80](../engine/src/graphics/ShadowMap.cpp:80) | Create- und Update-Funktionen überschreiben Bindings, `end()` bindet hart FBO 0, CubeShadowMap setzt die Clear-Farbe dauerhaft. Nur `arrayOf` stellt den Zustand wieder her. | Konvention festlegen (3.3) |
| GFX-18 | N | Inkons. | [EnvironmentIBL.hpp:37](../engine/include/fitzel/graphics/EnvironmentIBL.hpp:37) | „row 0 = up“ ist falsch. Texture.hpp beschreibt 1/2 Kanäle falsch. | Kommentare korrigieren |
| GFX-19 | N | Perf | [Texture3D.cpp:37](../engine/src/graphics/Texture3D.cpp:37) | Ein schwarzes Volumen wird immer allokiert. | Nur im Fallback |
| GFX-20 | N | Perf | [Texture.cpp:139](../engine/src/graphics/Texture.cpp:139) | RGBA-Daten werden unnötig kopiert, Video doppelt pro Frame. | Direkt hochladen |
| GFX-21 | N | Perf | [InstancedMesh.cpp:77](../engine/src/graphics/InstancedMesh.cpp:77) | `glBufferData` pro Upload. | `stream()`-Strategie aus Mesh |
| GFX-22 | I | Perf | [CascadedShadowMap.cpp:26](../engine/src/graphics/CascadedShadowMap.cpp:26) | Manuelles 4-Tap-PCF statt `sampler2DArrayShadow`. Sechs Copies pro Cube statt `glCopyImageSubData`. Kein `glTexStorage`. Der Completeness-Check läuft jeden Frame. | Hardware-PCF, Immutable Storage |
| GFX-23 | I | Perf (Web) | [RenderTarget.cpp:108](../engine/src/graphics/RenderTarget.cpp:108) | `bind()` macht auf Emscripten mehr als 64 GL-Aufrufe und Queries. | Shadow-State-Cache |
| GFX-24 | I | Wartb. | [Shader.cpp:177](../engine/src/graphics/Shader.cpp:177) | `ShadowMap` ist toter Code. `#include` ohne include-once. Der Compile-Log ist auf 1024 Byte begrenzt, jede Nicht-Vertex-Stage heißt „fragment“. Equirect mit CLAMP auf S erzeugt eine Naht. | Aufräumen |

### 4.3 Renderer

| ID | S | Kat. | Ort | Befund | Empfehlung |
|---|---|---|---|---|---|
| REN-01 · **behoben** | H | Perf | [Renderer.cpp:953](../engine/src/render/Renderer.cpp:953) | `drawOne` lädt pro Draw und Pass etwa 200 Uniforms einzeln hoch, jeweils mit `glUseProgram` und Hash-Lookup. Kosten: Draws × Pässe (Haupt-, 6 Probe-, Wasser-Pässe) × 200. | UBOs für Frame-Konstanten, Programm-Cache |
| REN-02 | M | Perf | [Renderer.cpp:1129](../engine/src/render/Renderer.cpp:1129) | Keine Sortierung der opaken Liste nach Zustand. | Sortierschlüssel (Programm, Material, VAO) |
| REN-03 · **behoben** | M | Bug/Speicher | [Renderer.cpp:324](../engine/src/render/Renderer.cpp:324) | `m_lastModels` und `m_meshSeen` sind nach `Mesh*` geschlüsselt und wachsen unbegrenzt. Wird eine Adresse wiederverwendet, entstehen falsche Motion-Vektoren. | Nicht gesehene Einträge löschen, nach Revision schlüsseln |
| REN-04 · **behoben** | M | Leck/Ro5 | [Renderer.hpp:93](../engine/include/fitzel/render/Renderer.hpp:93) | Kein Destruktor, Point-Shadow-Arrays und Scene-Copies lecken. Impliziter Move lässt `m_envRead`/`m_envWrite` auf das Quellobjekt zeigen. | Destruktor schreiben, Copy/Move löschen |
| REN-05 · **behoben** | M | GL-State | [Renderer.cpp:794](../engine/src/render/Renderer.cpp:794) | `prepare*` setzt hart FBO 0 und den vollen Viewport. `renderScene` hinterlässt die aktive Texture-Unit 26. Probe-Clear mit geleakter weißer Clear-Farbe. | Zustand sichern und wiederherstellen |
| REN-06 · **behoben** | M | Bug | [Renderer.cpp:275](../engine/src/render/Renderer.cpp:275) | Mehr als 4 Kaskaden ergeben OOB im Shader (`MAX_CASCADES 4`), siehe GFX-10. | `kMaxCascades` |
| REN-07 | N | Bug (plaus.) | [Renderer.cpp:963](../engine/src/render/Renderer.cpp:963) | `uHasSceneCopy` stammt bei opaken Draws aus dem vorigen Pass. Opakes Glas samplt dann eine fremde Kopie. | Pro Pass zurücksetzen |
| REN-08 | N | Bug | [Renderer.cpp:459](../engine/src/render/Renderer.cpp:459) | `prepareShadows` deaktiviert `GL_CLIP_DISTANCE0` nicht, anders als die anderen Pässe. | Ergänzen |
| REN-09 | N | Perf | [Renderer.cpp:293](../engine/src/render/Renderer.cpp:293) | `glIsEnabled` bzw. `glGetIntegerv` mitten im Frame, obwohl der eigene Kommentar davor warnt. | Zustand selbst mitführen |
| REN-10 | N | Inkons. | [Renderer.cpp:1219](../engine/src/render/Renderer.cpp:1219) | `end()` leert die Queue. Das bricht den Vertrag von `submissions()` und die Motion-Historie. | Clear entfernen |
| REN-11 | N | UB/Perf | [Renderer.cpp:1177](../engine/src/render/Renderer.cpp:1177) | Der Comparator berechnet den Schlüssel bei jedem Vergleich neu. NaN verletzt die strict weak ordering, `std::sort` ist dann UB. | Schlüssel vorberechnen, NaN abbilden |
| REN-12 | N | Perf | [Renderer.cpp:940](../engine/src/render/Renderer.cpp:940) | `worldAabb` pro Draw und Pass, `buildCullBounds` zweimal pro Frame, Vektor-Allokationen pro Pass. | Beim Submit cachen, Member-Listen |
| REN-13 | N | Perf (plaus.) | [Renderer.cpp:674](../engine/src/render/Renderer.cpp:674) | Shadow-Slots werden per Float-Gleichheit der Position zugeordnet. Bewegte Lichter verdrängen den Cache jeden Frame. | Stabile Licht-ID |
| REN-14 | I | Wartb. | [Renderer.hpp:449](../engine/include/fitzel/render/Renderer.hpp:449) | Falsche Init-Reihenfolge (`-Wreorder`), Magic Number `0x0405`, `setPointLights` invalidiert den Shadow-State stillschweigend. | Aufräumen, dokumentieren |

### 4.4 Terrain

| ID | S | Kat. | Ort | Befund | Empfehlung |
|---|---|---|---|---|---|
| TER-01 ✔ · **behoben** | H | Bug | [Terrain.cpp:561](../engine/src/world/Terrain.cpp:561) | `t = min(h[i], …)` bei negativen Höhen: Die Zelle springt auf 0, das Sediment wird negativ. | Klemme gegen Ausgangshöhe oder Untergrenze |
| TER-02 · **behoben** | H | Robust. | [Terrain.cpp:746](../engine/src/world/Terrain.cpp:746) | Geladene `resolution`/`chunkSize`/`peakSharpness` sind ungeprüft. Folgen: NaN-Meshes, `length_error` auf dem Worker, `terminate`, `int`-Overflow. | `TerrainSettings::sanitized()`, try/catch im Worker |
| TER-03 ✔ · **behoben** | M | Deadlock | [Terrain.cpp:892](../engine/src/world/Terrain.cpp:892) | `m_stop` wird ohne `m_jobMutex` gesetzt, Lost Wakeup, `join()` hängt. | Unter Lock setzen, dann `notify_all` |
| TER-04 · **behoben** | M | Perf | [Terrain.cpp:819](../engine/src/world/Terrain.cpp:819) | Identische Indexpuffer (Basis plus LODs) pro Chunk neu erzeugt und hochgeladen. `reserve` ohne Skirt, toter Code `grid`. | EBO pro `(n, stride)` teilen |
| TER-05 · **behoben** | M | Perf | [Terrain.cpp:682](../engine/src/world/Terrain.cpp:682) | `terrainHeight()` nimmt pro Abfrage einen globalen Mutex und kopiert einen `shared_ptr`. Contention bei parallelen Abfragen. | `atomic<shared_ptr>` oder Snapshot-API |
| TER-06 | N | Perf | [Terrain.cpp:325](../engine/src/world/Terrain.cpp:325) | `smooth()` wertet die fBm-Basis zehnmal pro Zelle aus. | Lokales Raster wie in `erode()` |
| TER-07 | N | UB | [Terrain.cpp:252](../engine/src/world/Terrain.cpp:252) | `cell <= 0` bzw. NaN führt zu UB beim float→int-Cast. `cell` kommt ungeprüft aus der JSON. | Klemmen |
| TER-08 | N | Bug | [Terrain.cpp:603](../engine/src/world/Terrain.cpp:603) | Das Kraterprofil reicht bis 1,1 r, die Iteration nur bis r. Ergebnis ist eine Stufe. | Bis zur maximalen Trägerweite iterieren |
| TER-09 · **behoben** | N | NaN | [Terrain.cpp:62](../engine/src/world/Terrain.cpp:62) | `peakSharpness <= 0` ergibt `inf · 0 = NaN`. | Klemmen |
| TER-10 | N | Inkons. | [Terrain.cpp:819](../engine/src/world/Terrain.cpp:819) | Vertex-Layout `max(2, res+1)` gegenüber Indizes `res+1`. | Ein validiertes `n` |
| TER-11 | I | Inkons. | [Terrain.cpp:412](../engine/src/world/Terrain.cpp:412) | `erode()` ist nicht massenerhaltend, Material außerhalb der Scheibe geht verloren. | Apron zurückschreiben |
| TER-12 | I | Wartb. | [Terrain.hpp:91](../engine/include/fitzel/world/Terrain.hpp:91) | Handgeschriebenes `operator==` (C++20 `= default` möglich). Radius-Inkonsistenz. `n` per Wurzel rekonstruiert. Allokationen pro Frame in `update`. | Aufräumen |

### 4.5 Model (glTF / assimp) und Animation

| ID | S | Kat. | Ort | Befund | Empfehlung |
|---|---|---|---|---|---|
| MOD-01 ✔ · **behoben** | K | Sicherheit | [Model.cpp:743](../engine/src/world/Model.cpp:743) | Kein `cgltf_validate`. Index ≥ count, Accessor > BufferView, BufferView > Buffer und eingebettete Bilder führen zu Heap-OOB. | Validieren nach `cgltf_load_buffers`, zusätzlich `idx < count` |
| MOD-02 ✔ · **behoben** | H | Sicherheit | [Model.cpp:118](../engine/src/world/Model.cpp:118) | `cgltfRead` ignoriert `*size`. Eine kurze `.bin` gilt als Erfolg, `buffer->size` bleibt der deklarierte Wert. | Fehler bei `bytes.size() < *size` |
| MOD-03 · **behoben** | H | Sicherheit | [Model.cpp:778](../engine/src/world/Model.cpp:778) | IBM-Count wird nicht gegen `joints_count` geprüft, OOB. `float m[16]` uninitialisiert. | Count prüfen, Identität vorbelegen |
| MOD-04 · **behoben** | H | DoS | [Model.cpp:775](../engine/src/world/Model.cpp:775) | Zyklische Node-Hierarchien: Endlosschleife in `cgltf_node_transform_world`, Rekursion in `resolve`. | Durch MOD-01 abgedeckt, zusätzlich topologisch sortieren |
| MOD-05 · **behoben** | H | Bug | [Model.cpp:809](../engine/src/world/Model.cpp:809) | `interpolation` wird ignoriert. STEP wird linear interpoliert, CUBICSPLINE liest Tangenten als Werte. `output->count` ungeprüft. | Modus speichern, `3k+1` bzw. Hermite |
| MOD-06 · **behoben** | M | Bug | [Model.cpp:818](../engine/src/world/Model.cpp:818) | Keyframe-Arrays uninitialisiert, Rückgabewerte ungeprüft. Sparse Accessoren werden still zu 0. | `cgltf_accessor_unpack_floats` |
| MOD-07 · **behoben** | M | Bug | [Model.cpp:722](../engine/src/world/Model.cpp:722) | Nur `skins[0]` wird ausgewertet, Joints anderer Skins landen auf Index 0. | Skins vereinigen, IBMs pro Skin |
| MOD-08 | M | Bug/Inkons. | [Model.cpp:805](../engine/src/world/Model.cpp:805) | Animationen auf Nicht-Joint-Nodes gehen verloren. Starre Anhänge folgen keinem Bone. assimp nimmt Vorfahren auf, glTF nicht. | Pfade angleichen (`buildRig`) |
| MOD-09 · **behoben** | M | Thread | [Model.cpp:361](../engine/src/world/Model.cpp:361) | Globaler stb-Flip auf einem Worker-Thread (RetargetPanel), Data Race mit dem Main-Thread. | Siehe 3.3 |
| MOD-10 · **behoben** | M | Absturz | [Model.cpp:295](../engine/src/world/Model.cpp:295) | Buffer ohne URI hat `data == NULL`, Folge ist eine Null-Dereferenz in `decodeImage`. | `cgltf_buffer_view_data` mit Null-Check |
| MOD-11 · **behoben** | M | Sicherheit | [Model.cpp:335](../engine/src/world/Model.cpp:335) | Pfad-Traversal: absolute und `../`-URIs (auch prozentkodiert) verlassen das Asset-Verzeichnis. | Kanonisieren, Root erzwingen |
| MOD-12 · **behoben** | M | DoS | [Model.cpp:296](../engine/src/world/Model.cpp:296) | Keine Bildgrößen-Grenze bei bis zu 8 parallelen Decodes. | `stbi_info` plus Limit |
| MOD-13 | N | Bug | [Model.cpp:592](../engine/src/world/Model.cpp:592) | Ein beliebiges `JOINTS_n`/`WEIGHTS_n` wird genommen statt Index 0. | `at.index == 0` |
| MOD-14 | N | Inkons. | [Model.cpp:658](../engine/src/world/Model.cpp:658) | Die Normal-Map prüft das UV-Set nicht, die anderen Maps schon. | Prüfung ergänzen |
| MOD-15 | N | Bug | [Model.cpp:838](../engine/src/world/Model.cpp:838) | Iteriert über alle Nodes statt über die Szene. | Ab `data->scene` traversieren |
| MOD-16 | M | Inkons. | [Model.cpp:1342](../engine/src/world/Model.cpp:1342) | assimp ohne PBR, Emissive und doubleSided. Opacity wird nur als Cutout genutzt. HEIGHT gilt als Normal-Map. glTF-Fallback-Normale (0,1,0) statt flacher Normalen. | Gemeinsames Material-Mapping |
| MOD-17 | M | Inkons. | [Model.cpp:1739](../engine/src/world/Model.cpp:1739) | Rig ohne Clips: assimp backt die Transforms, glTF nicht (Armature-Skalierung fehlt). | Verhalten angleichen |
| MOD-18 | N | Robust. | [Model.cpp:1677](../engine/src/world/Model.cpp:1677) | Kein `aiProcess_ValidateDataStructure`, ungeprüfte Indizes, Rekursion ohne Tiefenlimit. | Flag setzen, iterativ traversieren |
| MOD-19 | N | Inkons. | [Model.cpp:1556](../engine/src/world/Model.cpp:1556) | Clip-Dauer und Clips der Länge 0 werden in beiden Pfaden unterschiedlich behandelt, die Indizes verschieben sich. | Gemeinsame Regel |
| MOD-20 | N | Perf | [Model.cpp:1276](../engine/src/world/Model.cpp:1276) | Der Unity-Fallback scannt bis zu 60 000 Einträge im Elternverzeichnis. Die Emission-Heuristik kann fremde Maps zuordnen. | Nur bei fehlender Textur |
| MOD-21 | N | Bug (plaus.) | [Model.cpp:1495](../engine/src/world/Model.cpp:1495) | Bone in mehreren Meshes mit unterschiedlichem Offset: der letzte gewinnt. | Pro Mesh halten |
| MOD-22 · **behoben** | M | Doku | [Model.hpp:125](../engine/include/fitzel/world/Model.hpp:125) | „looped over the clip duration“: Der Code klemmt (siehe Workaround in Herd.cpp). | `fmod` oder Doku korrigieren |
| MOD-23 | N | Bug | [Model.cpp:1841](../engine/src/world/Model.cpp:1841) | Quaternionen werden nicht normalisiert (Randkeys, normalisierte Int-Accessoren). | Beim Laden normalisieren |
| MOD-24 · **behoben** | M | Perf | [Model.cpp:1832](../engine/src/world/Model.cpp:1832) | Lineare Keyframe-Suche, vier Vektoren plus `std::function` pro `sampleSkeleton`-Aufruf und Figur. | `upper_bound`, vorsortierte Joints, Scratch-Puffer |
| MOD-25 | M | Perf | [Model.cpp:685](../engine/src/world/Model.cpp:685) | Alle Geometrie wird de-indiziert, etwa 6× mehr Vertices und CPU-Skinning. `JoinIdenticalVertices` bleibt wirkungslos. | Indexpuffer behalten |
| MOD-26 | N | Exception-Safety | [Model.cpp:742](../engine/src/world/Model.cpp:742) | Roher `cgltf_data*`, `reserve(count*8)` aus Dateiwerten, Thread-Pool nicht RAII-sicher (`terminate`). | `unique_ptr` mit `cgltf_free`, joinender Pool |
| MOD-27 | I | Wartb. | [Model.cpp:1012](../engine/src/world/Model.cpp:1012) | Kommentare an der falschen Funktion, doppelte Color-View- und Recenter-Logik, veraltete Header-Kommentare. | Aufräumen |
| MOD-28 | I | Inkons. (plaus.) | [Model.cpp:1676](../engine/src/world/Model.cpp:1676) | FBX ohne Einheitenkonvertierung, cm gegenüber m (Faktor 100, nicht verifiziert). | `AI_CONFIG_FBX_CONVERT_TO_M` prüfen |

### 4.6 Voxel

| ID | S | Kat. | Ort | Befund | Empfehlung |
|---|---|---|---|---|---|
| VOX-01 | N | Perf | [Voxel.cpp:132](../engine/src/world/Voxel.cpp:132) | Außerhalb des Chunks gilt alles als Air. Folge: unsichtbare Wände an jeder Chunk-Naht und an y = 0. | Nachbar-Lookup oder Rand |
| VOX-02 | N | Design | [Voxel.hpp:27](../engine/include/fitzel/world/Voxel.hpp:27) | Water gilt als `isSolid`, die Faces unter Wasser werden gecullt. | `isOpaque` trennen |
| VOX-03 | I | Perf | [Voxel.cpp:122](../engine/src/world/Voxel.cpp:122) | Kein `reserve`, kein Greedy-Meshing. Winding und Index-Grenzen sind korrekt. | – |

### 4.7 Audio

| ID | S | Kat. | Ort | Befund | Empfehlung |
|---|---|---|---|---|---|
| AUD-01 ✔ · **behoben** | K | Absturz | [Mixer.cpp:580](../engine/src/audio/Mixer.cpp:580) | `setInserts` verschiebt behaltene Effekte unter dem Lock aus `s->fx` und gibt den Lock frei. `Strip::process` ([Mixer.cpp:376](../engine/src/audio/Mixer.cpp:376)) iteriert dann ohne Null-Check, Null-Dereferenz im Audio-Thread. | Erst beim Swap verschieben, `if (e && …)` |
| AUD-02 · **behoben** | H | OOB | [SynthDsp.cpp:275](../engine/src/audio/SynthDsp.cpp:275) | `pos += n` rundet in float auf genau `n`, Lesezugriff auf `m_buffer[n]` (Chorus, Mixer-Delay). Werte landen im Feedback. | `if (i0 >= n) i0 -= n`, Ganzzahl plus Bruchteil |
| AUD-03 ✔ · **behoben** | H | Hang | [SynthDsp.cpp:315](../engine/src/audio/SynthDsp.cpp:315) | Die Fold-Schleife braucht O(\|x\|) Iterationen und terminiert ab etwa 1e8 bzw. bei Inf nicht mehr. Erreichbar über unbegrenzten Drive. | Geschlossene Form plus `isfinite` |
| AUD-04 · **behoben** | H | Absturz | [Mixer.cpp:167](../engine/src/audio/Mixer.cpp:167) | `std::clamp(NaN)` liefert NaN. `int(NaN)` ergibt einen INT_MIN-Index (Delay), Segfault. NaN-Pan macht den ganzen Strip zu NaN. | `isfinite` in allen öffentlichen Settern |
| AUD-05 · **behoben** | M | NaN | [Synth.cpp:720](../engine/src/audio/Synth.cpp:720) | Ein einziges NaN vergiftet Glättung und Biquad-Zustand dauerhaft, die Stimme bleibt stumm. | Eingänge sanitisieren, bei nicht-endlichem Wert reset |
| AUD-06 · **behoben** | M | Race | [SynthVoice.cpp:167](../engine/src/audio/SynthVoice.cpp:167) | `play()` ruft `graph.reset()` auf dem Game-Thread auf, während der Audio-Thread rendert. | Atomares Reset-Flag |
| AUD-07 · **behoben** | M | Bug | [Audio.cpp:289](../engine/src/audio/Audio.cpp:289) | Nach `stopFaded` bleibt ein erneutes `play()` stumm: Stop-Zeit und Fader werden nicht zurückgesetzt (miniaudio 0.11.21). | Stop-Zeit und Fade vor dem Start zurücksetzen |
| AUD-08 · **behoben** | M | Robust. | [Synth.cpp:117](../engine/src/audio/Synth.cpp:117) | `json::value()` wirft bei falschem Typ, eine defekte `.patch`-Datei führt zu `terminate`. | Typprüfung oder try/catch |
| AUD-09 · **behoben** | M | Inkons. | [Synth.cpp:593](../engine/src/audio/Synth.cpp:593) | `updateParams` erkennt keine Änderungen an Wires oder Modus, anders als dokumentiert. | Shape-Hash vergleichen |
| AUD-10 | M | Inkons. | [SynthDsp.cpp:383](../engine/src/audio/SynthDsp.cpp:383) | Reverb-„size“ ändert nur das Feedback. Der Mixer-Hall ist praktisch mono. `damp`-Klemme 0,95 gegenüber Slider 1,0. | Comb-Zeiten skalieren, Stereo-Spread |
| AUD-11 · **behoben** | M | Race | [SynthPlayer.cpp:76](../engine/src/audio/SynthPlayer.cpp:76) | „Notes (any thread)“, aber die Queue verträgt nur einen Produzenten. | Doku korrigieren oder CAS |
| AUD-12 | N | Inkons. | [Synth.cpp:537](../engine/src/audio/Synth.cpp:537) | Der Glättungskoeffizient gilt pro 256 Samples, wird aber pro Callback angewandt (etwa 480). | Aus den tatsächlichen Frames berechnen |
| AUD-13 | N | Bug | [SynthPoly.cpp:100](../engine/src/audio/SynthPoly.cpp:100) | Note-Off ignoriert den MIDI-Kanal. | Kanal mitvergleichen |
| AUD-14 | N | Bug | [MusicPlayer.cpp:20](../engine/src/audio/MusicPlayer.cpp:20) | Der Tap-Ring (8192) ist für Latenz plus FFT zu klein, besonders auf dem Web. | `kTap` passend dimensionieren |
| AUD-15 | N | Race | [Mixer.cpp:385](../engine/src/audio/Mixer.cpp:385) | `pre[k]` wird zweimal pro Block gelesen, der Aux-Bus bekommt dann alte Daten. | Einmal lokal laden |
| AUD-16 | N | Robust. | [Mixer.cpp:329](../engine/src/audio/Mixer.cpp:329) | `makeEffect` gibt für ungültige Typen nullptr zurück, das landet in der Kette. | Verwerfen |
| AUD-17 | N | Race | [SynthPlayer.cpp:137](../engine/src/audio/SynthPlayer.cpp:137) | `midiPlaying()` meldet direkt nach `playMidi` kurz false. | Generationszähler |
| AUD-18 | N | Inkons. | [Audio.cpp:115](../engine/src/audio/Audio.cpp:115) | SFX-Gruppe und Sounds ohne Output umgehen den Master-Strip. Toter `playOneShot`-Pfad. | Standardmäßig an Master hängen |
| AUD-19 | N | Lifetime | [Audio.cpp:130](../engine/src/audio/Audio.cpp:130) | Stimmen, die `Audio` überleben, führen zu `ma_sound_uninit` auf eine freigegebene Engine. Nur per Deklarationsreihenfolge abgesichert. | Lebenszeit-Token |
| AUD-20 | N | DoS/Perf | [Midi.cpp:182](../engine/src/audio/Midi.cpp:182) | Tempo-Map wird linear durchsucht (O(E·T)). `lround` gefolgt von `int` kann überlaufen und zu Rendering mit Chunk 1 führen. | Monotoner Index, klemmen |
| AUD-21 | N | Inkons. | [Midi.cpp:70](../engine/src/audio/Midi.cpp:70) | MIDI-Format 2 wird akzeptiert, obwohl undokumentiert. Running Status nach Meta/SysEx. | Ablehnen oder dokumentieren |
| AUD-22 | N | Inkons. | [Synth.cpp:304](../engine/src/audio/Synth.cpp:304) | Doppelte Eingangsnamen: `inputIndex` nimmt den ersten, `compile` den letzten. | Duplikate melden |
| AUD-23 | I | Wartb. | [Synth.cpp:230](../engine/src/audio/Synth.cpp:230) | `outGain` ist tot. Ein moved-from-`Graph` hat `m_impl == null`, die Methoden dereferenzieren es trotzdem. | Entfernen, Guards |
| AUD-P1 · **teilweise** | M | Perf | [Synth.cpp:747](../engine/src/audio/Synth.cpp:747) | Pro Sample `setFrequency`/`recompute`, `std::sin` in double, `std::pow` für Note, Lambda-Indirektion. | Pro Block auflösen, Phasen-Rotator |
| AUD-P2 | N | Perf | [Mixer.cpp:226](../engine/src/audio/Mixer.cpp:226) | Kompressor mit `log10`/`pow` pro Sample. | Schnelle Approximationen |
| AUD-P3 | N | Perf | [Mixer.cpp:383](../engine/src/audio/Mixer.cpp:383) | Pro Strip werden 16 Send-Busse pro Block gememsettet. | Bitmaske |
| AUD-P4 | N | Perf | [MusicPlayer.cpp:184](../engine/src/audio/MusicPlayer.cpp:184) | Filterkoeffizienten alle 32 Frames neu, `spectrum()` allokiert, `analyzeBands` braucht etwa 4× PCM-Speicher. `hop`/`rate` 0 ergeben eine Division durch null. | Cachen, streamen, validieren |
| AUD-P5 | N | Perf | [SynthPoly.cpp:89](../engine/src/audio/SynthPoly.cpp:89) | Voice-Steal leert alle Delay- und Reverb-Puffer im Audio-Thread. | Nur Hüllkurven, Oszillatoren und Filter zurücksetzen |
| AUD-W1 | I | Wartb. | [SynthDsp.cpp:190](../engine/src/audio/SynthDsp.cpp:190) | Drei Biquad-Implementierungen, dreimal Data-Source-Boilerplate, doppelter Scope-Ring und Spatial-Setter. | Gemeinsame Basis |
| AUD-W2 | I | Denormals | – | FTZ/DAZ nur über den miniaudio-Callback auf x86. Offline-Pfade (Editor-Scope, synthcheck) und ARM sind ungeschützt. | FTZ selbst setzen |

### 4.8 Asset / Pak / VFS

| ID | S | Kat. | Ort | Befund | Empfehlung |
|---|---|---|---|---|---|
| AST-01 ✔ · **behoben** | H | Sicherheit | [Pak.cpp:96](../engine/src/asset/Pak.cpp:96) | `reserve(count)` mit ungeprüftem 32-Bit-`count` aus dem Klartext-Header führt zu `length_error`/`bad_alloc` und `terminate`. Ohne Schlüssel möglich. Widerspricht dem eigenen Kommentar „fail cleanly“. | `count <= indexSize / 26` |
| AST-02 · **behoben** | H | Sicherheit | [Pak.cpp:79](../engine/src/asset/Pak.cpp:79) | Index, Offset und Größe der Einträge werden nicht gegen die Dateigröße geprüft. `read()` allokiert bis zu 2⁶⁴. Kürzung auf wasm32 (plausibel). | Überlauffreie Bounds-Checks in `open()` |
| AST-03 ✔ · **behoben** | H | Krypto/Konfig | [PakKey.hpp:18](../engine/src/asset/PakKey.hpp:18) | Standardschlüssel im Repo. Der Override `FITZEL_PAK_KEY_HEX` wird von keiner CMakeLists weitergereicht. Ungültige Hex-Zeichen werden still zu 0. | CMake-Option mit Validierung, Warnung im Editor |
| AST-04 · **teilweise** | M | Sicherheit | [Pak.cpp:97](../engine/src/asset/Pak.cpp:97) | Ein falscher Schlüssel wird nur zufällig erkannt (kein Magic, kein MAC, keine Prüfung `at == size`). Ciphertext lässt sich per Bit-Flip manipulieren. | Verschlüsseltes Magic plus MAC (Poly1305 oder BLAKE2) |
| AST-05 | N | Krypto | [Pak.cpp:173](../engine/src/asset/Pak.cpp:173) | Salt mit nur 32 Bit Entropie. Nonces `salt+i` überlappen zwischen Archiven. 32-Bit-Counter. `memset`-Wipe kann wegoptimiert werden. | 64-Bit-CSPRNG, Präfix-Nonce, `explicit_bzero` |
| AST-06 · **behoben** | M | Bug | [Pak.cpp:154](../engine/src/asset/Pak.cpp:154) | `pak::write` meldet `ok` auch bei übersprungenen oder gekürzten Dateien. ProjectIO löscht danach die losen Ordner, Folge ist ein stiller Asset-Verlust. Die CLI kann die eigene Ausgabe einpacken. | Fehler propagieren, tmp-Datei plus Rename |
| AST-07 · **behoben** | M | Bug | [AssetDatabase.cpp:122](../engine/src/asset/AssetDatabase.cpp:122) | Nur `parse` liegt im try. `value()` wirft bei falschem Typ und bricht `refresh`/`pollChanges` ab. | Typprüfung |
| AST-08 · **behoben** | M | Datenverlust | [AssetDatabase.cpp:125](../engine/src/asset/AssetDatabase.cpp:125) | Eine korrupte `.meta` erzeugt still eine neue GUID, alle Referenzen brechen. Schreiben nicht atomar, unbekannte Keys gehen verloren. | Nicht überschreiben, `.corrupt` sichern, atomar schreiben |
| AST-09 · **teilweise** | M | Perf | [AssetDatabase.cpp:371](../engine/src/asset/AssetDatabase.cpp:371) | `pollChanges` scannt mehrmals pro Sekunde rekursiv mit `weakly_canonical` pro Datei. `erase(remove…)` ist O(n·m). | Lexikalische Keys, inotify bzw. `ReadDirectoryChangesW` |
| AST-10 | N | Bug | [AssetDatabase.cpp:190](../engine/src/asset/AssetDatabase.cpp:190) | Eine GUID-Kollision wird bei jedem Poll neu importiert, Log-Spam. | Konfliktpfade merken |
| AST-11 | N | Lifetime | [AssetDatabase.cpp:75](../engine/src/asset/AssetDatabase.cpp:75) | Veraltete `sourceIndex` nach Unmount. `entry()`/`allAssets()` werden durch Poll ungültig, keine Sync-Doku. | Stabile IDs, Thread-Doku |
| AST-12 | N | Thread | [Vfs.cpp:19](../engine/src/asset/Vfs.cpp:19) | Unsynchronisierte Globals, `mount` parallel zu `read` ergibt Use-after-free. `..` fällt auf die Platte zurück. Groß/Klein-Kollisionen werden überschattet. | Mount vor Threads, Kollisionen melden |
| AST-13 | N | Perf/Inkons. | [Vfs.cpp:141](../engine/src/asset/Vfs.cpp:141) | Lineare Listen-Scans mit `lowerKey`-Allokation. Pfadform je nach Quelle unterschiedlich, die Deduplizierung greift nicht. | Vorberechnen, sortiert, einheitliche Form |
| AST-14 | N | Doku | [AssetTypes.hpp:13](../engine/include/fitzel/asset/AssetTypes.hpp:13) | Model nennt nur „gltf, glb“, der Code akzeptiert auch `dae`/`fbx`. „Phase 2“ ist veraltet. | Kommentar |
| AST-15 | N | Portab. (plaus.) | [AssetDatabase.cpp:280](../engine/src/asset/AssetDatabase.cpp:280) | `path.string()` unter Windows in ANSI-Codepage, Unicode-Pfade brechen. | `fs::path`/`u8string` durchreichen |

Ohne Befund: `AssetId` (gut geseedet, Null-ID ausgeschlossen) und der ChaCha20-Kern (entspricht RFC 8439).

### 4.9 Physics (inkl. uncommittetem Diff)

| ID | S | Kat. | Ort | Befund | Empfehlung |
|---|---|---|---|---|---|
| PHY-01 ✔ · **behoben** | K | UAF | [Physics.cpp:748](../engine/src/physics/Physics.cpp:748) | `DestroyBody` läuft auch für nicht hinzugefügte oder veraltete IDs. Jolt prüft nur per Assert. Folgen: Müllzeiger-Delete, fremder Body zerstört, OOB bei `0xFFFFFFFF`. Aufgerufen u. a. von TownTraffic, TramSystem und SoftBodySystem. | `BodyLockRead`-Existenzprüfung, sonst `return` |
| PHY-02 · **behoben** | H | Bug | [Physics.cpp:236](../engine/src/physics/Physics.cpp:236) | Fehlschlag liefert `cInvalidBodyID` (`0xFFFFFFFF`) statt der dokumentierten 0. Aufrufer prüfen auf 0. Eine Sequenznummer 0 kann eine gültige ID 0 ergeben. | Zentrales `toId()`, Fehler loggen |
| PHY-03 · **behoben** | H | UAF | [Physics.cpp:469](../engine/src/physics/Physics.cpp:469) | `VehicleConstraint` und StepListener werden bei `removeBody` nicht entfernt. Ein zweites `addVehicle` hinterlässt einen verwaisten Constraint. | `removeVehicle()`, Aufruf aus `removeBody` |
| PHY-04 · **behoben** | M | Bug | [Physics.cpp:382](../engine/src/physics/Physics.cpp:382) | Fahrzeugmasse ungeklemmt (≤ 0 ergibt NaN), Shape-`HasError` ungeprüft, `Vec3` statt `RVec3`. | Klemmen, prüfen, `RVec3` |
| PHY-05 · **behoben** | M | Sicherheit | [Physics.cpp:540](../engine/src/physics/Physics.cpp:540) | Mesh-Indizes werden nicht geprüft. `MeshShapeSettings::Sanitize` liest vor der Validierung OOB. Doppeltes `Sanitize`, Kopie per Wert. | Prüfen wie in `addSoftMesh`, `move` |
| PHY-06 | M | Design | [Physics.cpp:622](../engine/src/physics/Physics.cpp:622) | Variables `dt` ohne Accumulator. Verhalten hängt von der Framerate ab, nicht deterministisch, Tunneling bei 0,1 s. | Fester Schritt plus Accumulator |
| PHY-07 · **behoben** | M (plaus.) | Skalierung | [Physics.cpp:218](../engine/src/physics/Physics.cpp:218) | Fest eingebaut: 4096 Bodies, 8192 Paare, 4096 Kontakte. Jolt verwirft überzählige Kontakte still. | Konfigurierbar, Überlauf loggen |
| PHY-08 | N | Perf (Diff) | [Physics.cpp:715](../engine/src/physics/Physics.cpp:715) | `applyImpulseAt` nimmt 3 Locks (`ActivateBody` ist redundant). Der neue Auftrieb kostet rund 25 Locks pro Body und Frame. | Ein `BodyLockWrite` oder Batch-API |
| PHY-09 | N | Inkons. | [Physics.cpp:783](../engine/src/physics/Physics.cpp:783) | Mischung aus NoLock- und Lock-Interface, Thread-Vertrag nicht dokumentiert. | Vereinheitlichen, dokumentieren |
| PHY-10 | I | Info | [Physics.cpp:103](../engine/src/physics/Physics.cpp:103) | `JPH::Trace`/`AssertFailed` werden nicht gesetzt, Jolt-Meldungen gehen verloren. Thread-Pool pro Welt. | Hooks setzen |

Die neuen Funktionen im Diff (`applyImpulseAt`, `getMass`, `getShapeBox`) sind fachlich korrekt. `GetLocalBounds` ist tatsächlich um den Schwerpunkt zentriert, der Kommentar stimmt.

---

## 5. Vorgeschlagene Roadmap

**Phase 1 – Absturz- und Sicherheitsfixes (klein, lokal, etwa 1–2 Tage)**
PHY-01/02/03, AUD-01/02/03/04, MOD-01/02/03/10, AST-01/02, TER-01/02/03, GFX-01 (und MOD-09/CORE-23 gleich mit), CORE-13/14.

**Phase 2 – Robustheit und Konsistenz**
NaN-Härtung aller öffentlichen Setter (AUD-05, TER-07/09, CORE-21), JSON-Typprüfungen (AST-07/08, AUD-08), `pak::write`-Fehlerpfade (AST-06), AST-03 (Key-Override verdrahten), GL-State-Konvention (GFX-05/15/17, REN-05/08), Renderer-Destruktor (REN-04), glTF-Animation (MOD-05/06/07/22/23), Doku-Korrekturen aus 3.5.

**Phase 3 – Performance**
Uniform-Pfad mit UBOs und Programm-Cache (REN-01, GFX-07), State-Sortierung (REN-02), indizierte Modelle (MOD-25), allokationsfreies Animation-Sampling (MOD-24), geteilte Terrain-EBOs und lockfreie Höhenabfrage (TER-04/05), IBL-Precompute (GFX-04), sRGB-Texturen und Hardware-PCF (GFX-08/22), Synth-Innenschleifen (AUD-P1), Fixed-Step-Physics (PHY-06).

**Phase 4 – Absicherung gegen Rückfälle**
- Fuzz-Targets für `pak::Archive::open`, `loadGltf` und `midi::parse` (libFuzzer, das ASan-Build existiert bereits mit `build-asan.bat`).
- Ein Sanitizer-Lauf (ASan/UBSan/TSan) über die vorhandenen `sandbox/tools/*check`-Programme. TSan dürfte AUD-06, TER-03 und AST-12 direkt zeigen.
- `-Wreorder` ist über `-Wall` bereits aktiv (REN-14 sollte also schon warnen). Zusätzlich `-Wshadow` und `-Wconversion` testweise aktivieren und einen `-Werror`-CI-Job für `engine/` prüfen.
