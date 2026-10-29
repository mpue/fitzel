add_library(fitzel STATIC
    src/core/Window.cpp
    src/core/Input.cpp
    src/core/LogCapture.cpp
    # The render hardware interface: the device every GPU call goes through,
    # and its one backend so far. src/rhi/gl is the only code in the tree that
    # includes an OpenGL header (see docs/rhi.md).
    src/rhi/Rhi.cpp
    src/rhi/gl/GlDevice.cpp
    src/rhi/gl/GlResources.cpp
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
    src/audio/SampleData.cpp
    src/audio/SamplePlayer.cpp
    src/audio/Mixer.cpp
    # VST3 plugins as mixer inserts; without FITZEL_VST3 it finds none.
    src/audio/Vst3Host.cpp
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
        glm::glm
        imgui
        Threads::Threads
    PRIVATE
        # OpenGL is the rhi backend's business (src/rhi/gl) and nobody else's:
        # private, so <glad/gl.h> is not even on the include path of anything
        # that links the engine (docs/rhi.md).
        glad_gl_core_43
        stb_headers
        tinyexr_dep
        miniaudio_dep
        cgltf_dep
        nlohmann_json::nlohmann_json
        Jolt
        assimp
)

# The VST3 host (cmake/Dependencies.cmake says where and why).
if(FITZEL_VST3)
    target_compile_definitions(fitzel PRIVATE FITZEL_VST3=1)
    target_link_libraries(fitzel PRIVATE vst3_pluginterfaces ${CMAKE_DL_LIBS})
endif()

# The Vulkan backend, next to the OpenGL one (FITZEL_VULKAN, desktop only).
if(FITZEL_VULKAN)
    target_sources(fitzel PRIVATE
        src/rhi/vk/VulkanDevice.cpp
        src/rhi/vk/VkResources.cpp
        src/rhi/vk/VkShaders.cpp
        src/rhi/vk/VkFormats.cpp
        src/rhi/vk/VkImGui.cpp
        src/rhi/vk/vma_impl.cpp)
    target_compile_definitions(fitzel PRIVATE FITZEL_RHI_VULKAN=1)
    # `VkFoo info{VK_STRUCTURE_TYPE_FOO}` -- the way Vulkan structs are made --
    # leaves the rest zeroed on purpose.
    if(NOT MSVC)
        set_source_files_properties(
            src/rhi/vk/VulkanDevice.cpp src/rhi/vk/VkResources.cpp src/rhi/vk/VkShaders.cpp
            src/rhi/vk/VkImGui.cpp
            PROPERTIES COMPILE_OPTIONS "-Wno-missing-field-initializers")
    endif()
    target_link_libraries(fitzel PRIVATE
        fitzel_vulkan_deps glslang glslang-default-resource-limits)
endif()

# In the browser the "loader" is the WebGL2 link flags, and those have to reach
# the executable.
if(EMSCRIPTEN)
    target_link_options(fitzel INTERFACE
        $<TARGET_PROPERTY:glad_gl_core_43,INTERFACE_LINK_OPTIONS>)
endif()

target_compile_features(fitzel PUBLIC cxx_std_20)

# Enable a reasonable warning level across compilers.
if(MSVC)
    target_compile_options(fitzel PRIVATE /W4)
else()
    target_compile_options(fitzel PRIVATE -Wall -Wextra -Wpedantic)
endif()
