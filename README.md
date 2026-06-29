# Fitzel

A portable OpenGL starter that serves as the base for a graphics engine.
Cross-platform (Windows / Linux / macOS), self-contained, modern C++20.

## Stack

| Concern            | Library                | How it's pulled in        |
| ------------------ | ---------------------- | ------------------------- |
| Windowing / input  | [GLFW](https://www.glfw.org/) 3.4 | CMake `FetchContent` |
| OpenGL loader      | [GLAD2](https://github.com/Dav1dde/glad) (gl 3.3 core) | `FetchContent` (generated at configure time) |
| Math               | [GLM](https://github.com/g-truc/glm) 1.0.1 | `FetchContent` |
| Image loading      | [stb_image](https://github.com/nothings/stb) | `FetchContent` |
| Debug UI           | [Dear ImGui](https://github.com/ocornut/imgui) 1.91.5 | `FetchContent` |

All dependencies are fetched at configure time — **no system-wide installs
required**. The first configure/build needs a network connection and Python 3
with Jinja2 (used by GLAD to generate the loader):

```sh
python -m pip install jinja2
```

## Layout

```
fitzel/
├── CMakeLists.txt          # top-level build
├── CMakePresets.json       # ready-made configure/build presets
├── cmake/Dependencies.cmake# FetchContent for GLFW / GLAD / GLM
├── engine/                 # the engine, built as a static lib `fitzel`
│   ├── include/fitzel/     # public API (<fitzel/...>)
│   └── src/                # implementation
└── sandbox/                # example app linking the engine
    ├── src/main.cpp        # streamed infinite terrain + CSM + materials
    └── assets/shaders/     # GLSL shaders (copied next to the binary)
```

## Build

### With presets (recommended)

```sh
cmake --preset default        # configure (Ninja, Debug)
cmake --build --preset default
./build/default/bin/sandbox   # run (sandbox.exe on Windows)
```

Other presets: `release` (Ninja, optimized) and `vs` (Visual Studio solution).

### Plain CMake

```sh
cmake -S . -B build
cmake --build build
```

On Windows the bundled CMake/Ninja/compiler ship with Visual Studio — run the
commands from a *Developer PowerShell for VS* so the toolchain is on `PATH`.

## Extending the engine

The engine exposes a small, RAII-based core to build on:

- `fitzel::Window`  — GLFW window + OpenGL 3.3 context, frame loop helpers.
- `fitzel::Input`   — per-frame keyboard/mouse polling, mouse delta, cursor lock.
- `fitzel::Shader`  — compile/link GLSL, set uniforms (incl. GLM types).
- `fitzel::Mesh`    — VAO/VBO/EBO wrapper with interleaved `Vertex` data; `Mesh::cube()`.
- `fitzel::Texture` — 2D textures from file (stb), raw pixels, or a checkerboard.
- `fitzel::Camera`  — first-person fly camera producing view/projection matrices.
- `fitzel::Material` — a `Shader` plus named uniform/texture parameters (`apply()`).
- `fitzel::Renderer` — forward renderer driving cascaded shadows + a lit pass over
  submitted `(mesh, material, model)` tuples, with per-pass frustum culling.
- `fitzel::CascadedShadowMap` — frustum-split directional shadows in a depth array.
- `fitzel::ShadowMap` — single-cascade depth FBO (simpler alternative to CSM).
- `fitzel::RenderTarget` — off-screen color+depth FBO for render-to-texture passes.
- `fitzel::Terrain` — `TerrainStreamer` streams an infinite, seamless fBm terrain
  as `TerrainChunk`s around the camera; `terrainHeight()` queries the field.
- `fitzel::Gui`     — Dear ImGui context + GLFW/OpenGL3 backends; call ImGui:: directly.

The sandbox ties it together: an **infinite, streamed procedural landscape** under a
**day/night sky with volumetric clouds**, lit by a directional sun with **cascaded
shadow mapping** (PCF) and **Blinn-Phong** shading, cubes that cast shadows onto the
terrain, **atmospheric fog** (aerial perspective), and a **planar-reflective water
plane** (reflecting sky + clouds) flooding the valleys. The renderer exposes both a one-call path
(`begin()` → `submit()` → `end()`) and multi-pass building blocks
(`prepareShadows()` + `renderScene(view, proj, eye, clipPlane)`) used to render the
reflection/refraction passes. A live ImGui panel tweaks the light, cascade split,
water (level/waves/tint) and regenerates the terrain. Controls: WASD + Q/E to move,
hold right mouse to look, scroll to zoom, ESC to quit.

### Rendering notes

- **Colour management & post**: the scene renders linear into an HDR (RGBA16F)
  buffer; a composite pass adds **bloom**, **god rays** (radial march from the sun's
  screen position) and an analytic **lens flare**, then applies **ACES filmic
  tonemapping** + exposure + gamma. Authored sRGB colours are linearised on use and
  the sun is an HDR radiance, so highlights bloom and the sun reads as a sun.
- **Terrain texturing**: four PBR albedo sets (sand / rocky ground / cliff / snow)
  are **triplanar-mapped** (projected on the three world axes, blended by the normal)
  so steep faces don't stretch, and blended by height + slope.
- **Water**: planar reflection/refraction (rendered at half-res, distortion hides it)
  with multi-octave animated normals, **Schlick Fresnel**, depth-tinted refraction and
  a sharp HDR sun glint that the bloom picks up.
- On laptops the app exports `NvOptimusEnablement` so it runs on the discrete GPU.

### Terrain textures (not in the repo)

The terrain albedo textures are large and **git-ignored**. Drop these 4K diffuse
JPGs from [Poly Haven](https://polyhaven.com/textures) into `textures/`:
`coast_sand_01_diff_4k.jpg`, `aerial_rocks_01_diff_4k.jpg`,
`rocky_terrain_02_diff_4k.jpg`, `snow_02_diff_4k.jpg`. CMake injects the folder path
at build time (`FITZEL_TEXTURE_DIR`).

- **Cascaded shadows**: the camera frustum is split into 4 depth ranges (practical
  log/uniform blend); each cascade is fit to its sub-frustum and rendered into a
  layer of a 2048² depth `GL_TEXTURE_2D_ARRAY`. The lit pass selects a cascade by
  view-space depth and samples it with a 3×3 PCF kernel + slope/cascade-scaled bias.
- **Terrain streaming**: chunks are generated from world-space noise, so neighbours
  tile seamlessly (shared edges sample the same continuous field, incl. normals).
  Generation runs on a **worker-thread pool** (CPU `MeshData` only); the render
  thread uploads a few finished chunks per frame, so crossing chunk borders never
  stalls the frame. A generation counter discards work made stale by a rebuild.
- **Frustum culling**: each pass extracts its 6 frustum planes (Gribb-Hartmann) and
  tests every submittable's world AABB, so the reflection/refraction/main passes each
  cull against their own frustum. The ImGui panel reports visible vs culled draws.
- **Terrain detail**: the height field combines domain warping (organic, non-grid
  shapes), a rolling fBm base, and a ridged-multifractal mountain layer masked onto
  the highlands. `lit.frag` adds close-up micro-detail via a procedural value-noise
  normal bump and albedo break-up.
- **Terrain colour** is procedural (sand → grass → rock → snow) by world height and
  slope — steep faces turn to rock, snow only settles on flat high ground.
- **Sky & day/night**: a fullscreen pass reconstructs the world view ray per pixel
  and shades a sun-driven sky gradient. The time of day rotates the sun, which drives
  the light direction, colour (warm at the horizon) and ambient — and the whole scene
  darkens into night. The same pass also feeds the water reflection (it runs with the
  mirrored view), so clouds reflect on the water.
- **Volumetric clouds**: the sky pass raymarches a cloud slab (3D value-noise fBm
  density with a height falloff), with a secondary light-march toward the sun for
  self-shadowing and a Henyey-Greenstein phase for the silver lining.
- **Atmospheric fog**: exponential height fog + aerial perspective applied in
  `lit.frag`/`water.frag`. Distant geometry fades into a horizon haze whose colour
  tracks the time of day (and warms toward the sun via in-scatter), giving depth and
  hiding the streaming edge; valleys and water pick up ground mist.
- **Water**: planar reflection + refraction. The scene is rendered twice off-screen
  (a mirror-matrix camera with the underwater half clipped for the reflection, the
  above-water half clipped for the refraction), then the water surface blends the two
  by Fresnel, with animated noise ripples distorting the projective lookups and a
  specular sun glint. A world-space clip plane (`uClipPlane` in `lit.vert`,
  `GL_CLIP_DISTANCE0`) drives the clipping.

Add new subsystems under `engine/src/` and their headers under
`engine/include/fitzel/`, then list the sources in `engine/CMakeLists.txt`.
Natural next steps: triplanar terrain texturing with real albedo maps, a
material/texture asset system, model loading (glTF/OBJ), and a scene graph.
