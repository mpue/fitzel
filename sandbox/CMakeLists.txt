add_executable(sandbox
    src/main.cpp
    src/Primitives.cpp
    src/SandboxMath.cpp
    src/CameraPath.cpp
    src/PresetStore.cpp
    src/ScriptSystem.cpp)

target_link_libraries(sandbox PRIVATE fitzel lua_dep nlohmann_json::nlohmann_json)

# Inject the path to the project's content root (textures, models, sounds) at the
# repo root, so the sandbox can load the large, git-ignored assets without copying
# ~250 MB into the build dir. The AssetDatabase scans this tree.
target_compile_definitions(sandbox PRIVATE
    FITZEL_CONTENT_DIR="${CMAKE_SOURCE_DIR}/content"
    FITZEL_TEXTURE_DIR="${CMAKE_SOURCE_DIR}/content/textures"
    FITZEL_SOUND_DIR="${CMAKE_SOURCE_DIR}/content/sounds"
    FITZEL_MODEL_DIR="${CMAKE_SOURCE_DIR}/content/models")

# Copy shader assets next to the executable so relative paths resolve at runtime.
add_custom_command(TARGET sandbox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:sandbox>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts
            $<TARGET_FILE_DIR:sandbox>/scripts
    COMMENT "Copying sandbox assets")
