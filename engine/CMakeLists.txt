add_library(fitzel STATIC
    src/core/Window.cpp
    src/core/Input.cpp
    src/core/GlCaps.cpp
    src/core/LogCapture.cpp
    src/graphics/Shader.cpp
    src/graphics/Mesh.cpp
    src/graphics/InstancedMesh.cpp
    src/graphics/Texture.cpp
    src/graphics/Texture3D.cpp
    src/graphics/VideoTexture.cpp
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
    src/audio/Waveform.cpp
    src/audio/Mixer.cpp
    src/audio/miniaudio_impl.cpp
    # The modular synth: the sound-making primitives and the patch graph they
    # run in. Part of the runtime, not the editor -- a shipped game plays the
    # patches, it just cannot edit them.
    src/audio/SynthDsp.cpp
    src/audio/Synth.cpp
    src/audio/SynthVoice.cpp
    src/audio/SynthPoly.cpp
    src/audio/SynthPlayer.cpp
    src/audio/Midi.cpp
    # A song with a clock, a filter and an analyser tap, plus the band analysis
    # a rhythm game finds its beats in.
    src/audio/MusicPlayer.cpp
    src/asset/AssetId.cpp
    src/asset/AssetTypes.cpp
    src/asset/AssetDatabase.cpp
    src/asset/Pak.cpp
    src/asset/Vfs.cpp
    src/physics/Physics.cpp
)

# Public headers live under include/, exposed as <fitzel/...>. The generated
# build tree dir carries <fitzel/Version.hpp> (stamped from git by the root
# CMakeLists), so anything linking fitzel can ask for the version.
# The key .fpak archives are sealed with. Empty: the default in PakKey.hpp,
# which is in the public source -- anyone with the source opens archives packed
# with it. A private build passes its own, 64 hex characters; it is checked here
# and again at compile time, because a typo would otherwise become a different
# key and every archive packed before it would stop opening.
set(FITZEL_PAK_KEY_HEX "" CACHE STRING
    "64 hex characters: the key .fpak archives are sealed with (empty = the public default)")
if(FITZEL_PAK_KEY_HEX)
    string(LENGTH "${FITZEL_PAK_KEY_HEX}" _fzKeyLen)
    if(NOT _fzKeyLen EQUAL 64 OR NOT FITZEL_PAK_KEY_HEX MATCHES "^[0-9a-fA-F]+$")
        message(FATAL_ERROR "FITZEL_PAK_KEY_HEX must be exactly 64 hexadecimal characters")
    endif()
    set_source_files_properties(src/asset/Pak.cpp PROPERTIES
        COMPILE_DEFINITIONS "FITZEL_PAK_KEY_HEX=\"${FITZEL_PAK_KEY_HEX}\"")
    message(STATUS "fitzel: .fpak archives use the key from FITZEL_PAK_KEY_HEX")
else()
    message(STATUS "fitzel: .fpak archives use the PUBLIC default key "
                   "(set FITZEL_PAK_KEY_HEX for a private one)")
endif()

target_include_directories(fitzel
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
)
add_dependencies(fitzel fitzel_version)

find_package(Threads REQUIRED)

target_link_libraries(fitzel
    PUBLIC
        glfw
        glad_gl_core_43
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
