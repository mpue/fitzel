# Runtime sources: everything needed to load a saved project and render/simulate
# it at play time. Shared by the editor (sandbox) and the editor-free player.
set(SANDBOX_RUNTIME_SOURCES
    src/SceneGraph.cpp
    src/SceneSubmit.cpp
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
    # The circuit records. Runtime, not editor-only: the shipped player is where
    # a race is actually driven, and a record written nowhere is not a record.
    src/Leaderboard.cpp
    # The loading screen is RUNTIME: the shipped player is where a level
    # change would otherwise show a frozen window. Only the dialog that
    # configures it is editor-only.
    src/LoadingScreen.cpp
    src/PropertyMeta.cpp
    src/Component.cpp
    src/PrefabSystem.cpp
    src/VegetationSystem.cpp
    src/TiledScatter.cpp
    src/RoadSystem.cpp
    src/RoadSet.cpp
    src/RoadBridge.cpp
    src/RoadTunnel.cpp
    src/RoadLoop.cpp
    # Decals on the carriageway. Runtime, like the side objects: the scene
    # file holds the rules and the patches are lofted from them on load, so
    # the shipped player has to be able to build them too.
    src/RoadDecal.cpp
    src/RaceSim.cpp
    src/Difficulty.cpp
    src/RaceGrid.cpp
    # The scene's cameras. Runtime, like everything a saved scene contains: a
    # follow camera is scene data, and the shipped player has to resolve it too.
    src/CameraSystem.cpp
    # The multishot camera: the shot director the Camera component's Multishot
    # mode runs on. Runtime for the same reason as the cameras themselves -- a
    # shot list is scene data, and an attract screen or a replay in the shipped
    # player is exactly where it earns its keep.
    src/MultiShot.cpp
    # The post chain (SSAO, bloom, tonemap, speed blur, FXAA). Runtime: the
    # shipped player draws the same frame the editor does.
    src/PostChain.cpp
    # Volumetric fog (the marched mist volume, not lit.frag's height haze).
    # Runtime: a fog bank is part of the world the scene author built, so the
    # shipped player has to march it too.
    src/VolumetricFog.cpp
    src/FogNoise.cpp
    # The in-game graphics menu. Runtime by definition: fitting the game to
    # the machine is the shipped player's job, not the editor's.
    src/GraphicsMenu.cpp
    src/RaceHud.cpp
    src/Showroom.cpp
    src/RoadSide.cpp
    # Fences, walls and railway track along a spline. Runtime, not editor-only:
    # a run is derived from its path + rule on load, so the shipped player has to
    # be able to build it too -- the scene file holds the rule, not the geometry.
    src/SplineGen.cpp
    src/SplineSystem.cpp
    # Brooks, rivers and canals. Runtime, and more so than the fences: a
    # watercourse CUTS THE TERRAIN, so the shipped player has to re-derive the
    # bed on load or the ground the scene was authored against is not the ground
    # it is played on. The file holds the path and the rule, never the channel.
    src/RiverGen.cpp
    src/RiverSystem.cpp
    # The tower generator and the roadside city are RUNTIME, not editor-only: a
    # district is derived from its biome rules on load, so the shipped player has
    # to be able to build it too -- the scene file holds the rules, not the towers.
    src/BuildingGen.cpp
    src/CityGen.cpp
    src/RainRenderer.cpp
    src/SpraySystem.cpp
    src/ParticleSystem.cpp
    src/SkidSystem.cpp
    # Soft bodies (Jolt SoftBody): jelly, balloons, cloth. Runtime, like the rest
    # of the physics -- a wobbling object is scene data, and the shipped player
    # has to simulate and draw it too.
    src/SoftBodySystem.cpp
    src/TrailSystem.cpp
    # Lock-on missiles: gameplay, so the shipped player needs it too (the
    # settings panel inside it is simply never called there).
    src/WeaponSystem.cpp
    src/WorldAudio.cpp
    src/CarAudio.cpp
    src/GliderAudio.cpp
    src/UiOverlay.cpp
    src/UiStyle.cpp
    src/VideoLibrary.cpp
    # Profiler + its window are runtime, not editor-only: measuring the shipped
    # player is the case that matters most, and the editor's own panels are
    # exactly the overhead you want excluded when you do.
    src/Profiler.cpp
    src/GpuTimer.cpp
    # Loading a baked light grid and lighting from it. Runtime by definition:
    # it is what the shipped game does with the editor's bake.
    src/LightGrid.cpp
    src/DebugOverlay.cpp)

# Editor-only sources: panels, tools and dialogs reachable only from the editor
# UI. Compiled into `sandbox` but not `player`. (TerrainPanel.hpp stays included
# by both -- the runtime render uses its TerrainLook/TerrainLayer structs -- but
# TerrainPanel.cpp, the panel widget, is editor-only.)
set(SANDBOX_EDITOR_SOURCES
    # Crash recovery: the periodic scene snapshot and the dialog that offers it
    # back. Editor-only -- the player has no unsaved work to lose.
    src/Autosave.cpp
    src/GridRenderer.cpp
    # Standard views + middle-mouse pan for the editor viewport. Editor-only:
    # the player has a game to look at, not a viewport to navigate.
    src/ViewportNav.cpp
    src/MeshPaint.cpp
    src/MaterialsPanel.cpp
    src/MeshPaintPanel.cpp
    src/ModelingPanel.cpp
    src/UvPanel.cpp
    src/ModelsPanel.cpp
    src/PrefabsPanel.cpp
    src/AssetDrop.cpp
    src/VideoImport.cpp
    src/RoadPanel.cpp
    src/SplinePanel.cpp
    src/SplineEdit.cpp
    src/RiverPanel.cpp
    src/RiverEdit.cpp
    src/TerrainPanel.cpp
    src/SculptPanel.cpp
    src/PaintPanel.cpp
    src/FolderDialog.cpp
    src/GameSettingsPanel.cpp
    # Shipping: the game's icon (a PNG scaled into the exe's resources) and the
    # setup that wraps a finished export. Editor-only -- the player has no
    # export menu, and neither the image resizer nor the PE resource writer
    # would ever run inside a shipped game.
    src/IconEmbed.cpp
    src/InstallerBuild.cpp
    src/ScatterTool.cpp
    # Stamping prefabs along a road. Editor-only: it turns a road + a rule into
    # ordinary entities once, and the shipped player only ever loads the result.
    src/RoadPrefab.cpp
    src/BuildingPanel.cpp
    src/CityPanel.cpp
    # The offline path tracer: the still-image renderer behind the Render panel.
    # Editor-only, all three files. A shipped game has no use for a renderer that
    # takes minutes a frame, and the tracer's whole point is the picture you make
    # OF the game rather than one you play in.
    src/PathTrace.cpp
    src/PathTraceCapture.cpp
    src/PathTracePanel.cpp
    # The same tracer on the GPU. Editor-only like the rest of it -- and unlike
    # the rest of it, optional at RUNTIME too: a context below 4.3 has no
    # compute shaders and the machine renders on the CPU (see gputrace::available).
    src/GpuTrace.cpp
    # The same tracer, left running in the viewport (the Pathtraced shading
    # mode). Editor-only for the same reason as the three above it.
    src/ViewportTrace.cpp
    # Filling a light grid needs the whole path tracer; sampling one needs a
    # texture lookup. Only the BAKE is editor-only -- LightGrid.cpp itself is
    # runtime, below, so a shipped game lights from a bake without linking a
    # line of the renderer that made it.
    src/LightGridBake.cpp
    src/VehicleGizmo.cpp
    src/VehicleTool.cpp
    src/GliderTool.cpp)

# The editor: full sandbox with all panels/tools.
add_executable(sandbox ${SANDBOX_RUNTIME_SOURCES} ${SANDBOX_EDITOR_SOURCES})

# The Render panel writes the finished image out: stb_image_write for the PNG,
# tinyexr for the linear .exr beside it. Both are header-only interfaces whose
# implementation TUs live in the engine (tinyexr) or in PathTracePanel.cpp (stb
# write); this only puts their include directories on the editor, and the player
# gets neither.
target_link_libraries(sandbox PRIVATE stb_headers tinyexr_dep)

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

# The shader check (sandbox/tools/shadercheck.cpp): compiles every shader in
# assets/shaders against a real GL context and exits non-zero if the driver
# rejects one. A console program on purpose -- the editor is /SUBSYSTEM:WINDOWS
# in Release and has nowhere to print a compile error to, which is exactly how a
# broken shader ends up silently doing nothing. Run it after editing a shader:
#   build\release\bin\shadercheck.exe sandbox\assets\shaders
add_executable(shadercheck tools/shadercheck.cpp)
target_link_libraries(shadercheck PRIVATE glfw glad_gl_core_43)

# The spatial-audio check (sandbox/tools/audiocheck.cpp): drives one sound around
# a stationary listener through the engine's own API, so "I hear no direction"
# can be pinned on the device, the engine or the game instead of all three at
# once. Console program for the same reason as above -- it prints the device's
# channel count, and a ONE channel output is the single most likely reason for a
# world with no direction in it.
#   build/release/bin/audiocheck.exe content/sounds/swoosh.wav
add_executable(audiocheck tools/audiocheck.cpp)
target_link_libraries(audiocheck PRIVATE fitzel)

# The city check (sandbox/tools/citycheck.cpp): generates a district over a
# straight, a sweeper and a hairpin and measures how far the worst SOLID piece
# reaches past the kerb. A tower overhanging the road is a wall you hit at speed
# on a stretch that looked clear, and neither the editor nor the panel's
# placed/skipped counters can show it -- from the generator's side those lots
# built fine. Exits non-zero if anything solid crosses the kerb.
#   build/release/bin/citycheck.exe
# (MultiShot.cpp comes along because the Camera component saves its shot list by
#  name -- Component.cpp calls into it, so anything that links the components
#  links this too. It pulls in nothing of its own.)
add_executable(citycheck tools/citycheck.cpp src/CityGen.cpp src/BuildingGen.cpp
                         src/Component.cpp src/Primitives.cpp src/EditMesh.cpp
                         src/PropertyMeta.cpp src/MultiShot.cpp)
target_link_libraries(citycheck PRIVATE fitzel nlohmann_json::nlohmann_json)

# The autosave check (sandbox/tools/autosavecheck.cpp): drives the crash-snapshot
# writer over a simulated afternoon of editing and round-trips a scene through a
# snapshot file. Autosave is the one subsystem whose failure is invisible -- a
# snapshot never written, or written with its material GUIDs lost, looks exactly
# like a working one until the day somebody needs it, and by then the work is
# gone. Built with FITZEL_PLAYER so ProjectIO leaves the exporter (icon writer,
# installer) out: the harness has no use for it.
#   build/release/bin/autosavecheck.exe [workDir]
add_executable(autosavecheck tools/autosavecheck.cpp src/Autosave.cpp
                             src/ProjectIO.cpp src/GameSettings.cpp
                             src/Component.cpp src/Primitives.cpp src/EditMesh.cpp
                             src/PropertyMeta.cpp src/MultiShot.cpp)
target_compile_definitions(autosavecheck PRIVATE FITZEL_PLAYER)
target_link_libraries(autosavecheck PRIVATE fitzel nlohmann_json::nlohmann_json)

# The mesh-paint check (sandbox/tools/meshpaintcheck.cpp): paints a modelled box
# and then does everything the modelling tools can do to it. Paint lives on the
# mesh's corners, in an array parallel to the corners themselves -- an operation
# that moves one and not the other leaves the mesh drawing perfectly and the
# paint on the wrong faces some strokes later, which is not a thing anyone finds
# by looking at the viewport. Built with FITZEL_PLAYER, like the other harnesses.
#   build/release/bin/meshpaintcheck.exe
add_executable(meshpaintcheck tools/meshpaintcheck.cpp src/MeshPaint.cpp
                              src/EditMesh.cpp src/SandboxMath.cpp
                              src/CameraPath.cpp src/Component.cpp
                              src/Primitives.cpp src/PropertyMeta.cpp
                              src/MultiShot.cpp)
target_compile_definitions(meshpaintcheck PRIVATE FITZEL_PLAYER)
target_link_libraries(meshpaintcheck PRIVATE fitzel nlohmann_json::nlohmann_json)

# modelcheck (sandbox/tools/modelcheck.cpp): the loop cut and the per-face
# materials. Both fail without looking like failures -- a cut that lost track of
# which way round an edge it was measuring still splits every face and still
# draws, only the new edge zigzags; a face material is one array beside another,
# and an operation that moves one without the other puts a material on the wrong
# face several edits later. This measures the two things a screenshot cannot: that
# the cut is planar and leaves the surface closed, and that a dressing survives
# every operation and the scene file.
#   build/release/bin/modelcheck.exe
add_executable(modelcheck tools/modelcheck.cpp src/EditMesh.cpp
                          src/SandboxMath.cpp src/CameraPath.cpp
                          src/Component.cpp src/Primitives.cpp
                          src/PropertyMeta.cpp src/MultiShot.cpp)
target_compile_definitions(modelcheck PRIVATE FITZEL_PLAYER)
target_link_libraries(modelcheck PRIVATE fitzel nlohmann_json::nlohmann_json)

# importcheck (sandbox/tools/importcheck.cpp): a model authored as many objects
# has to arrive as many objects. This one fails without any symptom at all -- a
# structured import that silently collapses still draws the right picture, it is
# just one entity where there should be twenty, and you find out when you try to
# select a wheel and get the car. Checks that a .glb yields its nodes, that the
# textures living inside the file reach them, and that each part's reported
# centre puts it back where the flat import has it.
#   build/release/bin/importcheck.exe [model.glb ...]
add_executable(importcheck tools/importcheck.cpp)
target_link_libraries(importcheck PRIVATE fitzel)

# softcheck (sandbox/tools/softcheck.cpp): drops each kind of soft body on a
# floor and measures the wobble. A soft body has no transform to inspect -- it is
# a cloud of particles -- so "it simulated" and "it silently handed back its rest
# pose" look identical in a screenshot. This is where the difference is caught.
#   build/release/bin/softcheck.exe
add_executable(softcheck tools/softcheck.cpp src/SoftBodySystem.cpp
                         src/EditMesh.cpp src/SandboxMath.cpp src/CameraPath.cpp
                         src/Component.cpp src/Primitives.cpp src/PropertyMeta.cpp
                         src/MultiShot.cpp)
target_compile_definitions(softcheck PRIVATE FITZEL_PLAYER)
target_link_libraries(softcheck PRIVATE fitzel nlohmann_json::nlohmann_json)

# viewcheck (sandbox/tools/viewcheck.cpp): renders a project's scene offscreen
# and writes it to a PNG. Every other harness measures; this one LOOKS, which is
# the only way to answer whether a wall is wearing the brick or the default grey.
# Draws through scenesubmit::submit -- the same code the editor draws through --
# because a harness with its own submit loop would eventually show a picture the
# screen does not, and a wrong answer that looks like evidence is worse than none.
# Links imgui for ImGuizmo alone: the editor composes every object's matrix with
# it, and a second implementation would be a picture of a slightly different scene.
#   build/release/bin/viewcheck.exe <projectFolder> [out.png] [--size WxH]
add_executable(viewcheck tools/viewcheck.cpp src/SceneSubmit.cpp
                         src/SceneGraph.cpp
                         src/ProjectIO.cpp src/GameSettings.cpp
                         src/ModelLibrary.cpp src/Component.cpp
                         src/Primitives.cpp src/EditMesh.cpp
                         src/PropertyMeta.cpp src/MultiShot.cpp)
target_compile_definitions(viewcheck PRIVATE FITZEL_PLAYER
    # The same engine content root the editor falls back to, so a material's
    # textures resolve here too -- a harness that renders textured surfaces
    # untextured would lie about the one question it exists to answer.
    FITZEL_CONTENT_DIR="${CMAKE_SOURCE_DIR}/content")
target_link_libraries(viewcheck PRIVATE fitzel imgui stb_headers
                                        nlohmann_json::nlohmann_json)

# rivercheck (sandbox/tools/rivercheck.cpp): the water is the only thing here
# that both READS the terrain and WRITES it, and that pairing fails in two ways
# nobody can see. The bed creeps -- a solve that catches sight of its own cut
# sinks the channel a little further every drag, invisible at two centimetres a
# time and a canyon by the evening. Or the ground does not come back: move a
# course and the abandoned trench stays, or the hill sculpted beside it is quietly
# eaten. There is no undo for the height field (the cut is derived, not stored),
# so a wrong difference is wrong for good.
#
# So it measures: ten re-cuts must leave the field bit-identical, a moved course
# must give its ground back to the millimetre, a sculpt beside the water must come
# through untouched, and every profile must descend or SAY that it could not. Then
# it renders the courses through the real river.vert/river.frag over the ground
# they cut, because whether water looks like water has no other answer.
#   build/release/bin/rivercheck.exe [outDir] [shaderDir]
add_executable(rivercheck tools/rivercheck.cpp src/RiverGen.cpp src/RiverSystem.cpp
                          src/SandboxMath.cpp src/CameraPath.cpp)
target_link_libraries(rivercheck PRIVATE fitzel glfw glad_gl_core_43 glm::glm
                                         stb_headers nlohmann_json::nlohmann_json)

# roadcheck (sandbox/tools/roadcheck.cpp): a scene holds SEVERAL roads, and
# every way that can quietly collapse back into one is invisible in the viewport
# -- only the last road's corridor graded, a second Build sinking a road a little
# further, the ground query answering for one of them, a save that keeps one, a
# delete that frees a road the undo history still points at, or the vegetation
# mowing the gap between two of them because their centrelines arrived joined.
# So it measures all six against real RoadSystems (a GL context is opened for the
# materials; nothing is drawn).
#   build/release/bin/roadcheck.exe [shaderDir]
add_executable(roadcheck tools/roadcheck.cpp
                         src/RoadSet.cpp src/RoadSystem.cpp src/RoadBridge.cpp
                         src/RoadTunnel.cpp src/RoadLoop.cpp src/RoadSide.cpp
                         src/RoadDecal.cpp src/CityGen.cpp src/BuildingGen.cpp
                         src/Component.cpp src/Primitives.cpp src/EditMesh.cpp
                         src/PropertyMeta.cpp src/MultiShot.cpp
                         src/SandboxMath.cpp)
target_compile_definitions(roadcheck PRIVATE FITZEL_PLAYER
    FITZEL_CONTENT_DIR="${CMAKE_SOURCE_DIR}/content"
    FITZEL_TEXTURE_DIR="${CMAKE_SOURCE_DIR}/content/textures")
target_link_libraries(roadcheck PRIVATE fitzel glfw glad_gl_core_43 glm::glm
                                        nlohmann_json::nlohmann_json)

# The multishot check (sandbox/tools/shotcheck.cpp): runs the shot director over
# a parked car, a fast one, a lorry and a slope, and measures what a camera fault
# actually looks like -- the subject leaving the frame, the eye going through the
# ground, a shot repeating itself. None of that is visible in the editor, because
# the editor shows you the picture from inside the mistake.
#   build/release/bin/shotcheck.exe
add_executable(shotcheck tools/shotcheck.cpp src/MultiShot.cpp)
target_link_libraries(shotcheck PRIVATE glm::glm)

# The sky look (sandbox/tools/skycheck.cpp): renders sky.vert/sky.frag on its own
# into an offscreen buffer, from a camera pointed where the clouds actually are,
# and writes the frames out as PNGs. Not a test -- nothing passes or fails. The
# sky is the one thing in the engine nobody can get a plain look at while working
# on it (behind the panels, above the default pitch, under the fog), so every
# change to it was previously made by reasoning about noise and hoping.
#   build/release/bin/skycheck.exe <outDir> sandbox/assets/shaders
add_executable(skycheck tools/skycheck.cpp)
target_link_libraries(skycheck PRIVATE glfw glad_gl_core_43 glm::glm stb_headers)

# The fog look (sandbox/tools/fogcheck.cpp): the same idea as skycheck, one layer
# down. Marches volfog.frag at FULL resolution against a synthetic depth buffer
# and writes the frames out. Volumetric fog is the hardest thing here to get a
# plain look at -- half-res, tent-blurred, under bloom and the tonemapper, mixed
# with the closed-form haze, and only visible from inside the volume -- so a
# density field that is wrong for arithmetic reasons had nowhere to be caught.
# Shares FogNoise.cpp with the engine on purpose: a check that bakes its own copy
# of the noise is wrong in the way that looks right.
#   build/release/bin/fogcheck.exe <outDir> sandbox/assets/shaders
add_executable(fogcheck tools/fogcheck.cpp src/FogNoise.cpp)
target_link_libraries(fogcheck PRIVATE glfw glad_gl_core_43 glm::glm stb_headers)

# The path-tracer check (sandbox/tools/pathcheck.cpp): renders scenes whose
# answer is known in advance -- a white furnace that must come back at radiance
# 1, a shadow whose position is arithmetic, the same frame twice from one seed --
# and exits non-zero when one of them is wrong. A renderer is the worst kind of
# code to judge by eye: every wrong answer still produces a picture, and one that
# is 8% too bright or missing a bounce looks exactly like one that is right.
# tracecheck: the path-traced viewport's state machine, driven with a clock the
# test owns. Needs the editor-only preview and everything under it.
add_executable(tracecheck tools/tracecheck.cpp
                          src/ViewportTrace.cpp src/GpuTrace.cpp
                          src/PathTrace.cpp src/PathTraceCapture.cpp)
target_link_libraries(tracecheck PRIVATE fitzel imgui glfw glad_gl_core_43
                                         glm::glm stb_headers)

# gpucheck: the GPU tracer against the CPU one, on the same frames. Needs a
# context (4.3), which it opens for itself and skips politely without.
add_executable(gpucheck tools/gpucheck.cpp src/GpuTrace.cpp src/PathTrace.cpp)
target_link_libraries(gpucheck PRIVATE fitzel glfw glad_gl_core_43 glm::glm
                                       stb_headers)

# No GL: PathTrace.cpp is deliberately pure arithmetic, so this runs anywhere.
#   build/release/bin/pathcheck.exe [outDir]
add_executable(pathcheck tools/pathcheck.cpp src/PathTrace.cpp)
target_link_libraries(pathcheck PRIVATE glm::glm stb_headers)

# The capture check (sandbox/tools/capturecheck.cpp): builds a small scene
# through the real engine types, submits it to a real Renderer, and harvests it
# exactly as the Render panel does -- then asserts on the things that have
# arithmetic answers. pathcheck proves the tracer computes light correctly;
# this proves it is handed the scene that was drawn. That is the half more
# likely to be quietly wrong, because a sheared normal, a texture read back
# upside down or a scale applied twice all still produce a picture.
# Needs a GL context: the geometry only exists inside the driver's buffers.
#   build/release/bin/capturecheck.exe [outDir]
add_executable(capturecheck tools/capturecheck.cpp
                            src/PathTrace.cpp src/PathTraceCapture.cpp)
target_link_libraries(capturecheck PRIVATE fitzel stb_headers)

# The icon check (sandbox/tools/iconcheck.cpp): runs a picture through the exact
# path "Export Game" uses -- build the .ico, patch a COPY of player.exe, then read
# the resources back the way the shell does. The exported icon is written into an
# already-linked exe, so nothing in the build can catch a resource directory
# Windows declines to read; the export reports success either way and the file
# shows the default sheet. Exits non-zero if Windows will not use the icon.
#   build/release/bin/iconcheck.exe [image] [exe]
# The sculpt check (sandbox/tools/sculptcheck.cpp): the proportional Pull tool
# writes the ground ABSOLUTELY -- the caller keeps its running total and hands
# over the difference -- so a gesture that wanders and comes back leaves the
# height it stopped at and nothing else. That contract lives half in the engine
# and half in the viewport loop, and a hill 30% too tall still looks like a hill,
# so it is measured rather than looked at. Pure arithmetic on a height field: no
# GL, no window, no assets.
add_executable(sculptcheck tools/sculptcheck.cpp)
target_link_libraries(sculptcheck PRIVATE fitzel glm::glm)

add_executable(iconcheck tools/iconcheck.cpp src/IconEmbed.cpp)
# stb_headers for the decode/resize IconEmbed does (the IMPLEMENTATION TU is in
# the engine, so the symbols arrive with fitzel); shell32 for ExtractIconEx,
# which is how the check asks the question the way Explorer asks it.
target_link_libraries(iconcheck PRIVATE fitzel stb_headers)
if(WIN32)
    target_link_libraries(iconcheck PRIVATE shell32)
endif()

# The archive tool (sandbox/tools/fpak.cpp): packs, lists and verifies the
# encrypted .fpak archives an export ships. Same pak/vfs code the player runs, so
# "fpak verify" answers the only question that matters before shipping one --
# does every file read back exactly as it went in.
#   build/release/bin/fpak.exe verify game.fpak <staging dir>
add_executable(fpak tools/fpak.cpp)
target_link_libraries(fpak PRIVATE fitzel)

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
    # Third-party licence notices, next to the exe rather than inside assets/:
    # assets/ is packed into the encrypted archive on export, and a notice
    # nobody can open is not a notice. "Export Game" copies this file into the
    # exported folder for the same reason.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/docs/third-party-licenses.md
            $<TARGET_FILE_DIR:sandbox>/third-party-licenses.md
    COMMAND ${CMAKE_COMMAND} -E touch ${FITZEL_ASSET_STAMP}
    DEPENDS ${FITZEL_SANDBOX_ASSETS} ${CMAKE_SOURCE_DIR}/images/splash.png
            ${CMAKE_SOURCE_DIR}/docs/third-party-licenses.md
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
