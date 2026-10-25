# Fitzel — Render Hardware Interface (`fitzel::rhi`)

Jeder Aufruf an die Grafikkarte geht durch **ein** Interface: `fitzel::rhi::Device`
(`engine/include/fitzel/rhi/Device.hpp`, Vokabular in `Types.hpp`). Es gibt zwei
Implementierungen: OpenGL (`engine/src/rhi/gl/`, Standard, auch WebGL2) und Vulkan
(`engine/src/rhi/vk/`, `FITZEL_RHI=vulkan`). Nur diese beiden Verzeichnisse sprechen mit
einer Grafik-API. Ausgewählt wird beim Erzeugen des Fensters über `WindowConfig::backend`.

Das wird vom Build erzwungen, nicht nur verabredet: `glad` hängt **privat** an der
Engine. Wer außerhalb von `engine/src/rhi/gl/` `<glad/gl.h>` einbindet, bekommt einen
Compilerfehler. Ebenso hängen volk und glslang privat an der Engine. `tools/check-no-gl.sh`
sucht zusätzlich nach `gl*`- und `vk*`-Aufrufen, Enums und Typen außerhalb ihrer Backends.

## Aufbau

```
engine/include/fitzel/rhi/Types.hpp    Handles, Formate, Sampler, Render-State, Caps
engine/include/fitzel/rhi/Device.hpp   das Interface + device()/createDevice()
engine/src/rhi/Rhi.cpp                 Format-Tabellen, aktuelles Device, Null-Device, Backend-Auswahl
engine/src/rhi/gl/GlDevice.{hpp,cpp}   OpenGL: State, Bindings, Draws, Queries, Fenster, ImGui
engine/src/rhi/gl/GlResources.cpp      OpenGL: Buffer, Texturen, Framebuffer, Programme, Vertex-Arrays
engine/src/rhi/gl/GlEnums.hpp          rhi-Enums -> GL-Enums
engine/src/rhi/vk/                     das Vulkan-Backend (siehe unten)
engine/src/rhi/Pool.hpp                die Slot-Tabelle beider Backends
engine/include/fitzel/ui/ImGuiTexture.hpp   fitzel::imTexture(handle) für ImGui::Image
```

Darüber liegt die **Fassade** der Engine, wie bisher: `Shader`, `Texture`, `Texture3D`,
`Mesh`, `InstancedMesh`, `RenderTarget`, `ShadowMap`, `CascadedShadowMap`, `CubeShadowMap`,
`CubeRenderTarget`, `EnvironmentIBL`, `Renderer`, `Gui`, `Window`. Sie sprechen nur noch mit
`rhi::device()`. Editor und Werkzeuge benutzen in erster Linie diese Klassen. Wo sie selbst
zeichnen (Vegetation, Post-Chain, Wasser, Pfadtracer …), rufen sie das Device direkt auf.

`rhi::device()` liefert das Device des zuletzt erzeugten Fensters. Ohne Fenster antwortet
ein **Null-Device**, das nichts tut. Eine Textur, die ihr Fenster beim Beenden überlebt,
wird so ins Leere freigegeben und nicht in einen Absturz.

## Das Modell

Die Schnittstelle ist so geschnitten, dass sie sich auf OpenGL **und** auf Vulkan
abbilden lässt, ohne dass sich die Aufrufer ändern:

| Konzept | API | OpenGL | Vulkan |
|---|---|---|---|
| Ressourcen | typisierte Handles (`TextureHandle`, `BufferHandle`, …), Slots in Tabellen des Backends, nie API-Namen | GL-Namen in `Pool<>` | `VkImage`/`VkBuffer` + Speicher in eigenen Pools |
| Erzeugen | ganz, aus einer Beschreibung (`TextureDesc`, `FramebufferDesc`, `VertexArrayDesc`) — nichts wird „zum Bearbeiten gebunden“ | Bind auf Scratch-Target bzw. aktive Unit, danach zurück | direkt |
| Sampler | Teil der Textur (`SamplerDesc`), änderbar mit `setSampler` | `glTexParameter` | ein `VkSampler` je unterschiedlicher Beschreibung, gecacht |
| Render-State | `RenderState` (Blend, Depth, Stencil, Raster, ColorMask), einzeln oder als Ganzes setzbar, **vom Device gemerkt** und abfragbar | sofort, mit Redundanzfilter | zusammen mit Programm, Vertex-Layout und Framebuffer-Formaten Schlüssel einer Pipeline (`VkPipeline`-Cache; Viewport/Scissor/Stencil-Ref dynamisch) |
| Framebuffer | Anhänge aus Texturen (Ebene, Layer, Würfelseite); `bindFramebuffer` + `clear` | FBO | Render Pass/Dynamic Rendering, beim ersten Draw bzw. Clear nach einem Bind lazy begonnen und beim nächsten Bind/Readback beendet; `clear` als Load-Op oder `vkCmdClearAttachments` |
| Vertex-Eingabe | `VertexArray` = Attribute + Buffer + Index-Buffer, konstante Attribute als `ConstantAttribute` | VAO, `glVertexAttrib4f` | Vertex-Input-State der Pipeline + Bindings; Konstanten als Buffer mit Stride 0 |
| Shader | GLSL, wie die Shaderdateien geschrieben sind (`#version 330 core`, 430 für Compute), Uniforms nach Name, Sampler über Texture-Units | direkt; GLSL ES 3.00 im Browser | glslang mit *relaxed Vulkan rules* (`GL_EXT_vulkan_glsl_relaxed`): lose Uniforms → ein Default-Uniform-Block, Sampler → Combined-Image-Sampler mit automatischen Bindings; Reflection (SPIRV-Reflect/-Cross) liefert Offsets; je Draw ein Ring-Buffer-Ausschnitt mit den aktuellen Werten |
| Uniforms | `uniformLocation` + `setUniform(program, loc, …)`, Werte bleiben am Programm | `glUniform*` | CPU-Kopie des Default-Blocks je Programm, Location = Index in die Reflection |
| Texture-Units | `bindTexture(unit, tex)`; ein Sampler-Uniform hat die Unit als Wert | Texture-Units | Descriptor-Set je Draw aus den Units, die das Programm benutzt (oder Descriptor Indexing) |
| Readbacks | `readPixels`, `readTexture`, `readBuffer`; asynchron `readPixelsToBuffer` + `createFence` | `glReadPixels`, PBO, `glFenceSync` | Copy in einen Host-sichtbaren Buffer, Fence/Timeline-Semaphore |
| Queries | `QueryType::TimeElapsed/Timestamp/AnySamplesPassed` | GL-Queries | `VkQueryPool` (Timestamp, Occlusion) |
| Compute | `dispatch`, `bindImage`, `bindBufferBase(Storage)`, `memoryBarrier` | 4.3 core | Compute-Pipeline, Storage-Image/-Buffer-Descriptoren, Pipeline-Barrieren |
| UI | `imguiInit/NewFrame/Render`, `imguiTextureId` | `imgui_impl_opengl3` | `imgui_impl_vulkan`, Texture-ID = `VkDescriptorSet` |
| Fenster | `prepareWindowHints`, `createDevice(window)`, `present`, `activate` | GL-Kontext, `glfwSwapBuffers` | `GLFW_NO_API`, Surface + Swapchain, `vkQueuePresentKHR` |

Zwei Dinge, die OpenGL abschaltbar macht und andere APIs einfach tun, sind immer an:
`gl_PointSize` aus dem Vertex-Shader und nahtlos gefilterte Cube-Maps.

Was die Semantik festlegt, steht in den Kommentaren von `Device.hpp`. Ein paar Punkte,
die man beim Lesen von altem Code kennen muss:

- **Es gibt keine aktive Texture-Unit** und kein „Binden zum Bearbeiten“. Erzeugen,
  Hochladen und Sampler-Setzen ändern an keiner Unit, was dort gebunden ist.
- **Ein gebundener Framebuffer**, zum Zeichnen und Lesen. `blit`, `readPixels` und
  `copyToTexture` nehmen Quelle und Ziel explizit und stellen die Bindung selbst wieder her.
- **`clear`** beachtet Scissor und Write-Masks wie ein Draw (das OpenGL-Verhalten). Clear-Farbe,
  -Tiefe und -Stencil bleiben gesetzt, bis sie geändert werden.
- **Zeilen** bei Uploads und Readbacks sind immer dicht gepackt und byte-ausgerichtet.
  `TextureRegion::rowLength` beschreibt Teilrechtecke aus einem breiteren Bild. Readbacks von
  Framebuffern liefern die unterste Zeile zuerst.
- **Formate**: `Format` beschreibt sowohl, was eine Textur speichert, als auch, wie CPU-Daten
  liegen. Weicht das Datenformat ab (z. B. `RGB32F`-Daten in eine `RGB16F`-Textur), konvertiert
  das Backend. OpenGL macht das im Treiber, ein Backend ohne diese Fähigkeit auf der CPU.
- Eine Textur ohne `UsageSampled` (nur `UsageRenderTarget`) darf das Backend günstiger halten
  (OpenGL: Renderbuffer, Vulkan: transient attachment).

## Das Vulkan-Backend

`engine/src/rhi/vk/` ist die zweite Implementierung: Vulkan 1.3 (Dynamic Rendering,
Synchronization2, Extended Dynamic State), über volk zur Laufzeit geladen. Gelinkt wird
gegen nichts, ein Rechner ohne Vulkan-Treiber startet also weiterhin.

**Einschalten:** `FITZEL_RHI=vulkan` in der Umgebung, für jedes Fenster, das ein Programm
öffnet. Das gilt für Editor, Player und jedes Prüfwerkzeug gleichermaßen. Im Code geht es
über `WindowConfig::backend`. Ein Spiel wählt seine API in den Game Settings (Abschnitt
*Graphics*, Schlüssel `"renderer": "opengl" | "vulkan"` in der `game.json` des Projekts). Der
Export schreibt sie in die Boot-`game.json` neben dem Spiel, ein Web-Export immer `opengl`.
`--play <projekt>` liest sie ebenfalls. Der Editor selbst bleibt bei OpenGL, solange
`FITZEL_RHI` nichts anderes sagt. Die Umgebungsvariable hat immer Vorrang. Startet Vulkan
nicht (kein Loader, keine GPU mit 1.3), meldet das Fenster den Grund und fällt auf OpenGL
zurück. `FITZEL_VK_DEVICE=<Namensteil>` wählt die
GPU. `FITZEL_VK_VALIDATION=1` schaltet die Khronos Validation Layer zu, wo sie installiert
sind oder über `VK_LAYER_PATH` gefunden werden. Warnungen und Fehler landen dann auf stderr.
Gebaut wird es mit `FITZEL_VULKAN=ON` (Standard auf dem Desktop, im Browser aus).

| Datei | Inhalt |
|---|---|
| `VulkanDevice.hpp/.cpp` | Instanz, GPU, Swapchain, Frames, Submission, Pipelines, Deskriptoren, Draws, Queries, Fences |
| `VkResources.cpp` | Buffer, Texturen, Sampler, Framebuffer, Vertex-Arrays, Kopien und Readbacks |
| `VkShaders.cpp` | GLSL → SPIR-V (glslang), Reflection (SPIRV-Reflect), Uniforms |
| `VkImGui.hpp/.cpp` | der ImGui-Renderer, als Client des Device |
| `VkFormats.cpp` | Formattabelle, CPU-Konvertierung der Pixeldaten |

So wird das OpenGL-artige Modell zu Vulkan:

- **Ein Command Buffer pro Frame**, aufgezeichnet in der Reihenfolge der Aufrufe. Ein
  Framebuffer-Bind öffnet einen Rendering-Scope erst beim ersten Draw oder Clear. Was nicht
  in einen Scope darf (Upload, Kopie, Dispatch, Readback), schließt ihn, und der nächste Draw
  öffnet ihn mit LOAD wieder. Eine Timeline-Semaphore zählt jede Submission. Fences, Queries,
  Readbacks und das verzögerte Freigeben hängen an ihr.
- **Jedes Bild lebt in `VK_IMAGE_LAYOUT_GENERAL`.** Layout-Übergänge gibt es keine, nur die
  Swapchain wechselt das Layout. Gefahren deckt eine volle Speicherbarriere zwischen Segmenten
  ab (Rendering-Scope, Transfer-Folge, Dispatch). Das ist grob, aber richtig. Zwei Transfers
  auf dieselbe Ressource bekommen eine Barriere dazwischen.
- **Alle Buffer liegen device-local.** Jedes Schreiben (`createBuffer` mit Daten,
  `bufferData`, `bufferSubData`) ist eine Staging-Kopie im Command Buffer. Die Reihenfolge
  stimmt also mit OpenGL überein: Draws davor lesen den alten Inhalt. Readback-Buffer sind
  host-sichtbar.
- **Pipelines** werden aus dem gemerkten State gebaut und gecacht. In den Schlüssel gehen
  Programm, Vertex-Layout, Blend, Farbmaske, Polygonmodus, Topologie und
  Attachment-Formate. Alles andere ist dynamischer State.
- **Shader** bleiben die GLSL-Dateien der Engine. glslang übersetzt unter
  `GL_EXT_vulkan_glsl_relaxed`: Die losen Uniforms eines Programms werden ein std140-Block
  (`fz_Default`, Binding 64), dessen CPU-Kopie `setUniform` beschreibt und den jeder Draw in
  einen Ring-Buffer lädt (dynamischer Offset). Sampler werden Combined Image Sampler, gespeist
  aus den Texture-Units. Bindings und Locations vergibt glslangs GLSL-IO-Mapper für das ganze
  Programm. Pro Stufe getrennt vergeben, landete ein Sampler des Vertex-Shaders auf dem
  Binding eines Fragment-Sampler.
- **Deskriptor-Sets** werden pro Frame nach Inhalt gecacht. Eine Folge von Draws mit denselben
  Texturen bindet ein einziges Set, nur der Uniform-Offset wandert.
- **Das Bild steht so herum wie unter OpenGL:** Zeile 0 im Speicher ist der untere Rand.
  Render-Targets, Readbacks und `gl_FragCoord` stimmen damit überein. Erst beim Kopieren in
  die Swapchain wird gespiegelt. Das Fenster-Framebuffer sind zwei Farbbilder im Wechsel
  (das zuletzt gezeigte liest `kReadFront`) plus Depth-Stencil.
- **ImGui** zeichnet ein eigener kleiner Renderer über das Device, mit Projektion, Scissor
  und Blend wie im OpenGL3-Renderer und den Texture-Updates aus ImGui 1.92.

Was beim Abgleich mit OpenGL aufgefallen ist und jetzt im Backend steht:

- **Wicklung:** Vulkan rechnet die Fläche eines Dreiecks mit nach unten laufendem y. Bei
  gleicher Zeilenlage ist OpenGLs „gegen den Uhrzeigersinn“ daher Vulkans „im Uhrzeigersinn“
  (`VK_FRONT_FACE_CLOCKWISE`).
- **Tiefe:** OpenGLs Clip-z läuft von -1 bis 1, Vulkans von 0 bis 1. Jeder Vertex-Shader
  bekommt eine `main()`-Hülle, die `gl_Position.z` umrechnet. Projektionsmatrizen und
  Tiefenwerte bleiben die des OpenGL-Backends.
- **Clip-Distanzen:** Unter OpenGL schneiden sie nur bei `GL_CLIP_DISTANCEi`, unter Vulkan
  immer, sobald der Shader sie schreibt. Die Hülle schreibt sie deshalb über eine Maske
  (`fz_ClipMask`), die das Device aus dem Raster-State füllt.
- **FMA:** Die Value-Noise der Shader hasht Gitterpunkte über `fract(p * 123.34)` u. ä. Fasst
  der Treiber eine Multiplikation und eine Addition zu einem FMA zusammen, bekommt dieselbe
  Zellenecke von zwei Seiten verschiedene Werte, und das Rauschen reißt an jeder Zellkante.
  Jede Fließkomma-Operation wird darum mit `NoContraction` dekoriert. Auf die Bits genau gleich
  wie unter OpenGL ist das Ergebnis trotzdem nicht: NVIDIAs GL-Compiler zieht in manchen
  Shadern zusammen und in anderen nicht. Hash-Muster (welches Fenster eines Hochhauses
  leuchtet, Wolkenrauschen) können sich deshalb unterscheiden, sichtbare Fehler entstehen
  dabei nicht.

**Stand der Prüfung** (RTX 3080, Linux): Alle Prüfwerkzeuge laufen unter Vulkan mit denselben
Exit-Codes wie unter OpenGL, die Validation Layer melden nichts. Material-Vorschau,
`viewcheck`, Nebel, Fluss, Captures und die Panels sind bis auf wenige LSB oder bis auf
Hash-Muster identisch. Der GPU-Pathtracer (Compute) besteht `gpucheck` und `tracecheck`.
Editor und Play-Modus (`die_insel`, `roads`) sehen gleich aus. Die GPU braucht pro Frame
weniger Zeit als unter OpenGL, das Abschicken auf der CPU noch mehr.

**Bekannte Grenzen:**

- `readPixelsToBuffer` kopiert Bytes und kann keine Formate wandeln. Das Datenformat muss das
  des Bildes sein.
- Ohne D32S8 nimmt das Backend D24S8, und dessen Tiefe liest sich nicht als float zurück.
- Ein Framebuffer-Anhang mit `layer = -1` an einem Array oder Cube (layered rendering) zeichnet
  in Layer 0.
- Randfarben: Vulkan kennt nur transparent schwarz, schwarz und weiß. Die nächstliegende wird
  genommen.
- Die CPU-Seite ist noch nicht optimiert. Jeder Draw kostet einen Pipeline-Lookup, einen
  Deskriptor-Lookup und eine Kopie des Uniform-Blocks.
- Wird der Treiber-Shader-Cache nicht genutzt, kosten neue Pipelines beim ersten Mal Zeit. Es
  gibt einen `VkPipelineCache`, aber noch keinen auf der Platte.

### Ein weiteres Backend

Dasselbe Muster: eine Unterklasse von `Device` in `engine/src/rhi/<api>/`, ein Wert in
`Backend`, zwei Zeilen in den `switch`-Anweisungen von `Rhi.cpp`, die Quellen in
`engine/CMakeLists.txt`. Die Vulkan-Implementierung zeigt, wie sich jede Semantik von
`Device.hpp` ohne OpenGL umsetzen lässt.

## Regeln für neuen Code

- Nie `<glad/gl.h>` außerhalb von `engine/src/rhi/gl/`. GPU-Arbeit geht über die Engine-Klassen
  oder `rhi::device()`.
- Handles statt Zahlen: `rhi::TextureHandle m_tex;` nicht `std::uint32_t m_tex;`.
- ImGui-Bilder mit `fitzel::imTexture(handle)`.
- State, der für einen Pass geändert wird, zurücksetzen: `const rhi::RenderState saved =
  gpu.renderState(); … gpu.setRenderState(saved);`. Das Abfragen kostet nichts, das Device kennt den
  Wert.
- Ein Feature, das Compute braucht, fragt `rhi::device().caps().compute`.
- Ein Testwerkzeug mit eigenem, unsichtbarem Kontext nimmt `fitzel::Window` mit
  `WindowConfig::visible = false`.
