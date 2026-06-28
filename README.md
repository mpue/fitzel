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
    ├── src/main.cpp        # textured, lit cube with a fly camera
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
- `fitzel::Gui`     — Dear ImGui context + GLFW/OpenGL3 backends; call ImGui:: directly.

The sandbox demonstrates all of these: a textured, lit cube you can fly around
with a live ImGui panel (FPS, light direction, rotation speed, background color,
wireframe). Controls: WASD + Q/E to move, hold right mouse to look, scroll to
zoom, ESC to quit.

Add new subsystems under `engine/src/` and their headers under
`engine/include/fitzel/`, then list the sources in `engine/CMakeLists.txt`.
Natural next steps: a material/render abstraction, model loading (glTF/OBJ),
and a scene graph.
