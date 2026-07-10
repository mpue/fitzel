add_executable(sandbox
    src/main.cpp
    src/Primitives.cpp
    src/ModelLibrary.cpp
    src/SandboxMath.cpp
    src/CameraPath.cpp
    src/ScriptSystem.cpp
    src/ProjectIO.cpp
    src/TerrainPanel.cpp
    src/SculptPanel.cpp
    src/PaintPanel.cpp
    src/PropertyMeta.cpp
    src/Component.cpp
    src/FolderDialog.cpp
    src/VegetationSystem.cpp
    src/RoadSystem.cpp)

target_link_libraries(sandbox PRIVATE fitzel lua_dep nlohmann_json::nlohmann_json)

# Native folder-picker (IFileOpenDialog) needs COM + the shell on Windows.
if(WIN32)
    target_link_libraries(sandbox PRIVATE ole32 shell32)
endif()

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
