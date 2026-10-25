# Fitzel — Render Hardware Interface (`fitzel::rhi`)

Jeder Aufruf an die Grafikkarte geht durch **ein** Interface: `fitzel::rhi::Device`
(`engine/include/fitzel/rhi/Device.hpp`, Vokabular in `Types.hpp`). Die einzige
Implementierung heute ist OpenGL (`engine/src/rhi/gl/`). Sie ist der **einzige** Code
im Baum, der `<glad/gl.h>` einbindet oder `gl*` aufruft. Ein Vulkan-Backend ist eine
zweite Unterklasse daneben (`engine/src/rhi/vk/`), ausgewählt beim Erzeugen des Fensters
über `WindowConfig::backend`.

Das wird vom Build erzwungen, nicht nur verabredet: `glad` hängt **privat** an der
Engine. Wer außerhalb von `engine/src/rhi/gl/` `<glad/gl.h>` einbindet, bekommt einen
Compilerfehler. `tools/check-no-gl.sh` sucht zusätzlich nach `gl*`-Aufrufen, `GL_*`-Enums
und GL-Typen außerhalb des Backends.

## Aufbau

```
engine/include/fitzel/rhi/Types.hpp    Handles, Formate, Sampler, Render-State, Caps
engine/include/fitzel/rhi/Device.hpp   das Interface + device()/createDevice()
engine/src/rhi/Rhi.cpp                 Format-Tabellen, aktuelles Device, Null-Device, Backend-Auswahl
engine/src/rhi/gl/GlDevice.{hpp,cpp}   OpenGL: State, Bindings, Draws, Queries, Fenster, ImGui
engine/src/rhi/gl/GlResources.cpp      OpenGL: Buffer, Texturen, Framebuffer, Programme, Vertex-Arrays
engine/src/rhi/gl/GlEnums.hpp          rhi-Enums -> GL-Enums
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

## Ein Vulkan-Backend daneben setzen

1. `Backend::Vulkan` in `Types.hpp` ergänzen.
2. `engine/src/rhi/vk/VkDevice.{hpp,cpp}` mit einer Klasse `VkDevice final : public Device`
   und den freien Funktionen `vk::prepareWindowHints(int)` (`GLFW_CLIENT_API = GLFW_NO_API`) und
   `vk::createDevice(GLFWwindow*, bool vsync)` anlegen. Vorlage für die Struktur (Pools, Shadow-State)
   ist `GlDevice`; die Tabelle oben sagt, was jeder Aufruf wird.
3. In `Rhi.cpp` die beiden `switch`-Anweisungen um `Backend::Vulkan` erweitern.
4. In `engine/CMakeLists.txt` die Quellen und die Abhängigkeiten (Vulkan-Loader, glslang,
   SPIRV-Reflect, `imgui_impl_vulkan`) hinzufügen, über `cmake/Dependencies.cmake` wie den Rest.
5. Auswahl: `WindowConfig::backend` (z. B. über eine Umgebungsvariable oder die
   Grafikeinstellungen in `sandbox/src/Startup.cpp`).

Nichts in `sandbox/` oder in den Engine-Klassen muss sich dafür ändern. Die Shader bleiben
GLSL-Dateien. Was ein Vulkan-Backend an ihnen braucht (Locations für Ein-/Ausgänge, Bindings),
vergibt glslang automatisch (`--auto-map-locations --auto-map-bindings`).

Was dabei Arbeit macht und wo sie hingehört:

- **Pipeline-Cache**: Schlüssel = `RenderState` + Programm + Vertex-Layout des gebundenen
  `VertexArray` + Farb-/Tiefenformate des Framebuffers + Primitive. Der Device-State ist dafür
  vollständig vorhanden, weil er gemerkt und nicht in die API „durchgereicht“ wird.
- **Render Passes**: Der Editor bindet Framebuffer und löscht sie wie unter OpenGL. Ein Pass
  beginnt mit dem ersten Draw oder Clear nach einem Bind und endet beim nächsten Bind, Readback,
  Blit oder `present`. Mit `VK_KHR_dynamic_rendering` braucht es dafür keine `VkRenderPass`-Objekte.
- **Uniforms**: je Programm eine CPU-Kopie des Default-Blocks; bei jedem Draw, dessen Kopie sich
  geändert hat, ein neuer Ausschnitt in einem Ring-Buffer (dynamischer UBO-Offset).
- **Hazards**: Layout-Übergänge und Barrieren aus der Nutzung ableiten (eine Textur, die gerade
  Attachment war, wird beim nächsten `bindTexture` + Draw `SHADER_READ_ONLY`).

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
