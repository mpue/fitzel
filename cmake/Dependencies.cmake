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
    GIT_TAG        v1.91.5
    GIT_SHALLOW    ON
)

# --- tinyexr: load OpenEXR (.exr) images (e.g. PBR normal maps) -------------
FetchContent_Declare(
    tinyexr
    GIT_REPOSITORY https://github.com/syoyo/tinyexr.git
    GIT_TAG        v1.0.8
    GIT_SHALLOW    ON
)

set(TINYEXR_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw glm glad stb imgui tinyexr)

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
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw)
