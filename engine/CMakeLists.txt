add_library(fitzel STATIC
    src/core/Window.cpp
    src/core/Input.cpp
    src/graphics/Shader.cpp
    src/graphics/Mesh.cpp
    src/graphics/InstancedMesh.cpp
    src/graphics/Texture.cpp
    src/graphics/Material.cpp
    src/graphics/ShadowMap.cpp
    src/graphics/CascadedShadowMap.cpp
    src/graphics/CubeShadowMap.cpp
    src/graphics/CubeRenderTarget.cpp
    src/graphics/EnvironmentIBL.cpp
    src/graphics/RenderTarget.cpp
    src/graphics/stb_image_impl.cpp
    src/graphics/tinyexr_impl.cpp
    src/render/Renderer.cpp
    src/scene/Camera.cpp
    src/world/Terrain.cpp
    src/world/Model.cpp
    src/ui/Gui.cpp
    src/audio/Audio.cpp
    src/audio/miniaudio_impl.cpp
    src/asset/AssetId.cpp
    src/asset/AssetTypes.cpp
    src/asset/AssetDatabase.cpp
    src/physics/Physics.cpp
)

# Public headers live under include/, exposed as <fitzel/...>.
target_include_directories(fitzel
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

find_package(Threads REQUIRED)

target_link_libraries(fitzel
    PUBLIC
        glfw
        glad_gl_core_33
        glm::glm
        imgui
        Threads::Threads
    PRIVATE
        stb_headers
        tinyexr_dep
        miniaudio_dep
        cgltf_dep
        nlohmann_json::nlohmann_json
        Jolt
        assimp
)

target_compile_features(fitzel PUBLIC cxx_std_20)

# Enable a reasonable warning level across compilers.
if(MSVC)
    target_compile_options(fitzel PRIVATE /W4)
else()
    target_compile_options(fitzel PRIVATE -Wall -Wextra -Wpedantic)
endif()
