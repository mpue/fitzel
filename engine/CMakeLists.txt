add_library(fitzel STATIC
    src/core/Window.cpp
    src/core/Input.cpp
    src/graphics/Shader.cpp
    src/graphics/Mesh.cpp
    src/graphics/Texture.cpp
    src/graphics/stb_image_impl.cpp
    src/scene/Camera.cpp
    src/ui/Gui.cpp
)

# Public headers live under include/, exposed as <fitzel/...>.
target_include_directories(fitzel
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fitzel
    PUBLIC
        glfw
        glad_gl_core_33
        glm::glm
        imgui
    PRIVATE
        stb_headers
)

target_compile_features(fitzel PUBLIC cxx_std_20)

# Enable a reasonable warning level across compilers.
if(MSVC)
    target_compile_options(fitzel PRIVATE /W4)
else()
    target_compile_options(fitzel PRIVATE -Wall -Wextra -Wpedantic)
endif()
