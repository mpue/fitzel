# Runtime sources: everything needed to load a saved project and render/simulate
# it at play time. Shared by the editor (sandbox) and the editor-free player.
set(SANDBOX_RUNTIME_SOURCES
    src/main.cpp
    src/Primitives.cpp
    src/ModelLibrary.cpp
    src/SandboxMath.cpp
    src/CameraPath.cpp
    src/ScriptSystem.cpp
    src/ProjectIO.cpp
    src/PropertyMeta.cpp
    src/Component.cpp
    src/VegetationSystem.cpp
    src/RoadSystem.cpp
    src/SkidSystem.cpp
    src/CarAudio.cpp)

# Editor-only sources: panels, tools and dialogs reachable only from the editor
# UI. Compiled into `sandbox` but not `player`. (TerrainPanel.hpp stays included
# by both -- the runtime render uses its TerrainLook/TerrainLayer structs -- but
# TerrainPanel.cpp, the panel widget, is editor-only.)
set(SANDBOX_EDITOR_SOURCES
    src/TerrainPanel.cpp
    src/SculptPanel.cpp
    src/PaintPanel.cpp
    src/FolderDialog.cpp
    src/ScatterTool.cpp
    src/VehicleTool.cpp)

# The editor: full sandbox with all panels/tools.
add_executable(sandbox ${SANDBOX_RUNTIME_SOURCES} ${SANDBOX_EDITOR_SOURCES})

# The player: the same runtime compiled with FITZEL_PLAYER, which #ifndef's out
# the editor UI in main.cpp. This is the binary shipped by "Export Game".
add_executable(player ${SANDBOX_RUNTIME_SOURCES})
target_compile_definitions(player PRIVATE FITZEL_PLAYER)

foreach(tgt sandbox player)
    target_link_libraries(${tgt} PRIVATE fitzel lua_dep nlohmann_json::nlohmann_json)

    # Inject the path to the project's content root (textures, models, sounds) at
    # the repo root, so the app can load the large, git-ignored assets without
    # copying ~250 MB into the build dir. The AssetDatabase scans this tree.
    target_compile_definitions(${tgt} PRIVATE
        FITZEL_CONTENT_DIR="${CMAKE_SOURCE_DIR}/content"
        FITZEL_TEXTURE_DIR="${CMAKE_SOURCE_DIR}/content/textures"
        FITZEL_SOUND_DIR="${CMAKE_SOURCE_DIR}/content/sounds"
        FITZEL_MODEL_DIR="${CMAKE_SOURCE_DIR}/content/models")

    # Copy shader assets + scripts next to the executable so relative paths
    # resolve at runtime. Both targets share one bin/ output dir, so this is a
    # (harmless) no-op overwrite for whichever builds second.
    add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/assets
                $<TARGET_FILE_DIR:${tgt}>/assets
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts
                $<TARGET_FILE_DIR:${tgt}>/scripts
        COMMENT "Copying ${tgt} assets")
endforeach()

# Native folder-picker (IFileOpenDialog) needs COM + the shell on Windows. Only
# the editor uses it (FolderDialog.cpp); the player has no file dialogs.
if(WIN32)
    target_link_libraries(sandbox PRIVATE ole32 shell32)
endif()
