# Fetches all third-party dependencies at configure time so the project is
# self-contained and portable -- no system-wide installs required.
include(FetchContent)

# Keep dependencies quiet and reproducible.
set(FETCHCONTENT_QUIET OFF)

# --- GLFW: windowing, input and the OpenGL context -------------------------
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    ON
)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)

# --- GLM: header-only math library -----------------------------------------
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    ON
)

# --- GLAD2: OpenGL function loader (generated at configure time) ------------
FetchContent_Declare(
    glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad.git
    GIT_TAG        v2.0.8
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  cmake
)

# --- stb: single-header image loading --------------------------------------
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
    GIT_SHALLOW    ON
)

# --- Dear ImGui: immediate-mode debug UI -----------------------------------
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking          # docking lives on this branch, not the release tags
    GIT_SHALLOW    ON
)

# --- ImGuizmo: 3D transform gizmos for the editor viewport -----------------
FetchContent_Declare(
    imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG        master
    GIT_SHALLOW    ON
)

# --- ImGuiColorTextEdit: syntax-highlighting code editor (Lua script editor) -
FetchContent_Declare(
    imcolortextedit
    GIT_REPOSITORY https://github.com/BalazsJako/ImGuiColorTextEdit.git
    GIT_TAG        master
    GIT_SHALLOW    ON
)

# --- tinyexr: load OpenEXR (.exr) images (e.g. PBR normal maps) -------------
# We only need tinyexr's headers + bundled miniz (we build our own target
# below). tinyexr's own CMakeLists compiles with `-Weverything -Werror`, which
# breaks on recent clang (e.g. -Wpoison-system-directories on arm64). Point
# SOURCE_SUBDIR at a non-existent dir so MakeAvailable only populates the
# sources and never processes tinyexr's CMake target.
FetchContent_Declare(
    tinyexr
    GIT_REPOSITORY https://github.com/syoyo/tinyexr.git
    GIT_TAG        v1.0.8
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  do-not-build
)

# --- miniaudio: single-header cross-platform audio playback ----------------
FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG        0.11.21
    GIT_SHALLOW    ON
)

# --- cgltf: single-header glTF / GLB loader ---------------------------------
FetchContent_Declare(
    cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG        v1.14
    GIT_SHALLOW    ON
)

# --- assimp: Collada (.dae) import, incl. skeletons + animations ------------
# Built lean: only the Collada importer, no exporters/tools/tests, static lib,
# bundled zlib. This is the heavy dependency (long first build) that gives
# robust .dae parsing with bones and animation clips.
FetchContent_Declare(
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG        v5.4.3
    GIT_SHALLOW    ON
)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_COLLADA_IMPORTER         ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER             ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT            ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS         OFF  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS  OFF  CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL             OFF  CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS  OFF  CACHE BOOL "" FORCE)
# assimp's bundled zlib (zutil.h) redefines fdopen() to NULL on Apple because
# TargetConditionals.h always defines TARGET_OS_MAC=1 -- that macro predates the
# NeXT/OS X split and here it wrongly triggers the "classic Mac, no fdopen"
# path, which then clashes with the SDK's <stdio.h> and fails to compile on
# recent clang. macOS ships zlib in the SDK, so use the system copy there and
# keep the bundled build on Windows (which has no system zlib).
if(APPLE)
    set(ASSIMP_BUILD_ZLIB      OFF  CACHE BOOL "" FORCE)
else()
    set(ASSIMP_BUILD_ZLIB      ON   CACHE BOOL "" FORCE)
endif()
set(ASSIMP_BUILD_DRACO         OFF  CACHE BOOL "" FORCE)
set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE BOOL "" FORCE)

# --- Lua 5.4: entity scripting (plain C API, compiled from source) ----------
FetchContent_Declare(
    lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG        v5.4.7
    GIT_SHALLOW    ON
)

# --- nlohmann/json: header-only JSON for asset .meta files and (later) scenes -
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    ON
)
set(JSON_BuildTests OFF CACHE INTERNAL "")

# --- Jolt Physics: rigid-body dynamics (its CMake lives in the Build/ subdir) -
FetchContent_Declare(
    jolt
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG        v5.2.0
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  Build
)
# Build only the library, and match our dynamic MSVC runtime (/MD); Jolt would
# otherwise default to the static runtime (/MT) and fail to link.
set(TARGET_UNIT_TESTS            OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD           OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST      OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES               OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER                OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS          OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)

set(TINYEXR_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
# assimp (and its bundled zlib) predate CMake 4's dropped <3.5 compatibility;
# allow their older cmake_minimum_required to still configure.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
# Build assimp as a static lib with the dynamic MSVC runtime (/MD), matching the
# rest of the project (Jolt is configured the same way).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw glm glad stb imgui tinyexr miniaudio cgltf lua nlohmann_json jolt assimp)

# ImGuizmo: fetch the sources only (its own CMakeLists would clash with our imgui
# target), then compile ImGuizmo.cpp into the imgui library below.
FetchContent_GetProperties(imguizmo)
if(NOT imguizmo_POPULATED)
    FetchContent_Populate(imguizmo)
endif()

# ImGuiColorTextEdit: sources only (compiled into the imgui library below).
FetchContent_GetProperties(imcolortextedit)
if(NOT imcolortextedit_POPULATED)
    FetchContent_Populate(imcolortextedit)
endif()

# Generate a GLAD loader for OpenGL 3.3 Core. Produces the target `glad_gl_core_33`.
glad_add_library(glad_gl_core_33 REPRODUCIBLE API gl:core=3.3)

# Expose stb headers as an interface target (implementation TU lives in engine/).
add_library(stb_headers INTERFACE)
target_include_directories(stb_headers INTERFACE ${stb_SOURCE_DIR})

# tinyexr + its bundled miniz. The TINYEXR_IMPLEMENTATION TU lives in engine/.
add_library(tinyexr_dep STATIC ${tinyexr_SOURCE_DIR}/deps/miniz/miniz.c)
target_include_directories(tinyexr_dep PUBLIC
    ${tinyexr_SOURCE_DIR}
    ${tinyexr_SOURCE_DIR}/deps/miniz)

# miniaudio headers (the MINIAUDIO_IMPLEMENTATION TU lives in engine/).
add_library(miniaudio_dep INTERFACE)
target_include_directories(miniaudio_dep INTERFACE ${miniaudio_SOURCE_DIR})

# cgltf headers (the CGLTF_IMPLEMENTATION TU lives in engine/).
add_library(cgltf_dep INTERFACE)
target_include_directories(cgltf_dep INTERFACE ${cgltf_SOURCE_DIR})

# Lua static library (the repo ships no CMakeLists; compile its sources directly,
# excluding the standalone interpreter, the amalgam and the internal test file).
file(GLOB LUA_SOURCES ${lua_SOURCE_DIR}/*.c)
list(REMOVE_ITEM LUA_SOURCES
    ${lua_SOURCE_DIR}/lua.c
    ${lua_SOURCE_DIR}/onelua.c
    ${lua_SOURCE_DIR}/ltests.c)
add_library(lua_dep STATIC ${LUA_SOURCES})
target_include_directories(lua_dep PUBLIC ${lua_SOURCE_DIR})

# Build Dear ImGui (core + GLFW/OpenGL3 backends) as a static library. The
# OpenGL3 backend ships its own GL loader, so it doesn't clash with GLAD.
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp        # 3D transform gizmos
    ${imcolortextedit_SOURCE_DIR}/TextEditor.cpp   # syntax-highlighting code editor
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imguizmo_SOURCE_DIR}/src
    ${imcolortextedit_SOURCE_DIR}
)
target_link_libraries(imgui PUBLIC glfw)
