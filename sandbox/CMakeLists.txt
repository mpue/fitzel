# Runtime sources: everything needed to load a saved project and render/simulate
# it at play time. Shared by the editor (sandbox) and the editor-free player.
set(SANDBOX_RUNTIME_SOURCES
    src/main.cpp
    src/Primitives.cpp
    # Editable meshes are RUNTIME, not editor-only: the shape is scene data, so
    # the shipped player has to be able to build and draw it too. Only the panel
    # that edits one is editor-only.
    src/EditMesh.cpp
    src/ModelLibrary.cpp
    src/SandboxMath.cpp
    src/CameraPath.cpp
    src/ScriptSystem.cpp
    src/ScriptBridge.cpp
    src/ProjectIO.cpp
    src/GameSettings.cpp
    src/PropertyMeta.cpp
    src/Component.cpp
    src/PrefabSystem.cpp
    src/VegetationSystem.cpp
    src/TiledScatter.cpp
    src/RoadSystem.cpp
    src/RoadBridge.cpp
    src/RoadLoop.cpp
    src/RaceSim.cpp
    src/RaceGrid.cpp
    src/RaceHud.cpp
    src/Showroom.cpp
    src/RoadSide.cpp
    # The tower generator and the roadside city are RUNTIME, not editor-only: a
    # district is derived from its biome rules on load, so the shipped player has
    # to be able to build it too -- the scene file holds the rules, not the towers.
    src/BuildingGen.cpp
    src/CityGen.cpp
    src/RainRenderer.cpp
    src/SpraySystem.cpp
    src/ParticleSystem.cpp
    src/SkidSystem.cpp
    src/TrailSystem.cpp
    src/CarAudio.cpp
    src/GliderAudio.cpp
    src/UiOverlay.cpp
    src/UiStyle.cpp
    src/VideoLibrary.cpp
    # Profiler + its window are runtime, not editor-only: measuring the shipped
    # player is the case that matters most, and the editor's own panels are
    # exactly the overhead you want excluded when you do.
    src/Profiler.cpp
    src/DebugOverlay.cpp)

# Editor-only sources: panels, tools and dialogs reachable only from the editor
# UI. Compiled into `sandbox` but not `player`. (TerrainPanel.hpp stays included
# by both -- the runtime render uses its TerrainLook/TerrainLayer structs -- but
# TerrainPanel.cpp, the panel widget, is editor-only.)
set(SANDBOX_EDITOR_SOURCES
    src/GridRenderer.cpp
    src/ModelingPanel.cpp
    src/AssetDrop.cpp
    src/VideoImport.cpp
    src/RoadPanel.cpp
    src/TerrainPanel.cpp
    src/SculptPanel.cpp
    src/PaintPanel.cpp
    src/FolderDialog.cpp
    src/GameSettingsPanel.cpp
    src/ScatterTool.cpp
    src/BuildingPanel.cpp
    src/CityPanel.cpp
    src/VehicleTool.cpp
    src/GliderTool.cpp)

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

    # Release on Windows: link as a GUI app so starting the exe doesn't put a
    # console window behind it. /ENTRY keeps plain main() as the entry point --
    # /SUBSYSTEM:WINDOWS would otherwise demand a WinMain. Debug keeps its
    # console: that's the build you attach a debugger to and watch logs in.
    # main() re-adopts the parent console when there is one (FITZEL_WINDOWED),
    # so running it FROM a terminal still prints.
    if(MSVC)
        target_compile_definitions(${tgt} PRIVATE $<$<CONFIG:Release>:FITZEL_WINDOWED>)
        target_link_options(${tgt} PRIVATE
            $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>
            $<$<CONFIG:Release>:/ENTRY:mainCRTStartup>)
    endif()

endforeach()

# Shader assets + Lua scripts next to the executable, so relative paths resolve at
# runtime. This is a real build step keyed on the asset files themselves, not a
# POST_BUILD hook on the executables: a POST_BUILD only runs when the exe relinks,
# so editing a shader and rebuilding left the *old* shader sitting next to the
# binary and the change silently didn't show up. CONFIGURE_DEPENDS re-globs on
# build, so a newly added shader is picked up without re-running CMake by hand.
file(GLOB_RECURSE FITZEL_SANDBOX_ASSETS CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/assets/*
     ${CMAKE_CURRENT_SOURCE_DIR}/scripts/*)
set(FITZEL_ASSET_STAMP ${CMAKE_CURRENT_BINARY_DIR}/assets.stamp)
add_custom_command(OUTPUT ${FITZEL_ASSET_STAMP}
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:sandbox>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts
            $<TARGET_FILE_DIR:sandbox>/scripts
    # Startup splash art. Kept in the repo's images/ folder (it's branding, not a
    # sandbox asset) but copied into assets/, which "Export Game" ships wholesale
    # -- so the exported player gets the same splash.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/images/splash.png
            $<TARGET_FILE_DIR:sandbox>/assets/splash.png
    COMMAND ${CMAKE_COMMAND} -E touch ${FITZEL_ASSET_STAMP}
    DEPENDS ${FITZEL_SANDBOX_ASSETS} ${CMAKE_SOURCE_DIR}/images/splash.png
    COMMENT "Copying sandbox assets")
add_custom_target(sandbox_assets ALL DEPENDS ${FITZEL_ASSET_STAMP})
# Both executables share one bin/ dir, so one copy serves both.
add_dependencies(sandbox sandbox_assets)
add_dependencies(player  sandbox_assets)

# ffmpeg, sitting next to the editor. The video importer (VideoImport.cpp) runs
# it as a child process rather than linking libav*: ffmpeg has no CMake build, so
# linking it would mean a prebuilt binary blob or vcpkg -- either of which breaks
# the property that every dependency here is FetchContent'd and the whole thing
# builds from one .bat. As a separate program it is also outside the LGPL/GPL
# question entirely. VideoImport falls back to the PATH, so a build without it
# still works for anyone who has ffmpeg installed; only the copy is skipped.
find_program(FITZEL_FFMPEG ffmpeg)
if(FITZEL_FFMPEG)
    # Keyed on a stamp rather than on the copied exe: the destination is only
    # expressible as a generator expression, and those aren't allowed in OUTPUT.
    # Same shape as the asset copy above, for the same reason.
    set(FITZEL_FFMPEG_STAMP ${CMAKE_CURRENT_BINARY_DIR}/ffmpeg.stamp)
    add_custom_command(OUTPUT ${FITZEL_FFMPEG_STAMP}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FITZEL_FFMPEG}
                $<TARGET_FILE_DIR:sandbox>/ffmpeg${CMAKE_EXECUTABLE_SUFFIX}
        COMMAND ${CMAKE_COMMAND} -E touch ${FITZEL_FFMPEG_STAMP}
        DEPENDS ${FITZEL_FFMPEG}
        COMMENT "Copying ffmpeg next to the editor")
    add_custom_target(sandbox_ffmpeg ALL DEPENDS ${FITZEL_FFMPEG_STAMP})
    add_dependencies(sandbox sandbox_ffmpeg)
else()
    message(STATUS
        "ffmpeg not found -- video import will look for it on the PATH instead. "
        "Install it (winget install Gyan.FFmpeg) and re-run CMake to have the "
        "build place a copy next to sandbox.exe.")
endif()

# The video importer reads back the JPEG frames ffmpeg wrote to check their size
# (VideoImport.cpp). Headers only -- the STB_IMAGE_IMPLEMENTATION TU lives in the
# engine, so the symbols come in with fitzel.
target_link_libraries(sandbox PRIVATE stb_headers)

# Native folder-picker (IFileOpenDialog) needs COM + the shell on Windows. Only
# the editor uses it (FolderDialog.cpp); the player has no file dialogs.
# psapi is for the Performance window's working-set figure and applies to both.
if(WIN32)
    target_link_libraries(sandbox PRIVATE ole32 shell32)
    target_link_libraries(sandbox PRIVATE psapi)
    target_link_libraries(player  PRIVATE psapi)
endif()
