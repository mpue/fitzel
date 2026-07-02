add_executable(sandbox
    src/main.cpp
    src/Primitives.cpp
    src/SandboxMath.cpp
    src/CameraPath.cpp
    src/PresetStore.cpp
    src/ScriptSystem.cpp)

target_link_libraries(sandbox PRIVATE fitzel lua_dep)

# Inject the path to the (large, git-ignored) terrain textures at the repo root,
# so the sandbox can load them without copying ~250 MB into the build dir.
target_compile_definitions(sandbox PRIVATE
    FITZEL_TEXTURE_DIR="${CMAKE_SOURCE_DIR}/textures"
    FITZEL_SOUND_DIR="${CMAKE_SOURCE_DIR}/sounds"
    FITZEL_MODEL_DIR="${CMAKE_SOURCE_DIR}/models")

# Copy shader assets next to the executable so relative paths resolve at runtime.
add_custom_command(TARGET sandbox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:sandbox>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts
            $<TARGET_FILE_DIR:sandbox>/scripts
    COMMENT "Copying sandbox assets")
