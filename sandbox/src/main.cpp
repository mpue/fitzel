#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <future>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <coroutine>
#include <emscripten.h>   // the browser's frame loop (emscripten_set_main_loop_arg)
#endif

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <imgui.h>
#include <ImGuizmo.h>       // 3D transform gizmos in the viewport (+ runtime matrix decompose)
#ifndef FITZEL_PLAYER
#include <imgui_internal.h> // DockBuilder API for the default panel layout (editor only)
#include <TextEditor.h>     // ImGuiColorTextEdit: the Lua script editor (editor only)
#endif
#include <glm/gtc/type_ptr.hpp>

#include <nlohmann/json.hpp>
#ifndef FITZEL_PLAYER
#include <stb_image_write.h>  // --profile-shot (editor build only; see BootConfig)
#endif

#include <fitzel/Fitzel.hpp>
#include <fitzel/Version.hpp>   // generated: x.y.z.<commits> + git hash
#include <fitzel/graphics/EnvironmentIBL.hpp>
#include <fitzel/graphics/VideoTexture.hpp>
#include <fitzel/physics/Physics.hpp>

#include "SceneTypes.hpp"
#include "Document.hpp"
#include "Command.hpp"
#include "PropertyMeta.hpp"
#include "RoadCommand.hpp"
#include "SplineCommand.hpp"
#include "RiverCommand.hpp"
#include "Primitives.hpp"
#include "ModelLibrary.hpp"
#include "NumberBlob.hpp"
#include "VideoLibrary.hpp"
#include "GpuTimer.hpp"
#include "Profiler.hpp"
#include "OcclusionGate.hpp"
#include "DebugOverlay.hpp"
#include "SandboxMath.hpp"
#include "AnimGraph.hpp"
#include "AnimSystem.hpp"
#include "CameraPath.hpp"
#include "ScriptSystem.hpp"
#include "ScriptBridge.hpp"
#include "BoneAttach.hpp"
#include "LimbIK.hpp"
#include "ProjectIO.hpp"
#include "PrefabSystem.hpp"
#include "PaintPanel.hpp"
#include "SculptPanel.hpp"
#include "AssetDrop.hpp"
#include "FrameRender.hpp"
#include "RainRenderer.hpp"
#include "EditMesh.hpp"
#include "MeshPaint.hpp"
#include "Selection.hpp"
#include "SceneGraph.hpp"
#include "SceneSubmit.hpp"
#include "PhysicsShapes.hpp"
#include "PlayWorld.hpp"
#include "MeshQuery.hpp"
#include "HierarchyPanel.hpp"
#include "InspectorPanel.hpp"
#include "MaterialsPanel.hpp"
#include "MixerPanel.hpp"
#include "Cursor3D.hpp"
#include "SceneOps.hpp"
#ifndef FITZEL_PLAYER
#include "AssetsPanel.hpp"
#include "UnityImportPanel.hpp"
#include "ViewPanels.hpp"
#include "Toolbar.hpp"
#include "UiOverlayPanel.hpp"
#endif
#include "MeshPaintPanel.hpp"
#include "ViewTool.hpp"
#include "ViewShade.hpp"
#include "ViewportPick.hpp"
#include "ModelsPanel.hpp"
#include "PrefabsPanel.hpp"
#ifndef FITZEL_PLAYER
#include "PrefabEdit.hpp"
#endif
#ifndef FITZEL_PLAYER
#include "Autosave.hpp"
#include "GridRenderer.hpp"
#include "ModelingKeys.hpp"
#include "ModelingPanel.hpp"
#include "ModelingTools.hpp"
#include "SynthPanel.hpp"
#include "LuaApiPanel.hpp"
#include "UvPanel.hpp"
#include "ViewportNav.hpp"
#include "GraphPanel.hpp"
#include "TimelinePanel.hpp"
#include "PathTracePanel.hpp"
#include "ViewportTrace.hpp"
#include "ShotList.hpp"
#include "EditorMenus.hpp"
#include "LuaCompletion.hpp"
#include "ThumbCache.hpp"
#include "ScriptEditor.hpp"
#include "LookPanels.hpp"
#include "EditorContext.hpp"
#include "SceneDrop.hpp"
#include "GroundBrush.hpp"
#include "ViewportOverlay.hpp"
#include "TransformGizmo.hpp"
#include "ModelMode.hpp"
#include "ViewportHud.hpp"
#include "ToolbarIcons.hpp"
#endif
#include "SpraySystem.hpp"
#include "ParticleSystem.hpp"
#include "TerrainPanel.hpp"
#include "FolderDialog.hpp"
#include "GameSettingsPanel.hpp"
#include "LoadingScreen.hpp"
#include "LightGrid.hpp"
#include "VegetationSystem.hpp"
#include "FarTerrain.hpp"
#include "CloudShadow.hpp"
#include "Wildlife.hpp"
#include "Motes.hpp"
#include "Soundscape.hpp"
#include "Herd.hpp"
#include "RoadSet.hpp"
#include "RoadSystem.hpp"
#include "SplineSystem.hpp"
#include "RiverSystem.hpp"
#include "RaceSim.hpp"
#include "RaceGrid.hpp"
#include "CameraSystem.hpp"
#include "CameraTexture.hpp"
#include "Decals.hpp"
#include "Shatter.hpp"
#include "SkinCopies.hpp"
#include "TramRiders.hpp"
#include "TramSystem.hpp"
#include "Swing.hpp"
#include "PostChain.hpp"
#include "VolumetricFog.hpp"
#include "WeatherPreset.hpp"
#include "SkyLayers.hpp"
#include "RaceHud.hpp"
#include "Showroom.hpp"
#include "Difficulty.hpp"
#include "Leaderboard.hpp"
#include "GraphicsMenu.hpp"
#include <chrono>
#include <future>

#include "LevelPanel.hpp"
#include "RoadPanel.hpp"
#include "RoadPrefab.hpp"
#include "SplinePanel.hpp"
#include "SplineEdit.hpp"
#include "RoadEdit.hpp"
#include "SplinePlace.hpp"
#include "RiverPanel.hpp"
#include "RiverEdit.hpp"
#include "WeatherPanel.hpp"
#include "NaturePanel.hpp"
#include "SkidSystem.hpp"
#include "SoftBodySystem.hpp"
#include "TrailSystem.hpp"
#include "WeaponSystem.hpp"
#include "WorldAudio.hpp"
#include "MusicSystem.hpp"
#include "SynthSystem.hpp"
#include "ScatterTool.hpp"
#include "BuildingGen.hpp"
#include "BuildingPanel.hpp"
#include "HouseGen.hpp"
#include "HousePanel.hpp"
#include "StreetSignPanel.hpp"
#include "TreeGenPanel.hpp"
#include "RetargetPanel.hpp"
#include "ImageEditPanel.hpp"
#include "ProcGraphPanel.hpp"
#include "TownTraffic.hpp"
#include "TownNav.hpp"
#include "TownLamps.hpp"
#include "TriggerReach.hpp"
#include "Modifiers.hpp"
#include "HalfResSky.hpp"
#include "CityPanel.hpp"
#include "CityPlanPanel.hpp"
#include "CityCommand.hpp"
#include "VehicleGizmo.hpp"
#include "VehicleTool.hpp"
#include "GliderTool.hpp"
#include "CarAudio.hpp"
#include "GliderAudio.hpp"
#include "UiOverlay.hpp"
#include "UiOverlayCommand.hpp"
#include "UiStyle.hpp"
#include "Startup.hpp"
#include "PostLook.hpp"
#include "ScriptNet.hpp"
#include "ScriptSfx.hpp"

using namespace fitzel;

// On laptops with hybrid graphics (NVIDIA Optimus / AMD PowerXpress), ask the
// driver to run us on the discrete high-performance GPU instead of the iGPU.
#if defined(_WIN32)
extern "C" {
    __declspec(dllexport) unsigned long NvOptimusEnablement = 1;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

#ifndef FITZEL_PLAYER
namespace {


#ifndef FITZEL_PLAYER
// Files the OS file manager has dropped on the window, waiting for the frame to
// pick them up. GLFW delivers them from inside pollEvents(), before any ImGui
// window is current, so the panel that wants them can't be asked at that moment --
// they're parked here instead and the Assets panel takes them if they landed on it.
//
// File-scope rather than hung off the window user pointer: Input already owns that
// pointer for its scroll callback (see Input.cpp), and overwriting it would kill
// the mouse wheel everywhere. GLFW only ever calls this on the main thread, from
// inside pollEvents/waitEventsTimeout, so no lock is needed.
struct FileDrop {
    std::vector<std::string> paths;
    // Where the cursor was when the drop happened: GLFW's callback carries no
    // coordinates, and by the time the frame runs the pointer has moved on.
    float x = 0.0f, y = 0.0f;
};
FileDrop g_fileDrop;
#endif

} // namespace
#endif // !FITZEL_PLAYER


namespace {

// --- Weather ambience ------------------------------------------------------

// The weather-driven sound layers: loops whose volume follows the storm (and,
// for the water one, submersion), plus the two one-shots the world triggers.
struct WeatherSounds {
    Sound rain, wind, breeze, storm;   // loops; volume follows the weather
    Sound water;                       // loop;  volume follows submersion
    Sound thunder, splash;             // one-shots
};

// Filled in place rather than returned: these start playing here, and a
// fitzel::Sound that is already playing must never be moved or reassigned --
// doing so uninitialises it mid-mix.
void loadWeatherSounds(Audio& audio, const std::string& soundDir,
                       WeatherSounds& out) {
    out.rain    = Sound::fromFile(audio, soundDir + "/rain.wav",    true);
    out.wind    = Sound::fromFile(audio, soundDir + "/wind.wav",    true);
    out.breeze  = Sound::fromFile(audio, soundDir + "/breeze.wav",  true);
    out.thunder = Sound::fromFile(audio, soundDir + "/thunder.wav", false);
    // Water: a one-shot splash when the car plunges in, and a loop that stays
    // audible (volume follows submersion) while it wades through.
    out.splash  = Sound::fromFile(audio, soundDir + "/splash.wav",  false);
    out.water   = Sound::fromFile(audio, soundDir + "/water.wav",   true);
    // Storm bed: a heavy loop that fades in as the weather peaks.
    out.storm   = Sound::fromFile(audio, soundDir + "/storm.wav",   true);
    out.rain.setVolume(0.0f);   out.rain.play();
    out.wind.setVolume(0.0f);   out.wind.play();
    out.breeze.setVolume(0.0f); out.breeze.play();
    out.water.setVolume(0.0f);  out.water.play();
    out.storm.setVolume(0.0f);  out.storm.play();
}

// --- Asset pickers ---------------------------------------------------------

// Asset file names of one type, sorted and deduplicated. Sounds and sprites are
// referenced by NAME rather than by GUID, here and in the scene file, because a
// filename survives a re-import and reads sensibly to whoever opens the .fitzel.
std::vector<std::string> assetNamesOfType(AssetDatabase& db, AssetType type) {
    std::vector<std::string> out;
    for (const AssetId& id : db.allAssets())
        if (db.typeForId(id) == type)
            if (const auto* e = db.entry(id))
                out.push_back(e->absPath.filename().string());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Inspector combo that assigns one of `names` to a string field. `emptyLabel` is
// what an unset field shows and what selecting it clears back to.
//
// A field naming something that is no longer in the list is called out instead of
// being drawn like any other setting: a missing asset is the one state the combo
// itself cannot show, and it looks exactly like a working one until the scene is
// played.
void assetPickerCombo(const char* label, std::string& field,
                      const std::vector<std::string>& names,
                      const char* emptyLabel, const char* what) {
    const std::string cur = field.empty() ? emptyLabel : field;
    if (ImGui::BeginCombo(label, cur.c_str())) {
        if (ImGui::Selectable(emptyLabel, field.empty())) field.clear();
        for (const std::string& n : names)
            if (ImGui::Selectable(n.c_str(), field == n)) field = n;
        ImGui::EndCombo();
    }
    if (!field.empty() && std::find(names.begin(), names.end(), field) == names.end())
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f), "Missing %s: %s",
                           what, field.c_str());
}

// --- The grid orbit --------------------------------------------------------
// The shot a race is held on until the player starts it: a slow ring around
// their craft, a little above it, looking down at the nose.
//
// Slow on purpose. The subject is not moving, so the only motion in the frame is
// the camera's own -- and a fast circle around a stationary object reads as a
// mistake rather than as a held moment.
constexpr float kGridOrbitRadius = 16.0f;   // metres out from the craft
constexpr float kGridOrbitHeight = 5.5f;    // metres above it
constexpr float kGridOrbitRate   = 0.28f;   // rad/s -- about 22 s for a full lap
constexpr float kGridOrbitFov    = 55.0f;   // a touch tighter than the chase cam


} // namespace

#ifdef __EMSCRIPTEN__
// The program as a coroutine, for the browser. A page cannot be looped in -- it
// calls its frames, and code that does not return to it freezes the tab -- and
// main() keeps every system as a local. Leaving main() to let the page run
// would destroy them all. As a coroutine its locals live in a heap frame that
// outlasts each return to the browser: the loop co_awaits after every frame,
// and main() below resumes it once per frame. On the desktop appMain is a
// plain function looping as it always has.
struct AppMain {
    struct promise_type {
        int result = 0;
        AppMain get_return_object() {
            return AppMain{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_never  initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_value(int v) { result = v; }
        void unhandled_exception() {
            std::fprintf(stderr, "Fatal: an exception left the program\n");
            result = 1;
        }
    };
    std::coroutine_handle<promise_type> handle;
};
#define FZ_MAIN_RETURN co_return
static AppMain appMain(int argc, char** argv) {
#else
#define FZ_MAIN_RETURN return
static int appMain(int argc, char** argv) {
#endif
    try {
        startup::adoptParentConsole();
        startup::setWorkingDirToExe(argc, argv);
        startup::mountGameArchive();

        const startup::BootConfig boot    = startup::loadBootConfig(argc, argv);
        const std::string& bootProject    = boot.project;
        const std::string& bootScene      = boot.scene;
        const bool         bootFullscreen = boot.fullscreen;
        const bool         playerMode     = !bootProject.empty();

        Window window(WindowConfig{
            .width     = 1280,
            .height    = 720,
            .title     = std::string("Fitzel ") + fitzel::kVersion,
            .vsync     = true,
            .maximized = true,
        });

        Input  input(window);                  // before Gui (callback chaining)
        Gui    gui(window);
#ifndef FITZEL_PLAYER
        // Accept files dragged in from the OS file manager. Nothing else claims
        // this callback -- ImGui's GLFW backend installs eight, and drop isn't one
        // of them -- so there's no previous handler to chain to.
        glfwSetDropCallback(window.nativeHandle(),
                            [](GLFWwindow* w, int count, const char** paths) {
            double mx = 0.0, my = 0.0;
            glfwGetCursorPos(w, &mx, &my);
            g_fileDrop.x = static_cast<float>(mx);
            g_fileDrop.y = static_cast<float>(my);
            for (int i = 0; i < count; ++i) g_fileDrop.paths.emplace_back(paths[i]);
        });
#endif
        Camera camera({0.0f, 10.0f, 30.0f}, -90.0f, -5.0f);
        camera.moveSpeed = 20.0f;
        // Player two's eye, for split screen. A second camera rather than a
        // second copy of the frame: the world is drawn twice from two
        // viewpoints, and everything else about the frame -- sun, weather,
        // shadows, the probe -- is shared, which is what keeps a second view
        // affordable at all.
        Camera camera2({0.0f, 10.0f, 30.0f}, -90.0f, -5.0f);
        camera2.moveSpeed = 20.0f;
        // The scene's own cameras. These two above are the VIEWS -- what the two
        // panes are drawn from; where they stand is decided by the camera
        // entities this resolves.
        camerasys::CameraSystem cams;
        // Did player two's eye resolve this frame? The second pane follows this,
        // not the checkbox: a pane with no camera behind it is worse than none.
        bool haveView2 = false;
        // Split the viewport vertically into two panes. Vertical because the
        // target is a 3440x1440 ultrawide: two 1720x1440 panes are close to
        // square, where a horizontal split would give two 3440x720 letterbox
        // slots nobody can fly in.
        bool splitScreen = false;

        // Content roots: prefer a `content/` next to the exe (a portable/exported
        // build ships its assets there), else the compile-time dev tree.
        const startup::ContentRoots roots = startup::resolveContentRoots();
        const std::string& contentRoot = roots.content;
        const std::string& modelDir    = roots.models;

        // The loading screen: the picture the game sits on whenever it is not
        // drawing a world -- at startup here, and between levels below. Its look
        // is per project (game.json), so the editor and an unconfigured project
        // get exactly what they had before: the engine splash with a bar on it.
        loadingscreen::Screen loading;
        if (playerMode) {
            loading.setProjectFolder(bootProject);
            loading.setStyle(game::load(bootProject).loading);
        }
        // One frame of it, between the synchronous GL-bound loads below, so the
        // window shows what it is doing instead of staying black while it works.
        auto showProgress = [&](float frac, const char* label) {
            loading.frame(window, gui, frac, label);
        };
        showProgress(0.02f, "Starting up...");

        // Central asset registry: scans the project's content/ tree, giving each
        // texture/model/sound a stable GUID (persisted in a `<file>.meta` sidecar)
        // and caching decoded assets so repeated loads are deduplicated. Model
        // imports and material textures below resolve through this database.
        showProgress(0.05f, "Scanning content library...");
        AssetDatabase assetDb(contentRoot);
        assetDb.refresh();

        startup::CoreShaders shaders;
        if (!startup::loadCoreShaders(shaders)) FZ_MAIN_RETURN 1;
        Shader& lit    = shaders.lit;
        Shader& water  = shaders.water;
        Shader& river  = shaders.river;
        Shader& sky    = shaders.sky;
        Shader& skybox = shaders.skybox;

        // Slope/height-driven terrain palette (TerrainLook, defined in
        // TerrainPanel.hpp), exposed as material parameters and edited in the
        // Terrain panel.
        TerrainLook look;

        // Textures folder: the road surfaces and the tree/billboard atlases
        // resolve their files against it.
        const std::string& texDir = roots.textures;
        float texScale       = 0.08f; // world units -> texture tiling
        float normalStrength = 1.0f;

        // Materials describe surface appearance; the renderer feeds in lighting.
        // Terrain texturing is driven by editor layers (uLayerTex[], bound each
        // frame to units 3..); the palette here is just the no-layer fallback.
        Material terrainMat(lit);
        terrainMat.set("uColorMode", 1);

        // World streaming + renderer with cascaded shadows.
        TerrainSettings settings;
        TerrainStreamer streamer(settings, /*radius=*/5);
        int             viewRadius = 5; // view distance in chunks
        // Where the camera stops drawing. Auto keeps it tied to the streamed
        // terrain, which is the only value that cannot show the world ending:
        // draw past the last chunk and you are looking at the edge of the ring.
        // Off the leash it is a straight quality/frame-time trade, so it is a
        // setting rather than a constant.
        bool            farPlaneAuto = true;
        float           farPlaneManual = 900.0f; // metres, when auto is off
        { // lift the camera to stand ~9 units above the terrain at its position
            const glm::vec3 cp = camera.position();
            camera.setPosition({cp.x, streamer.heightAt(cp.x, cp.z) + 9.0f, cp.z});
        }
        Renderer        renderer(2048, 4);
        DirectionalLight light;

        // Image-based lighting from an HDRI (chosen from the asset library).
        // Baked indirect light for the open scene. RUNTIME, not editor state:
        // the shipped player loads the same .fgrid beside its scene and lights
        // from it, and only the BAKE button lives in the editor.
        lightgrid::Runtime lightGrid;

        EnvironmentIBL environment;
        bool  iblEnabled   = false;
        bool  iblSkybox    = false;   // draw the HDRI as the sky background
        float iblIntensity = 1.0f;
        std::string hdriLoaded;       // relPath of the loaded HDRI ("" = none)
        // The same panorama as a file path. Kept alongside the relPath
        // because the offline renderer lights from the file directly (it has
        // no GL cubemap to sample), and resolving the library entry a second
        // time at render time would fail exactly when the library has been
        // re-scanned -- which is when a render would silently lose its sky.
        std::string hdriAbsPath;

        // Water: planar reflection/refraction targets + a surface quad.
        // A tessellated water grid so Gerstner waves can displace its vertices.
        Mesh waterMesh = startup::makeWaterGrid(400);
        // Half-resolution reflection/refraction: the water distortion hides it
        // and it roughly quarters the cost of those two textured passes.
        RenderTarget reflectRT(640, 360);
        // Whether the lake drew a pixel lately: its reflection and refraction
        // are rendered only then (see OcclusionGate.hpp).
        OcclusionGate waterGate;
        RenderTarget refractRT(640, 360, RenderTarget::Format::RGBA8, /*depthTex=*/true);

        float     waterLevel   = -2.0f;
        glm::vec3 waterColor{0.08f, 0.24f, 0.30f};
        float     waveStrength = 0.022f;
        float     waveScale    = 0.06f;
        float     foamWidth    = 2.5f;
        float     waveHeight   = 0.6f; // Gerstner swell amplitude
        float     waveChoppy   = 0.6f;
        float     waterReflectivity = 0.65f; // max mirror strength (Fresnel cap)
        float     waterClarity      = 1.0f;  // higher = clearer (less depth tint)
        float     waterIor          = 1.33f; // index of refraction (drives Fresnel + bend)

        Mesh fsQuad = startup::makeFullscreenQuad();
        // Puts the depth back to "nothing here" (1.0) wherever the stencil test
        // lets it through -- the far terrain's own depth, once it is drawn.
        Shader depthToFar = Shader::fromSource(
            "#version 330 core\n"
            "layout(location = 0) in vec3 aPos;\n"
            "void main() { gl_Position = vec4(aPos.xy, 1.0, 1.0); }\n",
            "#version 330 core\n"
            "void main() {}\n");
        HalfResSky halfSky;   // the main view's sky at reduced resolution (see HalfResSky.hpp)
        halfSky.init();

        // HDR scene buffer + the post chain (SSAO, bloom, tonemap, speed blur,
        // FXAA). The chain owns its shaders and its intermediate targets -- see
        // PostChain.hpp for why that ownership is the whole point of it.
        PostChain post;
        if (!post.init()) FZ_MAIN_RETURN 1;
        // Volumetric fog: the marched mist volume. Unlike the post chain this is
        // survivable -- a frame without it is a frame without fog, not a black
        // screen -- so a failure here is reported and carried past rather than
        // taking the program down with it.
        VolumetricFog volFog;
        if (!volFog.init())
            std::fprintf(stderr, "Volumetric fog disabled (shader/noise init failed)\n");
        VolumetricFog::Settings volFogSet;
        // The frame's placed volumes, gathered from the entities carrying a
        // VolumetricFogComponent. Kept out here rather than built in the render
        // block so a scene full of mist does not allocate a vector per frame.
        std::vector<VolumetricFog::Volume> volFogVolumes;
        int hdrW = 0, hdrH = 0;
        window.framebufferSize(hdrW, hdrH);
        // With a stencil: the near scene marks what it covered, so the far
        // terrain drawn after it shades only what is left (see the main pass).
        RenderTarget hdrRT(hdrW, hdrH, RenderTarget::Format::RGBA16F, /*depthTex=*/true,
                           /*stencil=*/true);
        // The post chain's knobs stay HERE, not on the chain: they are edited by
        // the Sky & atmosphere and Colour grade panels, saved with the project,
        // and driven by the weather -- all of which is main's business. The chain
        // is handed them per frame. All of them are one PostLook (PostLook.hpp).
        PostLook postLook;

        // The final composited image lives in this target and is shown as the
        // central "Viewport" dock panel (IDE/editor style). Its size tracks the
        // panel's content region, so the scene renders at the viewport's pixels.
        RenderTarget viewportRT(hdrW, hdrH, RenderTarget::Format::RGBA8);
        // What THIS SCENE starts as. -1 = whatever the game says (game.json), and
        // a showroom scene then opens its start screen as usual.
        //
        // Per scene, because that is where the answer differs: a start screen is
        // watched, a circuit is flown, a walkable level is walked, and one
        // project-wide setting can only be one of those. game.json still names
        // what the GAME opens as; this says what a scene is when it is played,
        // wherever it was reached from -- a level change, the start screen, or
        // Play pressed on it in the editor. That last one is why it exists: a
        // track being edited is played thirty times an hour, and going through
        // the start screen each time to pick a craft is the right route for a
        // player and an absurd one for the author.
        //
        // Scene data, so it travels in the .fitzel and the shipped game obeys it
        // too -- it is not an editor shortcut bolted to the side.
        int          sceneStartMode = -1;
        // --- The camera trace (F4) ----------------------------------------
        // Judder is never diagnosed by looking at it. The eye cannot tell a craft
        // moving unevenly from a camera moving unevenly -- they look identical,
        // and the two live in completely different parts of the frame (the sim's
        // fixed step, the camera's placement, the present). So it gets written
        // down instead: one row a frame, both positions and the offset between
        // them, and the answer is then arithmetic rather than opinion.
        //
        // The last time something juddered here, this is what settled it: the
        // eye's offset from the craft was breathing by 30 cm a frame while the
        // world was perfectly steady.
        int                      camTraceLeft = 0;
        std::vector<std::string> camTraceRows;
        bool                     prevF4 = false;
#ifndef FITZEL_PLAYER
        // What the SELECTED camera sees, drawn small into the corner of the
        // viewport. Aiming a camera by flying to it, looking through it, flying
        // back and doing it again is how it worked before -- and it is the kind of
        // back-and-forth that makes people give up on placing cameras at all.
        //
        // Its own target, kept between frames rather than made per frame: an
        // ImGui image is drawn at the END of the frame, so a texture freed when
        // this scope closed would be the font atlas by the time it was sampled
        // (that mistake has already been made here once).
        RenderTarget camPreviewRT(384, 216, RenderTarget::Format::RGBA8);
        bool         showCamPreview = true;
        // Which camera the corner is showing, and what to call it. Written by the
        // render pass and read by the UI pass, which runs EARLIER in the frame --
        // so the corner shows the previous frame's picture, exactly as the main
        // viewport image does. One frame is not visible; two code paths for the
        // same picture would be.
        int          camPreviewId = -1;
        std::string  camPreviewName;
        // When the corner may draw itself again, and which camera it last drew.
        //
        // The preview is a SECOND full traversal of the scene -- the same draw
        // list, seen from somewhere else -- and this renderer is CPU/driver-bound
        // rather than fill-bound (see the render notes), so its cost sits in the
        // draw calls and not in the 384x216 it fills. At sixty frames a second
        // that is a second scene's worth of CPU every frame, for a picture the
        // size of a postage stamp, while the thing being aimed at is a craft
        // doing 80 m/s -- which is exactly what longer, unevener frames spoil.
        //
        // Twenty a second is plenty for a corner that shows where a camera
        // points.
        double       camPreviewNext = 0.0;
        int          camPreviewLast = -1;
#endif
        // What cameras see, on the materials that show them (CameraTexture.hpp):
        // monitors and mirrors, in the editor, in Play and in the player.
        camtex::CameraTextures camTextures;
        // Images laid on whatever is under them: Decal objects, and the bullet
        // holes scripts throw (Decals.hpp).
        decals::System decalSys;
        // Glass shot to pieces, and the shards lying about (Shatter.hpp). Play only.
        shatter::System shatterSys;
        decalSys.setHoles([&](int id, const glm::vec3& p) { return shatterSys.gone(id, p); });
        // The target the viewport panel was resized AWAY from, kept alive until
        // the frame it still appears in has been drawn.
        //
        // WHY. The panel's image is recorded into Dear ImGui's draw list while
        // the UI is built -- as a bare GL texture NAME -- and that draw list is
        // not issued until gui.endFrame(), at the very bottom of the frame. The
        // resize below sits between the two. Delete the texture there and the
        // draw call at the end binds a name that no longer belongs to us: GL
        // hands it to whatever grabbed it next, which in practice is Dear ImGui's
        // own font atlas, and the viewport fills with giant letters for exactly
        // as long as the panel is being dragged. Outliving the frame costs one
        // target for a few milliseconds and the panel shows its previous picture
        // stretched into the new rectangle, which is what a resize looks like
        // everywhere else.
        std::optional<RenderTarget> retiredRT;
        // Chase-cam speed blur: the world point the camera follows (the driven
        // car/glider) is the streak focus, and its speed drives the streak length,
        // so the craft stays sharp while the surroundings smear past it.
        // The arcade racing sim (car / glider / opponents) owns its state in one
        // struct; below, each field is aliased as a reference so the rest of the
        // loop reads/writes them by their old names, while racesim::update*
        // mutate `race`. See RaceSim.hpp.
        racesim::RaceState race;
        glm::vec3& blurAnchorWorld = race.blurAnchorWorld;
        bool&      blurAnchorValid = race.blurAnchorValid;
        float&     blurSpeed01     = race.blurSpeed01; // craft speed 0..~1.4 -> streak len
        unsigned  taaFrame = 0;            // jitter sequence position
        glm::mat4 taaPrevVP[2]{glm::mat4(1.0f), glm::mat4(1.0f)}; // per pane, unjittered
        glm::vec3 taaPrevEye[2]{glm::vec3(0.0f), glm::vec3(0.0f)};
        bool      taaHavePrev[2]{false, false};
        int  viewW = hdrW, viewH = hdrH;
        bool viewportHovered = false;
        glm::vec2 viewportMouseNdc(0.0f); // cursor within the viewport, NDC [-1,1]
        bool viewportClicked = false;     // left-click landed on the viewport image
        glm::vec2 viewportRectMin(0.0f);  // viewport image top-left in screen px
        glm::vec2 viewportRectSize(0.0f); // viewport image size in screen px

        // Day/night cycle.
        float timeOfDay = 7.3f;    // hours [0,24)
        float dayLength = 240.0f;  // real seconds per full 24h (0 = frozen)
        // Where on the globe and when in the year: the sun's path. The defaults
        // are the engine's old sky -- an equatorial sun that rises due east and
        // stands nearly overhead at noon -- so scenes keep the light they were
        // lit for. The Alps in June are 47 and +20.
        float sunLatitude    = 0.0f;     // degrees north
        float sunDeclination = -10.4f;   // degrees: +23 midsummer .. -23 midwinter
        bool  timePaused = true;   // freeze the time of day where it is
        // What a script did to the day in Play (game.setTimeOfDay & co): its own
        // day length (< 0 = none, the scene's rules), the clock as it was before
        // the script first touched it (< 0 = untouched), put back at Stop; and
        // the street lamps by hand (-1 = by the dusk, 0 off, 1 on).
        float scriptDayLength = -1.0f;
        float playClockBackup = -1.0f;
        int   streetLampMode  = -1;
        bool  streetLampsLit  = false;

        // The air: the cumulus deck, the ice above it and the height haze, as
        // one value rather than fourteen loose floats. Fourteen floats cannot be
        // copied, compared or written to a file as a unit, which is exactly what
        // a weather preset has to do with them -- see WeatherPreset.hpp, where
        // the comments that used to live on each one moved with them.
        weather::Sky skySet;

        // Weather: 0 = clear .. 1 = storm. Drives clouds, light, fog, waves, rain.
        float storm     = 0.0f;
        bool  autoWeather = false;
        // Whether the sky may flash. Used to be implied by the dial alone, which
        // made every hard rain a thunderstorm; a preset can now say no.
        bool  lightning = true;
        // How much is falling, as a multiplier on what the dial derives. A COUNT
        // of drops and impacts, not an opacity -- see weather::Preset::rain.
        float rainAmount = 1.0f;
        // Per-weather gains on the looping sound layers, on top of what the dial
        // derives. See weather::Audio.
        weather::Audio wxGain;

        // The project's named skies. Seeded with the built-ins so a session with
        // no project open still has a weather to pick; re-read from the project
        // whenever the open one changes (the panel does it, because the panel is
        // the one place that runs after every way a project can be opened).
        std::vector<weather::Preset> weatherPresets = weather::builtins();
        std::string weatherPresetsFolder;  // which project that list came from
        // Which preset THIS SCENE is on, and what a save of it should reach.
        // Scene data, unlike the presets themselves -- see the settings registry.
        std::string weatherCurrent;
        bool        weatherSavesTime = false;
        bool        weatherSavesMist = false;
        char        weatherNameBuf[64] = "";

        // The live weather, bound for weather::apply/capture. Built on demand
        // rather than kept: a Live is all references, so one stored anywhere is a
        // dangling struct waiting for the first thing that outlives its frame.
        auto liveWeather = [&] {
            return weather::Live{storm, autoWeather, lightning, rainAmount,
                                 timeOfDay, skySet, wxGain,
                                 volFogSet.enabled, volFogSet.followCamera,
                                 volFogSet.center, volFogSet.size,
                                 volFogSet.medium};
        };
        // Ground wetness 0..1: builds while it rains, dries slowly after, so roads
        // and terrain stay shiny for a while once the rain passes.
        float roadWetness = 0.0f;

        // Rain streaks + boat spray own their own shaders and GL buffers now.
        RainRenderer rain;
        if (!rain.init()) FZ_MAIN_RETURN 1;
        SpraySystem  spray;
        spray.init(); // a missing spray shader costs droplets, not the session
#ifndef FITZEL_PLAYER
        // The editor's construction grid, on the 3D cursor's plane. A drawing aid,
        // so a shader that failed to compile costs the aid, not the session.
        GridRenderer grid;
        grid.init();
#endif
        // Authored emitters (ParticleComponent). Same bargain as the spray: a
        // shader that failed to compile costs the effects, not the session.
        ParticleSystem particles;
        particles.init();
        float sprayAccum = 0.0f;             // droplet emitter carry
        float foamAccum  = 0.0f;             // surface-foam emitter carry
        std::mt19937 sprayRng(1337u);

        // --- Vegetation: grass + ambient wildlife (birds/fireflies) ----------
        // Grass/birds/fireflies live in VegetationSystem now; main keeps the
        // shared paint-brush state (also used by the tree/flower brushes) and
        // orchestrates. Constructed here (before regenFlowers, which reads veg's
        // grass params) -- streamer/camera already exist above.
        VegetationSystem veg(streamer, camera);
        if (!veg.init()) FZ_MAIN_RETURN 1;

        // The ground past the streamed ring, out to the horizon (FarTerrain.hpp).
        // Optional: a failed shader costs the horizon, not the session.
        FarTerrain farTerrain;
        farTerrain.init();
        bool farTerrainOn = false;   // scene setting "farTerrain"
        // How completely the ground past the grass takes the field's colour
        // (meadow.glsl). 0 = the terrain layers alone, as before.
        float meadowTint = 0.0f;
        // Which terrain layer is the forest floor under the ecology's woods
        // (-1 = none: the woods leave the ground as the bands paint it).
        int forestFloorLayer = -1;
        // The breeze (Wind.hpp): mean strength in calm weather, where it blows
        // to (degrees in XZ, 0 = +X), and how gusty. The defaults are the old
        // fixed wind, so a scene that never set them sways as it did.
        float windStrength = 0.2f;
        float windAngle    = 26.57f;
        float windGust     = 0.6f;
        // The cumulus casts its shadow on the ground (CloudShadow.hpp).
        CloudShadow cloudShadow;
        bool cloudShadowsOn = false;
        bool soundscapeOn = false;   // scene setting "soundscape"
        bool timeFlows = false;      // scene setting "timeFlows": the day runs in Play
        // A herd grazing in a meadow (Herd.hpp): a skinned model from the
        // project, each animal on its own clock. Empty model = none.
        Herd herd;
        Herd::Config herdCfg;
        std::string  herdModel;          // project-relative
        std::string  herdLoadedKey;      // what the herd was last loaded from
        // The fauna (Wildlife.hpp): flocks, swallows, raptors, butterflies. In
        // place of the old circling birds, for scenes that ask for it.
        Wildlife wildlife;
        wildlife.init();
        bool wildlifeOn = false;
        // Pollen, seeds and dust drifting in the sunlight (Motes.hpp).
        Motes motes;
        motes.init();
        bool motesOn = false;

        // Which tool has the left mouse button in the viewport (ViewTool.hpp).
        ViewTool  viewTool       = ViewTool::None;
        bool      brushErase     = false;      // stamp vs erase (shared)
        float     brushRadius    = 4.0f;       // world units (shared)
        float     brushDensity   = 1.0f;       // scatter-count multiplier (shared)
        glm::vec2 lastStampPos(1e9f);          // throttles stamping during a drag
        std::mt19937 brushRng(0xB1A5Eu);

        // --- Terrain sculpting ---------------------------------------------
        // `sculptWork` is the live, main-thread-only edit field; every change is
        // published as an immutable snapshot the terrain samples (see
        // TerrainEditField). A 3D brush raises/lowers/smooths/flattens it.
        TerrainEditField sculptWork;
        sculptWork.cell = 1.0f;              // ~1 m grid (finer = more detail/RAM)
        auto publishSculpt = [&]{
            setTerrainEditSnapshot(std::make_shared<const TerrainEditField>(sculptWork));
        };
        publishSculpt();                     // install the (empty) snapshot
        // The brush's settings and the gesture in flight (see SculptPanel.hpp).
        sculptui::Brush sculpt;

        // --- Terrain texture painting --------------------------------------
        // A parallel sparse field of per-layer paint weights, baked into the terrain
        // vertices and blended over the automatic height/slope look. A 3D brush
        // paints the chosen layer, or erases back toward the automatic blend.
        TerrainPaintField paintWork;
        paintWork.cell = 1.0f;
        auto publishPaint = [&]{
            setTerrainPaintSnapshot(std::make_shared<const TerrainPaintField>(paintWork));
        };
        publishPaint();                      // install the (empty) snapshot
        int   paintLayer    = 0;             // which of the first 4 texture layers to paint
        float paintRadius   = 8.0f;          // world units
        float paintStrength = 0.5f;          // 0..1 brush intensity
        bool  paintErase    = false;         // paint vs revert-to-auto

        // --- Mesh texture painting ------------------------------------------
        // A brush that puts textures on a modelled object: the painting itself
        // is in MeshPaint.cpp, the panel and the gesture in the viewport in
        // MeshPaintPanel.cpp, the brush's settings and stroke in meshBrush.
        meshpaintui::Brush meshBrush;

        // --- Object scatter -------------------------------------------------
        // A 3D brush that sprinkles imported models over the terrain as regular
        // Model entities, grouped under a root "Scattered" Empty; one stamp =
        // one undo step. Settings/placement/panel live in ScatterTool.
        scatterui::Settings scatterCfg;

        // --- Procedural buildings -------------------------------------------
        // Skyscraper/megastructure generator (BuildingGen): the parameters, plus
        // the id of the building last generated -- the one "Rebuild in place"
        // and "Save as prefab" act on (-1 = none, or it was deleted since).
        buildings::Params buildingCfg;
        int  buildingLiveId   = -1;
        bool buildingAuto     = false;   // re-generate on every parameter edit
        bool buildingPending  = false;   // an edit is waiting for the widget release
        char buildingNameBuf[64] = "Skyscraper";

        // --- Procedural houses (HouseGen) -----------------------------------
        // Same shape as the buildings above: the parameters being edited, the
        // house last generated (what "Rebuild in place" and "Save as prefab" act
        // on) and the storey tab the panel shows.
        housegen::Params houseCfg;
        int  houseLiveId  = -1;
        bool houseAuto    = true;    // a plan is tuned by looking at the result
        bool housePending = false;   // an edit is waiting for the widget release
        int  houseLevel   = 0;
        char houseNameBuf[64] = "House";


        // --- Trees: instanced model + billboard LOD (owned by VegetationSystem)
        if (!veg.initTrees(modelDir, texDir)) FZ_MAIN_RETURN 1;

        // --- Roads / paths (owned by RoadSet) --------------------------------
        // A ribbon mesh along a Catmull-Rom spline, draped on the terrain -- and as
        // many of them as the author draws, each complete with its own width,
        // surface, rails and roadside city, the same deal the watercourses get.
        // RoadSystem owns one road's mesh/material/collider/centreline and RoadSet
        // owns the list; main keeps the control-point editor state (shares the LMB)
        // and the roadPickTerrain helper below (used by every viewport brush, not
        // just roads).
        //
        // There is always at least one road (see RoadSet.hpp), so the editor below
        // can say "the road being edited" without asking whether there is one.
        RoadSet roads(lit, assetDb, streamer, texDir);
        // The roadside city's screen-size cull is a QUALITY setting, not a
        // property of one road, so the graphics menu edits this and every road
        // takes it (the Roadside panel still edits the selected road's own).
        float cityDetailCull = roads.active().cityMinPixels;
        // The terrain panel's "the ground moved" signal. It is one flag for the
        // whole set rather than a road's own: regenerating the terrain moved the
        // ground under every corridor, not under the selected one.
        bool  roadsDirty = false;
        // --- Fences, walls and railway track (owned by SplineSystem) ---------
        // The same idea as the road, one step lighter: a path plus a rule, with
        // the geometry derived from the two and never saved. No Build step --
        // these structures don't touch the terrain, so a path rebuilds the frame
        // after it changes (see splines.update below).
        SplineSystem splines;
        splines.groundAt = [&streamer](float x, float z) {
            return streamer.heightAt(x, z);
        };
        // A tram track laid into the streets rides on the roads: their surface
        // at street level first (a flyover overhead is not the street), then at
        // any height (the street on a bridge) -- and its points snap to their
        // middle as they are placed.
        splines.roadSurfaceAt = [&roads, &streamer](float x, float z, float& y) {
            const glm::vec2 p(x, z);
            return roads.surfaceHeightAt(p, y, streamer.heightAt(x, z) + 2.5f) ||
                   roads.surfaceHeightAt(p, y, 1e9f);
        };
        splines.snapToRoad = [&roads](glm::vec2 p, glm::vec2& out) {
            float best = 1e30f;
            for (const RoadSystem* r : roads) {
                if (!r->enabled) continue;
                const std::vector<glm::vec2>& c = r->centerline();
                const float half = r->surfaceHalf();
                for (std::size_t i = 0; i + 1 < c.size(); ++i) {
                    const glm::vec2 ab = c[i + 1] - c[i];
                    const float L2 = glm::dot(ab, ab);
                    const float t = L2 > 1e-8f ? glm::clamp(glm::dot(p - c[i], ab) / L2, 0.0f, 1.0f) : 0.0f;
                    const glm::vec2 q = c[i] + ab * t;
                    const float d2 = glm::dot(p - q, p - q);
                    if (d2 < half * half && d2 < best) { best = d2; out = q; }
                }
            }
            return best < 1e29f;
        };
        // --- Brooks, rivers and canals (owned by RiverSystem) ----------------
        // The same path-plus-rule deal again, with the one difference that makes
        // it its own module: water has to know how HIGH it stands, so this reads
        // the terrain AND writes it. It cuts its bed into the same sculpt field
        // the road grades its corridor into -- see carveRivers below for when.
        RiverSystem rivers;
        // The BARE terrain and the edit field separately, never one combined
        // height -- see the header comment on RiverSystem for why that split is
        // the difference between a stable bed and one that jumps at a lip.
        rivers.baseAt = [&streamer](float x, float z) {
            return terrainPresent() ? terrainBaseHeight(streamer.settings(), x, z)
                                    : 0.0f;
        };
        rivers.edits = &sculptWork;
        // --- Whole towns (owned by CitySystem, see CityPlan.hpp) -------------
        // Rules only in the scene file; the streets are ordinary roads in
        // `roads`, the buildings are re-derived here (towns.update, below).
        CitySystem towns;
        int townBareRev = -1;   // the towns' revision the grass last kept off
        towns.groundAt = [&streamer](float x, float z) { return streamer.heightAt(x, z); };
        towns.isWater  = [&](float x, float z) {
            float surf = 0.0f;
            return rivers.sample(glm::vec2(x, z), surf) || streamer.heightAt(x, z) < waterLevel;
        };
        towns.loadTexture = [&assetDb](const fitzel::AssetId& id) { return assetDb.loadTexture(id); };
        towns.roadLines = [&roads] {
            std::vector<cityplan::RoadLine> out;
            for (const RoadSystem* r : roads)
                if (r->enabled && r->centerline().size() >= 2)
                    out.push_back({r->centerline(), r->surfaceHalf(), r->name, r->centerlineY()});
            return out;
        };
        // The towns' traffic and people (TrafficSim + TownTraffic), on the laid
        // roads' own surface so a bus crosses a bridge on its deck.
        traffic::TownTraffic townTraffic;
        townTraffic.surfaceAt = [&roads](glm::vec2 p, float& y) {
            return roads.surfaceHeightAt(p, y, 1e9f);
        };
        townTraffic.init();
        // The trams on the tram lines (TramSim + TramSystem): driven along the
        // track splines, braking for the traffic, which brakes for them.
        TramSystem trams;
        // ...and the town's people riding them: off their walks to a stop, in
        // at the doors, out again a few stops on (TramRiders).
        TramRiders tramRiders;
        TownLamps townLamps;         // the towns' street lamps, from their prefabs
        SkidSystem skids(lit);       // tyre skid marks laid while wheels slip in Play
        TrailSystem trails(lit);     // vapour contrails streaming behind the racers
        // Lock-on missiles for the flown glider. Owns its own targeting, flight,
        // effects and HUD (WeaponSystem.cpp); the loop below only hands it the
        // field and applies the hits it reports. Its cue/ground callbacks are
        // wired further down, where those helpers exist.
        WeaponSystem weapons(lit);
        // Player two's launcher: a second RUNTIME state -- its own lock, its own
        // rack, its own missiles in the air -- of the same authored weapon. The
        // settings panel edits `weapons`, and this one adopts them each frame
        // (see WeaponSettings), so there is exactly one weapon in the scene and
        // two people shooting it.
        WeaponSystem weapons2(lit);
        // --- Graphics settings (owned by GraphicsMenu) ----------------------
        // The player's own quality choices, kept in a per-MACHINE file next to
        // the exe rather than in the project: they say what this PC can manage,
        // which is not something to carry to another one with the game.
        gfxmenu::Settings gfxSet  = gfxmenu::load("graphics.json");
        // --- Difficulty (owned by Difficulty.hpp) ---------------------------
        // The player's own, and a separate file from the graphics on purpose:
        // that one says what the MACHINE can manage, this one says how the
        // PLAYER wants to be treated. The two would only ever travel together by
        // accident. Edited by the SKILL row on the start screen, which is why
        // there is no dialog for it here.
        static constexpr const char* kDifficultyFile = "difficulty.json";
        difficulty::Profile gameDifficulty = difficulty::load(kDifficultyFile);
        // The circuit records, beside the exe for the same reason difficulty.json
        // is: an exported game's content lives in a read-only archive, and a
        // record has to be writable the moment after it is driven. Loaded once
        // and kept -- the start screen reads it, a finished race adds to it.
        static constexpr const char* kScoresFile = "scores.json";
        leaderboard::Table raceRecords = leaderboard::load(kScoresFile);
        bool prevRaceFinished = false;   // edge, so a flag is written once
        gfxmenu::Menu     gfxUi;
        gfxmenu::Input    gfxIn;          // rebuilt every frame, read at draw time
        bool prevGfxKey     = false;      // F10 edge
        bool gfxOpenRequest = false;      // an authored menu button asked for it
        bool prevGfxUp = false, prevGfxDown = false, prevGfxLeft = false;
        bool prevGfxRight = false, prevGfxFire = false;
        // Everything the menu drives, in one place. `prev` is what tells apply()
        // which of the expensive follow-ups (re-seeding the grass, the swap
        // interval) actually have to run -- and passing the settings that are
        // already in force means "force the lot", which is what startup wants.
        float renderScale = 1.0f;   // the Graphics menu's render scale (Play only)
        auto applyGfx = [&](const gfxmenu::Settings& prev) {
            gfxmenu::Targets t;
            t.viewRadius    = &viewRadius;
            t.envProbeRes   = &postLook.envProbeRes;
            t.envProbeFaces = &postLook.envProbeFaces;
            t.fxaa          = &postLook.fxaaEnabled;
            t.taa           = &postLook.taaEnabled;
            t.grassEnabled  = &veg.grassEnabled;
            t.flowerEnabled = &veg.flowerEnabled;
            t.grassDensity  = &veg.grassDensity;
            t.grassRadius   = &veg.grassRadius;
            t.flowerDensity = &veg.flowerDensity;
            t.cityMinPixels = &cityDetailCull;
            t.regrowVegetation = [&] { veg.grassDirty = true; };
            t.renderScale = &renderScale;
            t.setVSync = [&](bool on) {
#ifdef __EMSCRIPTEN__
                // The browser presents on its own refresh; there is nothing
                // to switch (and the port's extension query does not work).
                (void)on;
                return;
#endif
                // Off for a benchmark run: a frame that waits for the display
                // measures the display (see profilePath).
                if (!on || !boot.profilePath.empty()) { glfwSwapInterval(0); return; }
                // Adaptive where the driver offers it: a frame that misses the
                // refresh is shown at once (a tear line, briefly) instead of
                // being held for the next one -- plain vsync turns a GPU that
                // needs 18 ms into 30 fps, not 55.
                static const bool tear = glfwExtensionSupported("WGL_EXT_swap_control_tear") ||
                                         glfwExtensionSupported("GLX_EXT_swap_control_tear");
                glfwSwapInterval(tear ? -1 : 1);
            };
            gfxmenu::apply(gfxSet, prev, renderer, t);
            for (RoadSystem* r : roads) r->cityMinPixels = cityDetailCull;
        };
        applyGfx(gfxSet);   // the saved choices, before the first frame is drawn

        int  roadSel      = -1;       // selected control point (-1 = none)
        int  roadSel2     = -1;       // shift-clicked second point (bridge far end)
        bool roadDragging = false;    // dragging the selected handle
        bool roadDragHeight = false;  // ...vertically (Ctrl held on grab) vs across the ground
        // Erase / insert a control point of the active road (roadedit keeps the
        // bridges, tunnels and loops that name points by index honest), and the
        // selection with it: an erase clears it, an insert selects the new point.
        auto removeRoadPoint = [&](int k) {
            if (roadedit::removePoint(roads.active(), k)) roadSel = roadSel2 = -1;
        };
        auto insertRoadPoint = [&](int at, glm::vec2 p) {
            roadSel  = roadedit::insertPoint(roads.active(), at, p);
            roadSel2 = -1;
        };
        // Raycast the terrain under a viewport NDC point; true + world hit on success.
        auto roadPickTerrain = [&](glm::vec2 ndc, const glm::mat4& vp, glm::vec3& out) {
            const glm::mat4 inv = glm::inverse(vp);
            glm::vec4 pn = inv * glm::vec4(ndc, -1.0f, 1.0f); pn /= pn.w;
            glm::vec4 pf = inv * glm::vec4(ndc,  1.0f, 1.0f); pf /= pf.w;
            const glm::vec3 ro = glm::vec3(pn);
            const glm::vec3 rd = glm::normalize(glm::vec3(pf) - glm::vec3(pn));
            float t = 0.0f;
            for (int i = 0; i < 2048 && t < 4000.0f; ++i) {
                const glm::vec3 p = ro + rd * t;
                const float h = streamer.heightAt(p.x, p.z);
                if (p.y <= h) { out = p; return true; }
                t += std::max(0.25f, (p.y - h) * 0.4f);
            }
            return false;
        };
        // Commit the road: grade it into the terrain deformation field (so the
        // ground under it is flush + gently sloped), republish the snapshot and
        // rebuild the affected chunks, then loft the drivable mesh + collider.
        auto buildRoad = [&] {
            glm::vec2 mn, mx;
            // Every road, not just the one being edited -- see RoadSet::buildAll
            // for why a crossing that is cut one road at a time drifts.
            if (roads.buildAll(sculptWork, mn, mx)) {
                publishSculpt();
                streamer.editsChanged(mn, mx);
                // Now that the corridors are graded into the live terrain, drape the
                // side objects (rails/curbs/posts) on them -- and re-plan the city,
                // whose facades stand on the same freshly cut ground.
                for (RoadSystem* r : roads) {
                    r->rebuildSideObjects();
                    r->rebuildCity();
                }
            }
            // The towns stand on the same ground and measure against the same
            // roads -- including ones this Build just took away.
            towns.markDirty();
            // ...and a tram track laid into the streets rides on their surface.
            splines.touch();
        };

        // Cut every watercourse's bed into the terrain and republish it. Called
        // when an EDIT ENDS -- a drag released, a slider let go, an undo stepped
        // -- never per frame: the cut is a scan over every cell of every channel
        // and the ground it left, which is a build-sized job, not a frame-sized
        // one. The water surface itself follows the drag live (rivers.update),
        // so nothing about that wait is visible.
        auto carveRivers = [&] {
            if (!rivers.carveDirty()) return;
            glm::vec2 mn, mx;
            if (rivers.carve(sculptWork, paintWork, mn, mx)) {
                publishSculpt();
                publishPaint();   // the bed material rides the same rebuild
                streamer.editsChanged(mn, mx);
                // Anything standing on the ground the channel just moved has to
                // come with it -- guard rails and kerbs drape on the terrain.
                roads.rebuildSideObjects();
                towns.markDirty();   // ...and so do the towns' houses
                splines.touch();     // ...and the tracks draped on the ground
                // ...and nothing may go on growing where the water now is.
                veg.wet = rivers.wetDiscs(0.6f);
                veg.grassDirty = true;
                veg.treeCenter = glm::vec2(1e9f);   // force a tree re-plan
            }
        };

        // --- Test-drive vehicle ------------------------------------------
        // A primitive car: a scaled cube for the body/cabin plus four cylinder
        // wheels. Drawn through the Renderer (colour-only lit material) so it
        // gets lighting, shadows and fog like everything else.
        Mesh     carCube  = Mesh::cube();
        Mesh     carWheel = Mesh::create(makeCylinderX(0.42f, 0.16f, 16));
        Material carBodyMat(lit);
        carBodyMat.set("uColorMode", 0).set("uAlbedo", glm::vec3(0.72f, 0.12f, 0.10f))
                  .set("uWaterLevel", -1.0e4f);
        Material carCabinMat(lit);
        carCabinMat.set("uColorMode", 0).set("uAlbedo", glm::vec3(0.11f, 0.13f, 0.17f))
                   .set("uWaterLevel", -1.0e4f);
        Material carWheelMat(lit);
        carWheelMat.set("uColorMode", 0).set("uAlbedo", glm::vec3(0.05f, 0.05f, 0.06f))
                   .set("uWaterLevel", -1.0e4f);

        bool  vehicleMode = false;
        bool  prevV       = false;
        // WHICH of the craft's own cameras is being looked through -- a counter,
        // stepped by C (or the pad's Y), taken modulo however many the craft
        // carries. A counter rather than an id, because the craft changes under
        // it: a restart, a different machine off the start screen, watching a
        // rival. "The second camera on whatever I am flying" survives all of
        // that; a remembered entity id does not.
        //
        // This is how a chase view and a cockpit view live on one craft: hang two
        // Camera children on it (Follow and Cockpit) and the key steps between
        // them. Nothing here knows what they are -- three cameras work, and so
        // does one.
        int   viewCam     = 0;
        bool  prevViewKey = false;
        bool  prevViewPad = false;
        PhysicsBodyId physCarId = 0;   // Jolt vehicle chassis (Play-mode drive)
        glm::vec3     physCarHalf{0.9f, 0.35f, 2.0f};   // ...and its box's half size
        bool  carPlaced   = false;
        bool  showVehicle = true;
        // What the game starts as is a GAME setting now -- game.json, the Game
        // Settings dialog, game::StartMode -- because it is a statement about the
        // game rather than about a level, and because two bools in two panels
        // could not say "open on a camera" at all.
        //
        // These two are what a scene saved BEFORE that move says instead. Still
        // read, so a project that has always dropped into its glider still does;
        // no longer written and no longer editable, so the first save of each
        // scene drops them and the dialog is the only place that decides. They
        // only apply while the game setting is still the default (on foot).
        bool  legacyStartVehicle = false;
        bool  legacyStartGlider  = false;
        bool  showCrosshair      = true;
        // Scene vehicle (a model with a VehicleComponent) being driven: its
        // entity id, and -- for the editor test-drive -- the transform snapshot
        // restored when drive mode ends (a test-drive must not edit the scene).
        int                 driveVehicleId    = -1;
        bool                editorDriveActive = false;
        std::vector<Entity> driveBackup;
        // The Jolt car's wheels (Play): each wheel's modelled rotation relative
        // to the car's model, and Jolt's own wheel frame at the start, so only
        // what Jolt CHANGES -- steer, spin -- is put on the model's wheel.
        glm::quat           joltWheelRest[4];
        glm::quat           joltWheelStart[4];
        bool                joltWheelStartSet[4] = {false, false, false, false};
        // Arcade car pose (state lives in `race`; aliased so the loop keeps the
        // old names). physSteer stays a plain local -- it's the Jolt car's input.
        glm::vec3& carPos     = race.carPos;
        float&     carYaw     = race.carYaw;      // heading (radians)
        float&     carSpeed   = race.carSpeed;    // m/s (negative = reverse)
        float&     wheelSpin  = race.wheelSpin;   // rolling angle (radians)
        float&     steerAngle = race.steerAngle;  // front-wheel steer (radians, arcade)
        float physSteer  = 0.0f;   // smoothed steer input -1..1 (Jolt car)
        float physThrottle = 0.0f; // its pedal: -1 reverse .. 1 full (game.drivenVehicle)
        // Engine-sound feed, refreshed each frame by whichever drive block runs
        // (physics or arcade); consumed in the audio mix block.
        bool&  engineDriving  = race.engineDriving;
        float& engineSpeedMps = race.engineSpeedMps;
        float& engineThrottle = race.engineThrottle;
        float& engineWheelR   = race.engineWheelR;
        // Glider jet-sound feed (same idea, separate voice): set by the glider
        // flight tick, consumed next to the car engine in the audio mix block.
        bool&  gliderAudioActive = race.gliderAudioActive;
        float& gliderSpeedMps    = race.gliderSpeedMps;
        float& gliderThrottle    = race.gliderThrottle;
        float& gliderTopSpeed    = race.gliderTopSpeed;
        bool  carInWater     = false;  // chassis was submerged last frame (splash edge)
        float carWaterSub    = 0.0f;   // 0..1 chassis submersion this frame (audio/FX)
        bool  boatMode       = false;  // vehicle floats deep enough -> motorboat controls
        float&     simAccum  = race.simAccum;   // fixed-timestep accumulator
        // --- Glider (Wipeout-style hover racer) drive state -------------------
        // Arcade in BOTH editor and Play (no Jolt body). gliderMode is the master
        // flag; driveGliderId is the entity being flown; gliderBackup restores its
        // transform when flight ends (an editor test-flight must not edit scene).
        bool  gliderMode       = false;
        bool  prevG            = false;
        int   driveGliderId    = -1;
        bool  gliderDriveActive = false;
        // Player two, for split screen: a second craft with its own flight state
        // and its own eye (camera2). Picked automatically from the craft already
        // in the scene when the second pane opens -- laying out a two-player
        // track should not mean assigning seats by hand.
        int   driveGliderId2   = -1;
        racesim::RaceState race2;
        std::vector<Entity> gliderBackup;
        glm::vec3& gliderPos   = race.gliderPos;   // body-centre world position
        float&     gliderYaw   = race.gliderYaw;   // heading (radians)
        glm::vec3& gliderVel   = race.gliderVel;   // world-space velocity (m/s)
        float&     gliderBank  = race.gliderBank;  // smoothed roll (deg)
        float&     gliderPitch = race.gliderPitch; // smoothed pitch (deg)
        float&     gliderOverspeed = race.gliderOverspeed; // cap above maxSpeed (pad)
        float&     gliderBoostHold = race.gliderBoostHold; // linger of last pad boost (s)
        bool&      gliderBoosting  = race.gliderBoosting;  // on/just-left a pad (HUD)
        bool&      gliderWasOnPad  = race.gliderWasOnPad;  // last frame's pad contact
        // Race / lap timing, driven by a Start/Finish line the glider crosses.
        bool&  raceActive   = race.raceActive;
        bool&  raceFinished = race.raceFinished;
        bool&  raceHasLine  = race.raceHasLine;
        float& raceClock = race.raceClock;
        float& lapClock  = race.lapClock;
        float& lastLap   = race.lastLap;
        float& bestLap   = race.bestLap;
        int&   raceLap  = race.raceLap;   // completed laps
        int&   raceLaps = race.raceLaps;  // target laps
        bool&  finishWasOver = race.finishWasOver; // edge-detect the line crossing
        float& finishArm = race.finishArm;         // re-arm guard so one pass counts once
        std::unordered_set<int>& cpPassed = race.cpPassed; // checkpoints passed this lap
        int&   cpTotal = race.cpTotal;             // checkpoints in the scene (for the HUD)
        float& raceMissedFlash = race.raceMissedFlash; // HUD flash after a short lap
        // Ready/Set/Go start: while > 0 the player craft AND opponents are frozen,
        // so nobody moves before GO. goFlash shows "GO!" briefly once it hits 0.
        float& raceCountdown = race.raceCountdown;
        float& goFlash       = race.goFlash;
        const float wheelR = 0.42f, bodyW = 1.8f, bodyH = 0.7f, bodyL = 4.0f;
        const float cabW = 1.5f, cabH = 0.6f, cabL = 1.8f;
        const float halfTrack = 0.85f, halfBase = 1.35f;
        auto placeCar = [&] {
            const glm::vec3 p = camera.position();
            carPos    = glm::vec3(p.x, streamer.heightAt(p.x, p.z), p.z);
            carYaw    = glm::radians(90.0f - camera.yaw()); // align with view heading
            carSpeed  = 0.0f;
            carPlaced = true;
        };

        // --- Scene entities: placeable objects (box / ramp / cylinder / light) ---
        Mesh rampMesh   = Mesh::create(makeRampVerts());
        Mesh cylMesh    = Mesh::create(makeCylinderYVerts());
        Mesh sphereMesh = Mesh::create(makeSphereVerts());
        Mesh planeMesh  = Mesh::create(makePlaneVerts());
        // The scene document owns the authored content (entities + materials);
        // `entities`/`materials` below are just aliases so existing code reads
        // unchanged. Every content edit goes through `history` (undo/redo).
        Document     document;
        CommandStack history;

        // --- Undo for road shape edits ---------------------------------------
        // The road is not in the Document (its mesh, collider and graded corridor
        // hang off RoadSystem), but its edits belong on the same timeline as
        // everything else. An interaction -- a drag, a button, a slider -- opens
        // with the shape it found and closes by pushing the difference, so a drag
        // across fifty frames is one undo step and not fifty.
        //
        // WHICH road is remembered with the shape: the list can be re-pointed
        // between opening an interaction and closing it (a scene load, an undo of
        // an "Add road"), and a step that put one road's points onto another
        // would be worse than no undo at all.
        RoadSystem::Shape roadUndoBefore;
        RoadSystem*       roadUndoTarget = nullptr;
        bool              roadUndoOpen = false;
        auto beginRoadEdit = [&]() {
            if (roadUndoOpen) return; // already inside an interaction
            roadUndoTarget = &roads.active();
            roadUndoBefore = roadUndoTarget->shape();
            roadUndoOpen   = true;
        };
        auto commitRoadEdit = [&](const char* label) {
            if (!roadUndoOpen) return;
            roadUndoOpen = false;
            if (!roadUndoTarget) return;
            RoadSystem& road = *roadUndoTarget;
            auto cmd = std::make_unique<RoadShapeCmd>(road, roadUndoBefore,
                                                      road.shape(), label);
            const bool changed = !cmd->trivial();
            // push(), not pushApplied(): RoadShapeCmd::redo does more than
            // assign -- it flags the rebuild the committed shape needs.
            if (changed) history.push(std::move(cmd), document);
            // Re-loft every road once the gesture is over -- on release, not
            // per frame. A junction is a fact about two roads and is worked out
            // by the pass in RoadSet (see RoadJunction.hpp), so dragging a road
            // across another one produced nothing at all until the next Build:
            // the crossing was there on the ground and not in the scene, which
            // reads as the feature being broken rather than as being deferred.
            // Bridges and loops get away with waiting because the panel button
            // that creates one re-lofts on the spot; a junction has no button.
            if (changed) { roads.rebuildMeshes(); towns.markDirty(); }
        };
        // --- The road list, as undoable steps --------------------------------
        // Adding or deleting a whole road. Deleting does not destroy it (see
        // RoadSet), so both directions are one flag and the RoadShapeCmds already
        // in the history keep pointing at a road that is still there.
        //
        // pushApplied, not push: the list has already been changed by the time
        // this is called, and re-running redo() would only set the flag it is
        // already at.
        auto addRoad = [&]() {
            const int i  = roads.add();
            const int id = roads.idAt(i);
            roadSel = roadSel2 = -1;
            if (id >= 0)
                history.pushApplied(
                    std::make_unique<RoadListCmd>(roads, id, true, "Add road"));
        };
        auto deleteRoad = [&](int i) {
            const int id = roads.idAt(i);
            if (id < 0 || !roads.remove(i)) return;   // the last road stays
            roadSel = roadSel2 = -1;
            history.pushApplied(
                std::make_unique<RoadListCmd>(roads, id, false, "Delete road"));
        };
        // Point #3 of the road you just left is not point #3 of this one.
        auto selectRoad = [&](int i) {
            roads.select(i);
            roadSel = roadSel2 = -1;
        };

        // The point edits above predate the history (they are declared before it,
        // next to the road); these are what the editor actually calls.
        auto addRoadPoint = [&](int at, glm::vec2 p) {
            beginRoadEdit();
            insertRoadPoint(at, p);
            commitRoadEdit("Add point");
        };
        auto deleteRoadPoint = [&](int k) {
            beginRoadEdit();
            removeRoadPoint(k);
            commitRoadEdit("Delete point");
        };
        // Undoing an "Add point" can leave the selection naming a point that no
        // longer exists. Nothing dereferences it unchecked, but a phantom
        // selection lights up the panel's height field for a point you can't see.
        auto clampRoadSel = [&]() {
            const int n = static_cast<int>(roads.active().roadPts.size());
            if (roadSel  >= n) roadSel  = -1;
            if (roadSel2 >= n) roadSel2 = -1;
        };

        // --- Spline editor state + undo --------------------------------------
        // The same three flags the road editor keeps, one level down: which path
        // is being edited, which of its points is selected, and whether a drag is
        // in flight. The undo bracket is the road's, with a Snapshot in place of
        // a Shape.
        bool showSplines      = false;  // the panel's open flag
        int  splineSel        = -1;     // selected path
        int  splinePtSel      = -1;     // selected control point of that path
        bool splineDragging   = false;
        bool splineDragHeight = false;  // Ctrl held on grab: raise instead of move
        SplineSystem::Snapshot splineUndoBefore;
        bool                   splineUndoOpen = false;
        auto beginSplineEdit = [&]() {
            if (splineUndoOpen) return; // already inside an interaction
            splineUndoBefore = splines.snapshot();
            splineUndoOpen   = true;
        };
        auto commitSplineEdit = [&](const char* label) {
            if (!splineUndoOpen) return;
            splineUndoOpen = false;
            auto cmd = std::make_unique<SplineShapeCmd>(splines, splineUndoBefore,
                                                        splines.snapshot(), label);
            if (!cmd->trivial()) history.push(std::move(cmd), document);
        };
        // An undo can drop the path (or the point) the editor was looking at, and
        // a phantom selection lights up controls for something you cannot see.
        auto clampSplineSel = [&]() {
            if (splineSel >= static_cast<int>(splines.paths.size())) {
                splineSel = -1; splinePtSel = -1;
            }
            if (splineSel >= 0 &&
                splinePtSel >= static_cast<int>(splines.paths[splineSel].points.size()))
                splinePtSel = -1;
        };

        // --- Water editor state + undo ---------------------------------------
        // The spline editor's flags again. The one difference is what a
        // commit means: pushing the undo step is also what re-cuts the bed, so
        // every gesture ends in exactly one carve however many frames it took.
        bool showRivers      = false;  // the panel's open flag
        int  riverSel        = -1;     // selected watercourse
        int  riverPtSel      = -1;     // selected control point of it
        bool riverDragging   = false;
        bool riverDragHeight = false;  // Ctrl held on grab: the water level here
        RiverSystem::Snapshot riverUndoBefore;
        bool                  riverUndoOpen = false;
        auto beginRiverEdit = [&]() {
            if (riverUndoOpen) return; // already inside an interaction
            riverUndoBefore = rivers.snapshot();
            riverUndoOpen   = true;
        };
        auto commitRiverEdit = [&](const char* label) {
            if (!riverUndoOpen) return;
            riverUndoOpen = false;
            auto cmd = std::make_unique<RiverShapeCmd>(rivers, riverUndoBefore,
                                                       rivers.snapshot(), label);
            if (!cmd->trivial()) history.push(std::move(cmd), document);
            carveRivers();   // the gesture is over: the ground catches up
        };
        // An undo can drop the watercourse (or the point) the editor was looking
        // at. It also puts different PATHS back, so the bed has to be re-cut --
        // which is why this is called wherever undo and redo are, and why it is
        // in EditMenuCtx.
        auto clampRiverSel = [&]() {
            if (riverSel >= static_cast<int>(rivers.paths.size())) {
                riverSel = -1; riverPtSel = -1;
            }
            if (riverSel >= 0 &&
                riverPtSel >= static_cast<int>(rivers.paths[riverSel].points.size()))
                riverPtSel = -1;
            carveRivers();
        };

        // --- Vehicle setup gizmo ---------------------------------------------
        // Half of VehicleComponent describes a shape -- where the axles are, how
        // wide the track is, how big the wheels are, where the collision box sits
        // and how far down the mass is -- and none of it was visible, so setting a
        // car up was guesswork. VehicleGizmo draws that shape in the viewport and
        // puts a handle on each number; what lives here is its selection, its drag
        // flag and its undo bracket.
        //
        // The bracket is the ENTITY, not the component: the component rides on the
        // entity, so ModifyEntityCmd already covers the whole edit and an undo
        // cannot leave the two out of step.
        bool   vehGizmoEdit      = false; // handles armed (else it only draws)
        int    vehGizmoSel       = vehiclegizmo::kNone;
        bool   vehGizmoDrag      = false;
        bool   vehGizmoOwnsMouse = false; // set per frame; keeps ImGuizmo off the LMB
        Entity vehGizmoBefore;
        bool   vehGizmoUndoOpen  = false;
        int    vehGizmoUndoId    = -1;
        auto beginVehicleEdit = [&](int entId) {
            if (vehGizmoUndoOpen) return; // already inside an interaction
            const Entity* e = document.find(entId);
            if (!e) return;
            vehGizmoBefore   = *e;
            vehGizmoUndoId   = entId;
            vehGizmoUndoOpen = true;
        };
        auto commitVehicleEdit = [&](const char*) {
            if (!vehGizmoUndoOpen) return;
            vehGizmoUndoOpen = false;
            Entity* after = document.find(vehGizmoUndoId);
            if (!after) return;
            auto cmd = std::make_unique<ModifyEntityCmd>(vehGizmoBefore, *after);
            if (!cmd->trivial()) history.pushApplied(std::move(cmd));
        };

        // --- Scene UI overlay (2D screen-space HUD authored per scene) --------
        // Text/button/image elements drawn over the view while playing. Not in the
        // Document (like the road), so it carries its own selection -- and, in the
        // editor, its own undo bracket (see UiOverlayPanel.hpp).
        UiOverlay              uiOverlay;
        int                    uiSel = -1;
#ifndef FITZEL_PLAYER
        uioverlayui::Bracket   uiEditBracket;
#endif

        std::vector<Entity>& entities = document.entities();
        // What is selected: the active object plus, when more than one is picked,
        // the whole set -- and the invariant tying them together. See Selection.hpp.
        Selection sel(entities);
        // Selecting in the viewport: the box being dragged, the stack a repeated
        // click cycles through (see ViewportPick.hpp).
        viewpick::Picker scenePick;
        int       renameId       = -1;   // hierarchy node being inline-renamed (entity id)
        bool      renameFocus    = false; // request keyboard focus on the rename field
        char      renameBuf[128] = "";
        bool      entityEditMode = true; // transform gizmo active; Esc -> selection
        // Select vs. Create: the toolbar's arrow/plus pair. Clicking empty ground
        // only ever drops a new object in Create mode -- in Select mode (the
        // default) it clears the selection, so a stray click cannot litter the
        // scene with boxes. Esc always steps back out to Select.
        bool      placeMode      = false;
        glm::vec3 entityNewHalf(1.0f, 1.0f, 1.0f); // default size (half-extents)
        // Half-thickness a new Plane gets (see SceneOps.hpp).
        using sceneops::kPlaneHalfY;
        EntityType entityNewType = EntityType::Box; // type placed on click
        int       entityCounter = 0; // for unique default names

        // Blender-style 3D cursor: a world-space reference point placed with
        // Shift+Right-click, used as a snap/placement anchor, and the snap steps
        // Ctrl rasters a gizmo drag to (see Cursor3D.hpp).
        cursor3d::Cursor cursor;
        // The construction grid draws that snap step on the cursor's plane, so
        // the lattice you aim at and the one "snap to grid" rounds to are the
        // same thing seen twice. Held here rather than on the renderer (which is
        // editor-only) because these two persist with the scene.
        bool      showGrid = true;
        float     gridFade = 220.0f; // metres until it has faded out entirely

        // Material library: named surface assets solids can be assigned. New
        // objects get the material selected in the Materials panel (matSel).
        std::vector<MaterialDef>& materials = document.materials();
        // The roadside city needs the four shared materials each biome's palette
        // slot names, which live in this library -- something a road has no
        // business reaching into. So it asks, once, through a hook (see
        // RoadSystem::cityPalettes), and everything downstream (Build, a scene
        // load, an undo) re-derives the district without main having to remember
        // to. Set here rather than at construction because `materials` is the
        // Document's and only exists from this line on.
        // Stones and reeds find-or-create their two shared materials in here.
        rivers.materials = &materials;
        rivers.touch();
        // Every road, and every road the author adds later -- so a second one is
        // wired the same way the first is without main having to remember.
        roads.onCreate = [&materials](RoadSystem& r) {
            r.cityPalettes = [&materials](const std::vector<city::Biome>& bs) {
                return city::ensurePalettes(materials, bs);
            };
        };
        for (RoadSystem* r : roads) roads.onCreate(*r);
        int  matSel          = 0;    // selected material in the Materials panel
        // Name filter for the Materials panel. A project's library grows into the
        // dozens (every imported model brings its own), and hunting one name in
        // that list by eye and scrollbar is the slowest thing in the panel.
        char matFilter[64]   = "";
        char matPickFilter[64] = ""; // the same, inside the Inspector's pickers
        // Secondary-panel visibility (toggled from the View menu). The default
        // layout is just Hierarchy | Scene | Inspector; everything else is hidden.
        bool showMaterials   = false;
        bool showModels      = false;
        bool showPrefabs     = false;
        char prefabNameBuf[64] = ""; // name field in the Prefabs panel
        bool showAssets      = false;
        // Asset browser: lazily-built, cached preview thumbnails (small textures,
        // kept alive here so they stay resident), plus its view options. Decoding
        // runs on ONE persistent background thread fed by a queue (thumbWork); the
        // render thread uploads finished decodes. A single worker -- rather than a
        // std::async per request -- avoids thread churn and keeps decoding serial,
        // so a texture panel listing dozens of images can't spawn a storm of
        // threads or rethrow a worker exception into the UI (which used to crash).
        // `assetThumbs` and `thumbRequested` are touched only on the render thread.
#ifndef FITZEL_PLAYER
        std::unordered_map<fitzel::AssetId, std::shared_ptr<Texture>> assetThumbs;
        std::unordered_set<fitzel::AssetId>                           thumbRequested;
        assetsui::State assetsBrowser;   // the Assets panel's size, filter, last drop

        struct ThumbWork {
            std::mutex              mutex;
            std::condition_variable cv;
            std::deque<std::pair<fitzel::AssetId, std::string>> queue;   // to decode
            std::vector<std::pair<fitzel::AssetId, fitzel::ImagePixels>> done; // decoded
            bool stop = false;
        };
        ThumbWork thumbWork;
        std::thread thumbThread([&thumbWork]{
            const std::filesystem::path cacheDir = thumbcache::thumbCacheDir();
            for (;;) {
                std::pair<fitzel::AssetId, std::string> job;
                {
                    std::unique_lock<std::mutex> lk(thumbWork.mutex);
                    thumbWork.cv.wait(lk, [&]{
                        return thumbWork.stop || !thumbWork.queue.empty(); });
                    if (thumbWork.stop) return;
                    job = std::move(thumbWork.queue.front());
                    thumbWork.queue.pop_front();
                }
                // Prefer the tiny disk-cached preview; only fall back to a full
                // (expensive) source decode when it's missing or stale, and cache
                // the result. decodeThumbnail never throws (empty image on failure).
                const std::filesystem::path cacheFile =
                    cacheDir / (job.first.toString() + ".fth");
                const long long mt = thumbcache::sourceMtime(job.second);
                fitzel::ImagePixels img;
                if (!thumbcache::loadThumbCache(cacheFile, mt, img)) {
                    img = Texture::decodeThumbnail(job.second, 128);
                    thumbcache::saveThumbCache(cacheFile, mt, img);
                }
                std::lock_guard<std::mutex> lk(thumbWork.mutex);
                thumbWork.done.emplace_back(job.first, std::move(img));
            }
        });
        // Stop + join the worker on any scope exit (normal or exception unwinding),
        // BEFORE thumbThread's own destructor runs -- a joinable std::thread that is
        // destroyed unjoined calls std::terminate. Declared after the thread so it
        // is destroyed first (reverse order).
        struct ThumbJoiner {
            ThumbWork& w; std::thread& t;
            ~ThumbJoiner() {
                { std::lock_guard<std::mutex> lk(w.mutex); w.stop = true; }
                w.cv.notify_all();
                if (t.joinable()) t.join();
            }
        } thumbJoiner{thumbWork, thumbThread};
        // Upload any thumbnails the worker finished (a 128px GL upload is basically
        // free). Runs once per frame so the cache serves every panel.
        auto pumpThumbnails = [&]{
            std::vector<std::pair<fitzel::AssetId, fitzel::ImagePixels>> done;
            {
                std::lock_guard<std::mutex> lk(thumbWork.mutex);
                done.swap(thumbWork.done);
            }
            for (auto& [id, img] : done) {
                auto t = std::make_shared<Texture>(Texture::fromImagePixels(img));
                assetThumbs[id] = t->isValid() ? t : nullptr; // null = bad/blank
            }
        };
        // Resolve a texture asset to a small preview GL id (0 until it is ready),
        // enqueueing a decode on first request. Shared by the material + terrain
        // texture pickers and the Assets browser. Render-thread only.
        auto thumbFor = [&](fitzel::AssetId id) -> unsigned {
            if (!id.valid()) return 0;
            const auto it = assetThumbs.find(id);
            if (it != assetThumbs.end()) return it->second ? it->second->id() : 0;
            if (thumbRequested.insert(id).second) { // enqueue once per id
                const std::string p = assetDb.pathForId(id).string();
                if (!p.empty()) {
                    std::lock_guard<std::mutex> lk(thumbWork.mutex);
                    thumbWork.queue.emplace_back(id, p);
                    thumbWork.cv.notify_one();
                }
            }
            return 0;
        };
        // Draw a frame-height texture preview (image + SameLine) inline before a
        // slot/combo. Prefers the already-loaded full-res handle, else the shared
        // thumbnail cache, else a blank square so the row still lines up.
        auto texSwatch = [&](const std::shared_ptr<Texture>& tex, fitzel::AssetId id) {
            const float h = ImGui::GetFrameHeight();
            const unsigned t = (tex && tex->isValid()) ? tex->id() : thumbFor(id);
            if (t) ImGui::Image((ImTextureID)(intptr_t)t, ImVec2(h, h));
            else   ImGui::Dummy(ImVec2(h, h));
            ImGui::SameLine();
        };
#endif // !FITZEL_PLAYER
        bool showAbout       = false;
#ifndef FITZEL_PLAYER
        luaapi::State luaApi;             // Help -> Lua API
#endif
        bool showStats       = false;
        bool showNature      = false;   // Advanced nature (NaturePanel.hpp)
        // Frame-cost window (F3). Lives outside the editor-only block: the
        // player build shows it too, which is the build whose numbers count.
        bool showPerf        = false;
        bool showCamera      = false;
        bool showWeather     = false;
        bool showSky         = false;
        bool showColorGrade  = false;
        bool showWater       = false;
        bool showTerrain     = false;
        bool showSculpt      = false;
        bool showPaint       = false;
        bool showVegetation  = false;
        bool showScatter     = false;
        bool showBuildings   = false;
        bool showHouses      = false;
        bool showSigns       = false;
        bool showTreeGen     = false;
        bool showRetarget    = false; // motions of other skeletons onto a character
        bool showImageEditor = false; // the small Photoshop for textures
        bool showCity        = false;
        bool showTowns       = false; // the town generator
        int  townSel         = -1;
        bool townEditing     = false; // an undo step for a town edit is open
        CitySystem::Snapshot townUndoBefore;
        std::string          townStatus;
        bool showCamPath     = false;
        bool showTimeline    = false;
        bool showGraphEditor = false;
        bool showRoads       = false;
        bool showLevelGen    = false;
        bool showUiOverlay   = false; // scene 2D UI overlay editor
        bool showCursor      = false; // 3D cursor panel
        bool showModeling    = false; // face-modelling panel
        bool showSynth       = false; // the modular synth's patch editor
        bool showMeshPaint   = false; // painting layers onto a modelled mesh
        bool showUv          = false; // where a face's texture sits on it
        bool showProcedural  = false; // node graphs that cook procedural objects
        // Which face of the selected mesh the modelling operations act on. Reset
        // whenever the selection moves to another object: a face index means
        // nothing on a different mesh.
        int  meshFaceSel     = -1;
        int  meshFaceOwner   = -1;   // entity id that index belongs to
#ifndef FITZEL_PLAYER
        // The modelling mode's picked corners and edges and a modal edit in
        // flight (see ModelMode.hpp); the face itself stays in meshFaceSel.
        modelmode::Session modelSess;
        // A transform-gizmo drag in flight, of the object or of the picked face
        // (see TransformGizmo.hpp).
        gizmo::Drag gizmoDrag;
#endif
        bool showVehiclePanel = false;
        bool showGliderPanel  = false;
        bool showEnv         = false;
#ifndef FITZEL_PLAYER
        // The offline renderer. Its open flag lives on the state rather
        // than beside the other bools because the panel, the harvest and
        // the running job are one thing, and splitting the visibility off
        // would be the only part of it main.cpp owned. Editor-only: the
        // shipped player has no reason to carry a renderer that takes
        // minutes a frame.
        pathpanel::State pathRender;
#endif
        bool showMixer       = false;
        bool showUnityImport = false;
        std::string modelFile;       // selected file in the Models panel
#ifndef FITZEL_PLAYER
        // "Import Unity Asset" panel: the browsed folder, the chosen FBX and its
        // texture-match preview (see UnityImportPanel.hpp).
        unityimportui::State unityImport;
#endif

        // The audio mixer. The desk itself lives in MixerPanel.hpp: Master
        // scales everything via the device, Ambient the looping weather/zone
        // voices, SFX the one-shot bus and the vehicles. What a fader is worth
        // right now is mix.ambientGain()/sfxGain()/masterGain() -- they are the
        // only place mute and solo are read.
        mixerui::Desk mix;

        // Projects: a project is a folder chosen by the user (New Project wizard)
        // containing <name>.fitzel + materials/. currentProject is the open
        // project's scene-file path ("" = unsaved/new). The default location the
        // wizard offers, plus the last-used location and a recent-projects list,
        // persist in editor.json next to the executable.
        const std::string defaultProjectsRoot =
            std::filesystem::absolute("projects").generic_string();
        std::string       currentProject;
        // Prefabs the running scripts have instantiated (game.spawnPrefab), cached
        // by lowercased name so repeat spawns don't re-read the file or re-import
        // its models. Cleared when the project changes (a different project has its
        // own prefabs/). Lives here (not editor-only) because scripts run in the
        // player too.
        std::unordered_map<std::string, prefab::Prefab> prefabCache;
        char              projNameBuf[64] = "";
#ifndef FITZEL_PLAYER
        // The Prefabs panel's rename/delete state. Here rather than in the panel
        // because it has to outlive the frames a modal is open (see PanelState).
        char        prefabRenameBuf[96] = "";
        std::string prefabSelPath, prefabSelName;   // the picked prefab, by file
        // Prefab packages (PrefabPackage.hpp): the import waiting for a yes in
        // its preview, what the last export or import had to say, and the folder
        // the last zip went to or came from.
        prefabpkg::Plan          prefabImportPlan;
        bool                     prefabImportPending = false;
        std::vector<std::string> prefabPackageNotes;
        std::string              prefabPackageDir;
#endif
        std::string       prefLocation = defaultProjectsRoot; // wizard default dir
        std::vector<std::string> recentProjects;              // folders, newest first
        const std::string prefsPath = "editor.json";
#ifndef FITZEL_PLAYER
        // Crash recovery. The writer snapshots the open scene every few minutes;
        // `pendingSnapshot` is what a session that never shut down left behind,
        // read once here and offered back by a dialog on the first frames. Beside
        // editor.json, for the same reason: this is the editor's own state on
        // this machine, not the project's. See Autosave.hpp.
        autosave::Autosave autoSave;
        autosave::Snapshot pendingSnapshot = autosave::pending(autoSave.dir());
#endif
        // UI comfort settings, also in editor.json. prefsDirty is written out at
        // the end of the frame, so dragging the size slider isn't one file write
        // per pixel.
        float       uiFontSize   = gui.fontSize();
        std::string uiFontFamily;
        bool        prefsDirty   = false;
        // New Project / Save As wizard state.
        bool wizardOpen  = false;   // request to (re)open the modal this frame
        bool wizardIsNew = true;    // true = New Project (reset scene), false = Save As
        char wizName[64]      = "";
        char wizLocation[512] = "";
        // Scene manager dialogs (a project may hold several .fitzel scenes).
        bool sceneNewOpen    = false;   // request to open the New Scene modal
        bool sceneRenameOpen = false;
        bool sceneDeleteOpen = false;
        char sceneNameBuf[64] = "";
        // Game Settings dialog (per-project: exe name, splash, start + export
        // scenes). Loaded from the project's game.json when the dialog is opened.
        bool          gameSettingsOpen = false;
        game::Settings gameSettings;
        // Scene look/settings serialization hooks. The tunable registry that
        // backs these is built later (once all the tunables exist), so saveScene/
        // loadScene call through these std::functions instead of the registry.
        std::function<void(nlohmann::json&)>       writeSettingsFn;
        std::function<void(const nlohmann::json&)> readSettingsFn;
        // Runs once per finished scene load, after the entity ids are settled.
        // Where a loaded scene is brought up to date with things that used to live
        // outside the entity list -- today: the terrain (see terrainWasEntity).
        std::function<void()>                      afterSceneLoadFn;
        // Seed a fresh project with the built-in materials (saved as project
        // .fmat files on first save); entities reference these by their GUID.
        auto seedDefaultMaterials = [&]() {
            document.addMaterial("Default", {0.72f, 0.72f, 0.74f}, 0.0f, 0.20f);
            document.addMaterial("Chrome",  {0.90f, 0.92f, 0.95f}, 1.0f, 0.04f);
            document.addMaterial("Red",     {0.72f, 0.12f, 0.10f}, 0.0f, 0.30f);
            // Glass: faint cool tint, smooth, a touch reflective; the glass flag
            // adds the Fresnel alpha (clear head-on, opaque reflective rim).
            document.addMaterial("Glass",   {0.85f, 0.92f, 0.95f}, 0.5f, 0.03f);
            MaterialDef& glass = materials.back();
            glass.opacity = 0.28f;
            glass.glass   = true;
        };
        seedDefaultMaterials();

        // Imported glTF/GLB models, uploaded to the GPU (see ModelLibrary). main
        // owns one registry and threads it in where models are placed/drawn.
        ModelLibrary models;
        // GPU copies of the entities' edited meshes (see MeshComponent). Keyed by
        // entity id and rebuilt when an edit stamps a new revision -- a GL mesh is
        // move-only, so it cannot live on the copyable Entity itself.
        EditMeshCache meshCache;
        // Videos playing into material textures (billboards). One decoder per
        // clip, shared by every material bound to it -- see VideoLibrary.
        VideoLibrary videos;
        // The scene always has exactly one Sun (directional light), non-deletable.
        {
            Entity sun;
            sun.type      = EntityType::Sun;
            sun.name      = "Sun";
            sun.id        = entityCounter++;
            sun.components.items.push_back(std::make_unique<SunComponent>());
            entities.push_back(std::move(sun));
        }
        // How the viewport draws the scene (see ViewShade.hpp).
        int viewShade = kShadeTextured;
#ifndef FITZEL_PLAYER
        viewtrace::State viewTrace;
#endif

        ImGuizmo::OPERATION gizmoOp = ImGuizmo::TRANSLATE; // Move / Scale (axis-aligned)
        // Gizmo reference frame: WORLD = global axes, LOCAL = the object's own axes.
        // Toggle from the toolbar or with X. (ImGuizmo forces SCALE to local anyway.)
        ImGuizmo::MODE gizmoMode = ImGuizmo::WORLD;
        // Where a new object goes when nothing else says where (a toolbar shape,
        // an imported model, a prefab, a generated building -- not a click in
        // Create mode or a drop, which name their own spot). Returns the point
        // its BASE rests on.
        //
        // While the 3D cursor is shown, that is the cursor, always -- Blender's
        // rule, and the one place you can aim precisely without dragging. With it
        // hidden, the object must land where you can see it, which "`dist` ahead,
        // dropped onto the ground" alone does not promise: from high up the
        // ground below that point is off the bottom of the screen, and looking
        // at the sky it is behind the horizon. So, in order:
        //  1. the ground in the middle of the view, if it is near enough that the
        //     object will not be a speck;
        //  2. `dist` ahead on the ground, if that ground is on screen;
        //  3. `dist` ahead in the air -- floating, but in front of you.
#ifndef FITZEL_PLAYER
        auto spawnPoint = [&](float dist) -> glm::vec3 {
            if (cursor.visible) return cursor.pos;
            const glm::vec3 eye = camera.position();
            const glm::mat4 vp  = camera.projectionMatrix(
                                      static_cast<float>(viewW) / static_cast<float>(viewH)) *
                                  camera.viewMatrix();
            glm::vec3 hit;
            if (roadPickTerrain(glm::vec2(0.0f), vp, hit) &&
                glm::length(hit - eye) <= dist * 3.0f)
                return hit;
            const glm::vec3 p = eye + camera.front() * dist;
            const glm::vec3 g(p.x, streamer.heightAt(p.x, p.z), p.z);
            const glm::vec4 c = vp * glm::vec4(g, 1.0f);
            if (c.w > 1e-4f && std::abs(c.x) <= 0.85f * c.w && std::abs(c.y) <= 0.85f * c.w)
                return g;
            return glm::vec3(p.x, std::max(p.y, g.y), p.z);
        };
#endif
        // Add an entity of the given type, sitting on the terrain at a world point.
        auto addEntity = [&](glm::vec3 groundPos, EntityType type) {
            Entity nb;
            nb.type   = type;
            // Light/Empty are markers with no real geometry: give them a small,
            // fixed half so they still get a clickable pick box in the viewport.
            // A Plane has no thickness to give, so it gets a thin one: the half
            // is what the pick box, the gizmo and the collider are all made of,
            // and a flat quad inside a two-metre cube reads as a bug in every
            // one of them.
            nb.half   = (type == EntityType::Light) ? glm::vec3(0.3f)
                      : (type == EntityType::Empty) ? glm::vec3(0.5f)
                      : (type == EntityType::Plane)
                            ? glm::vec3(entityNewHalf.x, kPlaneHalfY, entityNewHalf.z)
                      : entityNewHalf;
            nb.localCenter = nb.center =
                glm::vec3(groundPos.x, groundPos.y + nb.half.y, groundPos.z);
            if (type == EntityType::Light)
                nb.components.items.push_back(std::make_unique<LightComponent>());
            const bool solid = isSolidPrimitive(type);
            if (solid && !materials.empty()) {
                auto mc = std::make_unique<MaterialComponent>();
                mc->material = materials[glm::clamp(matSel, 0,
                                   static_cast<int>(materials.size()) - 1)].assetId;
                nb.components.items.push_back(std::move(mc));
            }
            nb.id     = entityCounter++;
            nb.name   = std::string(entityTypeName(type)) + " " + std::to_string(nb.id);
            history.push(std::make_unique<AddEntityCmd>(nb), document);
            sel.select(nb.id);
        };
        // World-space half-extents of a placed model (its local AABB * scale).
        auto modelHalf = [&](const LoadedModel& lm, float sc) {
            return 0.5f * lm.size() * sc;
        };
        // Build translate * rotate(euler deg) * scale via ImGuizmo's own compose
        // so the gizmo and the rendered transform share one Euler convention.
        // The scene's composition convention, shared with everything else that
        // has to agree with it exactly (see SceneGraph.hpp).
        auto composeModel = [](const glm::vec3& t, const glm::vec3& rotDeg,
                               const glm::vec3& s) {
            return scenegraph::compose(t, rotDeg, s);
        };
        // Place an imported model as a Model entity sitting on the terrain.
        auto addModelEntity = [&](glm::vec3 groundPos, int modelId) {
            LoadedModel* lm = models.byId(modelId);
            if (!lm) return;
            Entity nb;
            nb.type       = EntityType::Model;
            auto mc       = std::make_unique<ModelComponent>();
            mc->modelId   = modelId;
            mc->modelPath = lm->path;
            mc->scale     = 1.0f;
            nb.components.items.push_back(std::move(mc));
            nb.half       = modelHalf(*lm, 1.0f); // AABB (for picking/gizmo)
            // The render transform centres the model's AABB at nb.center, so lift
            // by half.y to rest its base on the ground.
            nb.localCenter = nb.center =
                glm::vec3(groundPos.x, groundPos.y + nb.half.y, groundPos.z);
            nb.id     = entityCounter++;
            nb.name   = lm->name + " " + std::to_string(nb.id);
            history.push(std::make_unique<AddEntityCmd>(nb), document);
            sel.select(nb.id);
        };
        // Structure-preserving import: one entity per model node under a group
        // root, so each element is separately selectable/movable in the scene.
        auto addModelHierarchy = [&](glm::vec3 groundPos, const std::string& path,
                                     bool flipV = true) {
            const auto& ns = models.nodes(path, flipV);
            if (ns.empty()) { // no node structure -> fall back to a single model
                const int id = models.import(path, assetDb, materials);
                if (id >= 0) addModelEntity(groundPos, id);
                return;
            }
            std::vector<int>       nodeIds(ns.size(), -1);
            std::vector<glm::vec3> nodeHalf(ns.size(), glm::vec3(0.1f));
            glm::vec3 lo(1e30f), hi(-1e30f);
            for (std::size_t i = 0; i < ns.size(); ++i) {
                nodeIds[i] = models.importNode(path, static_cast<int>(i), flipV, assetDb, materials);
                if (LoadedModel* lm = models.byId(nodeIds[i])) nodeHalf[i] = modelHalf(*lm, 1.0f);
                lo = glm::min(lo, ns[i].center - nodeHalf[i]);
                hi = glm::max(hi, ns[i].center + nodeHalf[i]);
            }
            const glm::vec3 oc = 0.5f * (lo + hi), oh = 0.5f * (hi - lo);
            const std::string stem = std::filesystem::path(path).stem().string();
            // Group root: a Model-type entity with NO ModelComponent, so it
            // renders nothing and just parents the parts. Placed at the model's
            // centre, lifted so its base rests on the ground.
            Entity root;
            root.type = EntityType::Model;
            root.name = stem;
            root.half = oh;
            root.localCenter = root.center = glm::vec3(
                groundPos.x + oc.x, groundPos.y + oh.y, groundPos.z + oc.z);
            root.id = entityCounter++;
            history.push(std::make_unique<AddEntityCmd>(root), document);
            const int rootId = root.id;
            for (std::size_t i = 0; i < ns.size(); ++i) {
                if (nodeIds[i] < 0) continue;
                Entity ch;
                ch.type   = EntityType::Model;
                ch.name   = ns[i].name.empty() ? ("part " + std::to_string(i)) : ns[i].name;
                ch.parent = rootId;
                auto mc = std::make_unique<ModelComponent>();
                mc->modelId = nodeIds[i]; mc->modelPath = path;
                mc->nodeIndex = static_cast<int>(i); mc->scale = 1.0f;
                ch.components.items.push_back(std::move(mc));
                ch.half        = nodeHalf[i];
                ch.localCenter = ns[i].center - oc; // relative to the root
                ch.id          = entityCounter++;
                history.push(std::make_unique<AddEntityCmd>(ch), document);
            }
            sel.select(rootId);
        };
        // --- Object scatter helpers (the brush application lives in the
        //     viewport block; panel + placement math live in ScatterTool) -----
        // Editor-only: scattered objects persist as ordinary Model entities, so
        // the player needs none of this (and ScatterTool is not linked into it).
#ifndef FITZEL_PLAYER
        // The root Empty grouping every scattered object, or -1 when absent.
        auto findScatterGroup = [&]() -> int {
            for (const Entity& e : entities)
                if (e.parent < 0 && e.type == EntityType::Empty &&
                    e.name == "Scattered")
                    return e.id;
            return -1;
        };
        // XZ of the group's children (spacing rejects placements against them).
        auto scatterOccupied = [&](int groupId) {
            std::vector<glm::vec2> out;
            if (groupId < 0) return out;
            for (const Entity& e : entities)
                if (e.parent == groupId)
                    out.emplace_back(e.center.x, e.center.z);
            return out;
        };
        // Adopt freshly built placements: assign ids/parent/name suffix and push
        // them (plus the group, if it had to be created) as ONE undoable step.
        auto commitScatter = [&](std::vector<Entity> placed) {
            if (placed.empty()) return;
            int groupId = findScatterGroup();
            std::vector<Entity> batch;
            batch.reserve(placed.size() + 1);
            if (groupId < 0) {
                Entity g;
                g.type = EntityType::Empty;
                g.half = glm::vec3(0.5f);
                g.name = "Scattered";
                g.id   = entityCounter++;
                groupId = g.id;
                batch.push_back(std::move(g));
            }
            for (Entity& e : placed) {
                e.id     = entityCounter++;
                e.parent = groupId;
                e.name  += " " + std::to_string(e.id);
                batch.push_back(std::move(e));
            }
            history.push(std::make_unique<AddEntitiesCmd>(std::move(batch), "Scatter"),
                         document);
        };
        // One scatter-brush stamp at world XZ `c`.
        auto scatterStamp = [&](glm::vec2 c) {
            commitScatter(scatterui::buildStamp(
                scatterCfg, models, streamer, c, waterLevel,
                scatterOccupied(findScatterGroup()), brushRng));
        };
        // Erase scattered objects under the brush as one undoable step.
        auto scatterErase = [&](glm::vec2 c) {
            const auto ids = scatterui::collectInBrush(document, findScatterGroup(),
                                                       c, scatterCfg.radius);
            if (!ids.empty())
                history.push(std::make_unique<DeleteEntitiesCmd>(document, ids),
                             document);
        };
        // Populate both roadsides (well, the configured side(s)) in one click.
        auto scatterRoadside = [&]() {
            const RoadSystem& road = roads.active();
            const RoadSystem::Preview pv = road.previewGeometry();
            if (pv.center.size() < 2) return;
            std::vector<glm::vec2> cl;
            cl.reserve(pv.center.size());
            for (const glm::vec3& p : pv.center) cl.emplace_back(p.x, p.z);
            commitScatter(scatterui::buildRoadside(
                scatterCfg, models, streamer, cl, road.width * 0.5f, waterLevel,
                scatterOccupied(findScatterGroup()), brushRng));
        };
        // Undoable "Clear all": the group and every child in one step.
        auto scatterClearAll = [&]() {
            const int groupId = findScatterGroup();
            if (groupId < 0) return;
            std::vector<int> ids{groupId};
            for (const Entity& e : entities)
                if (e.parent == groupId) ids.push_back(e.id);
            history.push(std::make_unique<DeleteEntitiesCmd>(document, ids), document);
            sel.clear();
        };
#endif // !FITZEL_PLAYER
        // Decide whether a model imports as a hierarchy (one entity per node,
        // separately selectable) or as a single flat Model entity.
        //   - An animated (skinned) model must stay on the flat path so CPU
        //     skinning still runs; the structured path bakes node transforms and
        //     drops the skeleton, so it's never used for animated models. That is
        //     what routes a rigged character -- FBX or glTF -- to a single entity.
        //   - Everything else splits only when it actually has more than one mesh
        //     node; a single-part model stays one clean entity.
        auto isStructuredModel = [&](const std::string& p) {
            std::string e = std::filesystem::path(p).extension().string();
            for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (e != ".fbx" && e != ".glb" && e != ".gltf" && e != ".dae") return false;
            if (auto md = assetDb.loadModelData(p); md && md->animated()) return false;
            return models.nodes(p).size() > 1;
        };

        // --- Project (scene) save / load / export ----------------------------
        // Serialization now lives in ProjectIO (projectio::). main still owns the
        // scene data + asset database; it threads them in through a Context of
        // references and callbacks, built once here. Thin forwarding lambdas keep
        // the existing call sites (menus, wizard, player boot) unchanged.
        std::string exportStatus; // shown under the File menu after an export
#ifndef FITZEL_PLAYER
        // The editor's core, for the tools that live outside main() (see
        // EditorContext.hpp). Built once: everything in it lives as long as main.
        EditorContext editorCtx{document, entities, sel, history, materials, matSel, assetDb,
                                models, camera, exportStatus, meshFaceOwner, meshFaceSel,
                                entityCounter, addModelEntity,
                                [&](glm::vec3 p, const std::string& path) {
                                    addModelHierarchy(p, path);
                                },
                                isStructuredModel};
#endif
#ifndef FITZEL_PLAYER
        // Editing a prefab on its own (see PrefabEdit.hpp). Declared HERE, above
        // every lambda that saves or replaces the document, because while a
        // session is up the document is not the scene and all of those have to
        // refuse -- a "Save Project" during one would write the prefab's stage
        // over the track.
        prefabedit::Session prefabEdit;
        auto prefabEditBusy = [&](const char* what) {
            if (!prefabEdit.active) return false;
            exportStatus = std::string(what) +
                           " is not available while a prefab is open for editing.";
            return true;
        };
#else
        struct { bool active = false; } prefabEdit;
        auto prefabEditBusy = [](const char*) { return false; };
#endif
        projectio::Context pio{
            entities, materials, matSel, entityCounter, sel,
            currentProject, projNameBuf, sizeof(projNameBuf), prefLocation,
            recentProjects, prefsPath, exportStatus, uiFontSize, uiFontFamily,
            assetDb, contentRoot, modelDir,
            [&]{ seedDefaultMaterials(); },
            [&](const std::string& p){ return models.import(p, assetDb, materials); },
            [&](const std::string& p, int n){ return models.importNode(p, n, true, assetDb, materials); },
            [&](int id){ return models.byId(id); },
            // Clearing the model library invalidates every cached prefab's resolved
            // modelIds, so drop the prefab cache on the same beat (every loadScene).
            // ...and the modelled meshes, keyed by entity id: the next scene's ids
            // start again from 0, so a stale entry would hand a new object the
            // shape of an old one.
            [&]{ models.clear(); prefabCache.clear(); meshCache.clear(); modifiers::clearCache(); },
            writeSettingsFn, readSettingsFn, afterSceneLoadFn,
        };
        projectio::loadPrefs(pio);
        // Apply the saved UI text settings (the typeface by name, so a prefs file
        // naming a font this machine doesn't have just keeps the default).
        gui.setFontSize(uiFontSize);
        for (int i = 0; i < gui.fontFamilyCount(); ++i)
            if (uiFontFamily == gui.fontFamilyName(i)) { gui.setFontFamily(i); break; }
        ui::setBoldFont(gui.boldFont());

        // Saving (any of the three ways) puts the work in the project, so the
        // snapshot has nothing left to offer and is dropped -- otherwise the next
        // start would ask about work that is not missing. In the player it is a
        // no-op: there is no editing session to lose.
#ifndef FITZEL_PLAYER
        auto noteSaved        = [&]{ autoSave.clear(); };
#else
        auto noteSaved        = []{};
#endif
        auto safeName             = [&](const std::string& s){ return projectio::safeName(s); };
        auto loadProjectMaterials = [&](const std::string& d){ projectio::loadProjectMaterials(pio, d); };
        // A new project and Save As get their folder HERE, not at an open: the
        // road-surface and tree lists have to scan it too, or whatever is saved
        // into it afterwards (a generated tree) never appears in their pickers.
        auto saveProjectTo        = [&](const std::string& f){
            if (prefabEditBusy("Saving the project")) return;
            projectio::saveProjectTo(pio, f);
            const auto norm = [](const std::string& p) {
                return std::filesystem::path(p).lexically_normal().generic_string();
            };
            if (norm(veg.projectDir()) != norm(f)) { roads.refreshTextures(f); veg.refreshTreeAssets(f); }
            noteSaved();
        };
        auto saveCurrent          = [&](){ if (prefabEditBusy("Saving the project")) return; projectio::saveCurrent(pio); noteSaved(); };
        auto exportGame           = [&](const std::string& o){ if (prefabEditBusy("Exporting the game")) return; projectio::exportGame(pio, o); };
        auto exportWeb            = [&](const std::string& o){ if (prefabEditBusy("Exporting the game")) return; projectio::exportGame(pio, o, projectio::ExportTarget::Web); };
        auto listProjectsIn       = [&](const std::string& r){ return projectio::listProjectsIn(r); };
        // Loading/creating a project replaces the document, so the undo history
        // must not survive the boundary.
        // Rescan road-surface textures and tree assets to include the project being
        // opened before the scene loads (loadScene restores the saved surface/trees
        // by name, so the project's files must already be in the lists by then).
        auto newProject           = [&](){ if (prefabEditBusy("Starting a project")) return; projectio::newProject(pio); history.clear(); prefabCache.clear(); roads.refreshTextures(std::string()); veg.refreshTreeAssets(std::string()); };

        // Non-blocking editor loads: kick off an incremental scene load, then step
        // it each frame (below) so the UI keeps drawing with a progress bar. The
        // player's boot and a scene trigger drive the same loader, but drain it in
        // one go behind the loading screen (openProjectShowing / loadSceneShowing
        // below) -- they need the scene complete before continuing.
        projectio::SceneLoad sceneLoad;
        auto openProjectAsync = [&](const std::string& f){
            if (prefabEditBusy("Opening a project")) return false;
            roads.refreshTextures(f); veg.refreshTreeAssets(f);
            history.clear(); prefabCache.clear();
            return projectio::beginOpenProject(pio, sceneLoad, f);
        };
        auto loadSceneAsync = [&](const std::string& p){
            if (prefabEditBusy("Opening a scene")) return false;
            history.clear(); prefabCache.clear();
            return projectio::beginLoadScene(pio, sceneLoad, p);
        };
#ifndef FITZEL_PLAYER
        // Crash recovery, which is an ordinary project open in every respect
        // except where the scene comes from -- so it does exactly what
        // openProjectAsync does around it, and differs in that one line.
        auto restoreSnapshot = [&](const autosave::Snapshot& snap){
            roads.refreshTextures(snap.projectFolder);
            veg.refreshTreeAssets(snap.projectFolder);
            history.clear(); prefabCache.clear();
            return projectio::beginRecoveredProject(pio, sceneLoad,
                                                    snap.projectFolder, snap.file,
                                                    snap.scenePath);
        };
#endif
        // Scenes within the open project. Switching/creating replaces the document,
        // so the undo history is cleared at the boundary (like opening a project).
        auto listScenesIn         = [&](const std::string& f){ return projectio::listScenesIn(f); };
        auto saveSceneFile        = [&](const std::string& p){ if (prefabEditBusy("Saving the scene")) return; projectio::saveScene(pio, p); noteSaved(); };
        auto loadSceneFile        = [&](const std::string& p){ if (prefabEditBusy("Opening a scene")) return false; const bool ok = projectio::loadSceneFile(pio, p); history.clear(); prefabCache.clear(); return ok; };
        auto newSceneInProject    = [&](const std::string& f, const std::string& n){ if (prefabEditBusy("Starting a scene")) return std::string(); auto p = projectio::newSceneInProject(pio, f, n); history.clear(); return p; };
        auto renameScene          = [&](const std::string& p, const std::string& n){ return projectio::renameScene(pio, p, n); };
        auto deleteSceneFile      = [&](const std::string& p){ return projectio::deleteSceneFile(p); };

        // A level change, with the loading screen up.
        //
        // It blocks -- the caller wants the new scene complete before it does
        // anything else -- but blocking is not the same as going dark: the
        // incremental loader is stepped by hand here and a frame of loading
        // screen painted between the steps. Loading it in ONE call is what left
        // the window unredrawn long enough for the desktop to paint its own white
        // rectangle over it, which is the thing this replaces.
        //
        // The style is re-read from the project each time rather than cached: it
        // is one small JSON, and reading it here means editing the screen in the
        // dialog shows up on the very next level change instead of after a
        // restart.
        auto loadSceneShowing = [&](const std::string& path, const std::string& what) {
            const std::string folder =
                std::filesystem::path(path).parent_path().generic_string();
            loading.setProjectFolder(folder);
            loading.setStyle(game::load(folder).loading);

            projectio::SceneLoad ld; // local: the editor's own sceneLoad is not ours
            if (!projectio::beginLoadScene(pio, ld, path)) return false;
            history.clear();
            prefabCache.clear();
            loading.frame(window, gui, 0.0f, "Loading " + what + "...");
            while (!ld.done) {
                // A bigger slice than the editor's 8 ms: nothing else is drawing,
                // so time spent painting more loading frames is time the level is
                // not loading.
                projectio::stepLoad(pio, ld, 24.0);
                loading.frame(window, gui, ld.progress, ld.label);
            }
            return ld.ok;
        };
        // Opening the whole project the same way, for the player's boot: mounts
        // and materials first, then the scene streamed in with the bar moving.
        // This is the longest wait the game ever has, and the one that used to be
        // spent staring at an unpainted window.
        auto openProjectShowing = [&](const std::string& folder) {
            roads.refreshTextures(folder);
            veg.refreshTreeAssets(folder);
            loading.setProjectFolder(folder);
            loading.setStyle(game::load(folder).loading);

            projectio::SceneLoad ld;
            if (!projectio::beginOpenProject(pio, ld, folder)) return false;
            history.clear();
            prefabCache.clear();
            while (!ld.done) {
                projectio::stepLoad(pio, ld, 24.0);
                loading.frame(window, gui, ld.progress, ld.label);
            }
            return ld.ok;
        };

        // World transform (translate*rotate, ImGuizmo Euler convention) of an
        // entity's cached world center/rotation. Scale is not part of the
        // hierarchy -- each entity keeps its own size (half).
        auto worldOf = [&](const Entity& e) { return scenegraph::worldOf(e); };
        // Convert a world-space edit (gizmo, physics) into the entity's LOCAL
        // transform (the source of truth), given its parent's world matrix (null
        // for a root). Also mirrors into center/rotation for this frame.
        auto setWorld = [&](Entity& e, const glm::vec3& wPos, const glm::vec3& wRot,
                            const glm::mat4* parentWorld) {
            scenegraph::setWorld(e, wPos, wRot, parentWorld);
        };
        // Rebase local onto a (changed) parent so the entity's current world stays
        // put -- used on reparent/unparent.
        auto rebaseLocal = [&](Entity& e, const glm::mat4* parentWorld) {
            setWorld(e, e.center, e.rotation, parentWorld);
        };
        // Scene-graph resolve: LOCAL transform is the source of truth; derive every
        // entity's WORLD (center/rotation, what all consumers read) from
        // parentWorld * local, parents first. Behaviours/scripts/inspector write
        // local, so children inherit a parent's motion + rotation automatically.
        std::function<void(Entity&, std::unordered_set<int>&)> resolveOne =
            [&](Entity& e, std::unordered_set<int>& done) {
                if (!done.insert(e.id).second) return;
                Entity* p = (e.parent >= 0) ? document.find(e.parent) : nullptr;
                if (p) resolveOne(*p, done);
                // Effective visibility: off if this object or any ancestor is off.
                e.activeInHierarchy = e.active && (!p || p->activeInHierarchy);
                if (!p) { e.center = e.localCenter; e.rotation = e.localRotation; }
                else {
                    const glm::mat4 w =
                        worldOf(*p) * composeModel(e.localCenter, e.localRotation, glm::vec3(1.0f));
                    float t[3], r[3], s[3];
                    ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(w), t, r, s);
                    e.center   = glm::vec3(t[0], t[1], t[2]);
                    e.rotation = glm::vec3(r[0], r[1], r[2]);
                }
            };
        auto resolveHierarchy = [&]() {
            std::unordered_set<int> done;
            done.reserve(entities.size());
            for (Entity& e : entities) resolveOne(e, done);
        };
        // World matrix of an entity's PARENT (identity for a root) -- for setWorld.
        auto parentWorldMat = [&](const Entity& e) -> glm::mat4 {
            return scenegraph::parentWorld(entities, e);
        };

        // Is anything selected (the 3D cursor's snap operations are in Cursor3D.cpp).
        auto cursorHaveSel = [&] {
            return sel.valid();
        };

#ifndef FITZEL_PLAYER
        // --- Face modelling (ModelMode.cpp) ---------------------------------------
        // The editable mesh on the selected object, if it has one; a solid made
        // one; a face operation run as one undo step.
        auto selectedMesh  = [&]() -> MeshComponent* { return modelmode::selectedMesh(editorCtx); };
        auto convertToMesh = [&] { modelmode::convertToMesh(editorCtx); };
        auto applyMeshEdit = [&](const std::function<int(MeshComponent&)>& op,
                                 const char* label) {
            modelmode::applyEdit(editorCtx, op, label);
        };
#endif // !FITZEL_PLAYER
#ifndef FITZEL_PLAYER
        // --- Operations on objects (SceneOps.cpp) -----------------------------
        // Delete / duplicate one object or the selection, unpack a prefab, set
        // the main camera, and the hierarchy menu's "add ..." family -- each one
        // undoable step. Named here for the panels and menus that take them.
        auto isUnderId          = [&](int a, int anc) { return sceneops::isUnder(entities, a, anc); };
        auto deleteEntity       = [&](int i) { sceneops::deleteEntity(editorCtx, i); };
        auto duplicateEntity    = [&](int i) { sceneops::duplicateEntity(editorCtx, i); };
        auto deleteSelection    = [&] { sceneops::deleteSelection(editorCtx); };
        auto duplicateSelection = [&] { sceneops::duplicateSelection(editorCtx); };
        auto unpackPrefab       = [&](int id) { sceneops::unpackPrefab(editorCtx, id); };
        auto setMainCamera      = [&](int id) { sceneops::setMainCamera(editorCtx, id); };
        auto addEmptyChild      = [&](int i) { sceneops::addEmptyChild(editorCtx, i); };
        auto addPrimitiveChild  = [&](int i, EntityType t) {
            sceneops::addPrimitiveChild(editorCtx, i, t, entityNewHalf);
        };
        auto addClothChild      = [&](int i, int which) { sceneops::addClothChild(editorCtx, i, which); };
        auto addShotCamera      = [&](int i) { sceneops::addShotCamera(editorCtx, i); };
        auto addCockpitCamera   = [&](int i) { sceneops::addCockpitCamera(editorCtx, i); };
        auto addEmptyParent     = [&](int i) { sceneops::addEmptyParent(editorCtx, i); };
        auto addVehicleLights   = [&](int i) { sceneops::addVehicleLights(editorCtx, i); };
#endif // !FITZEL_PLAYER
#ifndef FITZEL_PLAYER
        // --- Prefabs (reusable object templates; see PrefabSystem.hpp) ----------
        // The open project's prefabs/ folder ("" when no project is open -- prefabs
        // are per-project assets, like materials).
        auto prefabDir = [&]() -> std::string {
            if (currentProject.empty()) return std::string();
            return prefab::prefabsDirIn(
                std::filesystem::path(currentProject).parent_path().generic_string());
        };
        // Save the selected entity's subtree as a new .fprefab in the project. The
        // outcome (name saved, or why not) goes to the status line.
        auto createPrefabFromSelection = [&](const std::string& name) -> bool {
            if (!sel.valid())
                return false;
            const std::string dir = prefabDir();
            if (dir.empty()) {
                exportStatus = "Open a project first to save prefabs.";
                return false;
            }
            auto p = prefab::fromSubtree(entities, entities[sel.index()].id, name);
            if (!p) { exportStatus = "Can't make a prefab from this object."; return false; }
            if (!prefab::save(pio, *p, dir)) {
                exportStatus = "Failed to write prefab.";
                return false;
            }
            exportStatus = "Saved prefab: " + p->name;
            return true;
        };
        // Load a .fprefab and drop an instance into the scene at the spawn point
        // (3D cursor, or in view), as one undoable step. Selects the new root.
        auto instantiatePrefabFile = [&](const std::string& path) {
            auto p = prefab::load(pio, path);
            if (!p || p->entities.empty()) {
                exportStatus = "Failed to load prefab.";
                return;
            }
            const glm::vec3 g = spawnPoint(8.0f);
            std::vector<Entity> spawn = prefab::instantiate(*p, entityCounter, g, 0.0f);
            const int rootId = spawn.empty() ? -1 : spawn.front().id;
            history.push(std::make_unique<AddEntitiesCmd>(std::move(spawn), "Prefab"),
                         document);
            if (rootId >= 0) sel.select(rootId);
        };

#ifndef FITZEL_PLAYER
        // Rename a prefab file. The GUID is kept, so instances already placed in
        // scenes still resolve; the spawn cache is dropped because it is keyed by
        // NAME, and the old key would go on handing out the old file.
        auto renamePrefabFile = [&](const std::string& path,
                                    const std::string& newName) {
            std::string err;
            const std::string fresh = prefab::renameTo(path, newName, err);
            if (fresh.empty()) {
                exportStatus = "Rename failed: " + err;
                return;
            }
            prefabCache.clear();
            exportStatus = "Renamed prefab to: " + newName;
        };
        // ...and delete one. The panel asks first -- a file is not in the undo
        // history, so this is the one prefab operation Ctrl+Z cannot walk back.
        auto deletePrefabFile = [&](const std::string& path) {
            std::string err;
            if (!prefab::deleteFile(path, err)) {
                exportStatus = "Delete failed: " + err;
                return;
            }
            prefabCache.clear();
            exportStatus = "Deleted prefab: " +
                           std::filesystem::path(path).stem().generic_string();
        };

        // --- Prefabs to and from other projects (see PrefabPackage.hpp) ---------
        // The zips start out beside the projects, where the other project is.
        auto packageDir = [&]() -> std::string {
            if (!prefabPackageDir.empty()) return prefabPackageDir;
            return currentProject.empty() ? std::string()
                 : std::filesystem::path(currentProject).parent_path().parent_path()
                       .generic_string();
        };
        auto projectFolder = [&]() {
            return std::filesystem::path(currentProject).parent_path().generic_string();
        };
        auto exportPrefabZip = [&](const std::string& path, const std::string& name) {
            if (currentProject.empty()) { exportStatus = "Open a project first."; return; }
            std::string zip;
            if (!ed::saveFile(zip, packageDir(), projectio::safeName(name) + ".zip",
                              "Prefab package", "*.zip", "zip"))
                return;
            prefabPackageDir = std::filesystem::path(zip).parent_path().generic_string();
            // The open scene's graphs stand in for a prefab saved before prefabs
            // carried their own.
            prefabpkg::ExportSource src{projectFolder(), &assetDb, pio.animGraphs};
            prefabpkg::Report rep;
            std::string err;
            if (!prefabpkg::exportZip(src, path, zip, rep, err)) {
                exportStatus = "Export failed: " + err;
                return;
            }
            prefabPackageNotes = rep.notes;
            exportStatus = "Exported \"" + name + "\" with " + std::to_string(rep.files.size()) +
                           " files to " + zip;
        };
        auto importPrefabZip = [&] {
            if (currentProject.empty()) { exportStatus = "Open a project first."; return; }
            std::string zip;
            if (!ed::pickFile(zip, packageDir(), "Prefab package", "*.zip")) return;
            prefabPackageDir = std::filesystem::path(zip).parent_path().generic_string();
            std::string err;
            if (!prefabpkg::planImport({projectFolder(), &assetDb}, zip, prefabImportPlan, err)) {
                exportStatus = "Import failed: " + err;
                return;
            }
            prefabImportPending = true;   // the panel shows the plan and asks
        };
        auto confirmPrefabImport = [&] {
            prefabImportPending = false;
            prefabpkg::Applied done;
            std::string err;
            if (!prefabpkg::applyImport({projectFolder(), &assetDb}, prefabImportPlan, done, err)) {
                exportStatus = "Import failed: " + err;
                return;
            }
            // The new files join the asset database and the new materials the
            // library now -- the next project save would otherwise delete them.
            if (!projectio::adoptNewFiles(pio))
                projectio::loadProjectMaterials(pio, projectio::matsDirIn(projectFolder()));
            prefabCache.clear();          // an updated prefab must not spawn stale
            prefabPackageNotes = prefabImportPlan.notes;
            prefabSelPath = done.prefabPath;
            prefabSelName = prefabImportPlan.prefabName;
            exportStatus = "Imported \"" + prefabImportPlan.prefabName + "\": " +
                           std::to_string(done.written.size()) + " files written";
        };
#endif

        // --- Prefabs along the road (see RoadPrefab.hpp) -----------------------
        // Tool settings only; what they place is ordinary entities, so nothing
        // here is saved with the scene.
        roadprefab::Settings roadPrefabCfg;
        // The group every stamp lands under, so a run can be selected, moved or
        // deleted as one -- the same trick the scatter brush uses.
        auto findRoadPrefabGroup = [&]() -> int {
            for (const Entity& e : entities)
                if (e.parent < 0 && e.type == EntityType::Empty &&
                    e.name == "Road prefabs")
                    return e.id;
            return -1;
        };
        // Stamp the configured prefab along the built centreline: one instance per
        // station, all of them (plus the group, if it had to be created) as ONE
        // undoable step. The placement walk is the road's own, so a prefab lands
        // exactly where a side-object line with the same numbers would have.
        auto placeRoadPrefabs = [&]() {
            if (roadPrefabCfg.path.empty()) {
                exportStatus = "Pick a prefab to place along the road.";
                return;
            }
            const auto at = roads.active().placeLine(roadprefab::asLine(roadPrefabCfg));
            if (at.empty()) {
                exportStatus = "Nothing to place -- build the road first.";
                return;
            }
            auto p = prefab::load(pio, roadPrefabCfg.path);
            if (!p || p->entities.empty()) {
                exportStatus = "Failed to load prefab.";
                return;
            }
            std::vector<Entity> placed = roadprefab::stamp(*p, at, entityCounter);
            if (placed.empty()) return;
            std::vector<Entity> batch;
            batch.reserve(placed.size() + 1);
            int groupId = findRoadPrefabGroup();
            if (groupId < 0) {          // the group has to precede its children
                Entity g;
                g.type = EntityType::Empty;
                g.half = glm::vec3(0.5f);
                g.name = "Road prefabs";
                g.id   = entityCounter++;
                groupId = g.id;
                batch.push_back(std::move(g));
            }
            for (Entity& e : placed) {
                if (e.parent < 0) e.parent = groupId; // instance roots only
                batch.push_back(std::move(e));
            }
            history.push(std::make_unique<AddEntitiesCmd>(std::move(batch),
                                                          "Prefabs along the road"),
                         document);
            exportStatus = "Placed " + std::to_string(at.size()) + "x " +
                           roadPrefabCfg.name + " along the road.";
        };

        // --- Objects along a spline path (see SplinePlace.hpp) -----------------
        // The road stamp above, for any spline path: tool settings only, and what
        // they place is ordinary entities, one group per path, one undo step.
        splineplace::Settings splinePlaceCfg;
        auto placeAlongSpline = [&]() {
            if (splineSel < 0 || splineSel >= static_cast<int>(splines.paths.size()))
                return;
            const std::string pathName = splines.paths[splineSel].name;
            std::vector<splineplace::Spot> at =
                splineplace::spots(splines, splineSel, splinePlaceCfg);
            if (at.empty()) {
                exportStatus = "Nothing to place -- the path needs at least two points.";
                return;
            }
            prefab::Prefab tmpl;
            if (splinePlaceCfg.source == splineplace::Source::Prefab) {
                if (splinePlaceCfg.path.empty()) {
                    exportStatus = "Pick a prefab to place along the path.";
                    return;
                }
                auto p = prefab::load(pio, splinePlaceCfg.path);
                if (!p || p->entities.empty()) {
                    exportStatus = "Failed to load prefab.";
                    return;
                }
                tmpl = std::move(*p);
            } else {
                if (!sel.valid()) {
                    exportStatus = "Select the object to copy along the path first.";
                    return;
                }
                const Entity& src = entities[sel.index()];
                if (src.type == EntityType::Sun) {
                    exportStatus = "The sun can't be copied.";
                    return;
                }
                tmpl = splineplace::fromScene(entities, src.id);
                // Copies stand the way the original stands: as far above the path
                // as it is above the ground under it now. A box sitting on the
                // ground stays sitting; a lamp hung at three metres stays hung.
                const float clear =
                    src.center.y - streamer.heightAt(src.center.x, src.center.z);
                for (splineplace::Spot& s : at) s.pos.y += clear;
            }
            // A scene has one terrain; twenty copies of it would be twenty.
            for (const Entity& e : tmpl.entities)
                if (e.components.get<TerrainComponent>()) {
                    exportStatus = "The terrain can't be copied along a path.";
                    return;
                }
            std::vector<Entity> placed =
                splineplace::stamp(tmpl, at, splinePlaceCfg.scale, entityCounter);
            if (placed.empty()) return;

            // One group per path, so a run can be selected, moved or deleted as
            // one -- and placing again along the same path adds to it.
            const std::string groupName = "Along " + pathName;
            int groupId = -1;
            for (const Entity& e : entities)
                if (e.parent < 0 && e.type == EntityType::Empty && e.name == groupName) {
                    groupId = e.id;
                    break;
                }
            std::vector<Entity> batch;
            batch.reserve(placed.size() + 1);
            if (groupId < 0) {          // the group has to precede its children
                Entity g;
                g.type = EntityType::Empty;
                g.half = glm::vec3(0.5f);
                g.name = groupName;
                g.id   = entityCounter++;
                groupId = g.id;
                batch.push_back(std::move(g));
            }
            for (Entity& e : placed) {
                if (e.parent < 0) e.parent = groupId; // copy roots only
                batch.push_back(std::move(e));
            }
            history.push(std::make_unique<AddEntitiesCmd>(std::move(batch),
                                                          "Objects along a path"),
                         document);
            exportStatus = "Placed " + std::to_string(at.size()) + "x " + tmpl.name +
                           " along " + pathName + ".";
        };
#endif // !FITZEL_PLAYER
        // Ids of an entity and all its descendants (for a parented gizmo drag).
        auto collectSubtreeIds = [&](int rootId) { return scenegraph::subtree(entities, rootId); };
        auto snapshotEntities = [&](const std::vector<int>& ids) {
            std::vector<Entity> out;
            out.reserve(ids.size());
            for (int id : ids) if (const Entity* e = document.find(id)) out.push_back(*e);
            return out;
        };

#ifndef FITZEL_PLAYER
        // --- Procedural buildings (see BuildingGen.hpp) -------------------------
        // Generate a building at the spawn point (3D cursor, or in view) as one
        // undoable step, select it, and remember it as the "live" one so it can be
        // re-tuned and saved without hunting for it in the hierarchy.
        auto generateBuilding = [&]() {
            const glm::vec3 g = spawnPoint(60.0f);
            const buildings::Palette pal = buildings::ensurePalette(materials, buildingCfg);
            std::vector<Entity> es = buildings::generate(buildingCfg, pal, entityCounter, g);
            if (es.empty()) return;
            if (buildingNameBuf[0] != '\0') es.front().name = buildingNameBuf;
            buildingLiveId = es.front().id;
            history.push(std::make_unique<AddEntitiesCmd>(std::move(es), "Building"),
                         document);
            sel.select(buildingLiveId);
            exportStatus = "Generated building.";
        };
        // Re-generate the live building with the current parameters, keeping its
        // place in the world (and its name/parent). The old subtree and the new one
        // swap in a single undo step.
        auto rebuildBuilding = [&]() {
            const int idx = document.indexOf(buildingLiveId);
            if (idx < 0) { buildingLiveId = -1; return; }
            const Entity old = entities[idx];
            const std::vector<int> ids = collectSubtreeIds(old.id);
            const buildings::Palette pal = buildings::ensurePalette(materials, buildingCfg);
            std::vector<Entity> es =
                buildings::generate(buildingCfg, pal, entityCounter, old.localCenter);
            if (es.empty()) return;
            es.front().name          = old.name;
            es.front().parent        = old.parent;
            es.front().localRotation = old.localRotation;
            es.front().rotation      = old.rotation;
            buildingLiveId = es.front().id;
            history.push(std::make_unique<ReplaceEntitiesCmd>(document, ids,
                                                              std::move(es), "Building"),
                         document);
            sel.select(buildingLiveId);
        };
        // Save the live building as a prefab (the same path as the Prefabs panel,
        // just aimed at the generated root instead of the current selection).
        auto saveBuildingPrefab = [&]() {
            const int idx = document.indexOf(buildingLiveId);
            if (idx < 0) { exportStatus = "Generate a building first."; return; }
            sel.selectIndex(idx);
            createPrefabFromSelection(buildingNameBuf);
        };
        // Lift the derived building nearest the camera out of the city and into
        // the scene as a real, editable subtree -- the escape hatch from "derived"
        // to "authored" (see city::bake). It becomes the live building, so the
        // Buildings panel can re-tune it or save it as a prefab straight away.
        auto bakeNearestBuilding = [&]() {
            RoadSystem& road = roads.active();
            const city::District& d = road.district();
            const glm::vec3 eye = camera.position();
            int   best = -1;
            float bestD = 0.0f;
            for (int i = 0; i < static_cast<int>(d.buildings.size()); ++i) {
                if (d.buildings[i].params.floors <= 0) continue; // a skyway, not a tower
                const glm::vec3 dv = d.buildings[i].center - eye;
                const float     dd = glm::dot(dv, dv);
                if (best < 0 || dd < bestD) { best = i; bestD = dd; }
            }
            if (best < 0) { exportStatus = "No city building to bake."; return; }
            const std::vector<buildings::Palette> pals =
                city::ensurePalettes(materials, road.biomes);
            const int bi = d.buildings[best].biome;
            if (bi < 0 || bi >= static_cast<int>(pals.size())) return;
            std::vector<Entity> es = city::bake(d, best, pals[bi], entityCounter);
            if (es.empty()) return;
            buildingLiveId = es.front().id;
            history.push(std::make_unique<AddEntitiesCmd>(std::move(es), "Bake building"),
                         document);
            sel.select(buildingLiveId);
            exportStatus = "Baked the nearest city building into the scene.";
        };

        // --- Street-name signs (see StreetSignPanel.hpp: owns its actions) -----
        signui::StreetSignTool signTool({document, history, sel, entityCounter, spawnPoint,
                                         exportStatus});

        // --- The tree generator (see TreeGenPanel.hpp: owns its actions) --------
        treeui::TreeGenTool treeGen({currentProject, exportStatus,
                                     [&](const std::string& file, const std::string& name, float h) {
                                         veg.adoptTreeModel(file, name, h);
                                     },
                                     [&] { veg.rescanTreeFiles(); }});

        // --- Retargeting (see RetargetPanel.hpp: owns its actions) ---------------
        // It takes files dropped on its window, and the selected object's model
        // as the character on request.
        retargetui::RetargetTool retargetTool({currentProject, exportStatus,
                                               &g_fileDrop.paths, &g_fileDrop.x, &g_fileDrop.y,
                                               [&]() -> std::string {
                                                   if (!sel.valid()) return {};
                                                   const auto* mc = entities[sel.index()].components.get<ModelComponent>();
                                                   if (!mc || mc->modelPath.empty()) return {};
                                                   std::filesystem::path p(mc->modelPath);
                                                   if (p.is_relative() && !currentProject.empty())
                                                       p = std::filesystem::path(currentProject).parent_path() / p;
                                                   return p.generic_string();
                                               }});

        // --- The image editor (see ImageEditPanel.hpp: owns its pictures) ------
        imageui::ImageEditor imageEditor({currentProject, exportStatus,
                                          &g_fileDrop.paths, &g_fileDrop.x, &g_fileDrop.y});

        // --- Procedural objects: node graphs cooked into meshes (ProcGraph.hpp) --
        procui::Panel procPanel({editorCtx, spawnPoint, [&](glm::vec3& p) {
            if (!cursor.visible) return false;
            p = cursor.pos;
            return true;
        }});

        // --- Procedural houses (see HouseGen.hpp) -------------------------------
        // The same four actions as the buildings: generate at the spawn point,
        // rebuild the live one in place (one undo step either way), save it as a
        // prefab -- plus re-opening any placed house, whose root carries the
        // parameters it was built from (HouseComponent).
        auto generateHouse = [&]() {
            const glm::vec3 g = spawnPoint(40.0f);
            const housegen::Palette pal = housegen::ensurePalette(materials, houseCfg);
            std::vector<Entity> es = housegen::generate(houseCfg, pal, entityCounter, g);
            if (es.empty()) return;
            if (houseNameBuf[0] != '\0') es.front().name = houseNameBuf;
            houseLiveId = es.front().id;
            history.push(std::make_unique<AddEntitiesCmd>(std::move(es), "House"), document);
            sel.select(houseLiveId);
            exportStatus = "Generated house.";
        };
        auto rebuildHouse = [&]() {
            const int idx = document.indexOf(houseLiveId);
            if (idx < 0) { houseLiveId = -1; return; }
            const Entity old = entities[idx];
            const std::vector<int> ids = collectSubtreeIds(old.id);
            const housegen::Palette pal = housegen::ensurePalette(materials, houseCfg);
            std::vector<Entity> es =
                housegen::generate(houseCfg, pal, entityCounter, old.localCenter);
            if (es.empty()) return;
            es.front().name          = old.name;
            es.front().parent        = old.parent;
            es.front().localRotation = old.localRotation;
            es.front().rotation      = old.rotation;
            houseLiveId = es.front().id;
            history.push(std::make_unique<ReplaceEntitiesCmd>(document, ids, std::move(es),
                                                              "House"),
                         document);
            sel.select(houseLiveId);
        };
        auto saveHousePrefab = [&]() {
            const int idx = document.indexOf(houseLiveId);
            if (idx < 0) { exportStatus = "Generate a house first."; return; }
            sel.selectIndex(idx);
            createPrefabFromSelection(houseNameBuf);
        };
        // The generated house the selection is part of (its root's id), or -1:
        // clicking a wall should be enough to get back to the whole house.
        auto selectedHouseId = [&]() -> int {
            if (!sel.valid()) return -1;
            const Entity* e = &entities[sel.index()];
            for (int depth = 0; e && depth < 64; ++depth) {
                if (e->components.get<HouseComponent>()) return e->id;
                if (e->parent < 0) break;
                e = document.find(e->parent);
            }
            return -1;
        };
        auto loadSelectedHouse = [&]() {
            const int id = selectedHouseId();
            const Entity* e = id >= 0 ? document.find(id) : nullptr;
            if (!e) return;
            houseCfg    = e->components.get<HouseComponent>()->params;
            houseLiveId = id;
            std::snprintf(houseNameBuf, sizeof houseNameBuf, "%s", e->name.c_str());
            sel.select(id);
            exportStatus = "Editing " + e->name + ".";
        };
#endif // !FITZEL_PLAYER



        // --- Flowers (owned by VegetationSystem) -----------------------------
        if (!veg.initFlowers()) FZ_MAIN_RETURN 1;

        // Gameplay RNG for spawner launch-direction randomization (persists across
        // spawns so successive emits vary within a Play session).
        std::mt19937 spawnRng(1234u);
        std::uniform_real_distribution<float> spawnU(0.0f, 1.0f);


        // --- Audio: weather-driven sound layers --------------------------
        showProgress(0.82f, "Loading audio...");
        Audio audio;
        const std::string& soundDir = roots.sounds;
        WeatherSounds wx;
        loadWeatherSounds(audio, soundDir, wx);
#ifndef FITZEL_PLAYER
        // The synth's editor. After `audio` for the same reason as everything
        // else here: the preview voice it holds belongs to that engine, and a
        // voice outliving its mixer is the one way this all ends in a crash.
        synthui::Panel synthPanel;
#endif
        // The Synth components' players. After `audio`, like every voice.
        SynthSystem synths;
        synths.bind(audio, document, currentProject);
        // A script's song (the `music` table). After `audio`, like every voice.
        MusicSystem musicSys;
        // Birdsong, insects, leaves (Soundscape.hpp). After `audio`, so it is
        // destroyed before it: its voices belong to that engine.
        Soundscape soundscape;
        soundscape.init(audio, soundDir);
        Sound& rainSnd    = wx.rain;
        Sound& windSnd    = wx.wind;
        Sound& breezeSnd  = wx.breeze;
        Sound& thunderSnd = wx.thunder;
        Sound& splashSnd  = wx.splash;
        Sound& waterSnd   = wx.water;
        Sound& stormSnd   = wx.storm;
        // Engine sound: RPM-layered loops + an automatic gearbox. Voiced only
        // while a vehicle is being driven (see the audio mix block below).
        CarAudio carAudio;
        carAudio.load(audio, soundDir);
        GliderAudio gliderAudio;
        gliderAudio.load(audio, soundDir);
        // The world's own noises: rival engines and pass-by swooshes, both
        // positioned, both Doppler-shifted by the spatializer (see WorldAudio).
        // Its listener is the eye that renders, so it is fed from the camera and
        // not from the craft -- a chase view hears from where it watches.
        WorldAudio worldAudio;
        worldAudio.load(audio, soundDir);
        glm::vec3 listenerPrev{0.0f};
        glm::vec3 listenerVel{0.0f};
        bool      listenerHasPrev = false;
        bool  prevFlashOn  = false;

        bool requestDockRebuild = false; // set by "Reset layout" to re-apply the default

        // Presentation mode: borderless fullscreen with the editor UI hidden.
        bool presentMode = false;
        bool prevF11     = false;

        // First-person (walk on terrain) mode.
        bool        fpsMode  = false;
        bool        prevF    = false;
        bool        prevF3   = false;
        bool        prevEsc  = false;
        bool        prevSpace = false;
        bool        prevQkey = false, prevWkey = false, prevEkey = false; // gizmo tools
        bool        prevXkey = false; // X: toggle gizmo local/world space
        bool        camFocusing = false;      // F: smoothly gliding to a focus point
        glm::vec3   camFocusTarget{0.0f};
        // Frame a sphere: keep the view direction, back off until it fits with a
        // margin, and glide there (applied each frame below). Through an ortho
        // lens the distance frames nothing -- the zoom does: the radius and its
        // margin fill the height the perspective cone would have at that
        // distance. F on the selection, and F while modelling.
        auto frameSphere = [&](const glm::vec3& c, float r) {
            const float fov = glm::radians(glm::max(camera.fov(), 1.0f));
            camFocusTarget  = c - camera.front() * (r / std::max(std::tan(fov * 0.5f), 0.05f) * 1.3f);
            camFocusing     = true;
            if (camera.orthographic()) camera.setOrthoHalfHeight(r * 1.3f);
        };
#ifndef FITZEL_PLAYER
        // The viewport's other two ways of moving: the axis-aligned standard
        // views (numpad, Blender's layout) and middle-mouse panning. See
        // ViewportNav.hpp -- both are editor-only, the player has no viewport to
        // navigate.
        viewnav::Nav viewNav;
#endif

        // Undo/redo edge state.
        bool                prevUndo = false, prevRedo = false;
        bool                prevSave = false;   // Ctrl+S, edge-triggered
        // Inspector edit transaction: snapshot the selected entity's subtree while
        // a field is being touched, commit one ModifyEntities step when released.
        int                 inspEditId = -1;
        std::vector<int>    inspEditIds;
        std::vector<Entity> inspEditBefore;
        float       fpsVelY  = 0.0f;
        bool        grounded = false;
        const float eyeHeight = 1.8f;

        // Walk head-bob: the eye is offset by a small springy bob synced to the
        // distance actually walked (not the frame rate), so the first-person view
        // reads as footsteps instead of a rigid floating camera. State persists
        // across frames; the offset eases in when moving on the ground and out when
        // idle or airborne. Applied as a pure eye offset on top of the movement
        // result -- prevBobOffset is subtracted back off before the next move step
        // so it never feeds into collision/ground logic and drifts.
        float       bobPhase   = 0.0f;   // radians, advanced by metres walked
        float       bobAmt     = 0.0f;   // 0..1 smoothed gate (eases bob in/out)
        float       bobClock   = 0.0f;   // seconds, for the idle breathing term
        glm::vec3   bobOffset{0.0f};     // last applied eye offset (world space)
        glm::vec2   walkPrevXZ{0.0f};    // previous eye XZ, for the real ground speed

        // Camera path recorder/player (record/play/scrub + save, in CameraPath).
        CameraPathRecorder camPathRec;
        // Keyframe animation over inspector properties (AnimSystem.hpp). The
        // clip is scene data; the player is where its playhead is right now, in
        // a Timeline preview or in a running game.
        // The scene's animations, and which one the Timeline is editing. Never
        // empty: an editor with no clip has nowhere to put the first key, so one
        // is kept the way the material library keeps a Default.
        std::vector<anim::Clip> animClips{anim::Clip{}};
        int          animEditClip = 0;
        anim::Player animPlay;
        // What is running in the game: one playback per Animator that started,
        // so two objects can be moving to two different clips at once. Empty in
        // the editor, where the single animPlay above previews the clip being
        // edited instead.
        std::vector<anim::Playback> animRuntime;
        // The scene's animation state machines. The graphs are shared data; each
        // AnimGraphComponent holds its own run of one (see AnimGraph.hpp).
        std::vector<animgraph::Graph> animGraphs;
        // Prefabs carry the graphs their objects run and hand them to this scene
        // when they are loaded (prefab::load), so they need to know where it is.
        pio.animGraphs = &animGraphs;
        int animEditGraph = 0;      // which one the graph editor is showing
        // The clip the Timeline edits and previews. Clamped rather than trusted:
        // deleting a clip leaves the index one past the end for a frame.
        auto animEdited = [&]() -> anim::Clip& {
            if (animClips.empty()) animClips.push_back(anim::Clip{});
            animEditClip = std::clamp(animEditClip, 0,
                                      static_cast<int>(animClips.size()) - 1);
            return animClips[animEditClip];
        };
        // On by default: it only ever re-keys a property that is ALREADY
        // animated (see the Inspector), so it cannot start an animation by
        // accident -- and without it, posing an object while the timeline
        // previews is a fight the author loses every frame.
        bool         animAutoKey = true;

        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);

        std::puts("[Fitzel] Fly: WASD/QE, hold RMB=look. F = FPS mode (walk).");

        TerrainSettings uiSettings = settings; // editable copy for the panel

        // --- Scenes: Nature (full outdoor) vs Empty (flat build sandbox) -----
        const TerrainSettings natureSettings = settings;
        int  scene = 1; // 0 = Nature, 1 = Empty  (start empty for editing)
        auto applyScene = [&](int s) {
            scene = s;
            if (s == 1) {                 // Empty: flat ground, nothing growing, no water
                uiSettings.heightScale  = 0.0f;
                uiSettings.ridgeScale   = 0.0f;
                uiSettings.continentAmp = 0.0f;
                uiSettings.warpStrength = 0.0f;
                uiSettings.terrace      = 0.0f;
                uiSettings.islandRadius = 0.0f; // no island mask on the flat sandbox
                veg.grassEnabled = veg.treeEnabled = veg.flowerEnabled = false;
                veg.birdsEnabled = veg.fireflyEnabled = false;
                waterLevel = -1000.0f;
            } else {                       // Nature: restore the outdoor world
                uiSettings = natureSettings;
                veg.grassEnabled = veg.treeEnabled = veg.flowerEnabled = true;
                veg.birdsEnabled = veg.fireflyEnabled = true;
                waterLevel = -2.0f;
            }
            streamer.settings() = uiSettings;
            streamer.rebuild();
            streamer.update(camera.position());
            veg.grassDirty = true;
            veg.treeCenter = glm::vec2(1e9f);
            roads.rebuildMeshes(); // re-drape the committed roads on the new terrain
            towns.markDirty();
        };
        applyScene(scene); // start in the selected scene (Empty by default)

        // Reset the world to the editor's default (used by New Scene): flat "Empty"
        // terrain, no texture layers, no road, no hand-painted vegetation -- so a new
        // scene starts blank instead of inheriting the terrain you were just editing.
        auto resetWorldForNewScene = [&]() {
            look.layers.clear();
            // Back to one empty road -- a new scene has no roads in it, and the
            // editor always has one to draw into (see RoadSet::clear).
            roads.clear();
            roadSel = roadSel2 = -1;
            splines.clear();
            splineSel = splinePtSel = -1;
            towns.clear();
            townSel = -1;
            townEditing = false;
            // Give the ground back BEFORE dropping the paths: release publishes
            // the difference against what was cut, and a system with no paths
            // left has nothing to work that out from.
            {
                glm::vec2 mn, mx;
                if (rivers.release(sculptWork, paintWork, mn, mx)) {
                    publishSculpt();
                    publishPaint();
                    streamer.editsChanged(mn, mx);
                }
                veg.wet.clear();
            }
            rivers.clear();
            riverSel = riverPtSel = -1;
            veg.paintedBlades.clear();
            veg.paintedTrees.clear();
            veg.paintedFlowers.clear();
            veg.paintedDirty = true;
            veg.grassDirty   = true;
            applyScene(1); // flat default terrain + rebuild + re-drape the (now empty) road
        };

        // --- The terrain is an entity ----------------------------------------
        // A scene has ground because a Terrain component is in it -- nothing is
        // implied, the author puts it there (and can delete it again). What
        // follows is the seam between that component and the running world:
        //
        //   the component  = what gets SAVED with the scene (per-entity, undoable)
        //   `uiSettings`   = the working copy every tool edits (panel, presets)
        //   the streamer   = the ground actually being generated and drawn
        //
        // syncTerrainEntity mirrors the first two onto each other once a frame,
        // whichever side moved last, so the Terrain panel and the Inspector are
        // two views of one terrain rather than two terrains. With no component in
        // the scene there is no ground at all: the streamer drops every chunk and
        // every height query in the engine answers 0, so objects sit at y=0
        // instead of on an invisible landscape.
        int  terrainEntity  = -1;    // entity id carrying the terrain (-1 = none)
        bool terrainOn      = false; // was there ground last frame?
        bool terrainSynced  = false; // has the mirror run at least once?
        TerrainSettings compMirror{};             // component state as of last sync
        TerrainSettings uiMirror = uiSettings;    // working copy as of last sync
        auto syncTerrainEntity = [&] {
            // First sync after boot or a scene load: the world is being set up, not
            // edited, so nothing here is reported back to the author as a change.
            const bool fresh = !terrainSynced;
            // Did the GROUND itself move this sync? Anything derived from it has
            // to be re-derived, and the watercourses are the only thing here that
            // cannot wait for a button (see the end of this lambda).
            bool groundMoved = false;
            TerrainComponent* tc = nullptr;
            int owner = -1;
            for (Entity& e : entities) {
                if (!e.activeInHierarchy) continue;
                if (auto* c = e.components.get<TerrainComponent>()) {
                    tc = c; owner = e.id; break;   // one terrain: the first wins
                }
            }
            if (tc) {
                // A different terrain than last frame (loaded, added, undone, or
                // re-activated) is adopted wholesale; otherwise the side that
                // actually changed wins. Adoption regenerates, a mirror copy does
                // not -- the panel has already applied its own edits.
                const bool adopt = (owner != terrainEntity) || (tc->settings != compMirror);
                if (adopt) {
                    uiSettings = tc->settings;
                    streamer.settings() = uiSettings;
                    streamer.rebuild();
                    veg.grassDirty  = true;
                    veg.treeCenter  = glm::vec2(1e9f);
                    groundMoved     = true;
                    // The ground moved under a built road, so its graded corridor
                    // wants cutting again -- flag it, never rebuild behind the
                    // author's back. On a load there is nothing to report: that
                    // road was built on this terrain.
                    if (!fresh) roads.markNeedsBuild();
                } else if (uiSettings != uiMirror) {
                    tc->settings = uiSettings;
                }
                compMirror = tc->settings;
                uiMirror   = uiSettings;
            }
            const bool on = tc != nullptr;
            if (fresh || on != terrainOn) {
                terrainSynced = true;
                terrainOn     = on;
                fitzel::setTerrainPresent(on); // every height query in the engine
                streamer.setEnabled(on);       // chunks: streamed, or none at all
                veg.terrainPresent = on;       // nothing grows on a void
                if (on) { veg.grassDirty = true; veg.treeCenter = glm::vec2(1e9f); }
                // The committed road drapes on the ground, so re-loft it now that
                // the ground has arrived (or gone). A load lofts the road before
                // the terrain entity exists, which is exactly when this matters.
                roads.rebuildMeshes();
                towns.markDirty();
                if (!fresh) roads.markNeedsBuild(); // ...and re-cut their corridors
                groundMoved = true;
            }
            terrainEntity = owner;

            // ...and the water, which is the one thing here that cannot be left
            // to a button.
            //
            // A scene load re-cuts every watercourse from its saved path (the bed
            // is derived, never stored) -- but it does that while reading the
            // settings, which is BEFORE this mirror has run. So the profile is
            // solved against last scene's terrain, or against no terrain at all
            // when the previous scene had none, and the whole river comes out at
            // y=0: gone, until something happens to dirty it. Turning any knob in
            // the river panel does, which is exactly the shape of the report --
            // "after loading, the river is missing until I touch a slider".
            //
            // Re-solving here rather than moving the load order is deliberate:
            // this is the one place that knows the ground has changed, and it has
            // to answer for a terrain edited later just as much as for one that
            // arrived late.
            if (groundMoved) {
                rivers.touch();
                carveRivers();
            }
        };
        // One Empty carrying a Terrain component: the scene's ground as an object.
        // The entity's transform is only where its marker sits in the viewport --
        // the field itself is world-wide.
        auto makeTerrainEntity = [&](const TerrainSettings& s) {
            Entity t;
            t.type        = EntityType::Empty;
            t.name        = "Terrain";
            t.half        = glm::vec3(0.5f);
            t.id          = entityCounter++;
            t.localCenter = t.center = glm::vec3(0.0f);
            auto tc = std::make_unique<TerrainComponent>();
            tc->settings = s;
            t.components.items.push_back(std::move(tc));
            return t;
        };
        // Put ground in the scene (undoable), seeded with whatever terrain the
        // editor is currently showing. Returns the existing one if the scene
        // already has ground -- a scene has one terrain.
        auto addTerrainEntity = [&]() -> int {
            for (const Entity& e : entities)
                if (e.components.get<TerrainComponent>()) return e.id;
            const Entity t = makeTerrainEntity(uiSettings);
            history.push(std::make_unique<AddEntityCmd>(t), document);
            sel.select(t.id);
            return t.id;
        };
        // Did the scene being loaded come from a version that stores its terrain as
        // an entity? Set from the file's settings block; when it is false after a
        // load, the scene predates this and its terrain has to be migrated (see
        // afterSceneLoadFn) -- otherwise opening an old world would lose its ground.
        bool sceneStoredTerrainEntity = false;

        // --- Scene settings registry --------------------------------------
        // Every tunable is bound by name to a getter/setter and serialised as
        // part of the project scene (.fitzel "settings" object). Missing keys are
        // ignored, so scenes keep loading as fields come and go.
        //
        // A key of the WRONG TYPE is ignored too, and that is not belt-and-braces.
        // nlohmann's `value()` does not fall back on a type mismatch -- it throws
        // -- and the only thing above this to catch a json::type_error is main()'s
        // outermost handler, which prints "Fatal:" to a stderr no windowed build
        // has and exits. So one stale or mistyped field in a .fitzel does not fail
        // to load: it takes the whole editor down, with the project the user just
        // picked as the apparent culprit. (It did, when two settings were
        // registered under the same name and the file ended up holding the other
        // one's type.) A field we cannot read keeps its default instead.
        struct Setting {
            std::string                                key;
            std::function<void(nlohmann::json&)>       write;
            std::function<void(const nlohmann::json&)> read;
        };
        std::vector<Setting> tunables;
        auto addF = [&](const char* k, float& r) {
            tunables.push_back({k,
                [k, &r](nlohmann::json& j){ j[k] = r; },
                [k, &r](const nlohmann::json& j){
                    const auto it = j.find(k);
                    if (it != j.end() && it->is_number()) r = it->get<float>();
                }});
        };
        auto addB = [&](const char* k, bool& r) {
            tunables.push_back({k,
                [k, &r](nlohmann::json& j){ j[k] = r; },
                [k, &r](const nlohmann::json& j){
                    const auto it = j.find(k);
                    if (it != j.end() && it->is_boolean()) r = it->get<bool>();
                }});
        };
        auto addI = [&](const char* k, int& r) {
            tunables.push_back({k,
                [k, &r](nlohmann::json& j){ j[k] = r; },
                [k, &r](const nlohmann::json& j){
                    const auto it = j.find(k);
                    if (it != j.end() && it->is_number_integer())
                        r = it->get<int>();
                }});
        };
        // Strings, for the settings that NAME something rather than measure it.
        // There was no such adder, which is why the environment's HDRI could be
        // chosen and never saved: the panel had a std::string and the registry
        // only took numbers, so the one field that said WHICH panorama had
        // nowhere to go.
        auto addS = [&](const char* k, std::string& r) {
            tunables.push_back({k,
                [k, &r](nlohmann::json& j){ j[k] = r; },
                [k, &r](const nlohmann::json& j){
                    const auto it = j.find(k);
                    if (it != j.end() && it->is_string()) r = it->get<std::string>();
                }});
        };
        addF("moveSpeed", camera.moveSpeed);   addI("viewRadius", viewRadius);
        addB("farPlaneAuto", farPlaneAuto);    addF("farPlane", farPlaneManual);
        addB("farTerrain", farTerrainOn);
        addF("farSnowLevel", farTerrain.snowLevel);
        addF("farTreeLine", farTerrain.treeLine);
        addF("meadowTint", meadowTint);
        // The forest field (Ecology.hpp). Off in scenes from before it: they
        // keep the 120 m scatter they were planted with.
        addB("ecoForest", veg.eco.enabled);
        addF("ecoCover", veg.eco.cover);
        addF("ecoStandSize", veg.eco.standSize);
        addF("ecoSolitary", veg.eco.solitary);
        addF("ecoSlopeLove", veg.eco.slopeLove);
        addF("impostorStart", veg.impostorStart);
        addF("impostorShadowDist", veg.impostorShadowDist);
        addF("forestRadius", veg.forestRadius);
        addI("forestFloorLayer", forestFloorLayer);
        addF("windStrength", windStrength);
        addF("windAngle", windAngle);
        addF("windGust", windGust);
        addB("cloudShadows", cloudShadowsOn);
        addB("wildlife", wildlifeOn);
        addB("motes", motesOn);
        addB("soundscape", soundscapeOn);
        addB("timeFlows", timeFlows);
        addS("herdModel", herdModel);
        addI("herdCount", herdCfg.count);
        addF("herdX", herdCfg.centre.x);    addF("herdZ", herdCfg.centre.y);
        addF("herdRadius", herdCfg.radius); addF("herdHeight", herdCfg.height);
        addI("herdGrazeClip", herdCfg.grazeClip);
        addI("herdWalkClip", herdCfg.walkClip);
        addF("herdWalkSpeed", herdCfg.walkSpeed);
        addF("herdYaw", herdCfg.yawOffset);
        addF("grassDryGrowth", veg.grassDryGrowth);
        addB("autoWeather", autoWeather);      addF("weather", storm);
        addB("lightning", lightning);        addF("rainAmount", rainAmount);
        // The opening camera move. The KEYS are a blob written below (a list, not
        // a tunable); these three are how it behaves once the game starts.
        addB("camPathOnStart", camPathRec.playOnStart);
        addB("camPathLoop", camPathRec.loop);
        addF("camPathSpeed", camPathRec.speed);
        addF("wxRain", wxGain.rain);           addF("wxWind", wxGain.wind);
        addF("wxBreeze", wxGain.breeze);       addF("wxStorm", wxGain.storm);
        addF("wxThunder", wxGain.thunder);
        // Which preset this scene is on. The NAME travels here, never the
        // values: those live in the project's weather.json, so editing a preset
        // changes every scene that uses it -- which is the point of a preset.
        addS("weatherPreset", weatherCurrent);
        addB("weatherPresetTime", weatherSavesTime);
        addB("weatherPresetMist", weatherSavesMist);
        addB("muted", mix.master.mute);        addF("volume", mix.master.level);
        // Read but not written: see legacyStartVehicle. A no-op save lambda is
        // what "this key is on its way out" looks like in this registry -- the
        // value keeps working until the scene is next saved, and then it is gone.
        const auto addLegacyB = [&](const char* k, bool& r) {
            tunables.push_back({k,
                [](nlohmann::json&){},
                [k, &r](const nlohmann::json& j){
                    const auto it = j.find(k);
                    if (it != j.end() && it->is_boolean()) r = it->get<bool>();
                }});
        };
        addLegacyB("startInVehicleMode", legacyStartVehicle);
        addLegacyB("startInGliderMode", legacyStartGlider);
        addB("showCrosshair", showCrosshair);
        addB("skidMarks", skids.enabled);      addF("skidSlip", skids.slipThresh);
        addF("skidWidth", skids.markHalfW);    addF("skidDark", skids.opacity);
        addB("contrails", trails.enabled);     addF("trailLife", trails.life);
        addF("trailWidth", trails.width);      addF("trailOpacity", trails.opacity);
        addF("trailGlow", trails.glow);
        addF("mixAmbient", mix.ambient.level);  addB("mixAmbientMute", mix.ambient.mute);
        addF("mixSfx", mix.sfx.level);          addB("mixSfxMute", mix.sfx.mute);
        // Solo is deliberately NOT kept: it is a listening state, not a mix, and
        // a scene that opens with one bus soloed sounds broken.
        addF("timeOfDay", timeOfDay);          addF("dayLength", dayLength);
        addF("sunLatitude", sunLatitude);      addF("sunDeclination", sunDeclination);
        addF("coverage", skySet.coverage);     addF("cloudDensity", skySet.density);
        addF("cloudScale", skySet.scale);      addF("cloudWind", skySet.wind);
        addF("cloudBottom", skySet.base);      addF("cloudTop", skySet.top);
        // The sheet layers. Prefixed "sky<Type>", six keys each, flat rather
        // than nested because that is what this whole settings block is -- and
        // note "skyContrails" and not "contrails": that name belongs to the
        // vehicle trail toggle a few lines up (addB, a bool). Two settings
        // under one key write over each other in the file, and whichever loses
        // gets read back at the other's type, which is what threw
        // json::type_error out of a scene load once already.
        //
        // The adders above keep the key as a POINTER, so a name built here has
        // to outlive this scope -- hence the pool. A `(prefix + "Wind").c_str()`
        // would compile, read correctly for as long as the temporary happened
        // not to be overwritten, and then start losing settings.
        {
            static std::deque<std::string> skyKeys;
            const auto addSheet = [&](const char* prefix, weather::Sheet& sh) {
                const auto key = [&](const char* suffix) -> const char* {
                    skyKeys.push_back(std::string(prefix) + suffix);
                    return skyKeys.back().c_str();
                };
                addB(key("On"), sh.on);
                addF(key("Amount"), sh.amount);
                addF(key("Height"), sh.height);
                addF(key("Scale"), sh.scale);
                addF(key("Wind"), sh.wind);
                addF(key("Dir"), sh.dir);
            };
            addSheet("skyStratus", skySet.stratus);
            addSheet("skyStratocumulus", skySet.stratocumulus);
            addSheet("skyAltocumulus", skySet.altocumulus);
            addSheet("skyCirrus", skySet.cirrus);
            addSheet("skyCirrocumulus", skySet.cirrocumulus);
            addSheet("skyContrails", skySet.contrails);
        }
        addF("fogDensity", skySet.fogDensity); addF("fogFalloff", skySet.fogFalloff);
        // Image-based lighting. The panorama travels as its PROJECT-RELATIVE
        // path, not as a GUID and not as an absolute one: it survives a
        // re-import, it reads sensibly to whoever opens the .fitzel, and it is
        // the same string the Environment panel shows. Resolving it back to a
        // file on disk is afterSceneLoadFn's job (see applyHdri).
        addS("hdri", hdriLoaded);
        addB("ibl", iblEnabled);               addB("iblSkybox", iblSkybox);
        addF("iblIntensity", iblIntensity);
        // Volumetric fog. Every knob of it is scene data: where the bank stands,
        // how thick it is and how its noise moves are things the world's author
        // decided, so they travel with the world -- only `volFogSteps` and
        // `volFogRes` are a cost the machine gets a say in, and those are saved
        // here too because a scene that needs a heavy march should open with the
        // march its look was tuned against.
        addB("volFog", volFogSet.enabled);
        addF("volFogCenterX", volFogSet.center.x);
        addF("volFogCenterY", volFogSet.center.y);
        addF("volFogCenterZ", volFogSet.center.z);
        addF("volFogSizeX", volFogSet.size.x);
        addF("volFogSizeY", volFogSet.size.y);
        addF("volFogSizeZ", volFogSet.size.z);
        addB("volFogFollow", volFogSet.followCamera);
        addF("volFogEdge", volFogSet.medium.edge);
        addF("volFogHeightFalloff", volFogSet.medium.heightFalloff);
        addF("volFogDensity", volFogSet.medium.density);
        addF("volFogColorR", volFogSet.medium.color.x);
        addF("volFogColorG", volFogSet.medium.color.y);
        addF("volFogColorB", volFogSet.medium.color.z);
        addF("volFogCoverage", volFogSet.medium.coverage);
        addF("volFogNoiseScale", volFogSet.medium.noiseScale);
        addF("volFogNoiseVertical", volFogSet.medium.verticalDetail);
        addF("volFogDetail", volFogSet.medium.detail);
        addF("volFogWarp", volFogSet.medium.warp);
        addF("volFogWindX", volFogSet.medium.wind.x);
        addF("volFogWindY", volFogSet.medium.wind.y);
        addF("volFogWindZ", volFogSet.medium.wind.z);
        addF("volFogAnisotropy", volFogSet.medium.anisotropy);
        addF("volFogSun", volFogSet.medium.sunIntensity);
        addF("volFogAmbient", volFogSet.medium.ambientIntensity);
        addB("volFogShafts", volFogSet.medium.shafts);
        addB("volFogSelfShadow", volFogSet.medium.selfShadow);
        addI("volFogSteps", volFogSet.medium.steps);
        addI("volFogRes", volFogSet.resScale);
        addF("exposure", postLook.exposure);            addF("bloom", postLook.bloomIntensity);
        addF("rays", postLook.rayIntensity);            addF("ssao", postLook.ssaoStrength);
        addF("ssaoRadius", postLook.ssaoRadius);        addF("ssaoBias", postLook.ssaoBias);
        addF("bloomThreshold", postLook.bloomThreshold); addF("bloomKnee", postLook.bloomKnee);
        addF("cascadeSplit", renderer.shadows().splitLambda);
        addI("envProbeRes", postLook.envProbeRes);      addI("envProbeFaces", postLook.envProbeFaces);
        addF("hue", postLook.hueShift);                 addF("saturation", postLook.saturation);
        addF("value", postLook.valueGain);              addF("warmth", postLook.warmth);
        addF("gradeSplit", postLook.gradeSplit);        addF("gradeVibrance", postLook.gradeVibrance);
        addF("contrast", postLook.contrast);            addF("motionBlur", postLook.motionBlurStrength);
        addF("dofBlur", postLook.dofMax);               addF("dofNear", postLook.dofNear);
        addF("dofFar", postLook.dofFar);
        addI("tonemapCurve", postLook.tonemapCurve);    addB("autoExposure", postLook.autoExposure);
        addF("autoMinEv", postLook.autoMinEv);          addF("autoMaxEv", postLook.autoMaxEv);
        addF("adaptSpeed", postLook.adaptSpeed);        addB("ssr", postLook.ssrEnabled);
        addB("contactShadows", postLook.contactShadows);
        addF("vignette", postLook.vignette);            addF("filmGrain", postLook.filmGrain);
        addF("waterLevel", waterLevel);        addF("waveHeight", waveHeight);
        addF("waveChoppy", waveChoppy);        addF("waveStrength", waveStrength);
        addF("waveScale", waveScale);          addF("foamWidth", foamWidth);
        addF("waterColorR", waterColor.x);     addF("waterColorG", waterColor.y);
        addF("waterColorB", waterColor.z);
        addF("waterReflectivity", waterReflectivity); addF("waterClarity", waterClarity);
        addF("waterIor", waterIor);
        addF("cursorX", cursor.pos.x); addF("cursorY", cursor.pos.y); addF("cursorZ", cursor.pos.z);
        addF("cursorGrid", cursor.grid);
        addF("snapAngle", cursor.snapAngle);          addF("snapScale", cursor.snapScale);
#ifndef FITZEL_PLAYER
        addB("camPreview", showCamPreview);    // editor-only: the player has no viewport corner
#endif
        // What this scene is played as. Registered by hand rather than through
        // addI, because the generic reader leaves a missing key ALONE -- which is
        // right for a tunable with a sensible default and wrong for this one: a
        // scene that says nothing would inherit whatever the last scene said, and
        // the setting would behave as if it were global. Absent means -1 here,
        // every time.
        //
        // NOT inside the editor-only guard: the shipped player reads scene
        // settings through the same registry, and a scene that says how it is
        // played has to be obeyed there most of all.
        tunables.push_back({"startMode",
            [&](nlohmann::json& j){ j["startMode"] = sceneStartMode; },
            [&](const nlohmann::json& j){
                const auto it = j.find("startMode");
                sceneStartMode = (it != j.end() && it->is_number_integer())
                                     ? it->get<int>() : -1;
            }});
        addB("showGrid", showGrid);            addF("gridFade", gridFade);
        addF("terrHeight", uiSettings.heightScale);   addF("terrRidge", uiSettings.ridgeScale);
        addF("terrContinent", uiSettings.continentAmp); addF("terrBiome", uiSettings.biomeFreq);
        addF("terrTerrace", uiSettings.terrace);      addF("terrWarp", uiSettings.warpStrength);
        addF("terrFreq", uiSettings.frequency);       addI("terrOctaves", uiSettings.octaves);
        addF("terrSeed", uiSettings.seed);
        addF("terrValley", uiSettings.valleyDepth);   addF("terrPeak", uiSettings.peakSharpness);
        addF("terrRelief", uiSettings.reliefGain);
        addF("terrIslandRadius", uiSettings.islandRadius);
        addF("terrIslandCenterX", uiSettings.islandCenterX);
        addF("terrIslandCenterZ", uiSettings.islandCenterZ);
        addF("terrIslandShape", uiSettings.islandShape);
        addF("texScale", texScale);            addF("normalStrength", normalStrength);
        addF("rockSlope", look.rockSlope);     addF("slopeSharp", look.slopeSharpness);
        addF("snowLevel", look.snowLevel);     addF("detailStrength", look.detailStrength);
        addF("terrainGloss", look.gloss);
        addF("terrainHeightBlend", look.heightBlend);
        addB("grassEnabled", veg.grassEnabled);    addF("grassDensity", veg.grassDensity);
        addF("grassRadius", veg.grassRadius);      addF("grassHeight", veg.grassHeight);
        addF("grassChaos", veg.grassChaos);
        addF("grassTintR", veg.grassTint.x);       addF("grassTintG", veg.grassTint.y);
        addF("grassTintB", veg.grassTint.z);
        // The other vegetation on/off toggles (and flower density) persist too,
        // so a saved scene reloads with each layer in the state it was left in.
        addB("treeEnabled", veg.treeEnabled);
        addB("flowerEnabled", veg.flowerEnabled);  addF("flowerDensity", veg.flowerDensity);
        addB("birdsEnabled", veg.birdsEnabled);
        addB("fireflyEnabled", veg.fireflyEnabled);
        // Tree species config (name/LODs/billboard/density) is serialized as a
        // structured block by veg.serializeTrees() in writeSettingsFn below.

        // Wire the serialization hooks now that every tunable and the terrain/
        // vegetation state they drive are in scope. Reading settings applies them
        // and rebuilds the terrain + regrows vegetation (like Regenerate does).
        writeSettingsFn = [&](nlohmann::json& j){
            for (const Setting& s : tunables) s.write(j);
            nlohmann::json larr = nlohmann::json::array();
            for (const TerrainLayer& L : look.layers)
                larr.push_back({{"tex", L.texId.toString()}, {"name", L.name},
                                {"norm", L.normId.toString()},
                                {"hStart", L.heightStart}, {"hEnd", L.heightEnd},
                                {"sStart", L.slopeStart}, {"sEnd", L.slopeEnd},
                                {"scale", L.scale}});
            j["terrainLayers"] = larr;
            // Marker: this scene's terrain is an ENTITY (a Terrain component), so
            // the terr* keys above are only a legacy echo of it. Its absence is
            // what identifies an older scene whose ground has to be migrated into
            // an entity on load -- see afterSceneLoadFn. Deleting the terrain is a
            // real edit, so the marker stays true even with no terrain in the
            // scene: an emptied world must not grow ground again on reload.
            j["terrainEntity"] = true;
            // The camera path: 7 floats per keyframe, same compact-blob scheme as
            // the painted vegetation below. Scene data now -- an opening flythrough
            // belongs to the LEVEL, and campath.txt beside the executable (where
            // the only copy used to live) is in neither the scene nor an export.
            j["camPath"] = camPathRec.blob();
            // The property animation: one "anim" object holding the clip's
            // settings and a compact key blob per track. Scene data for the same
            // reason the camera path is -- the shipped player plays it.
            anim::save(j, animClips);
            animgraph::save(j, animGraphs);
            // Hand-painted grass: a compact space-separated float blob (7 per
            // blade). Stored as one JSON string so pretty-printing doesn't
            // explode into a line per number.
            NumberBlobWriter gs(7, veg.paintedBlades.size());
            for (float v : veg.paintedBlades) gs.put(v);
            j["paintedGrass"] = gs.take();
            // Tree species: name, LOD meshes, billboard config and per-species density.
            veg.serializeTrees(j);
            // Hand-painted trees: compact float blob (6 per tree: pos3, yaw, scale,
            // speciesIdx).
            NumberBlobWriter ts(7, veg.paintedTrees.size());
            for (float v : veg.paintedTrees) ts.put(v);
            j["paintedTrees2"] = ts.take();
            // Hand-painted flowers (8 per bloom: pos3, yaw, scale, rgb).
            NumberBlobWriter fs(7, veg.paintedFlowers.size());
            for (float v : veg.paintedFlowers) fs.put(v);
            j["paintedFlowers"] = fs.take();
            // Terrain sculpt: grid spacing + a compact "ix iz delta ..." blob of
            // every edited cell (one JSON string, same reasoning as the grass).
            j["terrainEditCell"] = sculptWork.cell;
            NumberBlobWriter es(7, sculptWork.deltas.size() * 3);
            for (const auto& [k, d] : sculptWork.deltas) {
                // Minus the watercourse beds. A channel is derived geometry like
                // the road's ribbon or the roadside city -- the file holds the
                // path and the rule, and the bed is re-cut on load. Writing it
                // here as well would save it twice, and the copy in the height
                // field would be the ground the next re-solve then dug into.
                const float own = d - rivers.mineAt(k);
                if (std::fabs(own) < 1e-5f) continue;
                const int ix = static_cast<int>(k >> 32);
                const int iz = static_cast<int>(
                    static_cast<std::int32_t>(static_cast<std::uint32_t>(k)));
                es.put(ix); es.put(iz); es.put(own);
            }
            j["terrainEdits"] = es.take();

            // Terrain texture paint: grid spacing + an "ix iz r g b a ..." blob of
            // every painted cell's four layer weights (same compact-string scheme).
            j["terrainPaintCell"] = paintWork.cell;
            NumberBlobWriter ps(5, paintWork.weights.size() * 6);
            for (const auto& [k, w0] : paintWork.weights) {
                const glm::vec4 w = glm::max(w0 - rivers.minePaintAt(k),
                                             glm::vec4(0.0f));
                if (glm::all(glm::lessThan(w, glm::vec4(1e-4f)))) continue;
                const int ix = static_cast<int>(k >> 32);
                const int iz = static_cast<int>(
                    static_cast<std::int32_t>(static_cast<std::uint32_t>(k)));
                ps.put(ix); ps.put(iz);
                ps.put(w.x); ps.put(w.y); ps.put(w.z); ps.put(w.w);
            }
            j["terrainPaint"] = ps.take();

            // Model-material overrides: edits to materials that come from an
            // imported model aren't written as standalone .fmat files (the model
            // owns them and regenerates them on re-import), so their user edits
            // would be lost. Persist them here keyed by a stable identity
            // (model file GUID | model/node name | primitive index) and re-apply
            // after the model re-imports on load.
            nlohmann::json ov = nlohmann::json::object();
            for (std::size_t mi = 0; mi < models.count(); ++mi) {
                const LoadedModel* lm = models.at(mi);
                if (!lm) continue;
                for (std::size_t p = 0; p < lm->primMaterialId.size(); ++p) {
                    const int idx = document.materialIndex(lm->primMaterialId[p]);
                    if (idx < 0 || !materials[idx].fromModel) continue;
                    const MaterialDef& md = materials[idx];
                    const std::string key = lm->assetId.toString() + "|" +
                                            lm->name + "|" + std::to_string(p);
                    ov[key] = {
                        {"pbr", 1},   // written after glTF PBR import (see the reader)
                        {"name", md.name},
                        {"albedo", {md.albedo.x, md.albedo.y, md.albedo.z}},
                        {"tint", {md.tint.x, md.tint.y, md.tint.z}},
                        {"reflectivity", md.reflectivity},
                        {"roughness", md.roughness},
                        {"opacity", md.opacity},
                        {"glass", md.glass},
                        {"ior", md.ior},
                        {"thickness", md.thickness},
                        {"alphaMode", static_cast<int>(md.alphaMode)},
                        {"alphaCutoff", md.alphaCutoff},
                        {"emission", {md.emission.x, md.emission.y, md.emission.z}},
                        {"emissionStrength", md.emissionStrength},
                    };
                    // Map slots: a bound texture asset stores its GUID, a slot the
                    // user emptied stores "" (so the model's own map isn't just
                    // restored on load), and an untouched slot stores nothing.
                    auto slotJson = [](const AssetId& id,
                                       const std::shared_ptr<Texture>& tex,
                                       const std::shared_ptr<Texture>& shipped,
                                       const char* key, nlohmann::json& out) {
                        if (id.valid())            out[key] = id.toString();
                        else if (shipped && !tex)  out[key] = "";
                    };
                    slotJson(md.texId, md.tex, md.modelTex, "texture", ov[key]);
                    // A video bound over a model's own base map. Plain GUID, no
                    // "emptied" marker: clearing the slot clears videoId, which
                    // then simply isn't written.
                    if (md.videoId.valid()) ov[key]["video"] = md.videoId.toString();
                    camtex::save(md, ov[key]);   // a camera over the base map, likewise
                    slotJson(md.normalTexId, md.normalTex, md.modelNormalTex,
                             "normalMap", ov[key]);
                    slotJson(md.emissionTexId, md.emissionTex, md.modelEmissionTex,
                             "emissionMap", ov[key]);
                    // Models ship no opacity map of their own (glTF has no
                    // slot for one), so there is no "emptied" state to keep.
                    slotJson(md.opacityTexId, md.opacityTex, nullptr,
                             "opacityMap", ov[key]);
                }
            }
            j["modelMaterialOverrides"] = std::move(ov);

            // Which LIBRARY material each of a model's primitives is pointed at.
            // Saved for the same reason as the overrides above -- a model's own
            // materials are recreated on every import, so an assignment made in the
            // inspector would be undone by the next load -- and keyed the same way.
            // Only primitives pointed AWAY from the model's own material are
            // written, so an untouched model costs nothing here.
            nlohmann::json asg = nlohmann::json::object();
            for (std::size_t mi = 0; mi < models.count(); ++mi) {
                const LoadedModel* lm = models.at(mi);
                if (!lm) continue;
                for (std::size_t p = 0; p < lm->primMaterialId.size(); ++p) {
                    const int idx = document.materialIndex(lm->primMaterialId[p]);
                    if (idx < 0 || materials[idx].fromModel) continue;
                    asg[lm->assetId.toString() + "|" + lm->name + "|" +
                        std::to_string(p)] = materials[idx].assetId.toString();
                }
            }
            j["modelMaterialAssign"] = std::move(asg);

            // The road owns its own scene state (the graded terrain corridor rides
            // along in "terrainEdits" above; the mesh is re-lofted on load).
            roads.save(j);

            // Fences, walls and track: paths + rules only. Every metre of geometry
            // is re-derived on load (see splines.update), exactly as the road's
            // ribbon is.
            splines.save(j["splines"]);

            // Brooks, rivers and canals: paths + rules only, exactly like the
            // fences. The bed they cut is NOT in "terrainEdits" above (see the
            // subtraction there) -- it is re-cut on load.
            rivers.save(j["rivers"]);

            // Towns: the rules only. Their streets are in `roads` above; their
            // buildings are re-derived on load.
            if (towns.count() > 0) towns.save(j["towns"]);

            // Scene 2D UI overlay (adds its own "uiOverlay" array to the settings).
            uiOverlay.save(j);

            // Editor fly-camera pose, so reopening a project returns to the exact
            // view it was saved from (position + look direction).
            const glm::vec3 camP = camera.position();
            j["editorCamera"] = {
                {"x", camP.x}, {"y", camP.y}, {"z", camP.z},
                {"yaw", camera.yaw()}, {"pitch", camera.pitch()},
            };
        };
        readSettingsFn = [&](const nlohmann::json& j){
            // Reset fields added after some scenes were saved: addF's read keeps the
            // *current* value when a key is absent, so without this the last-applied
            // island would bleed into every islandless (older) scene on load. Zero
            // the island mask first, so only scenes that actually stored it load as
            // islands.
            uiSettings.islandRadius  = 0.0f;
            uiSettings.islandCenterX = 0.0f;
            uiSettings.islandCenterZ = 0.0f;
            uiSettings.islandShape   = 0.0f;
            // Same for the horizon: only a scene that asked for one gets it.
            farTerrainOn          = false;
            farTerrain.snowLevel  = 1100.0f;
            farTerrain.treeLine   = 750.0f;
            meadowTint            = 0.0f;
            veg.eco               = ecology::Params{};
            veg.impostorStart     = 130.0f;
            veg.impostorShadowDist = 0.0f;
            veg.forestRadius      = 1600.0f;
            forestFloorLayer      = -1;
            windStrength = 0.2f; windAngle = 26.57f; windGust = 0.6f;
            cloudShadowsOn = false;
            wildlifeOn     = false;
            sunLatitude    = 0.0f;
            postLook.gradeSplit     = 0.0f;
            postLook.gradeVibrance  = 0.0f;
            sunDeclination = -10.4f;
            motesOn        = false;
            soundscapeOn   = false;
            timeFlows      = false;
            herdModel.clear();
            herdCfg = Herd::Config{};
            veg.grassDryGrowth = 0.0f;
            for (const Setting& s : tunables) s.read(j);
            // The probe size is the one setting that owns GPU memory: push it
            // through, or the scene's value sits in the variable while the
            // renderer keeps the cubes it already had.
            renderer.setEnvProbeResolution(postLook.envProbeRes);
            postLook.envProbeRes = renderer.envProbeResolution(); // as clamped/rounded
            renderer.setEnvProbeMaxFaces(postLook.envProbeFaces);
            postLook.envProbeFaces = renderer.envProbeMaxFaces();
            // Does this file keep its terrain in an entity? (Consumed and reset by
            // afterSceneLoadFn, which migrates the ones that don't.)
            sceneStoredTerrainEntity = j.value("terrainEntity", false);
            // Restore the editor fly-camera pose. Absent in scenes saved before this
            // existed -> fall back to the current pose so the view just stays put.
            if (j.contains("editorCamera") && j["editorCamera"].is_object()) {
                const auto& c = j["editorCamera"];
                const glm::vec3 cur = camera.position();
                camera.setPosition({c.value("x", cur.x),
                                    c.value("y", cur.y),
                                    c.value("z", cur.z)});
                camera.setYaw(c.value("yaw", camera.yaw()));
                camera.setPitch(c.value("pitch", camera.pitch()));
            }
            look.layers.clear();
            if (j.contains("terrainLayers") && j["terrainLayers"].is_array())
                for (const auto& lj : j["terrainLayers"]) {
                    TerrainLayer L;
                    L.texId       = AssetId::fromString(lj.value("tex", std::string{}));
                    L.normId      = AssetId::fromString(lj.value("norm", std::string{}));
                    L.name        = lj.value("name", std::string{});
                    L.heightStart = lj.value("hStart", -1000.0f);
                    L.heightEnd   = lj.value("hEnd",    1000.0f);
                    L.slopeStart  = lj.value("sStart",  0.0f);
                    L.slopeEnd    = lj.value("sEnd",    90.0f);
                    L.scale       = lj.value("scale",   0.08f);
                    if (L.texId.valid())  L.tex  = assetDb.loadTexture(L.texId);
                    if (L.normId.valid()) L.norm = assetDb.loadTexture(L.normId);
                    look.layers.push_back(std::move(L));
                }
            // Restore the camera path. Read even when the key is absent: an empty
            // blob clears the recorder, and it has to, or the last scene's opening
            // move follows the author into a level that has none of its own.
            camPathRec.setBlob(j.value("camPath", std::string{}));
            anim::load(j, animClips);
            animgraph::load(j, animGraphs);
            animEditGraph = 0;
            if (animClips.empty()) animClips.push_back(anim::Clip{});
            animEditClip = 0;
            animPlay = anim::Player{};   // a new scene, a playhead back at zero
            animRuntime.clear();
            // Restore hand-painted grass (empty for scenes saved before it existed).
            veg.paintedBlades.clear();
            if (j.contains("paintedGrass") && j["paintedGrass"].is_string()) {
                const std::string& blob = j["paintedGrass"].get_ref<const std::string&>();
                veg.paintedBlades.reserve(blob.size() / 9);
                NumberBlobReader gs(blob);
                float v;
                while (gs.next(v)) veg.paintedBlades.push_back(v);
                veg.paintedBlades.resize(veg.paintedBlades.size() / 7 * 7); // whole blades
            }
            veg.paintedDirty = true; // re-upload to the GPU next frame
            // Restore the tree species config (LODs, billboards, densities). Falls
            // back to the default single species when the scene predates it.
            veg.deserializeTrees(j);
            // Restore hand-painted trees (regenTrees re-appends them next frame,
            // triggered by the veg.treeCenter reset below). New scenes store 6
            // floats/tree (with a species index); legacy scenes stored 5 -> species 0.
            veg.paintedTrees.clear();
            if (j.contains("paintedTrees2") && j["paintedTrees2"].is_string()) {
                NumberBlobReader ts(j["paintedTrees2"].get_ref<const std::string&>());
                float v;
                while (ts.next(v)) veg.paintedTrees.push_back(v);
                veg.paintedTrees.resize(veg.paintedTrees.size() / 6 * 6); // whole trees
            } else if (j.contains("paintedTrees") && j["paintedTrees"].is_string()) {
                std::istringstream ts(j["paintedTrees"].get<std::string>());
                std::vector<float> old;
                float v;
                while (ts >> v) old.push_back(v);
                old.resize(old.size() / 5 * 5);
                for (std::size_t i = 0; i + 5 <= old.size(); i += 5) {
                    veg.paintedTrees.insert(veg.paintedTrees.end(),
                                            old.begin() + i, old.begin() + i + 5);
                    veg.paintedTrees.push_back(0.0f); // legacy trees -> species 0
                }
            }
            // Restore hand-painted flowers (regenFlowers re-appends them when the
            // grass pass runs, triggered by the veg.grassDirty reset below).
            veg.paintedFlowers.clear();
            if (j.contains("paintedFlowers") && j["paintedFlowers"].is_string()) {
                NumberBlobReader fs(j["paintedFlowers"].get_ref<const std::string&>());
                float v;
                while (fs.next(v)) veg.paintedFlowers.push_back(v);
                veg.paintedFlowers.resize(veg.paintedFlowers.size() / 8 * 8); // whole flowers
            }
            // Restore terrain sculpt edits (empty for scenes saved before it
            // existed). Publish before the rebuild below so chunks bake them in.
            sculptWork.deltas.clear();
            // The field is being replaced wholesale, so the record of what the
            // watercourses had cut into the OLD one is meaningless. Dropping it
            // without giving the ground back is exactly right here: that ground
            // has gone with the rest of the field.
            rivers.forget();
            sculptWork.cell = j.value("terrainEditCell", 1.0f);
            if (j.contains("terrainEdits") && j["terrainEdits"].is_string()) {
                NumberBlobReader es(j["terrainEdits"].get_ref<const std::string&>());
                int ix, iz; float d;
                while (es.next(ix) && es.next(iz) && es.next(d))
                    sculptWork.deltas[TerrainEditField::cellKey(ix, iz)] = d;
            }
            publishSculpt();

            // Restore terrain texture paint (empty for older scenes). Publish before
            // the rebuild so the streamed chunks bake the weights into their vertices.
            paintWork.weights.clear();
            paintWork.cell = j.value("terrainPaintCell", 1.0f);
            if (j.contains("terrainPaint") && j["terrainPaint"].is_string()) {
                NumberBlobReader ps(j["terrainPaint"].get_ref<const std::string&>());
                int ix, iz; glm::vec4 w;
                while (ps.next(ix) && ps.next(iz) && ps.next(w.x) && ps.next(w.y) &&
                       ps.next(w.z) && ps.next(w.w))
                    paintWork.weights[TerrainEditField::cellKey(ix, iz)] = w;
            }
            publishPaint();

            streamer.settings() = uiSettings;
            streamer.rebuild();
            streamer.update(camera.position());
            veg.grassDirty = true;
            veg.treeCenter = glm::vec2(1e9f);

            // Roads: reads the `roads` array, or the single `road` object a scene
            // from before roads were plural has. A scene with neither (saved
            // before roads were persisted at all) loads as one empty road rather
            // than inheriting the roads of the scene being replaced.
            roads.load(j);
            roadSel = roadSel2 = -1;
            // Splines: absent in scenes saved before they existed, which load as
            // none rather than as an error (load() clears first either way).
            if (j.contains("splines") && j["splines"].is_object())
                splines.load(j["splines"]);
            else
                splines.clear();
            splineSel = splinePtSel = -1;
            // Water: absent in scenes saved before it existed, which load as none.
            // The bed is not in the file, so cut it now -- before anything asks
            // the terrain how high it is, which on this path is the road's
            // re-loft immediately below.
            if (j.contains("rivers") && j["rivers"].is_object())
                rivers.load(j["rivers"]);
            else
                rivers.clear();
            riverSel = riverPtSel = -1;
            // Towns: absent in older scenes, which load as none.
            if (j.contains("towns") && j["towns"].is_object())
                towns.load(j["towns"]);
            else
                towns.clear();
            towns.markDirty();
            townSel = -1;
            townEditing = false;
            {
                glm::vec2 mn, mx;
                if (rivers.carve(sculptWork, paintWork, mn, mx)) {
                    publishSculpt();
                    publishPaint();
                    streamer.editsChanged(mn, mx);
                }
                veg.wet = rivers.wetDiscs(0.6f);
                veg.grassDirty = true;
            }
            // Scene 2D UI overlay: clears itself first, so scenes without the key
            // (older ones, or a fresh scene) load with an empty overlay.
            uiOverlay.load(j);
            uiSel = uiOverlay.empty() ? -1 : 0;
            // The graded corridor is already baked into the restored terrain
            // edits above, so just re-loft the committed mesh on that ground.
            roads.rebuildMeshes();

            // Re-apply model-material overrides now that every model has
            // re-imported (see writeSettings). Matched by the same stable key so
            // edits to model-owned materials survive save/load.
            if (j.contains("modelMaterialOverrides") &&
                j["modelMaterialOverrides"].is_object()) {
                const auto& ov = j["modelMaterialOverrides"];
                auto rd3 = [](const nlohmann::json& a, glm::vec3 d) {
                    return (a.is_array() && a.size() == 3)
                        ? glm::vec3(a[0].get<float>(), a[1].get<float>(), a[2].get<float>())
                        : d;
                };
                for (std::size_t mi = 0; mi < models.count(); ++mi) {
                    LoadedModel* lm = models.at(mi);
                    if (!lm) continue;
                    for (std::size_t p = 0; p < lm->primMaterialId.size(); ++p) {
                        const std::string key = lm->assetId.toString() + "|" +
                                                lm->name + "|" + std::to_string(p);
                        if (!ov.contains(key)) continue;
                        const int idx = document.materialIndex(lm->primMaterialId[p]);
                        if (idx < 0) continue;
                        MaterialDef& md = materials[idx];
                        const auto& e = ov[key];
                        md.name          = e.value("name", md.name);
                        md.albedo        = rd3(e.value("albedo", nlohmann::json{}), md.albedo);
                        md.tint          = rd3(e.value("tint", nlohmann::json{}), md.tint);
                        // Every model material is written here on every save,
                        // edited or not -- so an override from before the import
                        // read glTF's metallic-roughness and emission carries the
                        // OLD defaults (roughness 0.2, no metal, no glow), and
                        // would quietly undo the import's values in every existing
                        // project. Without the "pbr" mark, a field still at that
                        // old default was never touched and the import keeps it.
                        const bool legacy = !e.contains("pbr");
                        const float sRefl  = e.value("reflectivity", md.reflectivity);
                        const float sRough = e.value("roughness", md.roughness);
                        const bool  untouchedPbr = legacy && sRefl == 0.0f &&
                                                   std::abs(sRough - 0.2f) < 1e-4f;
                        if (!untouchedPbr) { md.reflectivity = sRefl; md.roughness = sRough; }
                        md.opacity       = e.value("opacity", md.opacity);
                        md.glass         = e.value("glass", md.glass);
                        md.ior           = e.value("ior", md.ior);
                        md.thickness     = e.value("thickness", md.thickness);
                        md.alphaMode     = static_cast<AlphaMode>(
                            e.value("alphaMode", static_cast<int>(md.alphaMode)));
                        md.alphaCutoff   = e.value("alphaCutoff", md.alphaCutoff);
                        const glm::vec3 sEmis = rd3(e.value("emission", nlohmann::json{}),
                                                    md.emission);
                        if (!(legacy && sEmis == glm::vec3(0.0f))) {
                            md.emission         = sEmis;
                            md.emissionStrength = e.value("emissionStrength", md.emissionStrength);
                        }
                        // Map slots (see writeSettings): a GUID re-binds the
                        // texture asset, "" means the user emptied the slot, and
                        // an absent key leaves the model's own map in place.
                        auto readSlot = [&](const char* key, AssetId& id,
                                            std::shared_ptr<Texture>& tex) {
                            if (!e.contains(key) || !e[key].is_string()) return;
                            const std::string s = e[key].get<std::string>();
                            if (s.empty()) { id = {}; tex.reset(); return; }
                            const AssetId gid = AssetId::fromString(s);
                            if (!gid.valid()) return;
                            id  = gid;
                            // The way the model's own maps are the right way
                            // up: unflipped (see loadTextureForModel).
                            tex = assetDb.loadTextureForModel(gid);
                        };
                        readSlot("texture", md.texId, md.tex);
                        // Reference only -- the bind pass in the frame loop opens
                        // it, the same as for .fmat materials.
                        if (e.contains("video") && e["video"].is_string())
                            md.videoId =
                                AssetId::fromString(e["video"].get<std::string>());
                        camtex::load(e, md);
                        readSlot("normalMap", md.normalTexId, md.normalTex);
                        readSlot("emissionMap", md.emissionTexId, md.emissionTex);
                        readSlot("opacityMap", md.opacityTexId, md.opacityTex);
                    }
                }
            }

            // Re-point the primitives the user reassigned. AFTER the overrides
            // above, and that order is not incidental: an override belongs to the
            // model's OWN material, and applying it once a primitive already
            // pointed elsewhere would stamp the model's colours onto a library
            // material the rest of the scene shares.
            if (j.contains("modelMaterialAssign") &&
                j["modelMaterialAssign"].is_object()) {
                const auto& asg = j["modelMaterialAssign"];
                for (std::size_t mi = 0; mi < models.count(); ++mi) {
                    LoadedModel* lm = models.at(mi);
                    if (!lm) continue;
                    for (std::size_t p = 0; p < lm->primMaterialId.size(); ++p) {
                        const std::string key = lm->assetId.toString() + "|" +
                                                lm->name + "|" + std::to_string(p);
                        if (!asg.contains(key) || !asg[key].is_string()) continue;
                        const AssetId gid =
                            AssetId::fromString(asg[key].get<std::string>());
                        // A material that no longer exists -- deleted, or a project
                        // this model was copied out of -- leaves the primitive on
                        // the model's own material rather than on nothing at all.
                        if (gid.valid() && document.materialIndex(gid) >= 0)
                            lm->primMaterialId[p] = gid;
                    }
                }
            }
        };

        // Post-load migration, run once per scene load with the entity ids settled.
        //
        // The terrain used to be part of the world's SETTINGS -- every scene had
        // ground whether it asked for it or not. It is an entity now, so a scene
        // saved back then has terrain parameters but no Terrain component, and
        // would open as an empty void. Recognise that case (no "terrainEntity"
        // marker in the file) and give the scene the ground it was authored with:
        // readSettings has just loaded those parameters into uiSettings, so the
        // migrated terrain is exactly the one the file described. Saving the scene
        // then writes the marker and the world is an entity from there on.
        // Light the world with the panorama the scene names. The settings carry
        // the project-relative path; EnvironmentIBL wants a file, so the asset
        // database is asked to turn one into the other -- which is also what
        // makes a scene survive the library being moved or re-scanned.
        //
        // A name that no longer resolves leaves the environment unlit and says
        // so, rather than quietly keeping the previous scene's sky: an HDRI that
        // followed you from the last scene is a lighting bug you go looking for
        // in the wrong place.
        auto applyHdri = [&] {
            if (hdriLoaded.empty()) return;
            for (const AssetId id : assetDb.allAssets()) {
                const AssetDatabase::Entry* e = assetDb.entry(id);
                if (!e || e->relPath != hdriLoaded) continue;
                if (environment.load(e->absPath.string()))
                    hdriAbsPath = e->absPath.string();
                else
                    std::fprintf(stderr, "[Fitzel] HDRI failed to load: %s\n",
                                 e->absPath.string().c_str());
                return;
            }
            std::fprintf(stderr, "[Fitzel] scene names an HDRI the asset library "
                                 "does not have: %s\n", hdriLoaded.c_str());
        };

        afterSceneLoadFn = [&] {
            applyHdri();
            bool have = false;
            for (const Entity& e : entities)
                if (e.components.get<TerrainComponent>()) { have = true; break; }
            if (!have && !sceneStoredTerrainEntity) {
                entities.push_back(makeTerrainEntity(uiSettings));
                std::puts("[Fitzel] Scene predates terrain objects: its terrain was "
                          "migrated into a Terrain entity.");
            }
            sceneStoredTerrainEntity = false; // consumed; the next load sets it again
            // Make the mirror adopt whatever the scene brought, settings and all.
            terrainEntity = -1;
            terrainSynced = false;
        };

        // --- Play mode: run the scene as a game -------------------------------
        // Play snapshots the editable scene state and drops the player into
        // first-person walk mode; Stop restores the snapshot and the edit camera
        // exactly, so play-time changes never leak into the edited scene.
        bool playMode = false;
#ifndef FITZEL_PLAYER
        // Open a prefab on a stage of its own: the scene steps aside and the
        // prefab becomes the document, edited with the editor's own tools (see
        // PrefabEdit.hpp). The eye is parked and framed on it; closing puts both
        // the scene and the eye back where they were.
        auto openPrefabForEdit = [&](const std::string& path) {
            if (playMode) {
                exportStatus = "Stop Play before editing a prefab.";
                return;
            }
            // On the ground at the origin, so there is a floor under it rather
            // than a void -- the terrain is not an entity and stays.
            const glm::vec3 at(0.0f, streamer.heightAt(0.0f, 0.0f), 0.0f);
            // The eye and the edit mode are remembered BEFORE the swap, because
            // after it there is no scene left to read them back out of.
            const glm::vec3 wasPos   = camera.position();
            const float     wasYaw   = camera.yaw();
            const float     wasPitch = camera.pitch();
            const bool      wasEdit  = entityEditMode;
            std::string status;
            if (!prefabedit::open(prefabEdit, pio, path, entities, history,
                                  entityCounter, at, status)) {
                exportStatus = status;
                return;
            }
            prefabEdit.camPos = wasPos;
            prefabEdit.camYaw = wasYaw;
            prefabEdit.camPitch = wasPitch;
            prefabEdit.entityEdit = wasEdit;
            exportStatus = status;
            sel.clear();
            entityEditMode = false;   // a fresh stage, not the last mesh edit
            placeMode      = false;
            resolveHierarchy();
            glm::vec3 eye(0.0f), look(0.0f);
            if (prefabedit::frame(prefabEdit, entities, camera.fov(), eye, look)) {
                camera.setPosition(eye);
                const glm::vec3 d = look - eye;
                if (glm::length(d) > 1e-4f)
                    camera.setBasis(glm::normalize(d), glm::vec3(0.0f, 1.0f, 0.0f));
            }
            if (prefabEdit.rootId >= 0) sel.select(prefabEdit.rootId);
        };
        // Put the scene back. The prefab's file is whatever the last Save left --
        // this does not write one, which is what makes Discard mean discard.
        auto closePrefabEdit = [&]() {
            if (!prefabEdit.active) return;
            const glm::vec3 pos = prefabEdit.camPos;
            const float yaw = prefabEdit.camYaw, pitch = prefabEdit.camPitch;
            const bool edit = prefabEdit.entityEdit;
            prefabedit::close(prefabEdit, entities, history);
            camera.setPosition(pos);
            camera.setYaw(yaw);
            camera.setPitch(pitch);
            entityEditMode = edit;
            sel.clear();
            resolveHierarchy();
        };
        // Save the stage back over the prefab's own file, keeping its GUID so
        // every instance already in a scene still points at it. The spawn cache
        // is dropped for that name, or the next rival built from it would be the
        // version this just replaced.
        auto savePrefabEdit = [&]() {
            std::string status;
            if (prefabedit::saveBack(prefabEdit, pio, entities, prefabDir(), status)) {
                std::string key = prefabEdit.name;
                for (char& c : key)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                prefabCache.erase(key);
            }
            exportStatus = status;
        };
#endif
        // The scene's UI overlay is open as a menu right now (see UiOverlay's
        // menu mode): the game is running but the overlay owns mouse + keyboard,
        // so no walking, driving or script input this frame.
        bool uiMenuOpen = false;
        bool prevUiKey  = false; // edge state for the menu's toggle key
        // Keyboard / gamepad navigation of the scene overlay's buttons. The
        // activation is deferred to the HUD pass, which is where the action sink
        // (scene load, quit, restart, ...) is assembled.
        bool uiActivate  = false;
        bool prevUiPrev  = false, prevUiNext = false, prevUiFire = false;
        // End-of-race question ("race again / start screen"): the HUD draws and
        // answers it, main owns the state, the key edges and the two actions.
        racehud::EndPrompt endPrompt;
        // Player two's board. Its own state because the HUD keeps the "board
        // acknowledged" flag in here and the two players finish at different
        // times; sharing it would let one player's classification dismiss the
        // other's. It is never asked the question -- see the draw call.
        racehud::EndPrompt endPrompt2;
        bool prevEndPrev = false, prevEndNext = false, prevEndFire = false;
        bool pendingStartScreen = false; // deferred: leave the race (see below)

        // --- Showroom: the start screen a race is launched from --------------
        // A scene carrying a Showroom component stops being a level while it
        // plays and becomes the craft/circuit picker (Showroom.cpp). Its own
        // key edges, mirroring the end-of-race question's.
        showroom::Showroom showroomUi;
        bool prevShLeft = false, prevShRight = false, prevShUp = false;
        bool prevShDown = false, prevShFire = false;
        // The craft the showroom picked, carried into the circuit. Held as JSON
        // rather than as live entities: loading the circuit clears the model
        // library, so a live copy's model ids would dangle -- the asset GUIDs in
        // the JSON re-import against the new one. Same round trip a .fprefab
        // makes, without the file.
        // Did this launch come from the START SCREEN, as opposed to a restart
        // re-sending the same craft? Only the first gets the grid orbit: a
        // restart is a second run of a circuit the player has already been
        // introduced to, and making them sit through the introduction again is
        // the opposite of what "race again" asks for.
        bool           pendingFromShowroom = false;
        nlohmann::json pendingCraftJson;
        std::string    pendingCraftName;
        // The second seat's craft, when the start screen was set to two players.
        // Carried the same way and for the same reason: the scene it will race in
        // is not loaded yet, so the choice travels as data, not as an entity.
        nlohmann::json pendingCraftJson2;
        std::string    pendingCraftName2;
        int            pendingCraftLaps = 0;
        // The rest of the start screen's race setup, travelling with the craft
        // for the same reason it does: the circuit is loaded a frame or two
        // later, and these are overrides laid over it once it is there. The
        // "leave the scene alone" values (-1, 1.0) are what a start screen
        // nobody touched hands over.
        int            pendingRaceMode    = -1;   // -1 = the circuit's own
        int            pendingRaceField   = -1;   // -1 = the field as authored
        // The difficulty step the next race runs at. The odd one out among these:
        // the others default to "leave the scene alone", this one defaults to the
        // player's own profile, because a race started without going past the
        // start screen (a level change, Play in the editor) still has a player
        // with a setting. PRO is simply the step whose every multiplier is 1.
        int            pendingRaceLevel   = gameDifficulty.level;
        // --- The launch, kept for as long as the race lasts -------------------
        // The pending values above are consumed the moment the craft arrives on
        // the grid, which is right: they are a message, and a message is read
        // once. But a restart ("Race again", the overlay's Restart) rewinds the
        // circuit to Play's snapshot -- and that snapshot was taken BEFORE the
        // chosen craft was spawned, so it does not contain it. Without a copy,
        // the second run of a circuit is flown in whatever glider the scene
        // itself parks on the grid: the start screen's answer would hold for one
        // race and then quietly expire.
        //
        // So the launch is kept here for the whole session on that circuit and
        // re-sent on every restart. Cleared when a scene is loaded (a level
        // change, or going back to the start screen), because the craft belongs
        // to the race it was chosen for, not to whatever is loaded next.
        nlohmann::json sessionCraftJson, sessionCraftJson2;
        std::string    sessionCraftName, sessionCraftName2;
        int            sessionCraftLaps   = 0;
        int            sessionRaceMode    = -1;
        int            sessionRaceField   = -1;
        int            sessionRaceLevel   = gameDifficulty.level;
        ScriptSystem scripts; // Lua entity scripts, ticked while playing

        // Where entity scripts live: the open project's scripts/ folder, or the
        // bundled scripts/ next to the exe when no project is open (demo scripts).
        auto scriptsDir = [&]() -> std::string {
            if (!currentProject.empty())
                return (std::filesystem::path(currentProject).parent_path() /
                        "scripts").generic_string();
            return "scripts";
        };
        auto scriptPath = [&](const std::string& file){
            return scriptsDir() + "/" + file;
        };
#ifndef FITZEL_PLAYER
        // --- Lua script editor (see ScriptEditor.hpp) ------------------------
        ScriptEditor scriptEditor;
        scriptEditor.scriptsDir = scriptsDir;
        auto listScripts = [&] { return scriptEditor.list(); };
        auto openScript  = [&](const std::string& file) { scriptEditor.open(file); };
        // Exported script parameters (module-level globals), cached per file and
        // re-scanned when the .lua changes on disk -- so editing a script and
        // returning to the Inspector shows the current set. The struct lives in
        // InspectorPanel.hpp: the Inspector is what renders these, and a
        // function-local struct cannot be named across a header.
        using inspectorui::ScriptParamScan;
        std::unordered_map<std::string, ScriptParamScan> scriptParamCache;
        auto scanScriptParams = [&](const std::string& file) -> const ScriptParamScan& {
            const std::string path = scriptPath(file);
            std::error_code ec;
            const auto mtime = std::filesystem::last_write_time(path, ec);
            auto it = scriptParamCache.find(path);
            if (it == scriptParamCache.end() || ec || it->second.mtime != mtime) {
                ScriptParamScan s;
                s.mtime = ec ? std::filesystem::file_time_type{} : mtime;
                s.defs  = scripts.scanParams(path, &s.err);
                s.ok    = s.err.empty();
                it = scriptParamCache.insert_or_assign(path, std::move(s)).first;
            }
            return it->second;
        };
#endif // !FITZEL_PLAYER
        // Sounds and sprites known to the asset database (engine + project), by
        // bare filename -- what game.playSound, CollectibleComponent and the
        // Inspector's pickers resolve against, so they are chosen and not typed.
        auto listSounds   = [&]{ return assetNamesOfType(assetDb, AssetType::Sound); };
        auto listTextures = [&]{ return assetNamesOfType(assetDb, AssetType::Texture); };
        auto texturePickerCombo = [&](const char* label, std::string& field) {
            assetPickerCombo(label, field, listTextures(), "(soft dot)", "texture");
        };
        auto soundPickerCombo = [&](const char* label, std::string& field) {
            assetPickerCombo(label, field, listSounds(), "(none)", "sound");
        };
        auto imagePickerCombo = [&](const char* label, std::string& field) {
            assetPickerCombo(label, field, listTextures(), "(none)", "texture");
        };
        std::vector<Entity>      playEntities;
        std::vector<MaterialDef> playMaterials;
        std::unique_ptr<PhysicsWorld> physics;      // rigid-body world during Play
        std::map<int, PhysicsBodyId>  physicsBody;  // entity id -> body handle
        // What hangs and swings when hit (Swing.hpp): made with the physics world
        // at Play start, gone with it.
        swing::System swingSys;
        // The entities that wobble instead of moving as one piece. Built with the
        // physics world at Play start and thrown away with it (see SoftBodySystem).
        SoftBodySystem                softBodies;
        float                         softWindTime = 0.0f; // seconds of Play, keys the gusts
        // Knockable road side objects (posts/bollards), see playworld::SidePost.
        std::vector<playworld::SidePost> sidePosts;

        // --- Scene-vehicle drive helpers (see VehicleTool for the setup UI) ---
        // The nearest entity carrying a VehicleComponent, or -1. Not one a CPU
        // driver has (TrafficDriverComponent): the traffic drives that one, and
        // handing it the player's controls too left the player's own car
        // standing -- scaper's start sat a metre nearer the CPU's car than to
        // the player's.
        auto findNearestVehicle = [&]() -> int {
            int best = -1;
            float bestD = 1e30f;
            const glm::vec3 cp = camera.position();
            for (const Entity& e : entities) {
                if (!e.components.get<VehicleComponent>()) continue;
                if (e.components.get<TrafficDriverComponent>()) continue;
                const float d = glm::length(e.center - cp);
                if (d < bestD) { bestD = d; best = e.id; }
            }
            return best;
        };
        // Where the model sits relative to the physics chassis: the box centre
        // rides higher than the model so the wheels (which hang `chassisY` of
        // suspension below the box bottom) land where they were modelled.
        //
        // `chassisY` is the one number both sides must agree on -- it goes into
        // the Jolt suspension as its rest length below, and into the setup gizmo
        // as where it draws the box. It used to be a 0.4 written out twice.
        auto vehicleVisualY = [](const VehicleComponent& vc) {
            return -vc.chassisHalf.y - vc.chassisY - vc.wheelY;
        };
        // Spawn the Jolt car from the entity's component at its transform (in
        // Play). True on success; physCarId/driveVehicleId are set.
        auto spawnSceneVehicle = [&](int id) -> bool {
            Entity* e = document.find(id);
            auto* vc = e ? e->components.get<VehicleComponent>() : nullptr;
            if (!vc || !physics || !e->activeInHierarchy) return false;
            glm::quat q = glm::quat(glm::radians(e->rotation));
            if (vc->forward == 1) // nose points -Z: chassis frame is yawed 180
                q = q * glm::angleAxis(glm::pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
            // Undo the render offset and nudge up so the suspension settles.
            const glm::vec3 sp = e->center -
                q * glm::vec3(0.0f, vehicleVisualY(*vc), 0.0f) +
                glm::vec3(0.0f, 0.3f, 0.0f);
            fitzel::PhysicsWorld::VehicleTuning tuning;
            tuning.comLower       = vc->comLower;
            tuning.suspensionFreq = vc->suspensionFreq;
            tuning.suspensionDamp = vc->suspensionDamp;
            tuning.antiRoll       = vc->antiRoll;
            tuning.grip           = vc->grip;
            tuning.drive          = vc->drive;
            tuning.uprightAssist  = vc->uprightAssist;
            tuning.suspensionRest = vc->chassisY;
            physCarHalf = glm::max(vc->chassisHalf, glm::vec3(0.05f));
            physCarId = physics->addVehicle(
                physCarHalf, vc->mass, sp, q,
                vc->wheelRadius, vc->wheelWidth, vc->halfTrack,
                vc->frontZ, vc->rearZ, vc->maxSteerDeg, vc->engineTorque, tuning);
            driveVehicleId = (physCarId != 0) ? id : -1;
            // The wheels as modelled, relative to the model: what they keep
            // wearing while Jolt steers and spins them (see the wheel sync).
            const glm::quat rootQ = glm::quat_cast(
                scenegraph::compose(glm::vec3(0.0f), e->rotation, glm::vec3(1.0f)));
            for (int i = 0; i < 4; ++i) {
                joltWheelStartSet[i] = false;
                joltWheelRest[i]     = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
                if (const Entity* w = document.find(vc->wheelId[i]))
                    joltWheelRest[i] = glm::inverse(rootQ) *
                        glm::quat_cast(scenegraph::compose(glm::vec3(0.0f), w->rotation,
                                                           glm::vec3(1.0f)));
            }
            return physCarId != 0;
        };
        // Editor test-drive: snapshot the root + wheels, then glue the arcade
        // sim onto the entity. endEditorDrive restores the snapshot -- driving
        // around in the editor never counts as a scene edit.
        auto beginEditorDrive = [&](int id) {
            Entity* e = document.find(id);
            auto* vc = e ? e->components.get<VehicleComponent>() : nullptr;
            if (!vc) return;
            driveBackup.clear();
            driveBackup.push_back(*e);
            for (int i = 0; i < 4; ++i)
                if (const Entity* w = document.find(vc->wheelId[i]))
                    driveBackup.push_back(*w);
            driveVehicleId    = id;
            editorDriveActive = true;
            carPos   = glm::vec3(e->center.x,
                                 streamer.heightAt(e->center.x, e->center.z),
                                 e->center.z);
            carYaw   = glm::radians(e->rotation.y) +
                       (vc->forward == 1 ? glm::pi<float>() : 0.0f);
            carSpeed = 0.0f;
        };
        auto endEditorDrive = [&] {
            if (!editorDriveActive) return;
            for (const Entity& b : driveBackup)
                if (Entity* e = document.find(b.id)) {
                    e->center = b.center;         e->rotation = b.rotation;
                    e->localCenter = b.localCenter; e->localRotation = b.localRotation;
                }
            driveBackup.clear();
            editorDriveActive = false;
            driveVehicleId    = -1;
        };
        // Enter drive mode (V key / Vehicle-panel checkbox): a scene vehicle
        // nearest to the camera takes precedence; with none, the primitive
        // test car behaves exactly as before.
        auto enterVehicleMode = [&] {
            fpsMode = false;
            boatMode = false; // every drive session starts on wheels
            input.setCursorLocked(false);
            const int sceneVeh = findNearestVehicle();
            if (playMode && physics) {
                if (!physics->hasVehicle()) {
                    if (sceneVeh < 0 || !spawnSceneVehicle(sceneVeh)) {
                        const glm::vec3 p = camera.position();
                        glm::vec3 f = camera.front(); f.y = 0.0f;
                        if (glm::length(f) < 1e-3f) f = glm::vec3(0, 0, 1);
                        f = glm::normalize(f);
                        const glm::quat q = glm::angleAxis(std::atan2(f.x, f.z),
                                                           glm::vec3(0, 1, 0));
                        const glm::vec3 sp(p.x, streamer.heightAt(p.x, p.z) + 1.2f, p.z);
                        physCarHalf = glm::vec3(0.9f, 0.35f, 2.0f);
                        physCarId = physics->addVehicle(
                            physCarHalf, 1200.0f, sp, q,
                            0.42f, 0.30f, 0.85f, 1.35f, -1.35f, 32.0f, 2500.0f);
                    }
                }
            } else if (sceneVeh >= 0) {
                beginEditorDrive(sceneVeh);
            } else if (!carPlaced) {
                placeCar();
            }
        };

        // --- Glider drive (arcade hover, editor + Play) -----------------------
        // Ground under (x,z): the terrain height, or the top of any solid block/
        // ramp/model whose axis-aligned footprint covers (x,z) and sits at/below
        // `yMax` -- so the craft floats over a track built from placed geometry
        // (rotation is ignored, like game.raycast). The flown craft (and its
        // children) are excluded so it never hovers on top of itself.
        // Scratch for the ground query below: the scene's racers, rebuilt per
        // call. A member rather than a local so a query on the sim's hot path
        // does not allocate on every fixed step.
        std::vector<int> racerRoots;
        // `ignoreId` is the craft doing the asking (-1 = nobody): its own model,
        // and anything hanging off it, is not the ground it hovers over. Passed
        // in rather than read from driveGliderId here, because with two players
        // there are two answers and this lambda serves both.
        auto gliderGround = [&](float x, float z, float yMax, int ignoreId) -> float {
            float h = streamer.heightAt(x, z);
            // No racer is ground. A craft is something you race past or crash
            // into, never a surface to hover over -- and treating one as ground
            // is unstable in both directions: two craft side by side on the grid
            // overlap in plan view, so each stands on the other's roof, lifts it,
            // and gets lifted in turn. That is the hopping. The rule covers every
            // racer rather than just the two seats, because a craft riding up an
            // opponent's tail is the same nonsense with one player.
            racerRoots.clear();
            for (const Entity& e : entities)
                if (e.components.get<GliderComponent>() ||
                    e.components.get<OpponentComponent>())
                    racerRoots.push_back(e.id);
            const auto isRacerPart = [&](const Entity& e) {
                for (int id : racerRoots)
                    if (e.id == id || e.parent == id) return true;
                return false;
            };
            for (const Entity& e : entities) {
                if (!e.activeInHierarchy) continue;
                if (ignoreId >= 0 && (e.id == ignoreId || e.parent == ignoreId)) continue;
                if (isRacerPart(e)) continue;
                if (!isSolidPrimitive(e.type) && e.type != EntityType::Model)
                    continue;
                // A modelled mesh is ground where its faces are, turned the way
                // it is drawn: the craft flies under an arch and over the notch
                // of an L rather than over their bounding boxes.
                if (const auto* mc = e.components.get<MeshComponent>()) {
                    float top;
                    if (meshquery::surfaceBelow(e, *modifiers::shown(e, *mc).mesh, x, z, yMax, top) &&
                        top > h)
                        h = top;
                    continue;
                }
                if (x < e.center.x - e.half.x || x > e.center.x + e.half.x) continue;
                if (z < e.center.z - e.half.z || z > e.center.z + e.half.z) continue;
                const float top = e.center.y + e.half.y;
                if (top <= yMax && top > h) h = top;
            }
            // Road/bridge deck: a bridged (elevated) road stretch is ground too --
            // without this the craft sinks through a high bridge to the terrain far
            // below. Same yMax gate as the blocks, so flying UNDER a bridge still
            // leaves the deck out of reach.
            // The ceiling goes INTO the query, not after it: where the road
            // crosses itself, asking without one comes back with whichever branch
            // is nearer in plan view, and if that is the flyover it is then thrown
            // away here -- taking the underpass with it, so a craft driving
            // through drops to the terrain. Passing yMax lets the query answer
            // with the storey the craft is actually on.
            float roadY = 0.0f;
            // The whole section counts as ground, raised edges included -- riding
            // up the lip is the point of it.
            if (roads.surfaceHeightAt(glm::vec2(x, z), roadY, yMax))
                if (roadY > h) h = roadY;
            return h;
        };
        auto findNearestGlider = [&]() -> int {
            int best = -1; float bestD = 1e30f;
            const glm::vec3 cp = camera.position();
            for (const Entity& e : entities) {
                if (!e.components.get<GliderComponent>()) continue;
                const float d = glm::length(e.center - cp);
                if (d < bestD) { bestD = d; best = e.id; }
            }
            return best;
        };
        // Start flying `id`: snapshot its transform (restored on exit) and seed the
        // flight state at its current pose, lifted to the hover rest height.
        // Seat a flight state in a craft: put it where the craft stands, at ride
        // height over the ground, facing the way it is turned, at rest and with
        // a full hull.
        //
        // Split out because player two needs exactly this and nothing else -- it
        // takes over a craft that is already in the scene, so it wants the same
        // seating without the backup/ownership half of beginGliderDrive. A state
        // that skips it starts at the world origin, which teleports the craft
        // there and sends its camera along.
        auto seatGliderState = [&](racesim::RaceState& st, int id) {
            Entity* e = document.find(id);
            auto* gc = e ? e->components.get<GliderComponent>() : nullptr;
            if (!gc) return false;
            st.gliderPos = e->center;
            // Ignoring the craft being seated: without that it is stood on its
            // own roof before it has flown a metre.
            st.gliderPos.y = gliderGround(e->center.x, e->center.z,
                                          e->center.y + 1000.0f, id) + gc->rideHeight;
            // Where the nose actually points -- a craft left banked by the
            // last flight has no heading in its rotation.y (see sceneHeading).
            st.gliderYaw = sceneHeading(e->rotation) +
                           (gc->forward == 1 ? glm::pi<float>() : 0.0f);
            st.gliderVel = glm::vec3(0.0f);
            st.gliderYawRate = 0.0f;   // seated, not mid-turn
            st.gliderBank = st.gliderPitch = 0.0f;
            st.gliderOverspeed = 0.0f;
            st.gliderWasOnPad  = false; // a pad under the start line still punches
            // Nothing to do about the camera here: the craft's camera entity
            // stands itself up the first frame it is seen (see CameraSystem).
            st.loopIndex = -1;  // not on a loop
            // Nothing to interpolate from: this pose was placed, not flown, and
            // blending out of wherever the craft last was would drag it across
            // the map for a frame.
            st.prevValid = false;
            // A fresh craft flies with a full hull: taking the controls is a new
            // run, so a wreck from the last one must not still be smoking.
            st.energyCapacity = glm::max(gc->energyCapacity, 1.0f);
            st.energy         = st.energyCapacity;
            st.energyOut      = false; st.energyLow      = false;
            st.energyIdle     = 0.0f;  st.energyHitFlash = 0.0f;
            st.energyLastHit  = 0.0f;  st.energyWarnT    = 0.0f;
            return true;
        };
        auto beginGliderDrive = [&](int id) {
            Entity* e = document.find(id);
            if (!e || !e->components.get<GliderComponent>()) return;
            gliderBackup.clear();
            gliderBackup.push_back(*e);
            driveGliderId     = id;
            gliderDriveActive = true;
            seatGliderState(race, id);
        };
        auto endGliderDrive = [&] {
            if (!gliderDriveActive) return;
            for (const Entity& b : gliderBackup)
                if (Entity* e = document.find(b.id)) {
                    e->center = b.center;         e->rotation = b.rotation;
                    e->localCenter = b.localCenter; e->localRotation = b.localRotation;
                }
            gliderBackup.clear();
            gliderDriveActive = false;
            driveGliderId     = -1;
            driveGliderId2    = -1;   // player two hands its craft back too
        };
        // Which craft player two flies, given the one player one has: the first
        // other glider in the scene. A two-player track is laid out with two
        // craft on it, so asking which is whose would be a dialog with one
        // sensible answer.
        //
        // A craft the AI is not already racing comes first; an entered opponent
        // is taken only if it is the only one left. Its tick is LEFT ALONE --
        // the sim skips it because RaceEnv names it (see playerGliderId2), which
        // keeps the authored field intact for the next start.
        auto pickPlayerTwo = [&](int excludeId) {
            for (int pass = 0; pass < 2; ++pass)
                for (Entity& ge : entities) {
                    if (ge.id == excludeId || !ge.activeInHierarchy) continue;
                    if (!ge.components.get<GliderComponent>()) continue;
                    if (pass == 0 && ge.components.get<OpponentComponent>()) continue;
                    return ge.id;
                }
            return -1;
        };
        // A prefab by name, loaded once and kept. Shared by the script host's
        // spawnPrefab and by the starting grid, which builds its field out of
        // prefabs too -- one lookup, one cache, one place a typo is reported.
        auto findPrefab = [&](const std::string& name) -> const prefab::Prefab* {
            if (currentProject.empty() || name.empty()) return nullptr;
            std::string key = name;
            for (char& c : key)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            auto it = prefabCache.find(key);
            if (it != prefabCache.end()) return &it->second;
            // Resolve the name to a .fprefab in the project's prefabs/ folder.
            const std::string dir = prefab::prefabsDirIn(
                std::filesystem::path(currentProject).parent_path().generic_string());
            std::string path;
            for (const auto& np : prefab::list(dir)) {
                std::string ln = np.first;
                for (char& c : ln)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (ln == key) { path = np.second; break; }
            }
            if (path.empty()) {
                std::fprintf(stderr, "[Fitzel] no prefab named '%s'\n", name.c_str());
                return nullptr;
            }
            auto loaded = prefab::load(pio, path);
            if (!loaded || loaded->entities.empty()) {
                std::fprintf(stderr, "[Fitzel] failed to load prefab '%s'\n",
                             name.c_str());
                return nullptr;
            }
            return &prefabCache.emplace(std::move(key), std::move(*loaded))
                        .first->second;
        };
        // The towns' traffic dresses vehicles in prefabs by name, too.
        townTraffic.findPrefab = findPrefab;
#ifndef FITZEL_PLAYER
        // ...and the procedural graphs' Prefab node places them.
        procPanel.prefabNames = [&]() {
            std::vector<std::string> names;
            if (currentProject.empty()) return names;
            for (const auto& np : prefab::list(prefab::prefabsDirIn(
                     std::filesystem::path(currentProject).parent_path().generic_string())))
                names.push_back(np.first);
            return names;
        };
        procPanel.spawnPrefab = [&](const std::string& name, int& counter) {
            const prefab::Prefab* p = findPrefab(name);
            return p ? prefab::instantiate(*p, counter, glm::vec3(0.0f), 0.0f) : std::vector<Entity>{};
        };
#endif
        townTraffic.models     = &models;
        townTraffic.meshCache  = &meshCache;
        // ...and stands its street lamps as prefabs, the same way.
        townLamps.findPrefab = findPrefab;
        townLamps.models     = &models;
        townLamps.meshCache  = &meshCache;

        // --- The level generator -------------------------------------------
        // The generator itself is pure (see LevelGen.hpp): it lays a circuit on a
        // landscape it is handed a sampler for and returns a description. This is
        // the half that turns a description into a world, and it lives here
        // because it needs findPrefab, one line up.
        levelgen::Params levelParams;
        levelgen::Report levelReport;
        bool levelReportValid = false;
        bool levelDiscardPaint = false;

        // --- Asynchronous, in two different ways -----------------------------
        // Laying a circuit out is arithmetic on data nobody else can see, so it
        // goes on a real thread and the sliders never wait for it. Putting one
        // INTO the world cannot: the corridor grading writes the shared height
        // field and every ribbon it lofts is a GL upload. That half is stepped
        // one phase per frame behind a modal instead -- the same bargain
        // ProjectIO makes for a scene load, and for the same reason.
        std::future<levelgen::Level> levelJob;
        bool levelJobRunning = false;
        bool levelJobStale   = false;   // the sliders moved while one was in flight
        levelgen::Level levelMade;      // the last circuit the thread finished
        int   levelStep     = -1;       // -1 = not applying
        float levelProgress = 0.0f;
        const char* levelLabel = "";

        auto startLevelJob = [&] {
            if (levelJobRunning) { levelJobStale = true; return; }
            // Captured BY VALUE. The thread must not read a slider the user is
            // still dragging, and the generator is a pure function of exactly
            // this -- plus a ground sampler, which is thread-safe by contract.
            const levelgen::Params snapshot = levelParams;
            levelJob = std::async(std::launch::async, [snapshot] {
                return levelgen::generate(snapshot, [](const TerrainSettings& ts,
                                                       float x, float z) {
                    return terrainBaseHeight(ts, x, z);
                });
            });
            levelJobRunning = true;
            levelJobStale   = false;
        };
        auto previewLevel = [&] { startLevelJob(); };
        auto applyLevel   = [&] { levelStep = 0; levelProgress = 0.0f; };

        // One phase of putting a generated circuit into the world, called once a
        // frame while an apply is running -- so the modal behind it is PAINTED
        // between phases. The corridor phase is still one long hitch; what the
        // slicing buys is that the dialog saying so is on screen before it
        // starts, instead of the desktop's own white rectangle.
        auto stepLevelApply = [&] {
            switch (levelStep) {
            case 0: {
                levelLabel = "Landscape";
                // The ground has to be announced BEFORE anything asks how high it
                // is: terrainBaseHeight answers 0 everywhere while
                // terrainPresent() is false, so a circuit laid out first would sit
                // on a flat void -- no gradient, no bridge and no tunnel anywhere,
                // which reads as the feature working badly rather than as a flag
                // nobody set.
                fitzel::setTerrainPresent(true);
                streamer.setEnabled(true);
                startLevelJob();
                levelStep = 1;
                levelProgress = 0.10f;
                break;
            }
            case 1: {
                levelLabel = "Laying out the circuit";
                if (levelJobRunning) break;          // still on the other thread
                const levelgen::Level& lvl = levelMade;
                streamer.settings() = lvl.terrain;
                uiSettings          = lvl.terrain;
                streamer.rebuild();
                if (Entity* te = document.find(terrainEntity)) {
                    if (auto* tc = te->components.get<TerrainComponent>())
                        tc->settings = lvl.terrain;
                } else {
                    entities.push_back(makeTerrainEntity(lvl.terrain));
                    terrainEntity = entities.back().id;
                }
                // Close syncTerrainEntity's loop by hand. Without this the next
                // frame sees a component it does not recognise, "adopts" the
                // terrain it already has and calls markNeedsBuild -- so a freshly
                // generated circuit opens with the Roads panel nagging to build it.
                compMirror    = lvl.terrain;
                uiMirror      = lvl.terrain;
                terrainOn     = true;
                terrainSynced = true;
                levelStep = 2;
                levelProgress = 0.30f;
                break;
            }
            case 2: {
                levelLabel = "Roads";
                const levelgen::Level& lvl = levelMade;
                // Give the old corridor back. buildAll writes deltas only inside
                // the NEW circuit's swept rectangle, so anything the old track cut
                // outside it would stay behind as a trench -- and those deltas are
                // measured against a base terrain that no longer exists.
                sculptWork.deltas.clear();
                publishSculpt();
                if (levelDiscardPaint) { paintWork.weights.clear(); publishPaint(); }

                // One road, and it is index 0: the race reads roads.active(), and
                // a scene load selects 0 whatever the editor was pointing at.
                // clear(), not remove(): the slots stay alive for the RoadShapeCmds
                // that borrow them, and slot 0 is the one onCreate already wired
                // with the city palettes.
                roads.clear();
                roads.select(0);
                RoadSystem& r = roads.at(0);
                r.name      = "Circuit";
                r.width     = lvl.track.width;
                r.grade     = lvl.track.grade;
                r.shoulder  = lvl.track.shoulder;
                r.edgeWidth = lvl.track.edgeWidth;
                r.edgeAngle = lvl.track.edgeAngle;
                r.bridgeStyle   = roadbridge::Params{};
                r.tunnelStyle   = roadtunnel::Params{};
                r.junctionStyle = roadjunction::Params{};
                // setShape keeps roadPts, ptLift and ptBank in lockstep AND
                // re-derives the side objects, the city and the decals.
                RoadSystem::Shape sh;
                sh.points = lvl.track.points;
                sh.lifts  = lvl.track.lift;
                sh.banks  = lvl.track.bank;
                sh.closed = lvl.track.closed;
                for (const levelgen::Span& b : lvl.track.bridges)
                    sh.bridges.push_back({b.a, b.b});
                for (const levelgen::Span& t : lvl.track.tunnels)
                    sh.tunnels.push_back({t.a, t.b});
                sh.loops       = lvl.track.loops;
                sh.sideObjects = lvl.sideLines;
                sh.decalRules  = lvl.decals;
                sh.biomes      = lvl.biomes;
                sh.label       = "Circuit";
                r.setShape(sh);
                levelStep = 3;
                levelProgress = 0.45f;
                break;
            }
            case 3: {
                levelLabel = "Cutting the corridor";
                glm::vec2 mn, mx;
                if (roads.buildAll(sculptWork, mn, mx)) {
                    publishSculpt();
                    streamer.editsChanged(mn, mx);
                }
                levelStep = 4;
                levelProgress = 0.75f;
                break;
            }
            case 4: {
                levelLabel = "Rails, kerbs and the city";
                // Only now: all of it stands on the ground the corridor just cut.
                for (RoadSystem* rr : roads) {
                    rr->rebuildSideObjects();
                    rr->rebuildCity();
                }
                // The ground moved under every watercourse; phase 2 took the beds
                // with it.
                carveRivers();
                levelStep = 5;
                levelProgress = 0.90f;
                break;
            }
            case 5: {
                levelLabel = "Race objects";
                const levelgen::Level& lvl = levelMade;
            // 6) The race objects, as one batch. Old ones out, new ones in.
            std::vector<int> old;
            for (const Entity& e : entities)
                if (e.components.get<FinishLineComponent>() ||
                    e.components.get<CheckpointComponent>() ||
                    e.components.get<GridPositionComponent>()) {
                    // A gate's component sits on a CHILD of the prefab root, and
                    // it is the whole subtree that has to go.
                    int root = e.id;
                    for (int guard = 0; guard < 8; ++guard) {
                        const Entity* pe = document.find(root);
                        if (!pe || pe->parent < 0) break;
                        root = pe->parent;
                    }
                    if (std::find(old.begin(), old.end(), root) == old.end())
                        old.push_back(root);
                }
            std::vector<int> oldAll;
            for (int id : old)
                for (int sub : collectSubtreeIds(id))
                    if (std::find(oldAll.begin(), oldAll.end(), sub) == oldAll.end())
                        oldAll.push_back(sub);

            std::vector<Entity> add;
            for (const levelgen::Marker& m : lvl.markers) {
                const glm::vec3 rot(0.0f, m.headingDeg, 0.0f);
                auto attach = [&](Entity& e) {
                    if (m.kind == levelgen::Marker::Kind::Finish) {
                        auto* fl = e.components.get<FinishLineComponent>();
                        if (!fl) {
                            e.components.items.push_back(
                                std::make_unique<FinishLineComponent>());
                            fl = e.components.get<FinishLineComponent>();
                        }
                        fl->laps = levelParams.laps; fl->mode = 0;
                        fl->gridBack = 14.0f; fl->gridRow = 8.0f;
                        fl->gridLane = std::min(2.6f, lvl.track.width * 0.18f);
                        fl->playerPole = false;
                        fl->width = m.gateW; fl->height = m.gateH; fl->depth = m.gateD;
                        fl->yaw = 0.0f;
                    } else if (m.kind == levelgen::Marker::Kind::Checkpoint) {
                        auto* cp = e.components.get<CheckpointComponent>();
                        if (!cp) {
                            e.components.items.push_back(
                                std::make_unique<CheckpointComponent>());
                            cp = e.components.get<CheckpointComponent>();
                        }
                        cp->width = m.gateW; cp->height = m.gateH; cp->depth = m.gateD;
                        cp->yaw = 0.0f;
                    } else {
                        auto* gp = e.components.get<GridPositionComponent>();
                        if (!gp) {
                            e.components.items.push_back(
                                std::make_unique<GridPositionComponent>());
                            gp = e.components.get<GridPositionComponent>();
                        }
                        gp->slot = m.slot; gp->player = m.player; gp->prefab = m.prefab;
                    }
                };
                // A grid slot is always a bare Empty -- that is what a hand-made
                // one is, and a marker that cannot fail is better than one that
                // can. The gates take their prefab when it resolves.
                const prefab::Prefab* pf =
                    (m.kind == levelgen::Marker::Kind::Grid || m.prefab.empty())
                        ? nullptr : findPrefab(m.prefab);
                if (pf) {
                    std::vector<Entity> inst =
                        prefab::instantiate(*pf, entityCounter, m.pos, m.headingDeg);
                    if (!inst.empty()) {
                        // ASSIGN the rotation, do not add to it: instantiate adds
                        // the yaw to whatever the prefab's root was authored at,
                        // and the start line prefab is authored three degrees off.
                        inst.front().localRotation = inst.front().rotation = rot;
                        // The gate component lives on a CHILD; put it where the
                        // marker asked, and let the root follow.
                        Entity* carrier = nullptr;
                        for (Entity& e : inst)
                            if (e.components.get<CheckpointComponent>() ||
                                e.components.get<FinishLineComponent>()) carrier = &e;
                        attach(carrier ? *carrier : inst.front());
                        for (Entity& e : inst) add.push_back(std::move(e));
                        continue;
                    }
                }
                Entity e;
                e.type = EntityType::Empty;
                e.id   = entityCounter++;
                e.name = (m.kind == levelgen::Marker::Kind::Finish) ? "Start/Finish"
                       : (m.kind == levelgen::Marker::Kind::Checkpoint)
                             ? ("Checkpoint " + std::to_string(add.size()))
                             : ("GridPosition" + std::to_string(m.slot + 1));
                e.half = glm::vec3(0.5f);
                e.localCenter = e.center = m.pos;
                e.localRotation = e.rotation = rot;
                e.parent = -1;
                attach(e);
                add.push_back(std::move(e));
            }
            history.push(std::make_unique<ReplaceEntitiesCmd>(document, oldAll,
                                                              std::move(add),
                                                              "Generate level"),
                         document);
            // World transforms NOW, not next frame: racegrid and racesim read the
            // derived center/rotation, and an autosave in between would catch
            // every marker sitting at the origin.
            resolveHierarchy();

            // A world-replacing operation is a boundary, like a scene load. The
            // corridor it just cut is not on the undo stack -- nothing in the
            // editor puts a sculpt there -- and an operation that is half
            // undoable is worse than one that is honestly not.
            sel.clear();
            history.clear();
            prefabCache.clear();
            levelStep = -1;
            levelProgress = 1.0f;
            break;
            }
            default: levelStep = -1; break;
            }
        };

        // Collect what the thread finished, and drive an apply if one is running.
        auto pumpLevel = [&] {
            if (levelJobRunning &&
                levelJob.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready) {
                levelMade        = levelJob.get();
                levelReport      = levelMade.report;
                levelReportValid = true;
                levelJobRunning  = false;
                // The sliders moved while it was working, so what came back is
                // already out of date -- ask again rather than show it. Unless an
                // apply is waiting on this one, which must get the circuit it
                // asked for and not a newer one.
                if (levelJobStale && levelStep < 0) startLevelJob();
            }
            if (levelStep >= 0) stepLevelApply();
        };

        // The starting grid builds its own field: every Grid Position marker that
        // names a prefab makes the rival that stands on it (see racegrid::populate
        // and GridPositionComponent).
        //
        // PLAY ONLY, and that is not a detail. This puts craft INTO the scene, and
        // the only thing that takes them out again is Play's snapshot being
        // restored on Stop. Run in the editor it would silently breed a field into
        // the .fitzel on disk -- the same rule every other race override follows.
        //
        // Straight into `entities`, not through the script host's deferred spawn
        // queue: the grid is lined up on the very next line, and a craft that only
        // exists at the end of the tick would miss it and start in the paddock.
        auto buildGridField = [&](int playerId, int playerId2, int rivalsWanted) {
            if (!playMode) return;
            racegrid::populate(
                entities, rivalsWanted, playerId,
                [&](const std::string& name, const glm::vec3& pos,
                    float yawDeg) -> int {
                    const prefab::Prefab* pf = findPrefab(name);
                    if (!pf) return -1;
                    std::vector<Entity> inst =
                        prefab::instantiate(*pf, entityCounter, pos, yawDeg);
                    if (inst.empty()) return -1;
                    const int rootId = inst.front().id;  // the root comes first
                    for (Entity& ne : inst) entities.push_back(std::move(ne));
                    return rootId;
                },
                playerId2);
        };

        // Enter glider mode (G key / Glider-panel checkbox): fly the nearest glider
        // entity. Arcade in both editor and Play, so no physics body is created.
        auto enterGliderMode = [&] {
            fpsMode = false;
            input.setCursorLocked(false);
            const int g = findNearestGlider();
            // Player two is chosen HERE, before the grid is drawn up, because the
            // grid has to give it a slot: a craft nobody lines up stays parked
            // wherever the scene author left it, which in a finished track is in
            // the paddock behind the stands -- with a camera inside the scenery.
            race2 = racesim::RaceState{};
            driveGliderId2 = (splitScreen && g >= 0) ? pickPlayerTwo(g) : -1;
            // Line the field up BEFORE taking the controls: an opponent seeds its
            // race distance from where it stands, and the drive reads the craft's
            // position, so the grid has to exist first or everyone starts from
            // wherever they were parked.
            if (g >= 0 && playMode) {
                // The grid builds its field first, or there is nothing to line up
                // but the craft the author happened to park.
                buildGridField(g, driveGliderId2, sessionRaceField);
                racegrid::lineUp(entities, roads.active(), g, /*applyParticipation=*/true,
                                 driveGliderId2);
            }
            if (g >= 0) beginGliderDrive(g);
            else        gliderMode = false; // nothing to fly
            // Seat player two only once the grid has moved its craft: the state
            // copies the pose, so seating it first would seat it in the paddock.
            // Its craft joins the drive's snapshot, so leaving glider mode puts
            // it back where player one's craft goes back to -- otherwise flying
            // it around the editor would quietly rewrite the scene.
            if (gliderMode && driveGliderId2 >= 0) {
                seatGliderState(race2, driveGliderId2);
                if (Entity* e2 = document.find(driveGliderId2))
                    gliderBackup.push_back(*e2);
            }
            // In Play, start a race with a Ready/Set/Go countdown when the scene
            // is a race (has opponents or a start/finish line) -- craft + opponents
            // are held frozen until GO so nobody jumps the start.
            if (gliderMode && playMode) {
                bool hasRace = false;
                for (const Entity& e : entities)
                    if (e.components.get<OpponentComponent>() ||
                        e.components.get<FinishLineComponent>()) { hasRace = true; break; }
                raceCountdown = hasRace ? 3.0f : 0.0f;
                goFlash = 0.0f;
                endPrompt = racehud::EndPrompt{};
                // Player two counts down with everyone else. Its own state runs
                // its own clock, so without this it leaves while the other is
                // still watching "Ready" -- and the whole point of holding the
                // grid is that nobody can jump the start.
                race2.raceCountdown = raceCountdown;
                race2.goFlash = 0.0f;
                endPrompt2 = racehud::EndPrompt{};
            }
        };

        // Play-mode camera state. Declared ahead of the script bridge because
        // game.setCamera reaches `activeCam`.
        glm::vec3 playCamPos{0.0f};
        float     playCamYaw = 0.0f, playCamPitch = 0.0f, playMoveSpeed = 20.0f;
        float     playCamFov = 60.0f;
        // What game.setCameraFov asked for (0 = nothing): the free camera is put
        // back to playCamFov every frame after the scripts ran, and a script's
        // field of view has to survive that or setCameraFov does nothing.
        float     scriptCamFov = 0.0f;
        // game.setFocus: a script's depth of field (far 0 = the view's own).
        // A camera it flies 200 m above a landscape is past the default focus.
        float     scriptFocusNear = 0.0f, scriptFocusFar = 0.0f;
        // A script placed the camera this Play (game.setCameraPos): it owns the
        // eye, and the walking player leaves it alone until Play ends.
        bool      scriptOwnsEye = false;
        // A script freed the pointer (game.showCursor): a game played with the
        // mouse. Whatever locks the cursor for the walking player leaves it free.
        bool      scriptCursorFree = false;
        // An orbit camera (CameraComponent::orbitMouse) locked the cursor, and
        // unlocks it again when it stops being the view.
        bool      orbitHeldCursor = false;
        bool      playPrevEdit = false;
        int       activeCam = -1; // entity id of the active Camera in Play (-1 = player)
        // Whose race you are watching: an opponent's entity id, or -1 for your
        // own craft. V steps through the field. It changes only what the eye is
        // hung on -- the sim, the controls and the HUD stay yours, because
        // watching a rival is a camera decision and nothing else.
        int       spectateId = -1;

        // --- Lua `game` API bridge -------------------------------------------
        // Scripts mutate the entity list only through deferred queues (the tick
        // loop iterates entities), applied once per frame after scripts run.
        ScriptHost                       host;
        std::vector<Entity>              pendingSpawns;
        std::unordered_map<int, glm::vec3> pendingSpawnVel; // spawn id -> velocity
        std::vector<int>                 pendingDestroy;
        std::string                      pendingSceneLoad; // scene a SceneTrigger asked to load (deferred)
        bool                             pendingRestart = false; // overlay "Restart level" (deferred)
        bool                             pendingQuit = false;    // game.quit (deferred)
        std::unordered_map<int, unsigned char> keyPrev, mousePrev; // edge state
        std::vector<int>                 keyQ, mouseQ;              // queried this frame
        // Gameplay input, as scripts see it. An open menu overlay swallows it all
        // (the menu's own buttons are handled by the overlay, not by scripts), so
        // a paused game doesn't keep shooting while the player picks an option.
        host.keyDown      = [&](int kc){ return !uiMenuOpen && input.isKeyDown(kc); };
        host.keyPressed   = [&](int kc){ keyQ.push_back(kc);
                                         return !uiMenuOpen &&
                                                input.isKeyDown(kc) && !keyPrev[kc]; };
        // Typed text for a script's own text field (game.textInput): gathered
        // from ImGui's character queue once a frame, after its beginFrame, and
        // handed out to the scripts of the frame after -- then dropped, so a
        // line nobody asked for does not turn up in the next one that asks.
        std::string scriptTyped;
        host.textInput = [&] {
            std::string s;
            if (!uiMenuOpen) s.swap(scriptTyped);
            return s;
        };
        host.mouseDown    = [&](int b){ return !uiMenuOpen && input.isMouseButtonDown(b); };
        host.mousePressed = [&](int b){ mouseQ.push_back(b);
                                        return !uiMenuOpen &&
                                               input.isMouseButtonDown(b) && !mousePrev[b]; };
        // The pointer over the view, for a game played with the mouse. The view
        // is the rect the script's HUD is drawn into: the whole window when
        // presenting (and in the player), the Scene panel's image in the editor.
        auto scriptView = [&](glm::vec2& vmin, glm::vec2& vsize) {
            if (presentMode || viewportRectSize.x < 1.0f) {
                vmin  = glm::vec2(0.0f);
                vsize = glm::vec2(ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
            } else {
                vmin  = viewportRectMin;
                vsize = viewportRectSize;
            }
            return vsize.x >= 1.0f && vsize.y >= 1.0f;
        };
        // ImGui forgets the pointer while its window is not in focus (MousePos
        // goes invalid); presenting, the view is the whole window, and the
        // window's own cursor position says the same thing in the same units.
        auto scriptMouse = [&](ImVec2& m) -> bool {
            m = ImGui::GetIO().MousePos;
            if (ImGui::IsMousePosValid(&m)) return true;
            if (!presentMode) return false;
            const glm::vec2 c = input.mousePosition();
            m = ImVec2(c.x, c.y);
            return true;
        };
        host.mouseWheel = [&]() -> float { return playMode ? input.scrollDelta() : 0.0f; };
        host.trees = [&](glm::vec2 lo, glm::vec2 hi, std::vector<glm::vec4>& out) {
            std::vector<float> raw;
            veg.treesIn(lo, hi, raw);
            for (std::size_t k = 0; k + TreeField::kStride <= raw.size(); k += TreeField::kStride)
                out.push_back({raw[k], raw[k + 1], raw[k + 2], raw[k + 4]});
        };
        host.clearTrees = [&](float x, float z, float r) {
            veg.treeClearings.push_back({x, z, r});
        };
        host.waterAt = [&](float x, float z, float& surf) -> bool {
            if (rivers.sample(glm::vec2(x, z), surf)) return true;
            if (streamer.heightAt(x, z) < waterLevel) { surf = waterLevel; return true; }
            return false;
        };
        host.mousePos = [&, scriptView, scriptMouse](glm::vec2& out) -> bool {
            glm::vec2 vmin, vsize;
            ImVec2    m;
            if (!scriptView(vmin, vsize) || !scriptMouse(m)) return false;
            const glm::vec2 p = glm::vec2(m.x, m.y) - vmin;
            out = p * (1080.0f / vsize.y);
            return viewportHovered && !input.isCursorLocked() && p.x >= 0.0f && p.y >= 0.0f &&
                   p.x < vsize.x && p.y < vsize.y;
        };
        host.mouseRay = [&, scriptView, scriptMouse](glm::vec3& origin, glm::vec3& dir) -> bool {
            glm::vec2 vmin, vsize;
            ImVec2    m;
            if (!scriptView(vmin, vsize) || !scriptMouse(m)) return false;
            const glm::vec2 ndc(2.0f * (m.x - vmin.x) / vsize.x - 1.0f,
                                1.0f - 2.0f * (m.y - vmin.y) / vsize.y);
            const glm::mat4 inv = glm::inverse(
                camera.projectionMatrix(vsize.x / vsize.y) * camera.viewMatrix());
            glm::vec4 pn = inv * glm::vec4(ndc, -1.0f, 1.0f); pn /= pn.w;
            glm::vec4 pf = inv * glm::vec4(ndc,  1.0f, 1.0f); pf /= pf.w;
            origin = glm::vec3(pn);
            dir    = glm::normalize(glm::vec3(pf) - glm::vec3(pn));
            return true;
        };
        // The other way round: a world point on the HUD canvas -- a prompt over
        // the thing it is about. False behind the eye.
        //
        // Through the eye the last picture was drawn from (hudCamera, taken once
        // the frame's camera is settled), NOT `camera` as it stands when the
        // script asks: a script before this one may have moved that (a figure
        // controller flying its own follow eye), and the view camera or --shots
        // overrides it again before anything is drawn. A label has to sit on the
        // picture, wherever the camera went in between.
        std::optional<Camera> hudCamera; // a copy, rebuilt every frame (no assignment)
        host.worldToHud = [&, scriptView](glm::vec3 p, glm::vec2& out) -> bool {
            glm::vec2 vmin, vsize;
            if (!scriptView(vmin, vsize) || vsize.y < 1.0f) return false;
            const Camera& eye = hudCamera ? *hudCamera : camera;
            const glm::vec4 c = eye.projectionMatrix(vsize.x / vsize.y) *
                                eye.viewMatrix() * glm::vec4(p, 1.0f);
            if (c.w <= 1e-4f) return false;
            const float k = 1080.0f / vsize.y;
            out = glm::vec2((c.x / c.w * 0.5f + 0.5f) * vsize.x * k,
                            (0.5f - c.y / c.w * 0.5f) * vsize.y * k);
            return true;
        };
        host.showCursor = [&](bool on) {
            if (!playMode) return;
            scriptCursorFree = on;
            input.setCursorLocked(!on && fpsMode);
        };
        host.spawn = [&](const ScriptSpawn& s) -> int {
            Entity e;
            e.type     = static_cast<EntityType>(s.type);
            e.localCenter   = e.center   = s.pos;
            e.half     = glm::max(s.half, glm::vec3(0.02f));
            e.localRotation = e.rotation = s.rot;
            e.name     = s.name.empty() ? "spawned" : s.name;
            e.parent   = s.parent;
            // Asset-driven spawn: `model` imports (or reuses) a Model asset and
            // makes this a Model entity sized from the model's own AABB.
            if (!s.model.empty()) {
                const int mid = host.loadModel ? host.loadModel(s.model) : -1;
                const LoadedModel* lm = mid >= 0 ? models.byId(mid) : nullptr;
                if (!lm) {
                    std::fprintf(stderr, "[Fitzel] game.spawn: no model '%s'\n",
                                 s.model.c_str());
                    return 0;
                }
                const float sc = glm::max(s.scale, 0.001f);
                e.type = EntityType::Model;
                auto mc       = std::make_unique<ModelComponent>();
                mc->modelId   = mid;
                mc->modelPath = lm->path;
                mc->scale     = sc;
                e.components.items.push_back(std::move(mc));
                e.half = glm::max(modelHalf(*lm, sc), glm::vec3(0.02f));
                if (e.name == "spawned") e.name = lm->name;
            }
            if (!s.material.empty() && host.setEntityMaterial) {
                // Resolve now (the entity is not in the document yet): find the
                // GUID, attach the component by hand.
                const std::string mid = host.findMaterial ? host.findMaterial(s.material)
                                                          : std::string();
                if (!mid.empty()) {
                    auto mc = std::make_unique<MaterialComponent>();
                    mc->material = AssetId::fromString(mid);
                    e.components.items.push_back(std::move(mc));
                }
            }
            if (s.physics != 0) {
                auto pc = std::make_unique<PhysicsComponent>();
                pc->dynamic = (s.physics == 2);
                pc->mass    = s.mass;
                e.components.items.push_back(std::move(pc));
            }
            if (!s.script.empty()) {
                auto sc = std::make_unique<ScriptComponent>();
                sc->file = s.script;
                e.components.items.push_back(std::move(sc));
            }
            // A light spawned as a light shines: without the component it would
            // be a marker box and nothing else. Colour from r/g/b; game.setLight
            // tunes the rest.
            if (e.type == EntityType::Light) {
                auto lc   = std::make_unique<LightComponent>();
                lc->color = s.color;
                e.components.items.push_back(std::move(lc));
            }
            e.id       = entityCounter++;
            pendingSpawnVel[e.id] = s.vel;
            pendingSpawns.push_back(e);
            return e.id;
        };
        host.destroy = [&](int id){ pendingDestroy.push_back(id); };
        // A copy of an object and everything under it, queued like a spawn: the
        // root keeps the original's parent, the rest hang off their own copies.
        host.clone = [&](int id, const std::string& name) -> int {
            const std::vector<int> ids = scenegraph::subtree(entities, id);
            if (!document.find(id)) return 0;
            std::unordered_map<int, int> newId;
            for (int old : ids) newId[old] = entityCounter++;
            for (int old : ids) {
                const Entity* src = document.find(old);
                if (!src) continue;
                Entity e = *src;
                e.id = newId[old];
                if (old == id) { if (!name.empty()) e.name = name; }
                else           e.parent = newId[e.parent];
                pendingSpawns.push_back(std::move(e));
            }
            return newId[id];
        };
        // Instantiate a prefab by name at a world position (yaw degrees). Mirrors
        // game.spawn: the whole subtree is queued into pendingSpawns and appears
        // next frame; returns the new root entity's id (0 on failure). The prefab
        // is loaded (and its models imported) once, then cached by name.
        host.spawnPrefab = [&](const std::string& name, glm::vec3 pos,
                               float yaw) -> int {
            const prefab::Prefab* pf = findPrefab(name);
            if (!pf) return 0;
            std::vector<Entity> inst =
                prefab::instantiate(*pf, entityCounter, pos, yaw);
            if (inst.empty()) return 0;
            const int rootId = inst.front().id; // instantiate emits the root first
            for (Entity& e : inst) pendingSpawns.push_back(std::move(e));
            return rootId;
        };
        host.getPos  = [&](int id, glm::vec3& out) -> bool {
            for (const Entity& e : entities)
                if (e.id == id) { out = e.center; return true; }
            return false;
        };
        host.setPos = [&](int id, glm::vec3 p){
            for (Entity& e : entities)
                if (e.id == id) {
                    const glm::mat4 pw = parentWorldMat(e);
                    setWorld(e, p, e.rotation, e.parent >= 0 ? &pw : nullptr);
                    break;
                }
        };
        host.setVelocity = [&](int id, glm::vec3 v){
            auto it = physicsBody.find(id);
            if (physics && it != physicsBody.end()) physics->setLinearVelocity(it->second, v);
        };
        host.applyImpulse = [&](int id, glm::vec3 j, const glm::vec3* at){
            // Something that hangs swings instead (Swing.hpp): the hook on its chain.
            if (swingSys.kick(entities, id, j, at)) return;
            auto it = physicsBody.find(id);
            if (physics && it != physicsBody.end()) physics->applyImpulse(it->second, j);
        };
        // Figures a script walks (game.moveCharacter): one capsule per object in
        // the physics world, made on the first call. It starts on whatever is
        // under the object -- the first thing a ray straight down from just above
        // its middle meets -- so a figure placed on a bridge starts on the
        // bridge, not on the ground below it. The table empties with the physics
        // world, at Play start and stop.
        std::unordered_map<int, int> scriptFigures;   // entity id -> figure handle
        host.moveCharacter = [&](int id, glm::vec2 vel, float dt, glm::vec3& foot,
                                 bool& onGround, bool& onTerrain) -> bool {
            if (!physics) return false;
            const Entity* fig = nullptr;
            for (const Entity& e : entities)
                if (e.id == id) { fig = &e; break; }
            if (!fig) return false;
            auto it = scriptFigures.find(id);
            if (it == scriptFigures.end() || !physics->hasFigure(it->second)) {
                // As tall as the object, within reason, and a third of a metre
                // across: a person, whatever arms held out in a T-pose make of
                // the box.
                const float height = glm::clamp(2.0f * fig->half.y, 0.8f, 2.6f);
                const float radius = 0.3f;
                glm::vec3 base = fig->center;
                glm::vec3 hit, normal;
                PhysicsBodyId body = 0;
                if (physics->castRay(fig->center + glm::vec3(0.0f, 0.5f, 0.0f),
                                     glm::vec3(0.0f, -1.0f, 0.0f), fig->half.y + 3.5f,
                                     hit, normal, body))
                    base.y = hit.y + 0.02f;
                const int handle = physics->addFigure(radius, 0.5f * height - radius, base);
                it = scriptFigures.insert_or_assign(id, handle).first;
            }
            PhysicsWorld::FigureStep step;
            if (!physics->moveFigure(it->second, glm::vec3(vel.x, 0.0f, vel.y), dt, step))
                return false;
            foot      = step.foot;
            onGround  = step.onGround;
            onTerrain = step.onHeightField;
            host.moveTurn = step.turn;   // what a tram it rides in turned it
            return true;
        };
        host.removeCharacter = [&](int id) {
            const auto it = scriptFigures.find(id);
            if (it == scriptFigures.end()) return;
            if (physics) physics->removeFigure(it->second);
            scriptFigures.erase(it);
        };
        // Scene vehicles from a script: the figure that walks up to a car, gets
        // in, drives and gets out again (game.spawnVehicle and friends). The car
        // itself is the one V drives -- spawnSceneVehicle, the same per-frame
        // sync and camera -- only WHICH car and WHEN are the script's. There is
        // one physics car per world, so a second one is refused rather than
        // swapped: the first is still standing where it was left.
        //
        // Taking the wheel lets go of the walking player the way V does; what it
        // was is kept and given back on leaving, so a scripted figure's game
        // carries on exactly as it stood.
        bool fpsBeforeScriptDrive = false;
        host.spawnVehicle = [&](int id) -> bool {
            if (!playMode || !physics) return false;
            if (physics->hasVehicle()) return driveVehicleId == id;
            if (!spawnSceneVehicle(id)) return false;
            // Parked: it stands where it was put instead of rolling off the
            // slope it was put on.
            physics->setVehicleInput(0.0f, 0.0f, 1.0f, 1.0f);
            return true;
        };
        host.driveVehicle = [&](int id) -> bool {
            if (gliderMode || !host.spawnVehicle(id)) return false;
            if (!vehicleMode) fpsBeforeScriptDrive = fpsMode;
            vehicleMode = true;
            fpsMode     = false;
            boatMode    = false;   // every drive session starts on wheels
            physSteer   = 0.0f;
            physThrottle = 0.0f;
            input.setCursorLocked(false);
            return true;
        };
        host.leaveVehicle = [&] {
            // Nothing feeds the car once it is not driven, so the last input
            // would stay on: brake it to a stand where it was left. Even when V
            // already let go of it -- that leaves the pedal where it was.
            if (physics && physics->hasVehicle())
                physics->setVehicleInput(0.0f, 0.0f, 1.0f, 1.0f);
            if (!vehicleMode) return;
            vehicleMode = false;
            physSteer = physThrottle = 0.0f;
            fpsMode = fpsBeforeScriptDrive;
            input.setCursorLocked(fpsMode);
        };
        host.drivenVehicle = [&]() -> ScriptHost::DrivenVehicle {
            ScriptHost::DrivenVehicle v;
            if (!vehicleMode || !physics || !physics->hasVehicle() || driveVehicleId < 0)
                return v;
            v.id       = driveVehicleId;
            v.steer    = physSteer;
            v.throttle = physThrottle;
            glm::vec3 cp(0.0f), vel(0.0f);
            glm::quat cq(1.0f, 0.0f, 0.0f, 0.0f);
            physics->getTransform(physCarId, cp, cq);
            physics->getLinearVelocity(physCarId, vel);
            v.speed = glm::dot(vel, cq * glm::vec3(0.0f, 0.0f, 1.0f));
            return v;
        };
        // What the figures carry (game.attach): a pistol in a hand follows the
        // hand as the skinning pass posed it (see BoneAttach.hpp). Emptied at
        // Play start and stop, with the capsules.
        boneattach::Attachments boneAttach;
        // Limbs bent after the animation: feet onto the ground (the IK
        // component), hands where scripts send them (game.reach). LimbIK.hpp.
        ik::System limbIk;
        // Figures of the same animated model, each in its own pose (the skinning
        // pass below; SceneSubmit draws the copies).
        SkinCopies skinCopies;
        host.boneWorld = [&](int id, const std::string& bone, glm::vec3& pos,
                             glm::vec3& rotDeg) -> bool {
            glm::mat4 m;
            if (!boneAttach.boneWorld(entities, models, id, bone, m)) return false;
            glm::vec3 s;
            scenegraph::decompose(m, pos, rotDeg, s);
            return true;
        };
        host.boneNames = [&](int id) { return boneAttach.boneNames(entities, models, id); };
        host.attach = [&](int child, int figure, const std::string& bone,
                          const glm::vec3* pos, const glm::vec3* rotDeg, float blend) -> bool {
            if (!playMode) return false;
            if (!pos) return boneAttach.attach(entities, models, child, figure, bone, nullptr);
            const glm::mat4 off = scenegraph::compose(
                *pos, rotDeg ? *rotDeg : glm::vec3(0.0f), glm::vec3(1.0f));
            return boneAttach.attach(entities, models, child, figure, bone, &off, blend);
        };
        host.detach = [&](int child) { boneAttach.detach(child); };
        // Resolve a sound filename to a path. Prefer the asset database -- it holds
        // the exact absolute path of every mounted sound (the same assets the
        // picker lists), so a picked sound always resolves to the right file
        // regardless of where the project lives. Fall back to the open project's
        // content/sounds/, then the engine's bundled sounds.
        auto resolveSoundPath = [&](const std::string& n) -> std::string {
            for (const AssetId& id : assetDb.allAssets())
                if (assetDb.typeForId(id) == AssetType::Sound)
                    if (const auto* e = assetDb.entry(id))
                        if (e->absPath.filename().string() == n)
                            return e->absPath.generic_string();
            if (!currentProject.empty()) {
                const std::string projSnd =
                    (std::filesystem::path(currentProject).parent_path() /
                     "content" / "sounds" / n).generic_string();
                if (fitzel::vfs::exists(projSnd)) return projSnd;
            }
            return soundDir + "/" + n;
        };
        host.playSound = [&](const std::string& n){
            audio.playOneShot(resolveSoundPath(n));
            mix.sfx.hit(mix.sfxGain());   // a one-shot the mixer's meter can see
        };
        // game.sound: pooled voices with volume, pitch and a place (ScriptSfx).
        // Names are resolved once: resolveSoundPath walks the whole asset
        // database, and a battle asks for the same dozen files every frame.
        ScriptSfx scriptSfx(audio);
        std::unordered_map<std::string, std::string> scriptSfxPaths;
        host.playSoundEx = [&](const std::string& n, float vol, float pitch, const glm::vec3* pos,
                               float nearM, float farM) {
            auto it = scriptSfxPaths.find(n);
            if (it == scriptSfxPaths.end()) it = scriptSfxPaths.emplace(n, resolveSoundPath(n)).first;
            scriptSfx.play(it->second, vol * mix.masterGain() * mix.sfxGain(), pitch, pos, nearM, farM);
            mix.sfx.hit(mix.sfxGain() * glm::clamp(vol, 0.0f, 1.0f));
        };
        // One-shot SFX voices, cached by sound file: boost punches, the Ready/Set/Go
        // samples, checkpoint gates. CRUCIAL: each file is loaded once and only
        // re-played (seek+start) -- never re-created while it may still be sounding.
        // Re-assigning a live Sound uninits its miniaudio instance out from under the
        // audio thread, which corrupted the mixer graph (the sound cut out, then the
        // app crashed). Same load-once/replay pattern as the ambience.
        std::unordered_map<std::string, Sound> cueVoices;
        // Fire one of those cues at a given gain/pitch. Every race SFX goes through
        // here, so they all share the cache and the master volume/mute.
        auto playCue = [&](const std::string& file, float gain, float pitch){
            if (file.empty()) return;
            auto it = cueVoices.find(file);
            if (it == cueVoices.end())
                it = cueVoices.emplace(file,
                        Sound::fromFile(audio, resolveSoundPath(file), false)).first;
            Sound& voice = it->second;
            if (!voice.isValid()) return;
            voice.setVolume(mix.masterGain() * glm::clamp(gain, 0.0f, 2.0f));
            voice.setPitch(glm::clamp(pitch, 0.2f, 3.0f));
            voice.play(); // seek-to-0 + start: safe to retrigger a live voice
        };
        // A boost pad's punch at its own gain/pitch (tunable + auditionable per pad);
        // a low pitch gives the deep thump. Shared by the glider's pad-entry (below)
        // and the Inspector's Preview button.
        auto playBoostPunch = [&](const BoostPadComponent& bp){
            playCue(bp.sound, bp.soundGain, bp.soundPitch);
        };
        // The missiles' two hooks into the app: the same cue voice pool as every
        // other race SFX, and the glider's own ground query -- so a missile that
        // misses ploughs into the same surface the craft flies over, bridges and
        // placed blocks included.
        weapons.playCue = playCue;
        weapons.groundHeight = [&](float x, float z, float yMax) {
            // A missile is nobody's craft: nothing is exempt from the ground it
            // can hit, the launching craft included.
            return gliderGround(x, z, yMax, -1);
        };
        weapons2.playCue      = weapons.playCue;
        weapons2.groundHeight = weapons.groundHeight;
        // Looping ambient voices for TriggerSound zones (entity id -> Sound),
        // created lazily in Play and cleared on stop. Sound is move-only.
        std::unordered_map<int, Sound> zoneSounds;
        // AudioSource voices (entity id -> Sound): music/ambient loops or one-shots,
        // started by playOnStart or by game.playAudio from a script, freed on stop.
        std::unordered_map<int, Sound> audioVoices;
        auto startAudioSource = [&](int id) {
            Entity* e = document.find(id);
            auto*   a = e ? e->components.get<AudioSourceComponent>() : nullptr;
            if (!a || a->sound.empty()) return;
            const std::string path = resolveSoundPath(a->sound);
            Sound& v = audioVoices[id];
            // Rebuild the voice each start (a one-shot voice can't be relooped),
            // then play it.
            v = Sound::fromFile(audio, path, a->loop);
            if (!v.isValid()) {
                std::fprintf(stderr, "[AudioSource] could not load '%s' (resolved '%s')\n",
                             a->sound.c_str(), path.c_str());
                return;
            }
            v.setVolume(a->volume * mix.ambientGain());
            v.play();
        };
        auto stopAudioSource = [&](int id) {
            auto it = audioVoices.find(id);
            if (it != audioVoices.end() && it->second.isValid()) it->second.stop();
        };
        host.playAudio = [&](int id){ startAudioSource(id); };
        host.synths    = &synths;
        musicSys.bind(audio, resolveSoundPath);
        host.music     = &musicSys;
        host.stopAudio = [&](int id){ stopAudioSource(id); };
        host.getVelocity = [&](int id, glm::vec3& out) -> bool {
            auto it = physicsBody.find(id);
            return physics && it != physicsBody.end() &&
                   physics->getLinearVelocity(it->second, out);
        };
        host.setAngularVelocity = [&](int id, glm::vec3 w){
            auto it = physicsBody.find(id);
            if (physics && it != physicsBody.end())
                physics->setAngularVelocity(it->second, w);
        };
        // Camera control from a script. Position/direction drive the player view
        // (a Camera entity, if one is active, overwrites it again at frame end --
        // game.setCamera(-1) hands control back to the script).
        host.setCamPos = [&](glm::vec3 p){
            camera.setPosition(p);
            if (playMode) scriptOwnsEye = true;
        };
        host.setCamDir = [&](glm::vec3 d){
            if (glm::length(d) < 1e-5f) return;
            d = glm::normalize(d);
            camera.setYaw(glm::degrees(std::atan2(d.z, d.x)));
            camera.setPitch(glm::degrees(std::asin(glm::clamp(d.y, -1.0f, 1.0f))));
        };
        host.getTimeOfDay = [&] { return timeOfDay; };
        host.setTimeOfDay = [&](float h) {
            if (playClockBackup < 0.0f) playClockBackup = timeOfDay;
            timeOfDay = std::fmod(std::fmod(h, 24.0f) + 24.0f, 24.0f);
        };
        host.getDayLength = [&] {
            if (scriptDayLength >= 0.0f) return scriptDayLength > 0.1f ? scriptDayLength : 0.0f;
            const bool runs = (!timePaused || (playMode && timeFlows)) && dayLength > 0.1f;
            return runs ? dayLength : 0.0f;
        };
        host.setDayLength = [&](float s) {
            if (playClockBackup < 0.0f) playClockBackup = timeOfDay;
            scriptDayLength = s < 0.0f ? -1.0f : s;
        };
        host.setStreetLamps = [&](int m) { streetLampMode = glm::clamp(m, -1, 1); };
        host.streetLamps = [&](bool& lit) {
            lit = streetLampsLit;
            return streetLampMode;
        };
        host.setCamFov = [&](float f){
            scriptCamFov = glm::clamp(f, 10.0f, 140.0f);
            camera.setFov(scriptCamFov);
        };
        host.setFocus = [&](float nearM, float farM) {
            scriptFocusFar  = farM > 0.0f ? std::max(farM, nearM + 1.0f) : 0.0f;
            scriptFocusNear = std::max(0.0f, nearM);
        };
        // HUD text sizes for a script's layout, in its 1080-high canvas units --
        // the unit the draw calls take, whatever the view's pixel size.
        host.measureText = [](const std::string& s, float size, bool bold) {
            ImFont* f = (bold && ui::boldFont()) ? ui::boldFont() : ImGui::GetFont();
            if (!f) return glm::vec2(0.0f);
            const ImVec2 sz = f->CalcTextSizeA(size, FLT_MAX, 0.0f, s.c_str());
            return glm::vec2(sz.x, sz.y);
        };
        host.setActiveCamera = [&](int id){ activeCam = id; };
        // Driving an object's state machine from Lua. Each one resolves the
        // entity and its graph fresh: a script may name an object that has no
        // graph (or none at all), and that has to be a no-op rather than a
        // reason for the whole script to stop.
        {
            auto machineOf = [&](int id, animgraph::Graph const** g)
                -> animgraph::Instance* {
                Entity* e = document.find(id);
                if (!e) return nullptr;
                auto* ag = e->components.get<AnimGraphComponent>();
                if (!ag) return nullptr;
                const int gi = animgraph::findGraph(animGraphs, ag->graph);
                if (gi < 0) return nullptr;
                *g = &animGraphs[gi];
                return &ag->runtime;
            };
            host.animTrigger = [&, machineOf](int id, const std::string& p) {
                const animgraph::Graph* g = nullptr;
                if (auto* in = machineOf(id, &g)) animgraph::fire(*g, *in, p);
            };
            host.animSetBool = [&, machineOf](int id, const std::string& p, bool v) {
                const animgraph::Graph* g = nullptr;
                if (auto* in = machineOf(id, &g)) animgraph::setBool(*g, *in, p, v);
            };
            host.animSetNumber = [&, machineOf](int id, const std::string& p, float v) {
                const animgraph::Graph* g = nullptr;
                if (auto* in = machineOf(id, &g)) animgraph::setNumber(*g, *in, p, v);
            };
            host.animState = [&, machineOf](int id) -> std::string {
                const animgraph::Graph* g = nullptr;
                animgraph::Instance* in = machineOf(id, &g);
                if (!in || !g || in->state < 0 ||
                    in->state >= static_cast<int>(g->states.size())) return {};
                return g->states[static_cast<std::size_t>(in->state)].name;
            };
        }
        // The data-driven half of the API (assets, models, materials, entity
        // queries, world helpers) lives in ScriptBridge; it only needs the few
        // hooks below that depend on main's own state.
        scriptbridge::install(host, [&]{
            scriptbridge::Deps d;
            d.doc     = &document;
            d.models  = &models;
            d.assetDb = &assetDb;
            d.setWorldRot = [&](int id, glm::vec3 r){
                if (Entity* e = document.find(id)) {
                    const glm::mat4 pw = parentWorldMat(*e);
                    setWorld(*e, e->center, r, e->parent >= 0 ? &pw : nullptr);
                }
            };
            d.reparent = [&](int id, int parent){
                Entity* e = document.find(id);
                if (!e || id == parent) return;
                // Refuse a cycle: the new parent must not be a descendant of `e`.
                for (int p = parent; p >= 0;) {
                    if (p == id) return;
                    const Entity* pe = document.find(p);
                    p = pe ? pe->parent : -1;
                }
                e->parent = (parent >= 0 && document.find(parent)) ? parent : -1;
                const glm::mat4 pw = parentWorldMat(*e);
                rebaseLocal(*e, e->parent >= 0 ? &pw : nullptr);
            };
            d.terrainHeight = [&](float x, float z){ return streamer.heightAt(x, z); };
            d.loadScene     = [&](const std::string& n){ pendingSceneLoad = n; };
            d.log = [](const std::string& line){
                std::fprintf(stderr, "[Lua] %s\n", line.c_str());
            };
            return d;
        }());
        scripts.setHost(&host);

        // The ground, for the multishot camera's clearance check. Handed over
        // once: a shot that ducks to wheel height is the one worth having, and it
        // is the one that ends up inside a hill without this.
        cams.setGround([&](float x, float z) { return streamer.heightAt(x, z); });

        // Road side objects reference a model by name/path/GUID; resolve (and
        // import) each once, then cache the library id. Failures aren't cached, so
        // a model imported later resolves on a subsequent frame. Reuses the same
        // asset resolution the Lua API uses (host.loadModel).
        std::unordered_map<std::string, int> sideModelCache;
        auto resolveSideModel = [&](const std::string& ref) -> LoadedModel* {
            if (ref.empty()) return nullptr;
            if (auto it = sideModelCache.find(ref); it != sideModelCache.end())
                return models.byId(it->second);
            const int mid = host.loadModel ? host.loadModel(ref) : -1;
            if (mid < 0) return nullptr;
            sideModelCache[ref] = mid;
            return models.byId(mid);
        };
        // A town standing an imported model on a block asks for its bounds here.
        towns.modelBounds = [&](const std::string& ref, glm::vec3& lo, glm::vec3& hi) {
            const LoadedModel* lm = resolveSideModel(ref);
            if (!lm) return false;
            lo = lm->boundsMin;
            hi = lm->boundsMax;
            return true;
        };

        // Terrain physics collider: a static heightfield around the action. It is
        // finite, so it follows the player/vehicle -- when the focus drifts more
        // than a quarter-span from the field centre it is rebuilt around the focus,
        // so driving far never runs off the collision. (~768 m span at 4 m samples.)
        PhysicsBodyId terrainCollId = 0;
        glm::vec2     terrainCollCenter{0.0f};
        const int     kThfN  = 192;   // heightfield resolution (even)
        const float   kThfSp = 4.0f;  // metres per sample
        auto refitTerrainCollision = [&](glm::vec2 centerXZ) {
            if (!physics) return;
            const float ox = centerXZ.x - (kThfN * 0.5f) * kThfSp;
            const float oz = centerXZ.y - (kThfN * 0.5f) * kThfSp;
            std::vector<float> heights(static_cast<std::size_t>(kThfN) * kThfN);
            for (int z = 0; z < kThfN; ++z)
                for (int x = 0; x < kThfN; ++x)
                    heights[z * kThfN + x] =
                        streamer.heightAt(ox + x * kThfSp, oz + z * kThfSp);
            if (terrainCollId) physics->removeBody(terrainCollId);
            terrainCollId = physics->addHeightField(
                heights.data(), kThfN, glm::vec3(ox, 0.0f, oz), kThfSp);
            terrainCollCenter = centerXZ;
        };

        // game.groundHeight: a ray straight down through the physics world. The
        // terrain's collider is far coarser than the terrain is drawn (a 4 m
        // grid), so it does not count as an answer: the ray goes on past it to
        // whatever lies on the ground there -- the road a pistol is dropped on
        // -- and the result is never below the DRAWN terrain.
        host.groundHeight = [&](glm::vec3 from, float maxDist, float& outY) -> bool {
            const float drawn = (terrainOn && host.terrainHeight)
                                    ? host.terrainHeight(from.x, from.z) : -1.0e30f;
            glm::vec3 o = from;
            if (physics) {
                for (int i = 0; i < 4; ++i) {
                    glm::vec3 hit, normal;
                    PhysicsBodyId body = 0;
                    const float left = maxDist - (from.y - o.y);
                    if (left <= 0.0f ||
                        !physics->castRay(o, glm::vec3(0.0f, -1.0f, 0.0f), left, hit, normal, body))
                        break;
                    if (hit.y < drawn - 0.05f) break;   // below the ground: nothing on it
                    if (body != terrainCollId) {
                        outY = std::max(hit.y, drawn);
                        return true;
                    }
                    o = hit - glm::vec3(0.0f, 0.01f, 0.0f);
                }
            }
            if (drawn < -1.0e29f || from.y - drawn > maxDist) return false;
            outY = drawn;
            return true;
        };
        // The towns as somewhere to go (TownNav.hpp): the pavements joined at
        // their crossings and every named place, derived anew the first time a
        // script asks after a town was.
        struct TownNavCache {
            int                             revision = -1;
            townnav::Nav                    nav;
            std::vector<cityplan::RoadLine> lines;
            std::vector<townnav::Place>     places;
        } townNav;
        auto freshTownNav = [&]() -> const TownNavCache& {
            if (townNav.revision == towns.revision()) return townNav;
            townNav.revision = towns.revision();
            townNav.lines = towns.roadLines ? towns.roadLines() : std::vector<cityplan::RoadLine>{};
            const auto& built = towns.built();
            std::vector<std::vector<glm::vec3>> walks;
            for (const auto& b : built)
                walks.insert(walks.end(), b.town.walks.begin(), b.town.walks.end());
            townNav.nav.build(walks, townNav.lines);
            townNav.places.clear();
            for (int i = 0; i < towns.count() && i < static_cast<int>(built.size()); ++i) {
                std::vector<townnav::Place> p = townnav::places(
                    i, towns.towns[static_cast<std::size_t>(i)],
                    built[static_cast<std::size_t>(i)].town, townNav.lines, townNav.nav);
                townNav.places.insert(townNav.places.end(), p.begin(), p.end());
            }
            std::fprintf(stderr, "[Fitzel] town paths: %d points, %d crossings, %zu places\n",
                         townNav.nav.nodeCount(), townNav.nav.crossingCount(),
                         townNav.places.size());
            return townNav;
        };
        host.townPlaces = [&] {
            std::vector<ScriptPlace> out;
            for (const townnav::Place& p : freshTownNav().places)
                out.push_back({p.name, p.kind, p.street, p.number, p.pos, p.at, p.town});
            return out;
        };
        host.townPath = [&](glm::vec2 from, glm::vec2 to) {
            return freshTownNav().nav.path(from, to);
        };
        host.streetAt = [&](glm::vec2 p, float maxDist, float& dist) {
            return townnav::streetAt(freshTownNav().lines, p, maxDist, &dist);
        };
        // game.castRay: what a shot meets. The bodies of the physics world first
        // -- passing through the terrain's coarse collider, for the same reason
        // as groundHeight -- then the terrain as it is drawn, marched in half-
        // metre steps and halved down to a centimetre. Whichever is nearer wins.
        host.castRay = [&](glm::vec3 o, glm::vec3 d, float maxDist,
                           ScriptRayHit& out) -> bool {
            const float len = glm::length(d);
            if (len < 1e-6f || maxDist <= 0.0f) return false;
            d /= len;
            float best = maxDist;
            bool  hit  = false;
            if (physics) {
                glm::vec3 from = o;
                float     gone = 0.0f;
                for (int i = 0; i < 8 && gone < best; ++i) {
                    glm::vec3 hp, n;
                    PhysicsBodyId body = 0;
                    if (!physics->castRay(from, d, best - gone, hp, n, body)) break;
                    const float t = gone + glm::length(hp - from);
                    if (body == terrainCollId) {   // the coarse ground: look past it
                        gone = t + 0.02f;
                        from = o + d * gone;
                        continue;
                    }
                    int who = -1;
                    for (const auto& [eid, bid] : physicsBody)
                        if (bid == body) { who = eid; break; }
                    // A pane shot out (Shatter.hpp): its collider is still there,
                    // the glass is not -- look past it, like the coarse ground.
                    if (shatterSys.gone(who, hp)) {
                        gone = t + 0.005f;
                        from = o + d * gone;
                        continue;
                    }
                    best = t;
                    out.pos = hp;
                    out.normal = n;
                    out.id = who;
                    hit = true;
                    break;
                }
            }
            if (terrainOn && host.terrainHeight) {
                const auto above = [&](float t) {
                    const glm::vec3 p = o + d * t;
                    return p.y - host.terrainHeight(p.x, p.z);
                };
                if (above(0.0f) > 0.0f) {
                    float prev = 0.0f;
                    for (float t = 0.5f;; t += 0.5f) {
                        const float tt = std::min(t, best);
                        if (above(tt) <= 0.0f) {
                            float lo = prev, hi = tt;
                            for (int k = 0; k < 12; ++k) {
                                const float m = 0.5f * (lo + hi);
                                (above(m) > 0.0f ? lo : hi) = m;
                            }
                            const glm::vec3 p = o + d * hi;
                            const float e = 0.25f;
                            const float hx = host.terrainHeight(p.x - e, p.z) -
                                             host.terrainHeight(p.x + e, p.z);
                            const float hz = host.terrainHeight(p.x, p.z - e) -
                                             host.terrainHeight(p.x, p.z + e);
                            best = hi;
                            out.pos = p;
                            out.normal = glm::normalize(glm::vec3(hx, 2.0f * e, hz));
                            out.id = -1;
                            hit = true;
                            break;
                        }
                        prev = tt;
                        if (tt >= best) break;
                    }
                }
            }
            if (!hit) return false;
            out.dist = best;
            return true;
        };
        host.orbitFrame = [&](float w, float dist, float side, float up, float fov) {
            camerasys::CameraSystem::OrbitFrame f;
            f.weight = w; f.dist = dist; f.side = side; f.up = up; f.fov = fov;
            cams.frameOrbit(f);
        };
        host.emit = [&](int id) { particles.restart(id); };
        host.reach = [&](int id, int side, glm::vec3 target, float weight) {
            limbIk.reach(id, side, target, weight);
        };
        // game.decal: a bullet hole, a splat (Decals.hpp). Its material is named
        // from the library -- or, with no name, the engine's own bullet hole, put
        // into the library the first time it is wanted (the library is put back
        // as it was when Play stops, so it never reaches a file).
        host.decal = [&](glm::vec3 p, glm::vec3 n, float size, const std::string& name, float spin) {
            const std::string want = name.empty() ? std::string("Bullet hole (engine)") : name;
            fitzel::AssetId mat;
            for (const MaterialDef& md : materials)
                if (md.name == want) { mat = md.assetId; break; }
            if (!mat.valid() && name.empty()) {
                MaterialDef md;
                md.assetId      = fitzel::AssetId::generate();
                md.name         = want;
                md.tex          = decals::bulletHoleTexture();
                md.alphaMode    = AlphaMode::Blend;
                md.reflectivity = 0.0f;
                md.roughness    = 0.9f;
                mat = md.assetId;
                materials.push_back(std::move(md));
            }
            if (!mat.valid()) return false;
            const decals::HeightFn ground =
                (terrainOn && host.terrainHeight)
                    ? decals::HeightFn([&](float x, float z) { return host.terrainHeight(x, z); })
                    : decals::HeightFn{};
            return decalSys.spawn(mat, p, n, size, spin, entities, models, ground);
        };
        // game.shatter: the glass a shot struck breaks (Shatter.hpp). A sheet of
        // glass that went as a whole takes its collider with it; a model's pane
        // keeps it, and castRay looks past the hole (above).
        host.shatter = [&](int id, glm::vec3 p, glm::vec3 d, float strength) {
            int whole = -1;
            if (!shatterSys.breakAt(entities, models, materials, document, id, p, d, strength, whole))
                return false;
            if (whole >= 0) {
                auto it = physicsBody.find(whole);
                if (physics && it != physicsBody.end()) {
                    physics->removeBody(it->second);
                    physicsBody.erase(it);
                }
            }
            return true;
        };
        // game.restart: the overlay's Restart, asked for by a script (a figure
        // that died). Deferred like the button, for the same reason.
        host.restart = [&] { pendingRestart = true; };
        // game.quit: a menu's "Exit". Deferred too: the script is mid-tick.
        host.quit = [&] { pendingQuit = true; };

        // The craft the PLAYER flies, when the scene holds none.
        //
        // A circuit does not have to contain a glider: the one being flown
        // normally arrives from the start screen, which is exactly what a forced
        // "Play as: flying the glider" skips. So the grid answers instead -- the
        // marker that reserves the player's slot names the craft that stands on
        // it, and this builds that one, there.
        //
        // A craft already in the scene WINS. An author who parked one on the grid
        // meant it, and quietly building a second beside it would be two craft on
        // one slot and a race against yourself.
        //
        // Play only, like everything else that puts objects into the scene: what
        // takes them out again is Play's snapshot being restored on Stop.
        auto ensurePlayerCraft = [&]() -> int {
            if (!playMode) return -1;
            for (const Entity& e : entities)
                if (e.activeInHierarchy && e.components.get<GliderComponent>() &&
                    !e.components.get<OpponentComponent>())
                    return e.id;    // already something to fly
            const int mk = racegrid::playerMarker(entities);
            const std::string name = racegrid::playerMarkerPrefab(entities);
            if (mk < 0 || name.empty()) return -1;
            const Entity* m = document.find(mk);
            if (!m) return -1;
            const glm::vec3 at = m->center;
            const prefab::Prefab* pf = findPrefab(name);
            if (!pf) {
                host.hud = "The player's grid position names a prefab that is not "
                           "there: \"" + name + "\".";
                return -1;
            }
            // Yaw 0: the line-up that follows turns it to the marker's own facing
            // and lifts it to its ride height, and it knows about a model whose
            // nose is the other way round. Turning it here as well would be the
            // same rotation applied twice.
            std::vector<Entity> inst = prefab::instantiate(*pf, entityCounter, at, 0.0f);
            if (inst.empty()) return -1;
            const int rootId = inst.front().id;
            for (Entity& ne : inst) entities.push_back(std::move(ne));
            // Deliberately WITHOUT an Opponent component: this one is flown, not
            // raced by the AI, and populate() counts entered opponents to decide
            // how many rivals to build.
            resolveHierarchy();
            return rootId;
        };

        auto startPlay = [&] {
            if (playMode) return;
            // Play runs the SCENE, and the scene is stashed. Refusing beats the
            // alternative, which is a game made of one prefab and no way back.
            if (prefabEditBusy("Play")) return;
            endEditorDrive(); // a test-drive must not leak into the Play backup
            endGliderDrive(); // ...nor a test-flight
            playMode      = true;
            playEntities  = entities;
            playMaterials = materials;
            playCamPos    = camera.position();
            playCamYaw    = camera.yaw();
            playCamPitch  = camera.pitch();
            playMoveSpeed = camera.moveSpeed;
            playCamFov    = camera.fov();
            playPrevEdit  = entityEditMode;
            entityEditMode = false;
            placeMode      = false;
            sel.clear();
            vehicleMode    = false;
            gliderMode     = false;
            // Fresh race: no laps timed until the glider crosses the start line.
            raceActive = raceFinished = false;
            raceClock = lapClock = lastLap = bestLap = 0.0f;
            raceLap = raceLaps = 0; finishWasOver = false; finishArm = 0.0f;
            race.lapBegun = true;  // no countdown yet: a crossing starts the race
            cpPassed.clear(); cpTotal = 0; raceMissedFlash = 0.0f;
            raceCountdown = goFlash = 0.0f;
            // ...and off the grid. Play arms the hold itself when a launch
            // arrives; inheriting a held one from the last race would open with a
            // craft nobody can fly and a prompt for a race nobody started.
            race.onGrid = false; race.gridTime = 0.0f;
            // ...and an empty field: standings/winner are rebuilt from GO.
            race.standings.clear(); race.winnerName.clear();
            race.winnerIsPlayer = false; race.winnerTime = 0.0f;
            race.playerPlace = 0; race.raceOver = false; race.oppWasActive = false;
            // ...and a full hull: Play always starts on a craft that can still fly
            // (the tank itself is re-read from the glider when one is taken).
            race.energy = race.energyCapacity;
            race.energyOut = false; race.energyLow = false;
            race.energyIdle = 0.0f; race.energyHitFlash = 0.0f;
            race.energyLastHit = 0.0f; race.energyWarnT = 0.0f;
            endPrompt = racehud::EndPrompt{}; // no stale end-of-race question
            // The scene's opening camera move, if it has one. Before the active
            // camera is picked below and not instead of it: a path drives the
            // PLAYER's view, so a scene that also marks a camera active-on-start
            // gets that camera the moment the move ends or is skipped.
            camPathRec.beginAutoPlay();
            // The scene's keyframe animation, on the same terms as the camera
            // move. A preview running in the Timeline is ended first (its restore
            // hands the authored values back) so the game starts from the scene
            // as saved, not from wherever the editor's playhead was parked --
            // and so the two of them are never both writing the same property.
            if (animPlay.preview) anim::endPreview(animEdited(), entities, animPlay);
            animPlay.time    = 0.0f;
            animPlay.playing = false;
            // Every Animator that says so starts its clip, plus any clip marked
            // Play on start in the Timeline -- the shortcut for a scene with one
            // animation and no object worth hanging a component on. A clip named
            // by two Animators runs twice, which is harmless: both playbacks
            // write the same values at the same times.
            animRuntime.clear();
            for (const Entity& e : entities) {
                const auto* an = e.components.get<AnimatorComponent>();
                if (!an || !an->playOnStart || an->clip.empty()) continue;
                const int ci = anim::findClip(animClips, an->clip);
                if (ci < 0) continue;          // names a clip this scene lost
                anim::Playback pb;
                pb.clip = ci;
                pb.player.playing = true;
                animRuntime.push_back(pb);
            }
            for (int ci = 0; ci < static_cast<int>(animClips.size()); ++ci) {
                if (!animClips[ci].playOnStart || animClips[ci].empty()) continue;
                anim::Playback pb;
                pb.clip = ci;
                pb.player.playing = true;
                animRuntime.push_back(pb);
            }
            // Every state machine starts in its entry state with its parameters
            // at their defaults. Here rather than on load: a machine holds where
            // the GAME got to, and Play is when the game begins.
            for (Entity& e : entities)
                if (auto* ag = e.components.get<AnimGraphComponent>()) {
                    const int gi = animgraph::findGraph(animGraphs, ag->graph);
                    if (gi >= 0) animgraph::start(animGraphs[gi], ag->runtime);
                }
            // Start from the camera marked active-on-start, else the player view.
            activeCam = -1;
            for (const Entity& e : entities)
                if (const auto* cc = e.components.get<CameraComponent>();
                    cc && cc->activeOnStart) { activeCam = e.id; break; }
            // Re-init animations so autostart/range apply fresh at Play start
            // (instead of continuing from the editor preview position).
            for (Entity& e : entities)
                if (auto* ac = e.components.get<AnimationComponent>()) {
                    ac->started = false; ac->restart = false;
                }
            scripts.reset(); // fresh VM: scripts reload, start() runs again
            host.score = 0;
            host.hud.clear();
            host.hudCmds.clear();
            host.crosshair = true;
            scriptCamFov   = 0.0f;
            scriptFocusFar = 0.0f;
            scriptOwnsEye  = false;
            scriptCursorFree = false;
            // game.saveData keeps a game's saves under its project's name.
            host.saveGame = currentProject.empty()
                ? std::string()
                : std::filesystem::path(currentProject).parent_path().filename().string();
            host.scriptsDir = scriptsDir();
            pendingSpawns.clear();
            pendingSpawnVel.clear();
            pendingDestroy.clear();
            keyPrev.clear(); mousePrev.clear();
            // A menu overlay starts closed, a HUD starts shown (Play always
            // begins in the game, never in the scene's menu).
            uiOverlay.resetRuntime();
            uiMenuOpen = false;
            prevUiKey  = true; // the key that started Play must be released first
            resolveHierarchy(); // world transforms fresh before bodies are created

            // --- What the game starts as, and where ---------------------------
            // Both answered HERE, before the ground exists, because the ground is
            // built around ONE point (see refitTerrainCollision): a finite patch,
            // 768 m across. Deciding where the player stands after the ground was
            // poured puts a PlayerStart further away than half a span over the
            // void -- and the recentre in the physics step cannot rescue it,
            // because falling straight down never moves the focus in XZ.
            //
            // Read from the project's game.json rather than from the copy the
            // dialog edits, for the same reason the loading screen is re-read on
            // every level change: it is one small file, and reading it here means
            // a change in the dialog is in effect on the very next Play instead
            // of after a restart.
            game::StartMode startAs   = game::StartMode::Fps;
            bool            startAsSet = false;
            if (!currentProject.empty()) {
                const game::Settings gs =
                    game::load(std::filesystem::path(currentProject)
                                   .parent_path().generic_string());
                startAs    = gs.startMode;
                startAsSet = gs.startModeSet;
            }
            // A scene from before the setting moved (see legacyStartVehicle).
            // Only while the game says nothing at all -- asking whether the mode
            // is Fps would not be that question: "on foot" is the default value,
            // so a game deliberately set to it would be overruled by its own
            // scene's leftover flag and the dialog could never say "on foot".
            if (!startAsSet) {
                if (legacyStartVehicle)     startAs = game::StartMode::Vehicle;
                else if (legacyStartGlider) startAs = game::StartMode::Glider;
            }
            // ...and the SCENE's own answer beats both, because it is the most
            // specific one there is: game.json says what the game opens as, this
            // says what this level is. A scene that says nothing (-1) leaves the
            // game's setting standing.
            if (sceneStartMode >= 0) {
                startAs    = static_cast<game::StartMode>(sceneStartMode);
                startAsSet = true;
            }
            // Where the walking player stands: the first entity carrying a
            // PlayerStart component (adopting its facing and move speed),
            // otherwise the edit camera.
            glm::vec3 startPos     = camera.position();
            bool      havePlayerStart = false;
            for (const Entity& e : entities)
                if (const auto* ps = e.components.get<PlayerStartComponent>()) {
                    startPos         = e.center;
                    havePlayerStart  = true;
                    camera.setYaw(e.rotation.y);
                    camera.moveSpeed = ps->moveSpeed;
                    break;
                }
            // ...and say so when there is none. Falling back to the edit camera is
            // the right behaviour and the wrong silence: from inside the game the
            // two are indistinguishable, so a scene where the marker was never
            // finished looks exactly like a setting that does not work. The marker
            // is the COMPONENT -- an entity merely NAMED "playerStart" is a box
            // with a name, which is the easy mistake this line exists to catch.
            if (startAs == game::StartMode::Fps && !havePlayerStart)
                host.hud = "No Player Start in this scene -- spawning at the "
                           "editor camera. Select the marker and use "
                           "Add Component > Player Start.";

            // Physics: fresh world with the terrain as a static heightfield
            // ground, plus a rigid body per physics-tagged entity.
            physics = std::make_unique<PhysicsWorld>();
            scriptFigures.clear();   // their capsules were in the old world
            trams.dropBodies();      // and so were the trams' cars
            scriptDayLength = -1.0f; // the day as the scene has it, until a script says
            playClockBackup = -1.0f;
            streetLampMode  = -1;
            boneAttach.clear();      // and nothing is carried yet
            decalSys.clearThrown();  // no holes yet
            shatterSys.clear();      // and no glass broken
            physics->setGravity(glm::vec3(0.0f, -9.81f, 0.0f));
            // Fresh world: the previous collider id is void. Build the terrain
            // heightfield around wherever the game opens -- the PlayerStart when
            // the player walks out of it, the camera otherwise (a vehicle or a
            // watching camera is placed relative to the view, not to a marker the
            // scene may also carry) -- and it follows the focus from there.
            terrainCollId = 0;
            const glm::vec2 groundCenter =
                (startAs == game::StartMode::Fps && havePlayerStart)
                    ? glm::vec2(startPos.x, startPos.z)
                    : glm::vec2(camera.position().x, camera.position().z);
            refitTerrainCollision(groundCenter);
            // The static world: roads with their side objects and cities, the
            // towns' buildings, and the splines' walls (see PlayWorld.hpp).
            playworld::addStaticWorld(*physics, roads, towns, splines, resolveSideModel,
                                      sidePosts);

            skids.clear(); // no skid marks carry over from a previous Play session
            trails.clear(); // ...nor stale contrails
            particles.clear(); // ...nor a cloud of smoke from the last run
            weapons.reset();   // ...nor a missile still in the air, or a lock
            weapons2.reset();  // ...for either seat
            // Player two starts from scratch too, and gets re-seated: the craft
            // it flew belongs to the scene that is being restarted.
            race2 = racesim::RaceState{};
            driveGliderId2 = -1;
            playworld::addEntityBodies(*physics, entities, models, physicsBody);
            swingSys.begin(entities, *physics, physicsBody);   // what hangs, and its moving boxes
            // Jelly, balloons and cloth. After the loop above and after the world's
            // static geometry, so a soft body lands ON the ground rather than being
            // squeezed out of it on its first step.
            softBodies.spawn(entities, *physics);
            softWindTime = 0.0f;
            // Scene objects with a Traffic driver join the towns' traffic.
            townTraffic.beginPlay(entities, physics.get());
            // The player is a physics capsule (~1.8 m tall), standing on the
            // ground that was built around startPos above.
            //
            // Which height that is, is not simply the terrain's. The marker's own
            // Y counts whenever it is ABOVE the ground: a PlayerStart on a roof, a
            // platform or a bridge is a decision, and in a scene with no terrain at
            // all the marker is the only answer there is (heightAt says 0 there,
            // which is how a terrainless scene used to swallow the setting whole).
            // Below the ground the ground wins -- a marker nudged into the hillside
            // is a placement slip, not a request to spawn inside the rock. Without
            // a marker there is no Y worth trusting: startPos is then the edit
            // camera, which is usually in the air.
            const float ground = streamer.heightAt(startPos.x, startPos.z);
            const float feetY  = havePlayerStart ? std::max(startPos.y, ground)
                                                 : ground;
            physics->spawnCharacter(0.3f, 0.6f,
                glm::vec3(startPos.x, feetY, startPos.z));

            fpsMode        = true; // play as the walking player
            input.setCursorLocked(true);
            fpsVelY = 0.0f;
            camera.setPosition({startPos.x, feetY + eyeHeight, startPos.z});

            // Auto-start every AudioSource flagged play-on-start (music/ambient),
            // unless the object (or an ancestor) is deactivated.
            for (const Entity& e : entities)
                if (const auto* a = e.components.get<AudioSourceComponent>();
                    a && a->playOnStart && e.activeInHierarchy)
                    startAudioSource(e.id);
            // ...and every Synth: an editor preview stops, the game's music starts.
            synths.clear();
            for (const Entity& e : entities)
                if (const auto* s = e.components.get<SynthComponent>();
                    s && s->playOnStart && e.activeInHierarchy)
                    synths.play(e.id);

            // --- ...and the game becomes it ----------------------------------
            // `startAs` was decided before the world was built (it had to be: it
            // chooses where the ground goes). The walking player is set up above
            // and is the fallback for everything here, which is deliberate: every
            // other mode needs the scene to provide something (a vehicle, a
            // glider, a camera), and a game that cannot start the way it was
            // configured should start in the way that always works rather than
            // not start at all.
            //
            // The camera the watching modes hand the frame to. Multishot asks for
            // the first camera that cuts its own shots; both fall back to the one
            // marked Main Camera (already in `activeCam` from the loop above), and
            // then to any camera at all -- a scene with one camera and no main
            // flag is a mistake worth being forgiving about.
            const auto startCamera = [&](bool wantMultishot) {
                for (const Entity& e : entities) {
                    const auto* cc = e.components.get<CameraComponent>();
                    if (!cc || !e.activeInHierarchy) continue;
                    if (wantMultishot && cc->mode != CameraComponent::Multishot)
                        continue;
                    return e.id;
                }
                return -1;
            };

            switch (startAs) {
            case game::StartMode::Vehicle:
                // enterVehicleMode spawns/drives the nearest scene vehicle (or a
                // fallback car) and takes over from the first-person setup above.
                vehicleMode = true;
                enterVehicleMode();
                break;
            case game::StartMode::Glider:
                // enterGliderMode flies the nearest glider entity (no-op if the
                // scene has none, which then leaves the walking player) -- so
                // first make sure there IS one, out of the grid's player slot.
                ensurePlayerCraft();
                gliderMode = true;
                enterGliderMode();
                break;
            case game::StartMode::MainCamera:
            case game::StartMode::Multishot: {
                // Watching, not playing. The character capsule stays spawned and
                // simply stands there -- exactly what a showroom scene does a few
                // lines below, and for the same reason: what makes this a picture
                // rather than a level is that nothing is driven by the keyboard
                // and the cursor is free, not that the physics world is emptied.
                // Order matters: the mode's OWN answer first, then the camera
                // the author marked as main, then any camera at all. Asking for
                // "a camera" before "the main one" would open a scene with three
                // of them on whichever happens to come first in the list.
                int cam = (startAs == game::StartMode::Multishot)
                              ? startCamera(true) : -1;
                if (cam < 0) cam = activeCam;          // the Main Camera, if any
                if (cam < 0) cam = startCamera(false); // ...else any camera
                if (cam >= 0) {
                    activeCam = cam;
                    fpsMode = false;
                    input.setCursorLocked(false);
                }
                break;
            }
            case game::StartMode::Fps:
            default:
                break;   // the walking player, already standing up
            }

            // ...and say so when the mode found nothing to be. Every case above
            // falls back to the walking player, which is the right behaviour and
            // the wrong silence -- exactly the trap the missing PlayerStart
            // message exists for. It bites hardest on a FORCED mode: the answer
            // was given a second ago in the viewport corner, so a game that
            // quietly opens on foot instead reads as the picker not working.
            //
            // A circuit with no craft parked on it is the ordinary case here, not
            // a broken scene: the craft normally arrives from the start screen,
            // which is the very thing being skipped.
            if (sceneStartMode >= 0) {
                const char* missing = nullptr;
                switch (startAs) {
                case game::StartMode::Glider:
                    if (!gliderMode)
                        missing = "no Glider in this scene, and the grid position "
                                  "marked \"Player starts here\" names no prefab to "
                                  "build one from.";
                    break;
                case game::StartMode::Vehicle:
                    if (!vehicleMode)
                        missing = "no Vehicle in this scene.";
                    break;
                case game::StartMode::MainCamera:
                case game::StartMode::Multishot:
                    if (activeCam < 0)
                        missing = "no camera in this scene to watch through.";
                    break;
                default: break;
                }
                if (missing)
                    host.hud = std::string("Play as \"") +
                               game::startModeName(startAs) + "\": " + missing +
                               " Standing here on foot instead.";
            }

            // A showroom scene is a start screen, not a level: no walking player
            // and no locked cursor -- the picker owns the frame, and it takes the
            // scene over from here (arranging the craft, driving the camera).
            // Checked last so it overrules whichever start mode ran above.
            // A scene that names its own mode means "this is what I am", and a
            // start screen is the one thing that would not be: it takes the scene
            // over and asks which craft and which circuit. So naming a mode skips
            // it -- which is also how a showroom scene is tested as a level.
            if (sceneStartMode < 0 && showroom::Showroom::isShowroomScene(entities)) {
                fpsMode = false;
                vehicleMode = gliderMode = false;
                input.setCursorLocked(false);
                std::vector<std::string> otherScenes;
                if (!currentProject.empty()) {
                    const std::string me =
                        std::filesystem::path(currentProject).stem().string();
                    for (const auto& sc : projectio::listScenesIn(
                             std::filesystem::path(currentProject)
                                 .parent_path().generic_string()))
                        if (sc.first != me) otherScenes.push_back(sc.first);
                }
                // The SKILL row opens on what the player last chose, wherever
                // they chose it -- the profile, not this session. It is the same
                // row either way, so the screen is the editor for the setting and
                // the setting is what the screen remembers.
                showroomUi.setDifficulty(gameDifficulty.level);
                showroomUi.setRecords(&raceRecords);
                showroomUi.begin(entities, otherScenes, camera);
            }
        };
        auto stopPlay = [&] {
            if (!playMode) return;
            townTraffic.endPlay();
            // Hand the craft and the camera back before the snapshot restore.
            showroomUi.end(entities, camera);
            playMode  = false;
            uiMenuOpen = false;     // back to the editor: the scene's menu is gone
            vehicleMode = false;    // the physics car is gone with the world
            driveVehicleId = -1;    // the scene restore below un-drives the model
            gliderMode = false;     // stop flying; the scene restore un-flies the craft
            driveGliderId = -1;
            gliderDriveActive = false; gliderBackup.clear(); // Play restore owns the transform
            animPlay.playing = false;  // the clip stops with the game it was running in
            animRuntime.clear();
            skids.clear();          // drop skid marks so they don't linger in the editor
            trails.clear();         // and the contrails
            weapons.reset();        // and anything the launcher still had in the air
            weapons2.reset();
            terrainCollId = 0;      // the collider dies with the world below
            physics.reset();
            scriptnet::reset();     // the game's connections and hosted relay end with it
            scriptSfx.clear();      // and its sound voices
            scriptSfxPaths.clear();
            scriptFigures.clear();
            trams.dropBodies();
            // A script's day was the game's: the scene's clock as it was.
            if (playClockBackup >= 0.0f) timeOfDay = playClockBackup;
            playClockBackup = -1.0f;
            scriptDayLength = -1.0f;
            streetLampMode  = -1;
            boneAttach.clear();
            decalSys.clearThrown();   // the holes were the game's
            shatterSys.clear();       // and so was the broken glass
            swingSys.clear();         // its boxes died with the world
            physicsBody.clear();
            softBodies.clear();  // the particles died with the world
            zoneSounds.clear(); // stop + free any looping TriggerSound voices
            audioVoices.clear(); // stop + free any AudioSource voices
            synths.clear();      // and the Synth players
            musicSys.clear();    // and a script's song
            // The wheel corrections are set while driving (the inspector's
            // Wheel orientation) -- throwing them away with the rest of the
            // run would make them impossible to set at all. They go back into
            // the scene as it was before Play.
            for (const Entity& e : entities) {
                const auto* vc = e.components.get<VehicleComponent>();
                if (!vc) continue;
                for (Entity& b : playEntities) {
                    if (b.id != e.id) continue;
                    if (auto* bv = b.components.get<VehicleComponent>()) {
                        for (int i = 0; i < 4; ++i) bv->wheelTurn[i] = vc->wheelTurn[i];
                    }
                    break;
                }
            }
            entities  = std::move(playEntities);
            materials = std::move(playMaterials);
            fpsMode   = false;
            // Give the VIEW back too. A camera entity had it whenever the game
            // started on one or a CameraSwitcher cut to one during the run, and
            // the free camera restored below is overwritten by that camera on the
            // very next frame otherwise -- the editor camera moving on its own,
            // with nothing on screen to say what had taken it.
            activeCam = -1;
            input.setCursorLocked(false);
            camera.setPosition(playCamPos);
            camera.setYaw(playCamYaw);
            camera.setPitch(playCamPitch);
            camera.moveSpeed = playMoveSpeed;
            camera.setFov(playCamFov);
            scriptCamFov     = 0.0f;
            scriptFocusFar   = 0.0f;
            scriptOwnsEye    = false;
            scriptCursorFree = false;
            entityEditMode = playPrevEdit;
            sel.clear();
        };

        showProgress(0.95f, "Generating world...");
        streamer.update(camera.position()); // kick off the initial terrain ring
        showProgress(1.0f, "Ready");
        loading.release(); // up and running -- give the backdrop's VRAM back

        // Player build: load the game project, hide the editor, go fullscreen and
        // start playing immediately. Esc quits (handled in the input loop).
        if (playerMode) {
            if (openProjectShowing(bootProject)) {
                // Boot into the configured start scene (materials/mounts already
                // set up by the open above); empty keeps the default scene.
                if (!bootScene.empty()) {
                    const std::string scenePath =
                        bootProject + "/" + bootScene + ".fitzel";
                    if (fitzel::vfs::exists(scenePath))
                        loadSceneShowing(scenePath, bootScene);
                }
                // Borderless, not exclusive: a game nobody can screenshot or
                // stream is a game nobody can show anyone (see setFullscreen).
                if (bootFullscreen) window.setFullscreen(true);
                presentMode = true;
                startPlay();
            } else {
                std::fprintf(stderr,
                    "[Fitzel] player: project not found: %s\n", bootProject.c_str());
            }
        } else if (!boot.editorOpen.empty()) {
            openProjectShowing(boot.editorOpen);
#ifndef FITZEL_PLAYER
            // Exporting from the command line: the same export as the File
            // menu's, for builds nobody wants to click through.
            if (!boot.exportDir.empty()) {
                projectio::exportGame(pio, boot.exportDir,
                                      boot.exportWeb ? projectio::ExportTarget::Web
                                                     : projectio::ExportTarget::Desktop);
                if (exportStatus.rfind("Export", 0) != 0) std::exit(1);   // "Exported ..." / "Export finished with warnings"
                window.requestClose();
            }
#endif
        }

        double lastTime = window.time();
        double nextAssetPoll = 0.0; // next wall-clock time to scan for asset edits

        // Frame pacing. Two caps keep the laptop cool without feeling sluggish:
        //   * Idle  (~15 FPS): while the editor sits with no input, sleep on events
        //     instead of spinning at the monitor rate. Any input wakes it instantly.
        //   * Active (~60 FPS): while editing/interacting, cap with a hard sleep.
        //     A continuous drag floods GLFW with events, so waitEventsTimeout would
        //     return immediately and we'd spin at full refresh -- sleep instead.
        // Only play/player mode runs uncapped (games want the monitor's full rate).
        // A short grace after the last input keeps easing/hover smooth. `activeFrame`
        // decides the NEXT iteration's pacing, so it's recomputed each frame's end.
        bool         activeFrame = true;
        // ...with one exception: a camera that is ANIMATING ITSELF. The caps
        // above are driven by input, and a view change or an F-focus is the one
        // thing that keeps moving after the input that asked for it is over --
        // so it was being drawn at the idle rate, and a quarter-second sweep
        // came out as three or four frames. Nothing about it looked like an
        // interpolation problem, because it was not one. Set while the eye is
        // still travelling; costs a fraction of a second of full rate.
        bool         camAnimating = false;
        double       lastActive  = window.time();
        double       frameStart  = window.time();
        const double kIdleGrace  = 0.4;        // s of full-rate after last input
        const double kIdleFrame  = 1.0 / 10.0; // idle cap period
        const double kActiveFrame = 1.0 / 25.0; // active (editing) cap period
        // Play has a cap of its own, but only on the game's say-so: a script
        // that calls game.rest() every frame nothing moves (a board game
        // waiting for its player) lets the loop wait for input instead of
        // drawing the same picture at the monitor rate. Only after a short
        // grace of asking without a break: the last moving frames leave TAA,
        // motion blur and exposure a few frames from settled.
        double       restSince   = window.time();
        const double kRestGrace  = 0.25;

#ifndef FITZEL_PLAYER
        // The menu bar's slice of the state above, gathered once (everything it
        // names lives for the whole loop) and redrawn from every frame.
        editormenu::FileMenuCtx fileMenu{
            window, currentProject, prefLocation, recentProjects, exportStatus,
            autoSave.status(), projNameBuf,
            wizName, sizeof(wizName), wizLocation, sizeof(wizLocation),
            wizardOpen, wizardIsNew, gameSettings, gameSettingsOpen,
            saveCurrent, exportGame, exportWeb, openProjectAsync, listProjectsIn, playMode,
        };
        editormenu::SceneMenuCtx sceneMenu{
            currentProject, sceneNameBuf, sizeof(sceneNameBuf),
            sceneNewOpen, sceneRenameOpen, sceneDeleteOpen,
            saveSceneFile, loadSceneAsync, listScenesIn,
        };
        editormenu::EditMenuCtx editMenu{
            history, document, entities, sel,
            prefabNameBuf, sizeof(prefabNameBuf), showPrefabs,
            clampRoadSel, clampSplineSel, clampRiverSel,
            duplicateSelection, deleteSelection,
        };
        // The View menu, as data (see PanelEntry). "Close all panels" walks this
        // same table, so it can no longer fall behind the menu.
        const std::vector<editormenu::PanelEntry> viewPanels = {
            {"World",    "Terrain",            nullptr, &showTerrain},
            {"World",    "Terrain sculpt",     nullptr, &showSculpt},
            {"World",    "Terrain paint",      nullptr, &showPaint},
            {"World",    "Water",              nullptr, &showWater},
            {"World",    "Rivers & brooks",    nullptr, &showRivers},
            {"World",    nullptr,              nullptr, nullptr},
            {"World",    "Sky & atmosphere",   nullptr, &showSky},
            {"World",    "Weather & audio",    nullptr, &showWeather},
            {"World",    "Colour grade",       nullptr, &showColorGrade},
            {"World",    "Environment",        nullptr, &showEnv},
            {"World",    "Advanced nature",    nullptr, &showNature},
            {"Planting", "Vegetation",         nullptr, &showVegetation},
            {"Planting", "Scatter",            nullptr, &showScatter},
            {"Planting", "Tree generator",     nullptr, &showTreeGen},
            {"Track",    "Roads",              nullptr, &showRoads},
            {"Track",    "Splines & bridges", nullptr, &showSplines},
            {"Track",    "City",               nullptr, &showCity},
            {"Track",    "Town generator",     nullptr, &showTowns},
            {"Track",    "Level generator",    nullptr, &showLevelGen},
            {"Track",    "Buildings",          nullptr, &showBuildings},
            {"Track",    "Houses",             nullptr, &showHouses},
            {"Track",    "Street signs",       nullptr, &showSigns},
            {"Track",    nullptr,              nullptr, nullptr},
            {"Track",    "Vehicle",            nullptr, &showVehiclePanel},
            {"Track",    "Glider",             nullptr, &showGliderPanel},
            // Shaping and placing the things in the scene. The 3D cursor sits
            // here rather than under "Inspect", where it had been sitting alone:
            // it is the anchor the grid is drawn on and objects are placed at,
            // so the three belong on one submenu, not scattered by which panel
            // happens to draw them. Modeling and Grid stay out of the close-all
            // sweep -- Grid is the construction grid itself, not a panel.
            {"Objects",  "Modeling",           nullptr, &showModeling, false},
            {"Objects",  "Mesh paint",         nullptr, &showMeshPaint},
            {"Objects",  "UV",                 nullptr, &showUv},
            {"Objects",  "Procedural",         nullptr, &showProcedural},
            {"Objects",  nullptr,              nullptr, nullptr},
            {"Objects",  "3D cursor",          nullptr, &showCursor},
            {"Objects",  "Grid",               nullptr, &showGrid, false},
            {"Assets",   "Materials",          nullptr, &showMaterials},
            {"Assets",   "Models",             nullptr, &showModels},
            {"Assets",   "Retarget animations", nullptr, &showRetarget},
            {"Assets",   "Image editor",       nullptr, &showImageEditor},
            {"Assets",   "Prefabs",            nullptr, &showPrefabs},
            {"Assets",   "Assets",             nullptr, &showAssets},
            {"Assets",   "Scripts",            nullptr, &scriptEditor.visible},
            // The synth lives with the assets it makes: a patch is a sound
            // file's replacement, authored here and played by the game.
            {"Assets",   "Synth",              nullptr, &showSynth},
            {"Assets",   nullptr,              nullptr, nullptr},
            {"Assets",   "Import Unity asset", nullptr, &showUnityImport},
            {"Presentation", "UI Overlay",     nullptr, &showUiOverlay},
            {"Presentation", "Camera",         nullptr, &showCamera},
            {"Presentation", "Camera path",    nullptr, &showCamPath},
            {"Presentation", "Timeline",       nullptr, &showTimeline},
            {"Presentation", "Animation graph", nullptr, &showGraphEditor},
            {"Presentation", "Render",         nullptr, &pathRender.open},
            {"Presentation", "Mixer",          nullptr, &showMixer},
            {"Inspect",  "Camera preview",     nullptr, &showCamPreview, false},
            {"Inspect",  "Performance",        "F3",    &showPerf},
            {"Inspect",  "Stats",              nullptr, &showStats},
        };
#endif


        // --- Benchmark mode ---------------------------------------------------
        // See BootConfig::profilePath. Vsync goes off for the run: a frame that
        // waits for the display measures the display.
        double profileStart = 0.0;
        if (!boot.profilePath.empty()) glfwSwapInterval(0);
#ifndef FITZEL_PLAYER
        shotlist::Runner shotRunner;
        if (!boot.shotsPath.empty() && !bootProject.empty())
            shotRunner.load(boot.shotsPath, boot.shotsOut);
        if (boot.shotsTrace > 0)
            pathpanel::attachToShots(pathRender, shotRunner, boot.shotsTrace,
                                     [&](int& w, int& h) { window.framebufferSize(w, h); },
                                     boot.shotsTraceGpu);
        shotRunner.target = [&](int i, glm::vec3& t) {
            if (i == 2000 || i == 2001) {      // "@2000" = the fish, or its last ring
                if (i == 2001) wildlife.fishRate = 6.0f;   // ...and "@2001" with them busy
                if (wildlife.fishInAir(t)) return true;
                const std::vector<glm::vec4>& rp = wildlife.ripples();
                if (rp.empty()) return false;
                t = wildlife.lastRipple();
                return true;
            }
            if (i >= 1000) {                   // "@1000.." = the herd's animals
                if (i - 1000 >= herd.count()) return false;
                t = herd.animalPos(i - 1000) + glm::vec3(0.0f, 1.0f, 0.0f);
                return true;
            }
            if (i < 0 || i >= wildlife.birdsDrawn()) return false;
            t = wildlife.debugPos(i);
            return true;
        };
        // "@3000".. = riding in tram (n-3000)/10, view n%10: 0 down the middle
        // car's aisle, 1 its stop side from outside, 2 the first car towards
        // its cab, 3 across the middle car's doorway, 4 from the pavement
        // beside the middle car.
        shotRunner.eye = [&](int i, glm::vec3& e, glm::vec3& at) {
            if (i < 3000 || i >= 3100) return false;
            const int tram = (i - 3000) / 10, view = i % 10;
            if (tram >= trams.tramCount()) return false;
            const int car = view == 2 ? 0 : 1;
            const glm::mat4 f = trams.sim().carFrame(tram, car);
            const float s = static_cast<float>(trams.sim().doorSide(tram, car));
            const float F = tramsim::kFloor;
            auto w = [&](glm::vec3 p) { return glm::vec3(f * glm::vec4(p, 1.0f)); };
            switch (view) {
            case 0: e = w({0.0f, F + 1.6f, -4.3f}); at = w({0.0f, F + 1.2f, 4.0f}); break;
            case 1: e = w({s * 9.0f, 1.8f, -2.0f}); at = w({0.0f, 1.3f, 0.0f}); break;
            case 2: e = w({0.0f, F + 1.6f, -4.3f}); at = w({0.0f, F + 1.2f, 4.0f}); break;
            case 3: e = w({-s * 0.9f, F + 1.6f, -1.2f}); at = w({s * 1.5f, F + 1.0f, -2.4f}); break;
            default: e = w({s * 3.5f, 1.7f, 1.0f}); at = w({0.0f, F + 1.0f, -2.0f}); break;
            }
            return true;
        };
        shotRunner.status = [&] {
            const prof::FrameStats fs = prof::frameStats();
            char buf[400];
            std::snprintf(buf, sizeof buf, "chunks %d (+%d pending)  frame %.1f ms  eye %.0f %.0f %.0f"
                          "  fauna %d/%d birds %d flies %d  b0 %.0f %.0f %.0f  s0 %.0f %.0f %.0f  r0 %.0f %.0f %.0f  v %.1f goal %.0f upd %d",
                          streamer.loadedChunkCount(), streamer.pendingChunkCount(),
                          fs.avg, camera.position().x, camera.position().y,
                          camera.position().z, wildlifeOn ? 1 : 0, wildlife.ready() ? 1 : 0,
                          wildlife.birdsDrawn(), wildlife.fliesDrawn(),
                          wildlife.debugPos(0).x, wildlife.debugPos(0).y, wildlife.debugPos(0).z,
                          wildlife.debugPos(62).x, wildlife.debugPos(62).y, wildlife.debugPos(62).z,
                          wildlife.debugPos(69).x, wildlife.debugPos(69).y, wildlife.debugPos(69).z,
                          wildlife.debugSpeed(), wildlife.debugGoal(), wildlife.debugUpdates);
            // Why the grass does or does not grow at the eye (the placement's
            // own tests, GrassTrace::generateTile).
            {
                const glm::vec3 e = camera.position();
                const float h  = streamer.heightAt(e.x, e.z);
                const float hl = streamer.heightAt(e.x - 1, e.z), hr = streamer.heightAt(e.x + 1, e.z);
                const float hd = streamer.heightAt(e.x, e.z - 1), hu = streamer.heightAt(e.x, e.z + 1);
                const float ny = glm::normalize(glm::vec3(hl - hr, 2.0f, hd - hu)).y;
                const float mo = fitzel::terrainMoisture(streamer.settings(), e.x, e.z);
                char g[200];
                std::snprintf(g, sizeof g, "  birdsong %d phrases %d singers  grass@eye h %.1f ny %.2f moist %.2f bare %.2f bare2 %.2f blades %d",
                              soundscape.phrases(), soundscape.singing(),
                              h, ny, mo, valNoise2(e.x * 0.13f + 19.0f, e.z * 0.13f + 7.0f),
                              valNoise2(e.x * 0.31f + 3.0f, e.z * 0.31f + 23.0f), veg.grassCount);
                char f[200];
                std::snprintf(f, sizeof f, "  fish jumps %d rings %d motes %d  wind %.2f gust %.2f t %.1f",
                              wildlife.fishJumps(), static_cast<int>(wildlife.ripples().size()),
                              motes.drawn(), veg.wind.strength, veg.wind.gustiness, veg.wind.time);
                return std::string(buf) + g + f + "  " + herd.status();
            }
            return std::string(buf);
        };
#endif
        auto writeProfileReport = [&] {
            std::ofstream out(boot.profilePath);
            if (!out) return;
            const prof::FrameStats fs = prof::frameStats();
            int pw = 0, ph = 0;
            window.framebufferSize(pw, ph);
            const auto glStr = [](GLenum e) {
                const GLubyte* v = glGetString(e);
                return v ? reinterpret_cast<const char*>(v) : "?";
            };
            out << "fitzel " << fitzel::kVersion << " profile" << "\n";
            out << "project    " << bootProject << "\n";
            out << "scene      " << (bootScene.empty() ? "(default)" : bootScene) << "\n";
            out << "resolution " << pw << "x" << ph << "\n";
            out << "gpu        " << glStr(GL_RENDERER) << "\n";
            out << "gl         " << glStr(GL_VERSION) << "\n";
            out << "measured   " << boot.profileSeconds << " s over "
                << prof::history().size() << " frames" << "\n";
            // What the exposure meter read (log2 luminance) and what auto
            // exposure made of it -- the number PostChain::kAutoReferenceLog2
            // was calibrated from.
            out << "meter      log2 " << post.meteredLog2() << "   auto x"
                << post.autoExposureScale() << "\n" << "\n";
            out << "frame      avg " << fs.avg << " ms ("
                << (fs.avg > 0.0f ? 1000.0f / fs.avg : 0.0f) << " fps)"
                << "   worst " << fs.worst << " ms"
                << "   1% low " << fs.low1 << " fps"
                << "   spikes " << fs.spikes << "\n" << "\n";
            out << "where the time goes (ms; GPU rows are the pass on the card)" << "\n";
            std::vector<const prof::ZoneStat*> zs;
            for (const prof::ZoneStat& z : prof::zones()) zs.push_back(&z);
            std::sort(zs.begin(), zs.end(),
                      [](const prof::ZoneStat* a, const prof::ZoneStat* b) {
                          return a->avg > b->avg;
                      });
            for (const prof::ZoneStat* z : zs)
                out << "  " << z->name << "  avg " << z->avg
                    << "  worst " << z->worst << "\n";
            out << "\n" << "scene" << "\n";
            out << "  terrain chunks loaded   " << streamer.loadedChunkCount() << "\n";
            out << "  shadow caster draws     " << renderer.shadowDraws()
                << "  (summed over every cascade)" << "\n";
            out << "  shadow triangles        " << renderer.shadowTris() << "\n";
            out << "  tree shadow instances   " << veg.shadowInstances()
                << "  within " << veg.treeShadowDistance << " m" << "\n";
            out << "  tree shadow triangles   " << veg.shadowTriangles() << "\n";
            out << "  trees in range          " << veg.treeCount << "\n";
            out << "  tree instances drawn    " << veg.drawnInstances()
                << "  (summed over every pass in one frame)" << "\n";
            out << "  grass blades streamed   " << veg.grassCount << "\n";
            out << "  grass blades painted    "
                << veg.paintedBlades.size() / 7 << "\n";
            out << "  entities                " << entities.size() << "\n";
        };

        // One trip round the program: input, simulation, every pass of the
        // picture, the swap. A function rather than the body of a loop because
        // the browser calls it once per frame instead of being looped in (see
        // below); on the desktop it is looped exactly as before.
        auto frame = [&]() {
            // Taken and cleared each frame: a script that stops asking is back
            // at full rate on the next one.
            const float restFps = host.restFps;
            host.restFps = 0.0f;
            if (restFps <= 0.0f) restSince = window.time();
            const bool resting = (playMode || playerMode) && restFps > 0.0f &&
                                 window.time() - restSince >= kRestGrace;
            const bool uncapped = playMode || playerMode || camAnimating;
#ifdef __EMSCRIPTEN__
            // The browser paces the frames and must never be made to wait:
            // resting, capping and idling are its business here.
            (void)resting; (void)uncapped; (void)kActiveFrame; (void)kIdleFrame;
            window.pollEvents();
#else
            if (resting) {
                // A cap, not a fixed wait: the frame's own time counts, and any
                // event (the pointer moving onto a square) ends the wait at once.
                const double budget = 1.0 / restFps - (window.time() - frameStart);
                if (budget > 0.0) window.waitEventsTimeout(budget);
                else              window.pollEvents();
            } else if (uncapped) {
                window.pollEvents();
            } else if (activeFrame) {
                // Editing: enforce the active cap with a real sleep (events would
                // cut a waitEventsTimeout short mid-drag), then drain the queue.
                const double budget = kActiveFrame - (window.time() - frameStart);
                if (budget > 0.0)
                    std::this_thread::sleep_for(
                        std::chrono::duration<double>(budget));
                window.pollEvents();
            } else {
                // Idle: block until an event or the idle period elapses.
                window.waitEventsTimeout(kIdleFrame);
            }
#endif
            frameStart = window.time();
            // Opened after the polling block above so the editor's frame cap and
            // idle wait aren't billed as frame cost; closes at the bottom of the
            // loop and files the frame with the profiler.
            prof::Frame fzFrame;
            input.update();

            const double now = window.time();
            // Clamp the frame delta. A single long frame (the 0.5 s asset-poll
            // scan below, a texture/model hot-reload, a GC/stall) would otherwise
            // hand the arcade sims a huge dt: the glider integrates position
            // linearly and lurches forward, while the chase camera eases with a
            // saturating min(1, dt*k) and snaps fully onto the craft -- so the
            // craft slides *backward on screen* for one frame ("jumps back a
            // little"). Capping dt makes a hitch briefly slow time instead.
            //
            // This is THE guard against a runaway sim, and the reason the step
            // cap below can be generous: however long a frame took, the sims are
            // never handed more than this much of it.
#ifndef FITZEL_PLAYER
            // The crash snapshot. Held off while the document is not a faithful
            // picture of the user's work: play mode mutates entities that Stop
            // rolls back, a streaming load has only half a scene, and while the
            // recovery dialog is still up the document is the OLD session's --
            // snapshotting over the very file being offered would eat it.
            autoSave.tick(window.time(),
                          !playMode && !playerMode && !sceneLoad.active &&
                              !prefabEdit.active && !pendingSnapshot.valid(),
                          currentProject, history.revision(),
                          [&](const std::string& path){
                              return projectio::saveSceneWithMaterials(pio, path);
                          });
#endif

            constexpr double kMaxFrameDt = 0.05;   // 20 fps
            const float  dt  = static_cast<float>(std::min(now - lastTime, kMaxFrameDt));
            lastTime = now;

            // Fixed-timestep clock for the arcade sims (car + glider). Their
            // visible motion is integrated in constant H-second ticks and the
            // render pose is interpolated by simAlpha, so the frame-to-frame dt
            // jitter that vsync/DWM hands us stops showing up as micro-stutter
            // (worst in curves, where the motion is lateral on screen). Every
            // other subsystem keeps running at frame dt.
            constexpr float kSimH = 1.0f / 120.0f;
            // How many ticks one frame may ever need: the clamp above, plus the
            // sub-tick remainder carried over from the last frame. DERIVED rather
            // than typed, because the two must not drift apart -- and they had.
            //
            // This cap used to be a flat 5, which is 41.7 ms: tighter than the
            // 50 ms clamp it sits behind, so from ~24 fps down it fired every
            // frame and threw the leftover away. The craft lost that time; the
            // camera and the world, running on frame dt, did not -- so the craft
            // slid backwards on screen and snapped forward again on the next
            // frame that fitted under the cap. Exactly the symptom the dt clamp
            // above was added to cure, reintroduced one guard further in, and
            // worse the lower the frame rate (0.4 m per frame at 30 fps, 3.5 m
            // at 15, for a craft doing 100 m/s).
            //
            // +2, not +1: the division is float arithmetic and lands a hair
            // under the whole number as often as on it, and the whole point of
            // deriving this is that it must never come out SHORT.
            constexpr int kMaxSimSteps =
                static_cast<int>((kMaxFrameDt + kSimH) / kSimH) + 2;
            simAccum += dt;
            int simSteps = 0;
            while (simAccum >= kSimH && simSteps < kMaxSimSteps) {
                simAccum -= kSimH;
                ++simSteps;
            }
            // Drop a backlog only if there IS one -- i.e. the loop stopped at
            // the cap with time still queued, not because it had run the queue
            // dry. Testing the step count instead (as this did) throws away up
            // to a whole tick of motion on a frame that had already finished
            // its work, which is the same backward slide by another route.
            //
            // Unreachable while the two constants above agree; kept as the
            // honest last resort if the clamp is ever loosened without this
            // being rechecked. A hitch then slows time rather than spiralling,
            // which is right for a hitch and wrong as a routine event -- which
            // is what it had become.
            if (simAccum >= kSimH) simAccum = 0.0f;
            // In [0,1) by construction; clamped because a pose blend outside it
            // extrapolates, and that is a lurch rather than a smooth frame.
            const float simAlpha = glm::clamp(simAccum / kSimH, 0.0f, 1.0f);

            // Resolve the scene's cameras and point the view at the right one.
            //
            // Called at the END of whatever moved the scene this frame -- the
            // play tick when there is one, the control chain otherwise -- so the
            // eye follows where things ended up rather than trailing a frame.
            //
            // WHICH camera is the view: a Camera entity a CameraSwitcher or a
            // script has cut to wins outright; otherwise it is the camera hanging
            // on the craft being driven, because that is what "the camera belongs
            // to the object it hangs on" means from the viewer's side. A craft
            // without one leaves the free camera where it is, which is the honest
            // answer -- better than inventing an eye nobody placed.
            auto applyViewCamera = [&] {
                cams.update(entities, dt);
                // The camera children of `owner`, in scene order. A craft with
                // two is not an authoring mistake any more -- it is a chase view
                // and a cockpit view, and `viewCam` says which one is up.
                const auto childCams = [&](int owner) {
                    std::vector<int> ids;
                    if (owner < 0) return ids;
                    for (const Entity& e : entities)
                        if (e.parent == owner && e.activeInHierarchy &&
                            e.components.get<CameraComponent>())
                            ids.push_back(e.id);
                    return ids;
                };
                // ...and the one being looked through. Modulo, so the counter can
                // run on forever and every craft answers with a camera it
                // actually has: step past the end of a two-camera craft and you
                // are back on its first, while a rival carrying one is simply
                // never anything else.
                const auto childCam = [&](int owner) {
                    const std::vector<int> ids = childCams(owner);
                    if (ids.empty()) return -1;
                    return ids[static_cast<std::size_t>(viewCam) % ids.size()];
                };
                const int driven = gliderMode ? driveGliderId
                                 : vehicleMode ? driveVehicleId : -1;
                int viewId = activeCam;
                if (viewId < 0) viewId = childCam(driven);
                camerasys::Pose p;
                if (viewId >= 0 && cams.pose(viewId, p)) {
                    camera.setPosition(p.position);
                    camera.setBasis(p.front, p.up);
                    camera.setFov(p.fov);
                } else {
                    activeCam = -1;              // target vanished -> free camera
                    camera.setFov(scriptCamFov > 0.0f ? scriptCamFov : playCamFov);
                }

                // Watching a rival (V). Last, so it wins over whatever the view
                // would otherwise be: it is the one camera decision the person at
                // the keyboard made just now, by hand.
                //
                // A rival that carries its own camera child is looked THROUGH --
                // whose camera it is, is the hierarchy's answer, and an author who
                // hung an eye on that craft meant it. One that carries none gets
                // your own camera's settings, so a rival's view is the view you
                // already know rather than some other game's shot.
                if (!gliderMode) spectateId = -1;   // only meaningful in a race
                if (spectateId >= 0) {
                    const Entity* tgt = document.find(spectateId);
                    if (!tgt || !tgt->activeInHierarchy) {
                        spectateId = -1;
                    } else {
                        camerasys::Pose sp;
                        const int own = childCam(spectateId);
                        if (own < 0 || !cams.pose(own, sp)) {
                            camerasys::FollowShot shot;
                            if (const Entity* mine =
                                    viewId >= 0 ? document.find(viewId) : nullptr)
                                if (const auto* cc =
                                        mine->components.get<CameraComponent>()) {
                                    shot.offset     = mine->localCenter;
                                    shot.lookHeight = cc->lookHeight;
                                    shot.stiffness  = cc->stiffness;
                                    shot.rollWith   = cc->rollWith;
                                    shot.fov        = cc->fov;
                                }
                            // Negative key: the smoothing state is ours, not a
                            // camera entity's (see CameraSystem::follow).
                            sp = cams.follow(-(spectateId + 1), *tgt, shot, dt);
                        }
                        camera.setPosition(sp.position);
                        camera.setBasis(sp.front, sp.up);
                        camera.setFov(sp.fov);
                    }
                }
                // Player two's eye, same rule one craft over. Split screen is now
                // just a second camera entity being asked for its pose.
                //
                // haveView2 is what the renderer splits on -- NOT the checkbox.
                // A second pane is only worth having if something can be seen
                // through it: no second craft, or a craft with no camera on it,
                // and the frame stays whole rather than handing half the screen
                // to an eye standing wherever it was last left.
                // The grid orbit overrides whichever camera the scene would
                // otherwise be seen through. For these few seconds the subject is
                // the craft standing on the grid, and a chase camera sitting
                // behind a stationary object is a picture of nothing.
                if (race.onGrid && driven >= 0) {
                    if (const Entity* pc = document.find(driven)) {
                        const glm::vec3 at  = pc->center + glm::vec3(0.0f, 1.2f, 0.0f);
                        const float     ang = race.gridTime * kGridOrbitRate;
                        const glm::vec3 eye =
                            at + glm::vec3(std::cos(ang) * kGridOrbitRadius,
                                           kGridOrbitHeight,
                                           std::sin(ang) * kGridOrbitRadius);
                        camera.setPosition(eye);
                        camera.setBasis(glm::normalize(at - eye), glm::vec3(0.0f, 1.0f, 0.0f));
                        camera.setFov(kGridOrbitFov);
                    }
                }
                haveView2 = false;
                if (splitScreen && driveGliderId2 >= 0) {
                    const int id2 = childCam(driveGliderId2);
                    camerasys::Pose p2;
                    if (id2 >= 0 && cams.pose(id2, p2)) {
                        camera2.setPosition(p2.position);
                        camera2.setBasis(p2.front, p2.up);
                        camera2.setFov(p2.fov);
                        haveView2 = true;
                    }
                }
            };

            // Whatever this frame imported is on the GPU now: the decoded maps the
            // node cache kept for it can go (see ModelLibrary::releasePixels).
            // Not while a scene load is streaming in, though: it imports a few
            // nodes a frame, and every node after a release reads its whole file
            // again -- a 43-part house was 43 reads of 1.2 s each.
            if (!sceneLoad.active) models.releasePixels();

            // Hot reload: pick up on-disk asset edits ~twice a second. Textures
            // and models reload in place (existing handles update automatically);
            // edited/added/removed materials refresh the project's library.
            //
            // NOT WHILE PLAYING, and that is a frame-pacing decision rather than a
            // tidiness one. The scan walks the project's asset tree, and on a real
            // project it costs up to 12 ms -- measured, in a frame budget of 16.7.
            // Twice a second that is a dropped frame twice a second, at a fixed
            // interval, which is the worst kind of judder to look at: regular
            // enough to feel like a rhythm and small enough that nobody suspects
            // a file scan. It bites hardest in a cockpit view, where the whole
            // image is rigid with the craft and has nothing to hide behind.
            //
            // Nothing is lost: hot reload exists so an edit made in Photoshop
            // shows up while you work, and while Play is running you are not
            // editing files -- Stop picks up everything that changed meanwhile on
            // the very next scan.
            if (now >= nextAssetPoll && !playMode) {
                FZ_ZONE("assets (0.5s poll)");
                nextAssetPoll = now + 0.5;
                const std::vector<AssetChange> changes = assetDb.pollChanges();
                bool materialsChanged = false;
                for (const AssetChange& ch : changes)
                    if (ch.type == AssetType::Material) materialsChanged = true;
                if (materialsChanged && !currentProject.empty())
                    loadProjectMaterials(
                        std::filesystem::path(currentProject).parent_path()
                            .generic_string() + "/materials");
            }

            // Video materials. Binding happens here rather than at load time
            // because a material's videoId arrives from three directions (.fmat
            // files, scene overrides, the panel) and all of them only store the
            // GUID. Re-checking every frame is a walk over a few dozen materials
            // and makes the binding self-healing after a hot reload; the actual
            // open is cached in the VideoLibrary and happens once.
            {
            FZ_ZONE("video");
            for (MaterialDef& md : materials) {
                if (!md.videoId.valid()) continue;
                // A video that won't open leaves the slot untouched rather than
                // clearing it: the surface keeps whatever it had (a model's own
                // map, say) instead of turning flat because a file went missing.
                if (auto v = videos.get(assetDb, md.videoId))
                    if (md.tex != v->texture()) md.tex = v->texture();
            }
            videos.advanceAll(dt);
            }

            // F3 toggles the Performance window. A bare function key so it works
            // in play mode and in the player, where there is no menu bar.
            {
                const bool f3 = input.isKeyDown(GLFW_KEY_F3);
                if (f3 && !prevF3) showPerf = !showPerf;
                prevF3 = f3;
            }
            // F4 writes the next 300 frames of "where is the craft, where is the
            // eye" to camtrace.csv beside the project. A bare function key for the
            // same reason as F3: the moment worth measuring is while something is
            // being flown, and that is the one moment there are no menus.
            {
                const bool f4 = input.isKeyDown(GLFW_KEY_F4);
                if (f4 && !prevF4 && camTraceLeft == 0) {
                    camTraceLeft = 300;
                    camTraceRows.clear();
                    camTraceRows.push_back(
                        "frame,dt_ms,steps,alpha,craftX,craftY,craftZ,"
                        "simX,simY,simZ,camX,camY,camZ,offset,camId");
                    exportStatus = "Camera trace: recording 300 frames...";
                }
                prevF4 = f4;
            }

#ifndef FITZEL_PLAYER
            // The modelling mode -- Blender's Edit Mode: the Modeling panel open on
            // an editable mesh. While it is on, the viewport's keys are Blender's
            // (ModelingKeys.hpp) and the editor's own shortcuts below stand
            // aside; everywhere else they are what they always were. Tab goes in
            // and out, and a solid becomes a mesh on the way in.
            const bool modelling = showModeling && !playMode && selectedMesh() != nullptr;
            const bool meshBusy  = modelling && modelkeys::busy();
            if (!playMode && viewportHovered && !meshBusy && !ImGui::GetIO().WantTextInput &&
                ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
                if (!showModeling && !selectedMesh() && cursorHaveSel() &&
                    isSolidPrimitive(entities[sel.index()].type))
                    convertToMesh();
                showModeling = !showModeling && selectedMesh() != nullptr;
            }
#else
            const bool modelling = false, meshBusy = false;
#endif
            // --- Input ---------------------------------------------------
            // F frames the selected object; Shift+F toggles first-person walk mode.
            const bool fDown  = input.isKeyDown(GLFW_KEY_F);
            const bool shiftF = input.isKeyDown(GLFW_KEY_LEFT_SHIFT) ||
                                input.isKeyDown(GLFW_KEY_RIGHT_SHIFT);
            if (fDown && !prevF && !vehicleMode && !gliderMode && !modelling &&
                !ImGui::GetIO().WantTextInput) {
                if (shiftF) { // Shift+F: toggle first-person (cursor locks, mouse-look)
                    fpsMode = !fpsMode;
                    input.setCursorLocked(fpsMode);
                    fpsVelY = 0.0f;
                    if (fpsMode) { // drop to standing height immediately
                        const glm::vec3 p = camera.position();
                        camera.setPosition({p.x, streamer.heightAt(p.x, p.z) + eyeHeight, p.z});
                    }
                } else if (!fpsMode && sel.valid()) {
                    // Focus: frame the selected object's bounding sphere.
                    const Entity& e = entities[sel.index()];
                    frameSphere(e.center, glm::max(glm::length(e.half), 0.25f));
                }
            }
            prevF = fDown;

            // V has two jobs, and which one it is doing is never ambiguous
            // because they cannot both apply.
            //
            // FLYING A RACE: step the view through the field -- your own craft,
            // then each rival that entered, then back to your own. Entering a
            // car in the middle of a race is not a thing anyone wants that key
            // to do, and watching the field is, so while a glider is being flown
            // with rivals on the track, V is the view switch.
            //
            // OTHERWISE: toggle the drive-a-vehicle mode, as it always has. A
            // scene vehicle (a model with a Vehicle component, nearest to the
            // camera) takes precedence: in Play it spawns the Jolt car from the
            // component at the model, in the editor the arcade sim test-drives
            // the model itself. With no scene vehicle, the primitive test car
            // behaves as before.
            const bool vDown = input.isKeyDown(GLFW_KEY_V);
            if (vDown && !prevV && !modelling && !ImGui::GetIO().WantTextInput) {
                // The field, in scene order -- NOT in race order. A list that
                // reshuffles as places change would step somewhere different
                // every time it is pressed, and the one thing a view switch has
                // to be is predictable. Only racers that entered: the rest are
                // scenery this session.
                std::vector<int> field;
                if (gliderMode)
                    for (const Entity& e : entities) {
                        const auto* oc = e.components.get<OpponentComponent>();
                        if (oc && oc->entered && e.activeInHierarchy)
                            field.push_back(e.id);
                    }
                if (field.empty()) {
                    vehicleMode = !vehicleMode;
                    if (vehicleMode) enterVehicleMode();
                    else             endEditorDrive();
                } else {
                    const auto at = std::find(field.begin(), field.end(), spectateId);
                    spectateId = (at == field.end())      ? field.front()
                               : (at + 1 == field.end())  ? -1
                                                          : *(at + 1);
                }
            }
            prevV = vDown;

            // C (or the pad's Y) steps through the cameras of whatever is being
            // driven: chase, cockpit, and anything else hung on it.
            //
            // The switch is a COUNTER over the craft's own children rather than a
            // named mode, so what the key offers is whatever the author hung on
            // that craft -- and a craft with one camera simply has nothing to
            // step to. It applies to the second pane and to a rival being watched
            // as well: one person is pressing it, and having their own view
            // change while the other pane stayed behind would read as a fault.
            //
            // Only while something is being driven. Loose in the editor it would
            // be a key that does nothing visible, and those are the ones people
            // press twice.
            {
                const bool cDown = input.isKeyDown(GLFW_KEY_C);
                const bool pad   = input.gamepadButton(GLFW_GAMEPAD_BUTTON_Y);
                if (((cDown && !prevViewKey) || (pad && !prevViewPad)) &&
                    (gliderMode || vehicleMode) && !ImGui::GetIO().WantTextInput)
                    ++viewCam;
                prevViewKey = cDown;
                prevViewPad = pad;
            }

            // G toggles the fly-a-glider mode: take the nearest glider (a model
            // with a Glider component) and fly it with the arcade hover sim, in
            // the editor or in Play. Mutually exclusive with the car's drive mode.
            const bool gDown = input.isKeyDown(GLFW_KEY_G);
            if (gDown && !prevG && !modelling && !ImGui::GetIO().WantTextInput) {
                gliderMode = !gliderMode;
                if (gliderMode) {
                    if (vehicleMode) { vehicleMode = false; endEditorDrive(); }
                    enterGliderMode();
                } else {
                    endGliderDrive();
                }
            }
            prevG = gDown;

            // F11 toggles borderless-fullscreen presentation (UI hidden).
            const bool f11 = input.isKeyDown(GLFW_KEY_F11);
            if (f11 && !prevF11) {
                presentMode = !presentMode;
                window.setFullscreen(presentMode);
            }
            prevF11 = f11;

            // Scene menu overlay: while playing, its key opens/closes it. Opening
            // frees the mouse so the menu's buttons can be clicked; closing gives
            // the cursor back to the game if it was walking first-person.
            // The toggle key is a KEYBOARD setting (0 = the author turned it off),
            // so it no longer gates whether the menu can be opened at all: a pad
            // reaches it through its own Menu button, which is the pause button on
            // every console pad and is not the author's to reassign.
            const bool uiMenuArmed = (playMode || playerMode) && uiOverlay.menuMode() &&
                                     !uiOverlay.empty();
            if (uiMenuArmed) {
                const bool uiKeyDown =
                    (uiOverlay.toggleKey() != 0 &&
                     input.isKeyDown(uiOverlay.toggleKey())) ||
                    (input.hasGamepad() &&
                     input.gamepadButton(GLFW_GAMEPAD_BUTTON_START));
                if (uiKeyDown && !prevUiKey) {
                    uiOverlay.setRuntimeVisible(!uiOverlay.runtimeVisible());
                    input.setCursorLocked(uiOverlay.runtimeVisible() ? false
                                                                     : fpsMode && !scriptCursorFree);
                }
                prevUiKey = uiKeyDown;
            } else {
                prevUiKey = false;
            }
            uiMenuOpen = uiMenuArmed && uiOverlay.runtimeVisible();

            // Overlay navigation: arrow keys, D-pad or left stick move the focus,
            // Enter / pad A or Start fires it. Active whenever a visible overlay
            // has buttons at all -- Play locks the cursor and a pad has none, so
            // without this an on-screen menu is only reachable by feel.
            if ((playMode || playerMode) && uiOverlay.runtimeVisible() &&
                uiOverlay.anyButtons()) {
                const bool pad = input.hasGamepad();
                // GLFW's stick Y is -1 up. A half-deflection threshold plus the
                // edge check below makes one push move exactly one entry.
                const float stickY = pad ? input.gamepadStick(GLFW_GAMEPAD_AXIS_LEFT_Y)
                                         : 0.0f;
                const bool prevBtn =
                    input.isKeyDown(GLFW_KEY_UP) || input.isKeyDown(GLFW_KEY_LEFT) ||
                    stickY < -0.5f ||
                    (pad && (input.gamepadButton(GLFW_GAMEPAD_BUTTON_DPAD_UP) ||
                             input.gamepadButton(GLFW_GAMEPAD_BUTTON_DPAD_LEFT)));
                const bool nextBtn =
                    input.isKeyDown(GLFW_KEY_DOWN) || input.isKeyDown(GLFW_KEY_RIGHT) ||
                    stickY > 0.5f ||
                    (pad && (input.gamepadButton(GLFW_GAMEPAD_BUTTON_DPAD_DOWN) ||
                             input.gamepadButton(GLFW_GAMEPAD_BUTTON_DPAD_RIGHT)));
                // A confirms; Start does NOT. Start is the Menu button now (it
                // opens and closes this very overlay), and letting it confirm as
                // well would fire the focused entry on the same press that opened
                // the menu.
                const bool fireBtn =
                    input.isKeyDown(GLFW_KEY_ENTER) ||
                    input.isKeyDown(GLFW_KEY_KP_ENTER) ||
                    (pad && input.gamepadButton(GLFW_GAMEPAD_BUTTON_A));
                if (prevBtn || nextBtn || fireBtn)
                    std::fprintf(stderr, "[navdbg] prev=%d next=%d fire=%d pad=%d\n",
                                 prevBtn ? 1 : 0, nextBtn ? 1 : 0, fireBtn ? 1 : 0,
                                 pad ? 1 : 0);
                if (prevBtn && !prevUiPrev) uiOverlay.moveFocus(-1);
                if (nextBtn && !prevUiNext) uiOverlay.moveFocus(+1);
                if (fireBtn && !prevUiFire) uiActivate = true;
                prevUiPrev = prevBtn; prevUiNext = nextBtn; prevUiFire = fireBtn;
            } else {
                prevUiPrev = prevUiNext = prevUiFire = false;
                uiActivate = false;
            }

            // Same navigation for the end-of-race question, edge-detected here
            // where the input lives; the HUD pass below draws it and reports the
            // answer. Polled every frame so a key held across the finish line is
            // never read as an answer.
            racehud::EndInput endIn;
            {
                const bool pad = input.hasGamepad();
                const float sx = pad ? input.gamepadStick(GLFW_GAMEPAD_AXIS_LEFT_X) : 0.0f;
                const float sy = pad ? input.gamepadStick(GLFW_GAMEPAD_AXIS_LEFT_Y) : 0.0f;
                const bool prevB =
                    input.isKeyDown(GLFW_KEY_LEFT) || input.isKeyDown(GLFW_KEY_UP) ||
                    sx < -0.5f || sy < -0.5f ||
                    (pad && (input.gamepadButton(GLFW_GAMEPAD_BUTTON_DPAD_LEFT) ||
                             input.gamepadButton(GLFW_GAMEPAD_BUTTON_DPAD_UP)));
                const bool nextB =
                    input.isKeyDown(GLFW_KEY_RIGHT) || input.isKeyDown(GLFW_KEY_DOWN) ||
                    sx > 0.5f || sy > 0.5f ||
                    (pad && (input.gamepadButton(GLFW_GAMEPAD_BUTTON_DPAD_RIGHT) ||
                             input.gamepadButton(GLFW_GAMEPAD_BUTTON_DPAD_DOWN)));
                const bool fireB =
                    input.isKeyDown(GLFW_KEY_ENTER) || input.isKeyDown(GLFW_KEY_KP_ENTER) ||
                    input.isKeyDown(GLFW_KEY_SPACE) ||
                    (pad && input.gamepadButton(GLFW_GAMEPAD_BUTTON_A)); // Start = Menu
                endIn.prev    = prevB && !prevEndPrev;
                endIn.next    = nextB && !prevEndNext;
                endIn.confirm = fireB && !prevEndFire;
                prevEndPrev = prevB; prevEndNext = nextB; prevEndFire = fireB;
            }

            // The grid orbit ends when the player says so, on the same button as
            // the end-of-race question: this game has only ever asked anyone to
            // press one key to get on with it, and it should stay one key.
            if (race.onGrid && endIn.confirm) {
                race.onGrid = race2.onGrid = false;
                raceCountdown = race2.raceCountdown = 3.0f;
                goFlash = race2.goFlash = 0.0f;
                // The chase camera picks up from where the craft is, not from
                // where the orbit left the eye -- otherwise READY opens on a
                // swoop in from the side of the track.
                cams.reset();
            }

            // --- Graphics menu ------------------------------------------------
            // F9 anywhere -- editor, Play, the shipped player -- because a machine
            // that cannot draw the frame has to be fixable from wherever the
            // player happens to be, including a start screen with no menu of its
            // own. An authored pause menu reaches the same screen through the
            // Graphics button action, which is what sets gfxOpenRequest.
            //
            // NOT F10, which was the first choice: Windows treats F10 as
            // "activate the window menu" and DefWindowProc puts the window into
            // its menu loop on the way past GLFW, which is a good way to have a
            // screen open on the key and then ignore every key after it.
            {
                const bool f9 = input.isKeyDown(GLFW_KEY_F9);
                if ((f9 && !prevGfxKey) || gfxOpenRequest) {
                    gfxOpenRequest = false;
                    gfxUi.setOpen(!gfxUi.open());
                    // Free the cursor for the menu, and give it back the way it
                    // was found -- locked only if the game had it locked.
                    input.setCursorLocked(gfxUi.open() ? false : fpsMode && !scriptCursorFree);
                }
                prevGfxKey = f9;
                gfxUi.update(dt);
            }

            const bool escDown = input.isKeyDown(GLFW_KEY_ESCAPE);
            // The graphics menu's own input, edge-detected here where Escape has
            // just been read and while `prevEsc` still holds last frame's state.
            // Keyboard and pad both, for the same reason the scene overlay takes
            // both: Play locks the cursor and a pad has none.
            gfxIn = gfxmenu::Input{};
            if (gfxUi.open()) {
                const bool pad = input.hasGamepad();
                const float sx = pad ? input.gamepadStick(GLFW_GAMEPAD_AXIS_LEFT_X) : 0.0f;
                const float sy = pad ? input.gamepadStick(GLFW_GAMEPAD_AXIS_LEFT_Y) : 0.0f;
                const bool up = input.isKeyDown(GLFW_KEY_UP) ||
                                (pad && (input.gamepadButton(GLFW_GAMEPAD_BUTTON_DPAD_UP) ||
                                         sy < -0.5f));
                const bool dn = input.isKeyDown(GLFW_KEY_DOWN) ||
                                (pad && (input.gamepadButton(GLFW_GAMEPAD_BUTTON_DPAD_DOWN) ||
                                         sy > 0.5f));
                const bool lf = input.isKeyDown(GLFW_KEY_LEFT) ||
                                (pad && (input.gamepadButton(GLFW_GAMEPAD_BUTTON_DPAD_LEFT) ||
                                         sx < -0.5f));
                const bool rt = input.isKeyDown(GLFW_KEY_RIGHT) ||
                                (pad && (input.gamepadButton(GLFW_GAMEPAD_BUTTON_DPAD_RIGHT) ||
                                         sx > 0.5f));
                const bool fire = input.isKeyDown(GLFW_KEY_ENTER) ||
                                  (pad && input.gamepadButton(GLFW_GAMEPAD_BUTTON_A));
                gfxIn.up      = up   && !prevGfxUp;
                gfxIn.down    = dn   && !prevGfxDown;
                gfxIn.left    = lf   && !prevGfxLeft;
                gfxIn.right   = rt   && !prevGfxRight;
                gfxIn.confirm = fire && !prevGfxFire;
                gfxIn.back    = (escDown && !prevEsc) ||
                                (pad && input.gamepadButton(GLFW_GAMEPAD_BUTTON_B));
                prevGfxUp = up; prevGfxDown = dn; prevGfxLeft = lf;
                prevGfxRight = rt; prevGfxFire = fire;
            } else {
                prevGfxUp = prevGfxDown = prevGfxLeft = prevGfxRight = false;
                prevGfxFire = false;
            }

            // A menu on Escape owns the key outright -- that is what stops Escape
            // from dropping the player straight out of the game.
            const bool escIsMenuKey =
                uiMenuArmed && uiOverlay.toggleKey() == GLFW_KEY_ESCAPE;
            // ...and the graphics menu owns it while IT is up, for the same
            // reason: without this the press that closes it would also drop the
            // player out of the game behind it.
            // ...and so does a script's own menu while it holds the keys
            // (game.captureInput): Esc closes the inventory, not the game.
            if (escDown && !prevEsc && !escIsMenuKey && !gfxUi.open() &&
                !scripts.inputCaptured()) {
                if (playerMode)          { window.requestClose(); }
                else if (presentMode) {
                    presentMode = false;
                    window.setFullscreen(false);
                } else if (playMode)     { stopPlay(); }
                else if (vehicleMode)    { vehicleMode = false; endEditorDrive(); }
                else if (gliderMode)     { gliderMode = false; endGliderDrive(); }
                else if (fpsMode) { fpsMode = false; input.setCursorLocked(false); }
                // Modelling: Esc only ever cancels an operation (ModelingKeys).
                else if (modelling) {}
                // Plain editor: Esc steps back to selection (drop the transform
                // tool), then a second Esc clears the selection. Never quits.
                // A road point selection is the innermost thing to let go of, so
                // it clears first -- the bridge pair with it.
                else if (viewTool == ViewTool::Road && roadSel >= 0) { roadSel = roadSel2 = -1; }
                else if (viewTool == ViewTool::Spline && splinePtSel >= 0) { splinePtSel = -1; }
                else if (viewTool == ViewTool::River && riverPtSel >= 0) { riverPtSel = -1; }
                else if (placeMode) { placeMode = false; }
                else if (entityEditMode) { entityEditMode = false; }
                else if (sel.valid()) { sel.clear(); }
            }
            prevEsc = escDown;

            // Transform-tool shortcuts (Blender/Unity-style): Q/W/E pick the gizmo
            // and bring it back if Esc dropped it. They do not touch Select/Create
            // -- reaching for a handle is not asking for a new object, and the
            // shape toolbar is the only thing that arms placing. Only in the
            // plain editor, never while a
            // camera-fly drag (right mouse) or a text field owns the keys.
            if (!playMode && !fpsMode && !vehicleMode && !gliderMode && !presentMode &&
                !modelling && !ImGui::GetIO().WantTextInput &&
                !input.isMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)) {
                const bool qd = input.isKeyDown(GLFW_KEY_Q);
                const bool wd = input.isKeyDown(GLFW_KEY_W);
                const bool ed = input.isKeyDown(GLFW_KEY_E);
                const bool xd = input.isKeyDown(GLFW_KEY_X);
                if (qd && !prevQkey) { gizmoOp = ImGuizmo::TRANSLATE; entityEditMode = true; }
                if (wd && !prevWkey) { gizmoOp = ImGuizmo::ROTATE;    entityEditMode = true; }
                if (ed && !prevEkey) { gizmoOp = ImGuizmo::SCALE;     entityEditMode = true; }
                if (xd && !prevXkey) // toggle the gizmo's reference frame
                    gizmoMode = (gizmoMode == ImGuizmo::WORLD) ? ImGuizmo::LOCAL
                                                              : ImGuizmo::WORLD;
                prevQkey = qd; prevWkey = wd; prevEkey = ed; prevXkey = xd;
            } else { prevQkey = prevWkey = prevEkey = prevXkey = false; }

            // Undo / redo: Ctrl+Z, Ctrl+Y or Ctrl+Shift+Z. Suppressed while a
            // text field has focus (so typing a name doesn't undo the scene).
            if (!playMode && !ImGui::GetIO().WantTextInput) {
                const bool ctrl  = input.isKeyDown(GLFW_KEY_LEFT_CONTROL) ||
                                   input.isKeyDown(GLFW_KEY_RIGHT_CONTROL);
                const bool shift = input.isKeyDown(GLFW_KEY_LEFT_SHIFT) ||
                                   input.isKeyDown(GLFW_KEY_RIGHT_SHIFT);
                const bool z = input.isKeyDown(GLFW_KEY_Z);
                const bool y = input.isKeyDown(GLFW_KEY_Y);
                const bool wantUndo = ctrl && z && !shift;
                const bool wantRedo = ctrl && ((z && shift) || y);
                if (wantUndo && !prevUndo) { history.undo(document); sel.clear(); clampRoadSel(); clampSplineSel(); clampRiverSel(); }
                if (wantRedo && !prevRedo) { history.redo(document); sel.clear(); clampRoadSel(); clampSplineSel(); clampRiverSel(); }
                prevUndo = wantUndo;
                prevRedo = wantRedo;
            } else {
                prevUndo = prevRedo = false;
            }
#ifndef FITZEL_PLAYER
            // Save: Ctrl+S, the same as the toolbar's first button and File >
            // Save Project. Not in Play -- the scene is the game's then, and
            // saving would write the game's state over the one authored -- and
            // not while a text field or the script editor has the keyboard (the
            // script editor's own Ctrl+S saves the script).
            {
                const bool ctrl = input.isKeyDown(GLFW_KEY_LEFT_CONTROL) ||
                                  input.isKeyDown(GLFW_KEY_RIGHT_CONTROL);
                const bool shift = input.isKeyDown(GLFW_KEY_LEFT_SHIFT) ||
                                   input.isKeyDown(GLFW_KEY_RIGHT_SHIFT);
                const bool wantSave = ctrl && !shift && input.isKeyDown(GLFW_KEY_S) &&
                                      !playMode && !ImGui::GetIO().WantTextInput &&
                                      !scriptEditor.focused();
                if (wantSave && !prevSave && !currentProject.empty()) saveCurrent();
                prevSave = wantSave;
            }
#endif

            engineDriving = false; // re-armed by whichever drive block runs below
            gliderAudioActive = false; // re-armed by the glider flight tick below
            blurAnchorValid = false;   // re-armed by whichever chase-cam block runs
            blurSpeed01     = 0.0f;    // ...along with the craft's speed for the blur
            carWaterSub   = 0.0f;  // re-armed by the buoyancy block when submerged

            // Bundle the loop state the arcade racing sim reads, once per frame
            // (used by the car/glider dispatch here and the opponents update far
            // below). Member order must match racesim::RaceEnv.
            // One player's controls, resolved from the device that seat uses.
            // Seat 0 flies with the pad if one is plugged in, and WASD either
            // way; seat 1 is on the arrow keys. Two seats at one machine, which
            // is what "split screen" means here -- see RaceControls.
            auto controlsFor = [&](int seat) {
                racesim::RaceControls c;
                if (seat == 0) {
                    c.throttle = (input.isKeyDown(GLFW_KEY_W) ? 1.0f : 0.0f)
                               - (input.isKeyDown(GLFW_KEY_S) ? 1.0f : 0.0f);
                    c.steer    = (input.isKeyDown(GLFW_KEY_D) ? 1.0f : 0.0f)
                               - (input.isKeyDown(GLFW_KEY_A) ? 1.0f : 0.0f);
                    c.brake    = input.isKeyDown(GLFW_KEY_SPACE);
                    c.boost    = input.isKeyDown(GLFW_KEY_LEFT_SHIFT);
                    // Pad: RT accelerate / LT reverse, left stick steers, B
                    // brakes, A boosts.
                    if (input.hasGamepad()) {
                        c.throttle = glm::clamp(c.throttle
                            + input.gamepadTrigger(GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER)
                            - input.gamepadTrigger(GLFW_GAMEPAD_AXIS_LEFT_TRIGGER),
                            -1.0f, 1.0f);
                        c.steer = glm::clamp(
                            c.steer + input.gamepadStick(GLFW_GAMEPAD_AXIS_LEFT_X),
                            -1.0f, 1.0f);
                        if (input.gamepadButton(GLFW_GAMEPAD_BUTTON_B)) c.brake = true;
                        if (input.gamepadButton(GLFW_GAMEPAD_BUTTON_A)) c.boost = true;
                    }
                } else {
                    c.throttle = (input.isKeyDown(GLFW_KEY_UP)    ? 1.0f : 0.0f)
                               - (input.isKeyDown(GLFW_KEY_DOWN)  ? 1.0f : 0.0f);
                    c.steer    = (input.isKeyDown(GLFW_KEY_RIGHT) ? 1.0f : 0.0f)
                               - (input.isKeyDown(GLFW_KEY_LEFT)  ? 1.0f : 0.0f);
                    c.brake    = input.isKeyDown(GLFW_KEY_RIGHT_CONTROL);
                    c.boost    = input.isKeyDown(GLFW_KEY_RIGHT_SHIFT);
                }
                return c;
            };

            racesim::RaceEnv raceEnv{
                input, controlsFor(0), document, entities, streamer, roads.active(),
                driveVehicleId, driveGliderId, driveGliderId2, driveBackup,
                dt, kSimH, simAlpha, simSteps,
                (gliderMode ? gliderPos : carPos),               // player world pos
                (gliderMode ? gliderSpeedMps : engineSpeedMps),  // player speed
                (playMode && (vehicleMode || gliderMode)),       // a craft is driven
                setWorld, parentWorldMat, gliderGround, playBoostPunch, playCue,
            };

            // --- Showroom: the scene IS the start screen ---------------------
            // Poses the craft on the podium and drives the camera; the picker
            // itself is drawn in the HUD pass, which is where the answer comes
            // back. Runs before the control chain below, which it then owns.
            if (showroomUi.active()) {
                showroom::Input shIn;
                {
                    const bool pad = input.hasGamepad();
                    const float sx = pad ? input.gamepadStick(GLFW_GAMEPAD_AXIS_LEFT_X) : 0.0f;
                    const float sy = pad ? input.gamepadStick(GLFW_GAMEPAD_AXIS_LEFT_Y) : 0.0f;
                    const bool l = input.isKeyDown(GLFW_KEY_LEFT) || input.isKeyDown(GLFW_KEY_A) ||
                                   sx < -0.5f ||
                                   (pad && input.gamepadButton(GLFW_GAMEPAD_BUTTON_DPAD_LEFT));
                    const bool r = input.isKeyDown(GLFW_KEY_RIGHT) || input.isKeyDown(GLFW_KEY_D) ||
                                   sx > 0.5f ||
                                   (pad && input.gamepadButton(GLFW_GAMEPAD_BUTTON_DPAD_RIGHT));
                    const bool u = input.isKeyDown(GLFW_KEY_UP) || input.isKeyDown(GLFW_KEY_W) ||
                                   sy < -0.5f ||
                                   (pad && input.gamepadButton(GLFW_GAMEPAD_BUTTON_DPAD_UP));
                    const bool d = input.isKeyDown(GLFW_KEY_DOWN) || input.isKeyDown(GLFW_KEY_S) ||
                                   sy > 0.5f ||
                                   (pad && input.gamepadButton(GLFW_GAMEPAD_BUTTON_DPAD_DOWN));
                    const bool f = input.isKeyDown(GLFW_KEY_ENTER) ||
                                   input.isKeyDown(GLFW_KEY_KP_ENTER) ||
                                   input.isKeyDown(GLFW_KEY_SPACE) ||
                                   (pad && input.gamepadButton(GLFW_GAMEPAD_BUTTON_A));
                    // Start is the Menu button, not a confirm -- see the scene
                    // overlay's toggle above.
                    shIn.left = l && !prevShLeft;  shIn.right   = r && !prevShRight;
                    shIn.up   = u && !prevShUp;    shIn.down    = d && !prevShDown;
                    shIn.confirm = f && !prevShFire;
                    // `back` stays unwired: Esc already leaves Play (or quits the
                    // player) in the key handler above, and answering it twice
                    // would leave the scene half torn down.
                    prevShLeft = l; prevShRight = r; prevShUp = u;
                    prevShDown = d; prevShFire = f;
                }
                showroomUi.update(entities, camera, dt, shIn);
                for (const showroom::Cue& c : showroomUi.takeCues())
                    playCue(c.sound, c.gain, c.pitch);
            }

            // An orbit camera as the view owns the mouse (the branch below). When
            // it stops being the view, the cursor it held is let go again --
            // unless the walking player holds it for its own look.
            const bool orbitView = playMode && [&] {
                const Entity* ce = activeCam >= 0 ? document.find(activeCam) : nullptr;
                const auto* cc = ce ? ce->components.get<CameraComponent>() : nullptr;
                return cc && ce->activeInHierarchy && ce->parent >= 0 &&
                       cc->mode == CameraComponent::Follow && cc->orbitMouse;
            }();
            if (!orbitView && orbitHeldCursor) {
                orbitHeldCursor = false;
                if (!fpsMode) input.setCursorLocked(false);
            }

            if (gfxUi.open()) {
                // The graphics menu owns the frame: no look, no walking, no
                // driving. (The world keeps ticking, like the scene's own menu --
                // a settings screen that froze the picture behind it could not
                // show what the settings do to it.)
            } else if (showroomUi.active()) {
                // The picker owns the frame: no look, no walking, no driving.
            } else if (uiMenuOpen) {
                // The scene's menu is open: it owns mouse and keyboard, so no
                // look, no walking, no driving until it is closed again. (The
                // world keeps ticking -- this is a menu, not a pause.)
            } else if (vehicleMode && playMode && physics && physics->hasVehicle()) {
                // Physics car: WASD -> engine/steer/brake; chase camera from the
                // chassis. The vehicle updates during the physics step below.
                float fwdIn = (input.isKeyDown(GLFW_KEY_W) ? 1.0f : 0.0f) -
                              (input.isKeyDown(GLFW_KEY_S) ? 1.0f : 0.0f);
                float steerIn = (input.isKeyDown(GLFW_KEY_D) ? 1.0f : 0.0f) -
                                (input.isKeyDown(GLFW_KEY_A) ? 1.0f : 0.0f);
                float brake     = input.isKeyDown(GLFW_KEY_SPACE) ? 1.0f : 0.0f;
                float handBrake = 0.0f;
                // Gamepad (Xbox): RT accelerate, LT reverse, left stick steer,
                // A / right-bumper handbrake, B foot-brake. Added to the keyboard
                // inputs and clamped, so either can drive.
                if (input.hasGamepad()) {
                    fwdIn = glm::clamp(fwdIn
                        + input.gamepadTrigger(GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER)
                        - input.gamepadTrigger(GLFW_GAMEPAD_AXIS_LEFT_TRIGGER), -1.0f, 1.0f);
                    steerIn = glm::clamp(
                        steerIn + input.gamepadStick(GLFW_GAMEPAD_AXIS_LEFT_X), -1.0f, 1.0f);
                    if (input.gamepadButton(GLFW_GAMEPAD_BUTTON_A) ||
                        input.gamepadButton(GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER))
                        handBrake = 1.0f;
                    if (input.gamepadButton(GLFW_GAMEPAD_BUTTON_B)) brake = 1.0f;
                }
                // Ease the steer input toward the target at the component's steer
                // speed so the wheels don't snap to full lock in a single frame.
                Entity* sv  = (driveVehicleId >= 0) ? document.find(driveVehicleId) : nullptr;
                auto*   svc = sv ? sv->components.get<VehicleComponent>() : nullptr;
                const float steerSpd = svc ? svc->steerSpeed : 7.0f;
                physSteer += (steerIn - physSteer) * std::min(1.0f, dt * steerSpd);

                // Chassis state up front: the water/boat decision needs it before we
                // choose which control scheme (wheels vs boat) to feed the sim.
                glm::vec3 cp(0.0f); glm::quat cq(1.0f, 0.0f, 0.0f, 0.0f);
                physics->getTransform(physCarId, cp, cq);
                carPos = cp; // mirror the chassis so opponents can see the player
                blurAnchorWorld = cp; blurAnchorValid = true; // keep the car sharp
                glm::vec3 vel(0.0f);
                physics->getLinearVelocity(physCarId, vel);
                blurSpeed01 = glm::clamp(glm::length(vel) / 40.0f, 0.0f, 1.2f);
                const float halfH = svc ? glm::max(svc->chassisHalf.y, 0.1f) : 0.5f;
                const float mass  = svc ? glm::max(svc->mass, 1.0f)         : 1200.0f;
                // Running water counts as water. A brook sits ABOVE the lake's
                // level, not below it, so what the chassis is in is whichever
                // surface is higher -- and if it is the brook, it is also pushing.
                float surfY = waterLevel;
                glm::vec2 flowXZ(0.0f);
                {
                    float rSurf = 0.0f;
                    glm::vec2 rFlow(0.0f);
                    if (rivers.sample(glm::vec2(cp.x, cp.z), rSurf, nullptr, &rFlow) &&
                        rSurf > surfY) {
                        surfY  = rSurf;
                        flowXZ = rFlow;
                    }
                }
                const float depth = surfY - (cp.y - halfH);
                const float sub   = (depth > 0.0f)
                                  ? glm::clamp(depth / (2.0f * halfH), 0.0f, 1.0f) : 0.0f;
                // Resting submersion (the boat's float line, inspector-tunable). The
                // boat-mode thresholds ride relative to it so a high-floating boat
                // still engages. Hysteresis keeps the shoreline from flip-flopping.
                const float kFloat = svc ? glm::clamp(svc->boatFloat, 0.12f, 0.92f) : 0.45f;
                if      (sub > kFloat * 0.85f) boatMode = true;
                else if (sub < kFloat * 0.5f)  boatMode = false;

                const glm::vec3 fwd   = cq * glm::vec3(0.0f, 0.0f, 1.0f);
                const glm::vec3 right = cq * glm::vec3(1.0f, 0.0f, 0.0f);

                if (boatMode) {
                    // Motorboat: the wheels idle in the water. W/S is thrust along the
                    // flat heading, A/D yaw the hull, and a keel drag resists sideways
                    // slip so the boat tracks where its nose points.
                    physics->setVehicleInput(0.0f, 0.0f, 0.0f, 0.0f);
                    glm::vec3 fwdFlat(fwd.x, 0.0f, fwd.z);
                    if (glm::length(fwdFlat) > 1e-3f)   fwdFlat   = glm::normalize(fwdFlat);
                    glm::vec3 rightFlat(right.x, 0.0f, right.z);
                    if (glm::length(rightFlat) > 1e-3f) rightFlat = glm::normalize(rightFlat);
                    const float boatThrust = svc ? svc->boatThrust : 15.0f; // m/s^2
                    physics->applyImpulse(physCarId,
                        fwdFlat * (fwdIn * boatThrust * mass * dt));
                    // Yaw steering: turn better with some way on (a boat needs water
                    // flowing past the hull); reverse thrust steers the stern around.
                    const float fwdSpeed = glm::dot(vel, fwdFlat);
                    const float turnAuth = glm::clamp(0.4f + std::abs(fwdSpeed) * 0.12f,
                                                      0.4f, 1.2f);
                    glm::vec3 av(0.0f);
                    physics->getAngularVelocity(physCarId, av);
                    const float yawTarget = steerIn * 1.6f * turnAuth
                                          * (fwdSpeed < -0.2f ? -1.0f : 1.0f);
                    av.y = glm::mix(av.y, yawTarget, std::min(1.0f, dt * 4.0f));
                    // Keep the hull level on the water: steer pitch/roll back to flat
                    // (up x worldUp is the tilt axis) so it doesn't nose-dive or rear
                    // up under thrust. Yaw (av.y) is left to the steering above.
                    const glm::vec3 up   = cq * glm::vec3(0.0f, 1.0f, 0.0f);
                    const glm::vec3 tilt = glm::cross(up, glm::vec3(0.0f, 1.0f, 0.0f));
                    av.x = glm::mix(av.x, tilt.x * 4.0f, std::min(1.0f, dt * 3.0f));
                    av.z = glm::mix(av.z, tilt.z * 4.0f, std::min(1.0f, dt * 3.0f));
                    physics->setAngularVelocity(physCarId, av);
                    // Keel: strongly damp sideways drift, lightly damp forward glide.
                    const float latV = glm::dot(vel, rightFlat);
                    physics->applyImpulse(physCarId, rightFlat * (-latV * 3.0f * mass * dt));
                    physics->applyImpulse(physCarId, fwdFlat  * (-fwdSpeed * 0.5f * mass * dt));
                    engineThrottle = std::abs(fwdIn);
                } else {
                    // Standing (or all but) with no pedal pressed, the car holds
                    // where it is instead of creeping off down whatever slope it
                    // stopped on -- with nobody's foot on anything, a parked car
                    // rolled away while its driver was still getting out.
                    if (std::abs(fwdIn) < 0.05f && glm::length(glm::vec2(vel.x, vel.z)) < 1.0f)
                        handBrake = 1.0f;
                    physics->setVehicleInput(fwdIn, physSteer, brake, handBrake);
                    engineThrottle = std::abs(fwdIn);
                }
                physThrottle = fwdIn;

                // Feed the engine sound from the chassis' horizontal speed.
                engineDriving  = true;
                engineSpeedMps = glm::length(glm::vec2(vel.x, vel.z));
                engineWheelR   = svc ? svc->wheelRadius : 0.42f;

                // --- Water: buoyancy + splash/ambience ---------------------------
                // Vertical buoyancy floats the chassis toward the surface; the boat
                // path supplies its own keel/forward drag, so only the wading (car)
                // path gets the generic horizontal drag here.
                if (depth > 0.0f) {
                    // Stable float line: buoyant accel equals gravity at sub==kFloat
                    // (the inspector-tunable rest submersion computed above), so the
                    // chassis settles there instead of being shoved to the surface.
                    float up = 9.81f * (sub / kFloat) - 3.2f * vel.y;
                    up = glm::max(up, 0.0f);
                    physics->applyImpulse(physCarId, glm::vec3(0.0f, up * mass * dt, 0.0f));
                    if (!boatMode) {
                        const glm::vec3 hv(vel.x, 0.0f, vel.z);
                        physics->applyImpulse(physCarId, -hv * (1.3f * mass * dt));
                    }
                    // The current carries what floats in it. Scaled by how much
                    // of the hull is actually in the water, so a car with its roof
                    // out is nudged and a boat is taken.
                    if (flowXZ != glm::vec2(0.0f)) {
                        const glm::vec3 want(flowXZ.x, 0.0f, flowXZ.y);
                        const glm::vec3 rel = want - glm::vec3(vel.x, 0.0f, vel.z);
                        physics->applyImpulse(physCarId, rel * (1.6f * sub * mass * dt));
                    }
                    carWaterSub = sub;
                    // Foam: a flat surface layer clinging to the waterline (a gentle
                    // ring even at rest, a trailing wake when moving) plus airborne
                    // droplets that only fly when the hull is actually moving.
                    if (spray.ready()) {
                        const float hspeed = glm::length(glm::vec2(vel.x, vel.z));
                        const bool  moving = hspeed > 1.0f;
                        glm::vec3 vdir = (hspeed > 0.2f)
                            ? glm::normalize(glm::vec3(vel.x, 0.0f, vel.z))
                            : glm::normalize(glm::vec3(fwd.x, 0.0f, fwd.z) + glm::vec3(1e-4f));
                        const glm::vec3 sideV(-vdir.z, 0.0f, vdir.x);
                        const glm::vec3 hx = svc ? svc->chassisHalf : glm::vec3(0.9f, 0.35f, 2.0f);
                        const float sAmt = svc ? glm::max(svc->sprayAmount, 0.0f) : 1.0f;
                        const float sHgt = svc ? glm::max(svc->sprayHeight, 0.0f) : 1.0f;
                        spray.sizeScale  = svc ? glm::max(svc->spraySize, 0.05f) : 1.0f;
                        std::uniform_real_distribution<float> u(0.0f, 1.0f);
                        auto rnd = [&]{ return u(sprayRng); };

                        // --- Surface foam: hugs the water around the hull, drifts and
                        // spreads. Particles/sec: a gentle ring at rest, more with speed.
                        foamAccum += (28.0f + hspeed * 22.0f) * sub * sAmt * dt;
                        while (foamAccum >= 1.0f &&
                               spray.count() < SprayPool::kMax) {
                            foamAccum -= 1.0f;
                            SprayP p; p.flat = 1.0f;
                            const float ang = rnd() * 6.2831853f;
                            const float rad = glm::mix(0.5f, 1.15f, rnd());
                            // Ring around the hull footprint, biased to the stern wake.
                            glm::vec3 off = sideV * (std::cos(ang) * hx.x * rad)
                                          + vdir  * (std::sin(ang) * hx.z * rad
                                                     - hspeed * 0.06f);
                            p.pos = cp + off; p.pos.y = surfY + 0.03f;
                            p.vel = sideV * ((rnd() - 0.5f) * 1.2f)
                                  - vdir * (moving ? hspeed * 0.15f : 0.0f);
                            p.vel.y = 0.0f;
                            p.life = p.life0 = glm::mix(0.9f, 1.9f, rnd());
                            p.size = glm::mix(3.0f, 6.0f, rnd());
                            spray.add(p);
                        }

                        // --- Airborne droplets: only when moving, plus an entry burst.
                        if (moving)
                            sprayAccum += (hspeed - 1.0f) * sub * 45.0f * sAmt * dt;
                        int burst = (!carInWater) ? static_cast<int>(30 * sAmt) : 0;
                        while ((burst-- > 0 || sprayAccum >= 1.0f) &&
                               spray.count() < SprayPool::kMax) {
                            if (burst < 0) sprayAccum -= 1.0f;
                            SprayP p;
                            const float sway = (rnd() - 0.5f) * 2.0f;
                            p.pos = cp + vdir * (hx.z * 0.5f) + sideV * (sway * hx.x);
                            p.pos.y = surfY + 0.05f;
                            p.vel = glm::vec3(0.0f, glm::mix(2.0f, 4.5f, rnd()) * sHgt, 0.0f)
                                  + sideV * (sway * 3.0f)
                                  + vdir * (hspeed * 0.25f + rnd());
                            p.life = p.life0 = glm::mix(0.35f, 0.8f, rnd());
                            p.size = glm::mix(0.55f, 1.2f, rnd());
                            spray.add(p);
                        }
                    }
                    // Splash once on entry, scaled a touch by impact speed.
                    if (!carInWater) {
                        splashSnd.setVolume(glm::clamp(
                            0.5f + std::abs(vel.y) * 0.15f, 0.5f, 1.0f) * mix.sfxGain());
                        splashSnd.play();
                        carInWater = true;
                    }
                } else {
                    carInWater = false;
                }

                // (The chase camera used to be computed here, a third time, from
                // a third copy of the same five knobs. It is the vehicle's own
                // camera entity now -- see CameraSystem.)
            } else if (vehicleMode) {
                // Arcade car: fixed-step bicycle-model sim + interpolated chase
                // camera. (racesim::updateArcadeCar in RaceSim.cpp.)
                racesim::updateArcadeCar(race, raceEnv);
            } else if (gliderMode && driveGliderId >= 0) {
                // The difficulty step's half that belongs to the player -- how
                // hard a hit bites, how fast the hull heals, how much boost comes
                // back. Pushed EVERY frame rather than once as the race starts,
                // because a RaceState is reset down four different paths (a
                // restart, entering glider mode, seating player two, a launch off
                // the start screen) and a value written once is one of them away
                // from being silently dropped. It is three assignments.
                difficulty::applyToPlayer(race, sessionRaceLevel);
                // Wipeout-style hover racer: fixed-step flight sim, boost pads,
                // gate/checkpoint/lap logic, hover spring, interpolated chase cam.
                // (racesim::updateGlider in RaceSim.cpp.)
                racesim::updateGlider(race, raceEnv);

                // Player two flies the same sim with its own state, its own
                // controls and its own eye. Seat it in the first other craft in
                // the scene: a two-player track is laid out with two craft on
                // it, and asking which is whose before flying is a dialog nobody
                // wants. Released when the second pane closes, so the craft goes
                // back to being scenery (or an opponent).
                if (splitScreen) {
                    // The pane was opened mid-race (or the craft was deleted):
                    // seat player two in whatever is free. It takes over the
                    // craft WHERE IT STANDS -- the grid is long behind everyone
                    // by now, and hauling a craft to the line mid-lap would be a
                    // stranger thing to watch than a second player joining from
                    // the pit lane. Line both up by restarting the race.
                    if (driveGliderId2 < 0 || !document.find(driveGliderId2)) {
                        driveGliderId2 = pickPlayerTwo(driveGliderId);
                        // Seat the state where the craft actually stands. Without
                        // this the fresh RaceState starts at the origin and drags
                        // both the craft and its camera there.
                        if (driveGliderId2 >= 0) {
                            race2 = racesim::RaceState{};
                            seatGliderState(race2, driveGliderId2);
                            if (Entity* e2 = document.find(driveGliderId2))
                                gliderBackup.push_back(*e2);  // restored on exit
                        }
                    }
                    if (driveGliderId2 >= 0) {
                        racesim::RaceEnv env2{
                            input, controlsFor(1), document, entities,
                            streamer, roads.active(),
                            -1, driveGliderId2, driveGliderId, driveBackup,
                            dt, kSimH, simAlpha, simSteps,
                            race2.gliderPos, race2.gliderSpeedMps, true,
                            setWorld, parentWorldMat, gliderGround, playBoostPunch,
                            playCue,
                        };
                        difficulty::applyToPlayer(race2, sessionRaceLevel);
                        racesim::updateGlider(race2, env2);
                    }
                } else {
                    driveGliderId2 = -1;
                }
            } else if (orbitView && (scriptCursorFree || scripts.inputCaptured())) {
                // A script's menu has the pointer (game.showCursor) or the keys
                // (game.captureInput): the orbit camera holds still and leaves
                // the cursor free, or picking an item would swing the view.
            } else if (orbitView) {
                // An orbit camera is the view: the mouse (and the right stick)
                // swing it round the object it follows. The walking player
                // stands aside, as it does for a script's eye -- WASD belongs to
                // the figure now, and a capsule wandering off would drag the
                // streaming with it -- and the cursor is held, as in any
                // third-person game. Esc still leaves Play.
                if (!input.isCursorLocked()) input.setCursorLocked(true);
                orbitHeldCursor = true;
                glm::vec2 d = input.mouseDelta();
                if (input.hasGamepad()) {
                    const float look = 1200.0f * dt;
                    d += glm::vec2( input.gamepadStick(GLFW_GAMEPAD_AXIS_RIGHT_X) * look,
                                   -input.gamepadStick(GLFW_GAMEPAD_AXIS_RIGHT_Y) * look);
                }
                cams.steer(activeCam, d);
            } else if (fpsMode && scriptOwnsEye) {
                // A script flies the eye (see host.setCamPos): the walking player
                // stands aside rather than pull the camera back to its capsule --
                // everything streamed from here on (terrain, trees, grass) would
                // gather round the capsule instead of the picture.
            } else if (fpsMode) {
                // Mouse look is always active; movement is on the ground plane.
                const glm::vec2 d = input.mouseDelta();
                camera.processMouse(d.x, d.y);
                // Gamepad right stick looks around (~120 deg/s at full deflection;
                // scaled by dt into the same pixel-delta units processMouse expects).
                if (input.hasGamepad()) {
                    const float look = 1200.0f * dt;
                    camera.processMouse(
                         input.gamepadStick(GLFW_GAMEPAD_AXIS_RIGHT_X) * look,
                        -input.gamepadStick(GLFW_GAMEPAD_AXIS_RIGHT_Y) * look);
                }

                // Head-bob: turn the eye's real ground speed into a springy footstep
                // motion. Returns the movement result with the eye offset folded in;
                // both walk paths (physics + simple) apply it at their setPosition.
                bobClock += dt;
                auto applyHeadBob = [&](glm::vec3 basePos, bool onGround) -> glm::vec3 {
                    // Real horizontal speed from how far the eye actually moved this
                    // frame -- so walking into a wall stops the bob, not only letting
                    // go of the key.
                    const glm::vec2 xz(basePos.x, basePos.z);
                    const float dist  = glm::length(xz - walkPrevXZ);
                    const float speed = (dt > 1e-5f) ? dist / dt : 0.0f;
                    walkPrevXZ = xz;
                    // Gate the bob on only while moving on the ground, and ease it in/
                    // out so starting, stopping and jumping never snap.
                    const float nominal = glm::max(0.5f, camera.moveSpeed);
                    const float target  = (onGround && speed > 0.15f)
                                        ? glm::clamp(speed / nominal, 0.0f, 1.15f) : 0.0f;
                    const float rate = (target > bobAmt) ? 9.0f : 6.0f;
                    bobAmt += (target - bobAmt) * glm::clamp(rate * dt, 0.0f, 1.0f);
                    // Advance the stride phase by distance walked, so cadence tracks
                    // speed and is framerate-independent (~0.48 strides per metre --
                    // an unhurried walk, not a jog).
                    bobPhase += dist * 0.48f * 6.2831853f;
                    const float p = bobPhase;
                    // Break the metronome so it doesn't read as a pure sine: two slow
                    // incommensurate terms wander the intensity/cadence, and a 1x-per
                    // -stride term makes alternating footfalls uneven (a real gait is
                    // never perfectly symmetric left/right).
                    const float wob  = 1.0f + 0.20f * std::sin(p * 0.53f + 0.7f)
                                            + 0.13f * std::sin(p * 0.31f + 2.1f);
                    const float asym = 0.16f * std::sin(p); // uneven left/right dip
                    // Two vertical dips per stride (one per footfall) + the asymmetry,
                    // one lateral sway; amplitudes in metres, scaled by the gate.
                    const float vy = (std::sin(p * 2.0f) + asym) * 0.052f * wob * bobAmt;
                    const float hx = std::cos(p) * 0.046f
                                   * (1.0f + 0.16f * std::sin(p * 0.47f + 0.3f)) * bobAmt;
                    // A whisper of vertical breathing when essentially still, so a
                    // standing player isn't a dead-locked tripod.
                    const float breathe = std::sin(bobClock * 1.4f) * 0.006f * (1.0f - bobAmt);
                    glm::vec3 rt = camera.right(); rt.y = 0.0f;
                    if (glm::length(rt) > 1e-4f) rt = glm::normalize(rt);
                    bobOffset = glm::vec3(0.0f, vy + breathe, 0.0f) + rt * hx;
                    return basePos + bobOffset;
                };

                if (playMode && physics && physics->hasCharacter()) {
                    // Physics character controller: collides with the terrain
                    // heightfield and every rigid body in the world.
                    glm::vec3 cf = camera.front(); cf.y = 0.0f;
                    glm::vec3 cr = camera.right(); cr.y = 0.0f;
                    if (glm::length(cf) > 1e-4f) cf = glm::normalize(cf);
                    if (glm::length(cr) > 1e-4f) cr = glm::normalize(cr);
                    glm::vec3 mv(0.0f);
                    if (input.isKeyDown(GLFW_KEY_W)) mv += cf;
                    if (input.isKeyDown(GLFW_KEY_S)) mv -= cf;
                    if (input.isKeyDown(GLFW_KEY_D)) mv += cr;
                    if (input.isKeyDown(GLFW_KEY_A)) mv -= cr;
                    if (input.hasGamepad()) { // left stick walks (analog)
                        mv += cf * -input.gamepadStick(GLFW_GAMEPAD_AXIS_LEFT_Y);
                        mv += cr *  input.gamepadStick(GLFW_GAMEPAD_AXIS_LEFT_X);
                    }
                    if (glm::length(mv) > 1.0f) mv = glm::normalize(mv);
                    const bool space = input.isKeyDown(GLFW_KEY_SPACE) ||
                                       input.gamepadButton(GLFW_GAMEPAD_BUTTON_A);
                    const bool jump  = space && !prevSpace;
                    prevSpace = space;
                    bool onGround = false;
                    const glm::vec3 foot = physics->moveCharacter(
                        mv * camera.moveSpeed, jump, dt, onGround);
                    grounded = onGround;
                    camera.setPosition(applyHeadBob(
                        glm::vec3(foot.x, foot.y + eyeHeight, foot.z), onGround));
                } else {
                glm::vec3 fwd = camera.front(); fwd.y = 0.0f;
                glm::vec3 rgt = camera.right(); rgt.y = 0.0f;
                if (glm::length(fwd) > 1e-4f) fwd = glm::normalize(fwd);
                if (glm::length(rgt) > 1e-4f) rgt = glm::normalize(rgt);
                glm::vec3 move(0.0f);
                if (input.isKeyDown(GLFW_KEY_W)) move += fwd;
                if (input.isKeyDown(GLFW_KEY_S)) move -= fwd;
                if (input.isKeyDown(GLFW_KEY_D)) move += rgt;
                if (input.isKeyDown(GLFW_KEY_A)) move -= rgt;
                if (input.hasGamepad()) { // left stick walks (analog)
                    move += fwd * -input.gamepadStick(GLFW_GAMEPAD_AXIS_LEFT_Y);
                    move += rgt *  input.gamepadStick(GLFW_GAMEPAD_AXIS_LEFT_X);
                }
                if (glm::length(move) > 1.0f) move = glm::normalize(move);

                // --- Move + collide against solid blocks -------------------
                const float pr = 0.35f, stepH = 0.55f; // player radius, step height
                // Strip last frame's bob back off so movement/collision runs on the
                // true eye position, not the bobbed one (else the bob would feed
                // back and drift).
                glm::vec3 pos = camera.position() - bobOffset;
                const float feetY = pos.y - eyeHeight;
                const float mvx = move.x * camera.moveSpeed * dt;
                const float mvz = move.z * camera.moveSpeed * dt;

                // A block is a wall for us only where it spans our body above the
                // step height (low blocks are steps we climb, not walls).
                const float bodyLo = feetY + stepH, bodyHi = feetY + eyeHeight;
                // A modelled mesh is asked at the body's centre and four points
                // on its rim -- its faces, not its box (see MeshQuery.hpp).
                static const float rimX[5] = {0.0f, 1.0f, -1.0f, 0.0f, 0.0f};
                static const float rimZ[5] = {0.0f, 0.0f, 0.0f, 1.0f, -1.0f};
                auto wallHit = [&](const Entity& b, float px, float pz) {
                    if (const auto* mc = b.components.get<MeshComponent>()) {
                        for (int k = 0; k < 5; ++k)
                            if (meshquery::blocks(b, *modifiers::shown(b, *mc).mesh, px + rimX[k] * pr,
                                                  pz + rimZ[k] * pr, bodyLo, bodyHi))
                                return true;
                        return false;
                    }
                    if (b.type != EntityType::Box && b.type != EntityType::Cylinder &&
                        b.type != EntityType::Sphere) return false;
                    if (bodyHi <= b.center.y - b.half.y || bodyLo >= b.center.y + b.half.y) return false;
                    if (px + pr <= b.center.x - b.half.x || px - pr >= b.center.x + b.half.x) return false;
                    if (pz + pr <= b.center.z - b.half.z || pz - pr >= b.center.z + b.half.z) return false;
                    return true;
                };
                // A mesh has no edge to be pushed out to, so it just holds the
                // step back -- unless we already stand inside it (placed there,
                // or it was modelled round us), where holding would trap us.
                float nx = pos.x + mvx; // move X, then Z -> slide along faces
                for (const Entity& b : entities)
                    if (wallHit(b, nx, pos.z)) {
                        if (!b.components.get<MeshComponent>())
                            nx = (mvx > 0.0f) ? b.center.x - b.half.x - pr : b.center.x + b.half.x + pr;
                        else if (!wallHit(b, pos.x, pos.z))
                            nx = pos.x;
                    }
                pos.x = nx;
                float nz = pos.z + mvz;
                for (const Entity& b : entities)
                    if (wallHit(b, pos.x, nz)) {
                        if (!b.components.get<MeshComponent>())
                            nz = (mvz > 0.0f) ? b.center.z - b.half.z - pr : b.center.z + b.half.z + pr;
                        else if (!wallHit(b, pos.x, pos.z))
                            nz = pos.z;
                    }
                pos.z = nz;

                // Ground = terrain, raised to the top of any block we stand over.
                float groundY = streamer.heightAt(pos.x, pos.z);
                for (const Entity& b : entities) {
                    if (const auto* mc = b.components.get<MeshComponent>()) {
                        for (int k = 0; k < 5; ++k) {
                            float top;
                            if (meshquery::surfaceBelow(b, *modifiers::shown(b, *mc).mesh,
                                                        pos.x + rimX[k] * pr,
                                                        pos.z + rimZ[k] * pr,
                                                        feetY + stepH + 0.01f, top) &&
                                top > groundY)
                                groundY = top;
                        }
                        continue;
                    }
                    if (b.type == EntityType::Light || b.type == EntityType::Sun ||
                        b.type == EntityType::Model || b.type == EntityType::Empty)
                        continue; // markers/models: no AABB stand surface
                    if (pos.x + pr > b.center.x - b.half.x && pos.x - pr < b.center.x + b.half.x &&
                        pos.z + pr > b.center.z - b.half.z && pos.z - pr < b.center.z + b.half.z) {
                        float top;
                        if (b.type == EntityType::Ramp) { // sloped top: rises along +Z
                            float f = (pos.z - (b.center.z - b.half.z)) / (2.0f * b.half.z);
                            top = (b.center.y - b.half.y) + glm::clamp(f, 0.0f, 1.0f) * (2.0f * b.half.y);
                        } else {
                            top = b.center.y + b.half.y;
                        }
                        if (top <= feetY + stepH + 0.01f && top > groundY) groundY = top;
                    }
                }
                const float groundEye = groundY + eyeHeight;

                // Gravity + jump.
                const bool space = input.isKeyDown(GLFW_KEY_SPACE) ||
                                   input.gamepadButton(GLFW_GAMEPAD_BUTTON_A);
                if (space && !prevSpace && grounded) fpsVelY = 9.0f;
                prevSpace = space;
                fpsVelY -= 25.0f * dt;
                pos.y += fpsVelY * dt;

                if (pos.y <= groundEye) { pos.y = groundEye; fpsVelY = 0.0f; grounded = true; }
                else                    { grounded = false; }
                camera.setPosition(applyHeadBob(pos, grounded));
                }
            } else {
                // Look only when dragging over the viewport panel (or already
                // locked into a drag); the surrounding dock panels keep the mouse.
                // Shift+Right is reserved for placing the 3D cursor (Blender-style),
                // so it must not also grab mouse-look.
                const bool shiftHeld = input.isKeyDown(GLFW_KEY_LEFT_SHIFT) ||
                                       input.isKeyDown(GLFW_KEY_RIGHT_SHIFT);
                const bool mouseLook = input.isMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)
                                       && !shiftHeld && !meshBusy
                                       && (viewportHovered || presentMode || input.isCursorLocked());
                if (mouseLook != input.isCursorLocked()) {
                    input.setCursorLocked(mouseLook);
                    // Ending a look: the OS cursor reappears where it was grabbed,
                    // which can be over the menu bar -> an accidental click. Drop
                    // it back in the viewport centre instead.
                    if (!mouseLook && viewW > 0 && viewH > 0)
                        glfwSetCursorPos(window.nativeHandle(),
                                         viewportRectMin.x + viewW * 0.5,
                                         viewportRectMin.y + viewH * 0.5);
                }
                if (mouseLook) {
                    const glm::vec2 d = input.mouseDelta();
                    camera.processMouse(d.x, d.y);
                }
#ifdef FITZEL_PLAYER
                // The player ships without the editor's viewport navigation, so
                // the wheel keeps its plain meaning here: the field of view.
                if (viewportHovered || presentMode) camera.processScroll(input.scrollDelta());
#endif
                // (In the editor the wheel dollies, and Ctrl+wheel is the field
                // of view -- ViewportNav below owns both.)
                // WASD/QE fly only while looking (right mouse held), so Q/W/E stay
                // free as the transform-tool shortcuts the rest of the time.
                if (mouseLook && !gui.wantsKeyboard()) {
                    if (input.isKeyDown(GLFW_KEY_W)) camera.processKeyboard(Camera::Direction::Forward, dt);
                    if (input.isKeyDown(GLFW_KEY_S)) camera.processKeyboard(Camera::Direction::Backward, dt);
                    if (input.isKeyDown(GLFW_KEY_A)) camera.processKeyboard(Camera::Direction::Left, dt);
                    if (input.isKeyDown(GLFW_KEY_D)) camera.processKeyboard(Camera::Direction::Right, dt);
                    if (input.isKeyDown(GLFW_KEY_E)) camera.processKeyboard(Camera::Direction::Up, dt);
                    if (input.isKeyDown(GLFW_KEY_Q)) camera.processKeyboard(Camera::Direction::Down, dt);
                }
                // Gamepad free-fly (no right-mouse needed): left stick moves in the
                // ground plane (analog via dt scaling), bumpers raise/lower, right
                // stick looks around.
                if (input.hasGamepad() && !gui.wantsKeyboard()) {
                    const float fy = -input.gamepadStick(GLFW_GAMEPAD_AXIS_LEFT_Y);
                    const float fx =  input.gamepadStick(GLFW_GAMEPAD_AXIS_LEFT_X);
                    if (fy != 0.0f) camera.processKeyboard(
                        fy > 0.0f ? Camera::Direction::Forward : Camera::Direction::Backward,
                        dt * std::fabs(fy));
                    if (fx != 0.0f) camera.processKeyboard(
                        fx > 0.0f ? Camera::Direction::Right : Camera::Direction::Left,
                        dt * std::fabs(fx));
                    if (input.gamepadButton(GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER))
                        camera.processKeyboard(Camera::Direction::Up, dt);
                    if (input.gamepadButton(GLFW_GAMEPAD_BUTTON_LEFT_BUMPER))
                        camera.processKeyboard(Camera::Direction::Down, dt);
                    const float look = 1200.0f * dt;
                    camera.processMouse(
                         input.gamepadStick(GLFW_GAMEPAD_AXIS_RIGHT_X) * look,
                        -input.gamepadStick(GLFW_GAMEPAD_AXIS_RIGHT_Y) * look);
                }
#ifndef FITZEL_PLAYER
                // The rest of the viewport's navigation: numpad 1/3/7 for the
                // standard views and middle-mouse panning (ViewportNav.cpp).
                // After the fly controls, so a look this frame is already in and
                // can cancel a view change that is still swinging.
                {
                    viewnav::Env nav{};
                    nav.viewportHovered = (viewportHovered || presentMode) && !meshBusy;
                    nav.numberRow       = !modelling;
                    // Number keys belong to the game while one is running, and to
                    // the text field while one is being typed into.
                    nav.keysFree  = !playMode && !playerMode &&
                                    !ImGui::GetIO().WantTextInput;
                    nav.looking   = mouseLook;
                    nav.viewportH = static_cast<float>(viewH);
                    if (sel.valid()) {
                        nav.haveSelection   = true;
                        nav.selectionCenter = entities[sel.index()].center;
                    }
                    nav.groundY = streamer.heightAt(camera.position().x,
                                                    camera.position().z);
                    viewNav.update(camera, input, nav, dt);
                }
#endif
            }

            // The orthographic lens is the editor's free camera's alone. Play,
            // walking, driving and a camera preview put an eye somewhere on
            // purpose and look through it the way a player would; the wish
            // stays in viewNav and comes back with the free camera.
#ifndef FITZEL_PLAYER
            camera.setOrthographic(viewNav.ortho() && !playMode && !playerMode &&
                                   !fpsMode && !vehicleMode && !gliderMode &&
                                   activeCam < 0);
#endif

            // Focus (F): glide the camera to the target, cancelled by any manual
            // camera input (right-mouse fly, a pan, a standard view) or leaving
            // the free camera. Two things easing the same position at once would
            // end up somewhere neither of them was asked for.
#ifndef FITZEL_PLAYER
            const bool navMovingCam = viewNav.gliding() || viewNav.panning();
#else
            const bool navMovingCam = false;
#endif
            if (camFocusing) {
                if (fpsMode || vehicleMode || gliderMode || playMode || navMovingCam ||
                    input.isMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)) {
                    camFocusing = false;
                } else {
                    const glm::vec3 pos = camera.position();
                    const glm::vec3 to  = camFocusTarget - pos;
                    if (glm::length(to) < 0.03f) {
                        camera.setPosition(camFocusTarget);
                        camFocusing = false;
                    } else {
                        camera.setPosition(pos + to * (1.0f - std::exp(-12.0f * dt)));
                    }
                }
            }

            // Is the eye still moving on its own? Decides the NEXT frame's
            // pacing (see the caps at the top of the loop): an animation nobody
            // is holding a key for still has to be drawn at full rate, or it is
            // not an animation, it is a slideshow.
            camAnimating = camFocusing || navMovingCam;

            // --- Camera path: record samples or drive playback ----------
            // An opening move is skippable, and the gesture that skips it is
            // simply wanting to move: the first step, the first look, the first
            // press of anything that would have driven the camera. A cutscene you
            // cannot get out of is the thing nobody forgives, and an explicit
            // "press Esc to skip" would be one more key to teach and one more
            // prompt to draw over the shot.
            //
            // Only an AUTO-started run is interruptible -- see interrupt().
            if (camPathRec.autoPlaying()) {
                const bool wantsToMove =
                    input.isKeyDown(GLFW_KEY_W) || input.isKeyDown(GLFW_KEY_A) ||
                    input.isKeyDown(GLFW_KEY_S) || input.isKeyDown(GLFW_KEY_D) ||
                    input.isKeyDown(GLFW_KEY_Q) || input.isKeyDown(GLFW_KEY_E) ||
                    input.isKeyDown(GLFW_KEY_SPACE) ||
                    input.isMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT) ||
                    input.isMouseButtonDown(GLFW_MOUSE_BUTTON_LEFT);
                if (wantsToMove) camPathRec.interrupt();
            }
            camPathRec.update(camera, dt, !vehicleMode && !gliderMode);

            // Terrain: adopt the scene's Terrain component (or, with none in the
            // scene, switch the ground off entirely). Before the streaming below,
            // so a terrain added/edited/removed this frame is what gets streamed.
            syncTerrainEntity();

            // View distance: drive the streaming radius and the camera far plane.
            streamer.setRadius(viewRadius);
            {
                // Auto ties the far plane to the streamed terrain -- 1.7 chunks
                // past the ring's edge, so the corners of the loaded area are
                // inside the frustum and nothing pops at the diagonal. Manual is
                // the same number set by hand, for a track that wants to see a
                // skyline further out than it wants to stream ground.
                const float terrainFar =
                    std::max(250.0f, viewRadius * streamer.settings().chunkSize * 1.7f);
                // ...but a skyline is precisely what you see PAST the streamed
                // ground, and the roadside city keeps its own, usually longer
                // reach (cityRange, Roads panel). Leaving the far plane on the
                // terrain's number meant the city was cut off long before its own
                // range ran out -- and the far plane does not fade, it SLICES: a
                // tower crossing it loses its top mid-air. So auto covers
                // whichever of the two reaches further, and the city goes back to
                // being culled by the one knob that is meant to control it.
                float autoFar = terrainFar;
                for (const RoadSystem* r : roads)
                    if (r->enabled && r->cityEnabled && !r->district().empty())
                        autoFar = std::max(autoFar, r->cityRange + 60.0f);
                autoFar = std::min(autoFar, 5000.0f); // the manual slider's ceiling
                // With the horizon drawn behind the ring, the ring itself must
                // never be sliced by the far plane -- from high up its corners
                // are further than the ring is wide, and a sliced corner shows
                // the far terrain's sunk hole instead of ground.
                if (farTerrainOn) {
                    const float    ringHalf = (viewRadius + 1.0f) * streamer.settings().chunkSize;
                    const glm::vec3 ep      = camera.position();
                    const float    alt      = std::max(0.0f, ep.y - streamer.heightAt(ep.x, ep.z));
                    autoFar = std::min(std::max(autoFar,
                                                std::sqrt(2.0f * ringHalf * ringHalf + alt * alt) + 30.0f),
                                       5000.0f);
                }
                const float farZ = farPlaneAuto ? autoFar
                                               : std::max(farPlaneManual, 50.0f);
                camera.setFarPlane(farZ);
                // The second pane too. It never got one, so split screen quietly
                // drew player two against the Camera default while player one used
                // this -- two different worlds' worth of draw distance side by side.
                camera2.setFarPlane(farZ);
                // The cascades are fitted to the camera frustum, so a far plane
                // pushed out for the skyline would stretch them across ground that
                // is not even streamed -- the same shadow texels spread over twice
                // the distance, i.e. every shadow in the scene going soft to shade
                // a city a kilometre out. Pin the shadowed range to the terrain:
                // past it there is nothing loaded to receive a shadow anyway.
                renderer.shadows().shadowDistance = terrainFar;
            }

            // Stream terrain chunks around the camera.
            // Terrain follows every eye that is drawn this frame, not just the
            // first: with two panes up, one ring around player one leaves player
            // two flying over a hole the moment the two are a few hundred metres
            // apart.
            {
                std::vector<glm::vec3> viewers{camera.position()};
                if (haveView2) viewers.push_back(camera2.position());
                streamer.update(viewers);
            }
            // ...and the ground past it, framing exactly the square the chunks
            // cover (see FarTerrain::update). Player one's eye only: a split
            // screen is a race, and its horizon can be player one's.
            {
                const float     cs = streamer.settings().chunkSize;
                const glm::vec3 ep = camera.position();
                const glm::vec2 cc(std::floor(ep.x / cs), std::floor(ep.z / cs));
                const float     r  = static_cast<float>(streamer.radius());
                farTerrain.enabled    = farTerrainOn;
                farTerrain.waterLevel = waterLevel;
                farTerrain.grassTint  = veg.grassTint;
                farTerrain.eco        = veg.eco;
                farTerrain.eco.treeLine   = farTerrain.treeLine;
                farTerrain.eco.waterLevel = waterLevel;
                farTerrain.canopy     = veg.canopyColour() * veg.treeBrightness;
                farTerrain.update(ep, streamer.settings(), streamer.enabled(),
                                  (cc - r) * cs, (cc + r + 1.0f) * cs);
            }

            // When the road settles (not mid-drag), regrow vegetation so it
            // clears off the new road; debounced to avoid thrashing while editing.
            {
                bool anyVegDirty = false;
                for (RoadSystem* r : roads)
                    if (r->vegDirty) { r->vegDirty = false; anyVegDirty = true; }
                if (anyVegDirty && !roadDragging) {
                    veg.grassDirty = true;
                    veg.treeCenter = glm::vec2(1e9f);
                }
            }

            // A new frame's worth of vegetation statistics (what the culling
            // actually submitted last frame -- see VegetationSystem::beginFrame).
            veg.beginFrame();
            // The forest's tree line is the horizon's (one mountain, one line),
            // and nothing grows in the lake.
            veg.eco.treeLine   = farTerrain.treeLine;
            veg.eco.waterLevel = waterLevel;
            // The air (Wind.hpp): the scene's breeze, veering a little over the
            // minutes, stiffening and gusting with the storm.
            {
                const float t   = static_cast<float>(now);
                const float ang = glm::radians(windAngle)
                                + 0.22f * std::sin(t * 0.011f)
                                + 0.09f * std::sin(t * 0.037f + 1.3f);
                veg.wind.dir       = glm::vec2(std::cos(ang), std::sin(ang));
                veg.wind.strength  = glm::mix(windStrength, 1.4f, storm);
                veg.wind.gustiness = glm::mix(windGust, 1.0f, storm * 0.5f);
                veg.wind.time      = t;
            }
            // Generated tree levels and impostors for species whose meshes
            // changed -- here, before any pass has bound its target.
            veg.prepareTrees();

            // Regrow grass (async) / trees when the camera has moved far enough.
            {
                const glm::vec2 camXZ(camera.position().x, camera.position().z);
                // Every road's centreline as one polyline, the runs separated by a
                // break marker so nothing mows a strip between two of them; the
                // clearance is the widest road's, which is the only number this
                // interface has room for.
                const std::vector<glm::vec2> cls = roads.centerlines();
                const float roadW = roads.maxWidth();
                // ...nor on a town's paved plots: a forecourt is not a meadow.
                if (towns.revision() != townBareRev) {
                    townBareRev = towns.revision();
                    veg.bare.clear();
                    for (const cityplan::Bare& b : towns.bareGround())
                        veg.bare.push_back({b.c, b.u, b.hu, b.hv});
                }
                // Past the pavements too: a blade taller than a kerb shows through.
                if (veg.updateGrass(camXZ, cls, std::max(roadW * 0.5f + 1.5f, towns.kerbReach()),
                                    waterLevel, look.snowLevel) && veg.flowerEnabled)
                    veg.regenFlowers(veg.grassCenter(), cls, roadW,
                                     waterLevel, look.snowLevel);
                veg.updateFlowers(); // finish + upload a pending async flower regen
                // Ground the forest leaves alone: where the player starts, and
                // under every placed model -- a procedural pine growing through
                // the hut's roof or out of the spawn point is the forest not
                // knowing the scene exists. Taken while editing (and once when a
                // game starts straight into Play): in Play things move, and every
                // move would regrow the field.
                if (!playMode || veg.treeClearings.empty()) {
                    std::vector<glm::vec3> clear;
                    for (const Entity& e : entities) {
                        if (!e.activeInHierarchy) continue;
                        if (e.components.get<PlayerStartComponent>())
                            clear.push_back({e.center.x, e.center.z, 6.0f});
                        else if (e.type == EntityType::Model)
                            clear.push_back({e.center.x, e.center.z,
                                             std::max(e.half.x, e.half.z) * 1.2f + 2.0f});
                    }
                    // ...and every building a town put up.
                    const std::vector<glm::vec3> townClear = towns.clearings();
                    clear.insert(clear.end(), townClear.begin(), townClear.end());
                    veg.treeClearings = std::move(clear);
                }
                veg.updateTrees(camXZ, cls, roadW, waterLevel, look.snowLevel);
            }

            // --- Weather: drift (auto) and derive storm parameters ----------
            if (autoWeather) {
                const float target = glm::clamp(
                    0.5f + 0.42f * std::sin(static_cast<float>(now) * 0.018f)
                         + 0.18f * std::sin(static_cast<float>(now) * 0.011f + 2.1f),
                    0.0f, 1.0f);
                storm += (target - storm) * std::min(1.0f, dt * 0.3f);
            }
            storm = glm::clamp(storm, 0.0f, 1.0f);

            const float effCoverage  = glm::mix(skySet.coverage, 0.97f, storm);
            const float effDensity   = glm::mix(skySet.density, 2.7f, storm);
            const float effWind      = glm::mix(skySet.wind, 26.0f, storm);
            const float effCloudBot  = glm::mix(skySet.base, 80.0f, storm);
            const float effWaveH     = glm::mix(waveHeight, 2.4f, storm);
            const float effWaveC     = glm::mix(waveChoppy, 0.95f, storm);
            // The weather's own haze plus a little for the rain in the air. It
            // was +0.011 at full storm: on top of a preset's own density that is
            // a 1/e distance of fifty metres -- the town gone behind a white
            // wall in a shower, when rain costs a valley kilometres, not all of it.
            const float effFog       = skySet.fogDensity + storm * 0.0015f;
            // How shut the sky is: a closed cumulus field or a stratus lid. Not
            // the storm alone -- an overcast day is a lid with nothing falling.
            const float overcast = glm::clamp(
                std::max(glm::smoothstep(0.55f, 0.95f, effCoverage),
                         skySet.stratus.on
                             ? glm::smoothstep(0.40f, 0.95f, skySet.stratus.amount)
                             : 0.0f),
                0.0f, 1.0f);
            // How much is coming down: the dial's own curve (the one the streaks
            // fall on, shared so the sound and the wet sheen cannot start before
            // there is anything falling) times the weather's amount.
            //
            // Two numbers out of it, because they answer different questions. The
            // FALL runs past 1 and is a count -- twice as many streaks, twice as
            // many rings. Everything that is a fraction of something (how wet the
            // road gets, how loud a loop is) takes the clamped one: a road cannot
            // be twice soaked and a loop cannot be twice full.
            const float rainFall =
                rainIntensityFor(storm) * glm::max(rainAmount, 0.0f);
            const float rainIntensity = glm::min(rainFall, 1.0f);
            rain.amount = rainAmount;   // the streak count follows it directly
            const float lightDim     = glm::mix(1.0f, 0.30f, storm);
            // Drop impacts on the carriageway, in two halves.
            //
            // This is the STRENGTH one: how much standing water there is to ring,
            // and nothing at all about how hard it is raining. Each road then
            // multiplies its own `rainRings` dial onto it when it is drawn -- the
            // strength is a property of the surface, and two roads in one scene do
            // not have to agree about it.
            //
            // The rain used to be in here too, which is why the comment that stood
            // here had to explain that rings must stop with the shower rather than
            // with the puddles: one number was doing both jobs and neither well.
            // It is `rainDensity` below that stops them now, by landing no drops.
            const float ringWeather = glm::min(roadWetness * 2.0f, 1.0f);
            // ...and the COUNT: what fraction of the ground gets hit at all. Zero
            // the moment nothing is falling, whatever is still lying about.
            const float rainDensity = rainIntensity;

            // Wetness eases toward the rain intensity: quick to soak (~2s), slow to
            // dry (~20s), so surfaces glisten for a while after the rain stops.
            {
                const float wetTau = (rainIntensity > roadWetness) ? 2.0f : 20.0f;
                roadWetness += (rainIntensity - roadWetness) *
                               (1.0f - std::exp(-dt / wetTau));
                roadWetness = glm::clamp(roadWetness, 0.0f, 1.0f);
            }

            // Lightning: brief flashes once the storm is strong -- and only if
            // this weather is a thunderstorm at all. Before the flag, every sky
            // past the halfway mark flashed, so "heavy rain" was not a weather
            // anyone could ask for.
            float flash = 0.0f;
            if (lightning && storm > 0.5f) {
                const float ft  = static_cast<float>(now) * 0.55f;
                const float rnd = glm::fract(std::sin(std::floor(ft) * 127.1f) * 43758.5f);
                if (rnd > 0.9f) {
                    flash = std::exp(-glm::fract(ft) * 7.0f) * (storm - 0.5f) * 2.0f;
                }
            }

            // Weather audio: cross-fade the looping layers, fire thunder on a
            // fresh lightning flash. Only audible while playing -- the editor
            // stays silent.
            // Mixer routing: Master to the device, SFX to the one-shot bus,
            // Ambient scales the looping weather layers.
            audio.setMasterVolume(mix.masterGain());
            audio.setSfxVolume(mix.sfxGain());
            const float amb = mix.ambientGain();
            // Each layer's level is the dial's curve times the weather's own
            // gain: the curve says when rain is falling at all, the gain says how
            // this particular sky sounds while it does. A downpour is loud rain
            // and little wind, a squall the other way round, and both sit at the
            // same place on the slider.
            // Named rather than set inline, because the mixer's meters read
            // them: what a bus is asked for is the loudest voice on it, and
            // there is nowhere else to find that out (nothing taps the device).
            const float vRain   = playMode ? rainIntensity * wxGain.rain * amb : 0.0f;
            const float vWind   = playMode ? glm::smoothstep(0.15f, 1.0f, storm) * 0.9f * wxGain.wind * amb : 0.0f;
            const float vBreeze = playMode ? (1.0f - glm::smoothstep(0.0f, 0.5f, storm)) * 0.5f * wxGain.breeze * amb : 0.0f;
            const float vStorm  = playMode ? glm::smoothstep(0.5f, 0.95f, storm) * wxGain.storm * amb : 0.0f;
            // Water ambience: louder the deeper the car is submerged (SFX bus).
            const float vWater  = playMode ? glm::clamp(carWaterSub, 0.0f, 1.0f) * mix.sfxGain() : 0.0f;
            rainSnd.setVolume(vRain);
            windSnd.setVolume(vWind);
            breezeSnd.setVolume(vBreeze);
            waterSnd.setVolume(vWater);
            // Storm bed: fades in as the weather peaks (ambient bus).
            stormSnd.setVolume(vStorm);
            mix.ambient.ask = std::max(std::max(vRain, vWind),
                                       std::max(vBreeze, vStorm));
            mix.sfx.ask     = vWater;
            const bool flashOn = flash > 0.25f;
            if (playMode && flashOn && !prevFlashOn) {
                const float vThunder =
                    glm::clamp(storm, 0.3f, 1.0f) * wxGain.thunder * amb;
                thunderSnd.setVolume(vThunder);
                thunderSnd.play();
                mix.ambient.hit(vThunder);   // a clap has no voice to read after
            }
            prevFlashOn = flashOn;

            // Engine sound: run the RPM-layered loops + auto gearbox while a car
            // is being driven; silence (and reset the box) the moment it stops.
            if (engineDriving) {
                if (!carAudio.running()) carAudio.start();
                carAudio.update(dt, engineSpeedMps, engineThrottle, engineWheelR,
                                mix.sfxGain());
                mix.sfx.ask = std::max(mix.sfx.ask, mix.sfxGain());
            } else if (carAudio.running()) {
                carAudio.stop();
            }

            // Glider jet thruster: whine + roar layers spooled by speed/throttle
            // while flying; silenced the moment flight ends.
            if (gliderAudioActive) {
                if (!gliderAudio.running()) gliderAudio.start();
                gliderAudio.update(dt, gliderSpeedMps, gliderTopSpeed, gliderThrottle,
                                   mix.sfxGain());
            } else if (gliderAudio.running()) {
                gliderAudio.stop();
            }

            // The world around the craft. Only while something is being flown or
            // driven: in the editor the camera teleports around, and a listener
            // that teleports produces a Doppler shift of several thousand.
            if (playMode && (gliderMode || vehicleMode)) {
                const glm::vec3 lp = camera.position();
                // Velocity by position delta, smoothed hard. It feeds Doppler
                // and nothing else, and a single stuttered frame would otherwise
                // put a siren through the whole mix -- so a spike costs a little
                // lag rather than a wail. Clamped as well, because a scene load
                // or a rescue moves the eye a hundred metres in one frame.
                glm::vec3 raw(0.0f);
                if (listenerHasPrev && dt > 1e-4f) raw = (lp - listenerPrev) / dt;
                if (glm::length(raw) > 400.0f) raw = glm::vec3(0.0f);
                listenerVel += (raw - listenerVel) * std::min(1.0f, dt * 8.0f);
                listenerPrev    = lp;
                listenerHasPrev = true;
                worldAudio.update(dt, lp, camera.front(), camera.up(), listenerVel,
                                  entities,
                                  roads.active().enabled ? &roads.active().district()
                                                         : nullptr,
                                  driveGliderId, driveGliderId2, mix.sfxGain());
            } else if (listenerHasPrev) {
                worldAudio.reset();
                listenerHasPrev = false;
                listenerVel     = glm::vec3(0.0f);
            }

            // Running water. Not gated on driving the way the rival engines are:
            // these voices have Doppler switched off, so nothing about them can be
            // pitched by a listener that jumps -- and on foot beside a brook is
            // exactly where you want to hear one. Silent in the editor, like every
            // other sound here.
            if (playMode) {
                if (!(gliderMode || vehicleMode))
                    worldAudio.setListener(camera.position(), camera.front(),
                                           camera.up());
                std::vector<WorldAudio::AmbiencePoint> amb;
                for (const RiverSystem::Audible& a :
                     rivers.audible(camera.position(), WorldAudio::kAmbienceVoices))
                    amb.push_back({a.pos, a.gain, a.pitch, a.range});
                worldAudio.setAmbience(amb, mix.sfxGain());
            } else {
                worldAudio.setAmbience({}, 0.0f);
            }
            // Synths: level, song settings, distance. Every frame and not only in
            // Play, so the Inspector's preview is heard in the editor too.
            synths.update(camera.position(), mix.ambientGain());
            musicSys.update(mix.ambientGain());

            // --- Day/night: advance time, derive sun direction and lighting ---
            // In Play the day can run on its own (scene setting timeFlows): the
            // editor's Pause is a working state, not a statement about the game.
            // ...or as fast as a script says (game.setDayLength), Pause or not.
            {
                const bool  byScript = playMode && scriptDayLength >= 0.0f;
                const float len      = byScript ? scriptDayLength : dayLength;
                if ((byScript || !timePaused || (playMode && timeFlows)) && len > 0.1f) {
                    timeOfDay += dt * (24.0f / len);
                    timeOfDay = std::fmod(timeOfDay, 24.0f);
                }
            }
            // The sun on its day arc: hour angle from local noon, latitude and
            // declination (sunLatitude/sunDeclination above). East is +X, south
            // +Z. At latitude 0 and -10.4 degrees this is the old sky exactly.
            const glm::vec3 sunDir = [&] {
                const float H   = (timeOfDay - 12.0f) / 24.0f * 6.2831853f;
                const float lat = glm::radians(sunLatitude);
                const float dec = glm::radians(sunDeclination);
                const float up    = std::sin(lat) * std::sin(dec)
                                  + std::cos(lat) * std::cos(dec) * std::cos(H);
                const float east  = -std::cos(dec) * std::sin(H);
                const float north = std::cos(lat) * std::sin(dec)
                                  - std::sin(lat) * std::cos(dec) * std::cos(H);
                return glm::normalize(glm::vec3(east, up, -north));
            }();
            const float dayF   = glm::smoothstep(-0.12f, 0.18f, sunDir.y);
            const float lowSun = 1.0f - glm::clamp(sunDir.y / 0.3f, 0.0f, 1.0f);
            const glm::vec3 sunCol =
                glm::mix(glm::vec3(1.0f, 0.97f, 0.9f), glm::vec3(1.0f, 0.55f, 0.26f), lowSun);
            light.direction = sunDir;
            // The Sun entity tints and scales the directional light.
            glm::vec3 sunTint(1.0f); float sunStrength = 1.0f;
            for (const Entity& e : entities)
                if (const auto* sc = e.components.get<SunComponent>()) {
                    // A deactivated Sun kills the directional light (ambient stays).
                    if (!e.active) { sunStrength = 0.0f; }
                    else { sunTint = sc->color; sunStrength = sc->intensity; }
                    break;
                }
            // HDR radiance: the sun is much brighter than 1 so tonemapping
            // produces highlights and contrast instead of a flat look.
            light.color   = sunCol * sunTint * (0.12f + 0.95f * dayF) * 3.4f * lightDim * sunStrength;
            // ...and none of it once the sun is under the horizon. It used to
            // keep a tenth of its strength all night, shining UP from below the
            // ground, so every slope facing the sunset stayed lit orange under
            // a sky full of stars. The night is the ambient's (and the moon's
            // glow in the sky) alone.
            light.color  *= glm::smoothstep(-0.03f, 0.03f, sunDir.y);
            light.ambient = glm::mix(glm::vec3(0.015f, 0.02f, 0.04f),
                                     glm::vec3(0.12f, 0.14f, 0.18f), dayF);
            // The golden hour's fill. A low sun has lost most of its strength to
            // the air and the sky it lights up has not, so shade at sunset is
            // brighter against the sun than shade at noon -- and no bluer: half
            // the dome is gold by then. Without it everything the sun does not
            // reach directly goes to black-green while the sky burns.
            // Weighted like the sky's golden hour (skyair.glsl).
            light.ambient += glm::vec3(0.075f, 0.07f, 0.06f)
                           * (1.0f - glm::smoothstep(0.0f, 0.35f, sunDir.y))
                           * glm::smoothstep(-0.10f, 0.0f, sunDir.y);
            // The moonlit sky takes over the little the set sun used to give.
            light.ambient += glm::vec3(0.012f, 0.016f, 0.028f) *
                             (1.0f - glm::smoothstep(-0.03f, 0.03f, sunDir.y));
            // Overcast: dimmer, greyer, cooler ambient.
            light.ambient = glm::mix(light.ambient,
                                     glm::vec3(0.05f, 0.06f, 0.08f), storm * 0.7f);
            // Lightning flash lights the scene briefly.
            light.color   += glm::vec3(0.8f, 0.85f, 1.0f) * (flash * 6.0f);
            light.ambient += glm::vec3(0.5f, 0.55f, 0.7f) * flash;
            // Times what auto exposure applied a couple of frames ago: the
            // renderer's exposure is what the path tracer's capture reads, and a
            // render has to come out as bright as the viewport it was taken from.
            renderer.setExposure(postLook.exposure * post.autoExposureScale());

            // Atmospheric fog, tinted by time of day to match the sky horizon.
            // Colours are authored in sRGB and linearised for the linear-space
            // blend (tonemapping converts back on output).
            Fog fog;
            fog.height        = waterLevel;
            fog.density       = effFog;
            fog.heightFalloff = skySet.fogFalloff;
            // Brighter, slightly warmer daytime haze so the distance reads as soft
            // atmosphere (like the reference) rather than a cool blue wash.
            // At the golden hour the haze takes the sky's horizon away from the
            // sun (skyair.glsl, same colour, same weight) -- the sun's side is
            // sunHazeDisp's. Mixed from the day and night hazes alone it was
            // half night-blue with the sun still up.
            const float gold = (1.0f - glm::smoothstep(0.0f, 0.35f, sunDir.y))
                             * glm::smoothstep(-0.10f, 0.0f, sunDir.y);
            const glm::vec3 hazeDisp = glm::mix(
                glm::mix(glm::vec3(0.03f, 0.04f, 0.09f), glm::vec3(0.76f, 0.82f, 0.90f), dayF),
                glm::vec3(0.42f, 0.42f, 0.50f), gold * 0.6f);
            const glm::vec3 sunHazeDisp =
                glm::mix(hazeDisp, glm::vec3(1.0f, 0.66f, 0.38f), 0.7f * dayF);
            // Under a shut sky the haze is the deck's grey, and a dim one: the
            // air is lit by the cloud's underside, not by the sun, and an
            // overcast horizon is darker than its zenith. The clear haze above
            // stayed daylight-white in the rain and turned every range into a
            // bright cut-out in front of a dark sky. No sun-side glow under it
            // either -- there is no sun to glow round. The sky's horizon takes
            // the same grey (skyair.glsl), so the ranges fade into what is
            // really behind them.
            const glm::vec3 overDisp = glm::mix(glm::vec3(0.035f, 0.04f, 0.055f),
                                                glm::vec3(0.36f, 0.39f, 0.44f), dayF);
            const glm::vec3 overLin = glm::pow(overDisp, glm::vec3(2.2f));
            fog.color    = glm::mix(glm::pow(hazeDisp, glm::vec3(2.2f)), overLin, overcast);
            fog.sunColor = glm::mix(glm::pow(sunHazeDisp, glm::vec3(2.2f)), overLin, overcast);
            fog.overcast      = overcast;
            fog.overcastColor = overLin;
            renderer.setFog(fog);
            // The fauna moves in the world just lit: after the sun, before the
            // passes that draw it.
            if (wildlifeOn && veg.birdsEnabled) {
                Wildlife::World ww;
                ww.eye        = camera.position();
                ww.ground     = [&](float x, float z) { return streamer.heightAt(x, z); };
                ww.waterLevel = waterLevel;
                ww.daylight   = dayF;
                ww.wind       = &veg.wind;
                ww.flowers    = &veg.flowerHeads();
                ww.forward    = camera.front();
                ww.water      = [&](float x, float z, float& surf, float& depth) {
                    float white = 0.0f;
                    if (rivers.sample(glm::vec2(x, z), surf, &depth, nullptr, &white))
                        return white < 0.3f;             // not in the falls and rapids
                    const float g = streamer.heightAt(x, z);
                    surf  = waterLevel;
                    depth = waterLevel - g;
                    return depth > 0.0f;
                };
                wildlife.enabled = true;
                wildlife.update(dt, ww);
                // A fish breaking the surface throws up a handful of drops.
                std::uniform_real_distribution<float> u01(0.0f, 1.0f);
                for (const Wildlife::Splash& sp : wildlife.takeSplashes()) {
                    if (!spray.ready()) break;
                    const int n = static_cast<int>(6.0f + 18.0f * sp.strength);
                    for (int k = 0; k < n; ++k) {
                        const float a = u01(sprayRng) * 6.2831853f, o = u01(sprayRng);
                        SprayP p;
                        p.pos  = sp.pos + glm::vec3(std::cos(a) * 0.08f, 0.02f, std::sin(a) * 0.08f);
                        p.vel  = glm::vec3(std::cos(a) * (0.4f + o * 1.0f),
                                           (1.2f + u01(sprayRng) * 2.2f) * (0.6f + 0.4f * sp.strength),
                                           std::sin(a) * (0.4f + o * 1.0f));
                        p.life = p.life0 = 0.35f + u01(sprayRng) * 0.45f;
                        p.size = 0.10f + u01(sprayRng) * 0.14f;
                        p.flat = 0.0f;
                        spray.add(p);
                    }
                }
            }
            if (motesOn) {
                Motes::World mw;
                mw.eye     = camera.position();
                mw.ground  = [&](float x, float z) { return streamer.heightAt(x, z); };
                mw.waterLevel = waterLevel;
                mw.wind    = &veg.wind;
                mw.weather = std::max(storm, rainIntensity);
                motes.update(dt, mw);
            }
            // ...and what it sounds like (Soundscape.hpp): singers on their
            // perches, insects by the hour, the wind in the leaves. Play only,
            // like the weather loops.
            if (soundscapeOn) {
                Soundscape::Frame sf;
                sf.eye      = camera.position();
                sf.hour     = timeOfDay;
                sf.daylight = dayF;
                sf.wind     = veg.wind.strength;
                sf.gust     = wind::gust(veg.wind, glm::vec2(sf.eye.x, sf.eye.z));
                sf.rain     = rainIntensity;
                sf.storm    = storm;
                sf.gain     = mix.ambientGain();
                sf.trees    = &veg.treeInstances();
                sf.ground   = [&](float x, float z) { return streamer.heightAt(x, z); };
                soundscape.update(dt, sf, playMode);
            }
            // The herd: (re)loaded when the scene names a different one, then
            // grazing and wandering whether or not the game is running.
            {
                const std::string dir = currentProject.empty() ? std::string()
                    : std::filesystem::path(currentProject).parent_path().generic_string();
                char key[512];
                std::snprintf(key, sizeof key, "%s|%s|%d|%.1f|%.1f|%.1f|%.2f|%d|%d|%.2f|%.1f",
                              dir.c_str(), herdModel.c_str(), herdCfg.count, herdCfg.centre.x,
                              herdCfg.centre.y, herdCfg.radius, herdCfg.height,
                              herdCfg.grazeClip, herdCfg.walkClip, herdCfg.walkSpeed,
                              herdCfg.yawOffset);
                if (herdLoadedKey != key) {
                    herdLoadedKey = key;
                    herd = Herd{};
                    if (!herdModel.empty() && !dir.empty()) {
                        Herd::Config c = herdCfg;
                        c.model = dir + "/" + herdModel;
                        herd.load(c, lit);
                    }
                }
                // The grass parts around whoever walks through it: the player
                // on foot, and the animals of the herd.
                veg.grassPushers.clear();
                if (playMode && fpsMode) {
                    const glm::vec3 e = camera.position();
                    veg.grassPushers.push_back({e.x, e.y - eyeHeight, e.z, 0.9f});
                }
                for (int i = 0; i < herd.count() && veg.grassPushers.size() < 8; ++i) {
                    const glm::vec3 p = herd.animalPos(i);
                    veg.grassPushers.push_back({p.x, p.y, p.z, 1.3f});
                }
                if (herd.loaded()) {
                    Herd::World hw;
                    hw.ground   = [&](float x, float z) { return streamer.heightAt(x, z); };
                    hw.walkable = [&](float x, float z) {
                        return streamer.heightAt(x, z) > waterLevel + 0.8f &&
                               !inDiscs(veg.wet, x, z);
                    };
                    herd.update(dt, hw);
                }
            }
            renderer.setEnvironmentIBL(&environment, iblEnabled, iblIntensity);

            // --- Physics: step the world, sync dynamic bodies back to entities -
            if (playMode && physics) {
                // Flags and curtains in a breeze: the air's push goes into the
                // particles before the step that moves them.
                softWindTime += dt;
                softBodies.blow(*physics, softWindTime, dt);
                // What hangs swings on, its boxes led there for the step.
                swingSys.update(entities, dt, physics.get());
                physics->step(dt);
                // The walking player was moved before this step; whatever it
                // stands on (a tram) has moved on in it -- and is drawn where
                // it now is. The eye goes along, and turns with it.
                if (fpsMode && physics->hasCharacter()) {
                    float turn = 0.0f;
                    const glm::vec3 moved = physics->carryCharacter(turn);
                    if (glm::dot(moved, moved) > 0.0f || turn != 0.0f) {
                        camera.setPosition(camera.position() + moved);
                        camera.setYaw(camera.yaw() - glm::degrees(turn));
                    }
                }
                // Shards of broken glass fall, bounce and lie down on what is
                // under them (Shatter.hpp).
                shatterSys.update(dt, [&](const glm::vec3& from, float& y) {
                    return host.groundHeight && host.groundHeight(from, 50.0f, y);
                });
                // Keep the terrain collider centred on the action: once the focus
                // (camera = player head / chase cam) drifts a quarter-span from the
                // field centre, rebuild it around the focus so far driving/walking
                // never runs off the finite heightfield and falls through.
                {
                    const glm::vec2 fxz(camera.position().x, camera.position().z);
                    const float recenterAt = (kThfN * 0.5f) * kThfSp * 0.5f;
                    if (glm::length(fxz - terrainCollCenter) > recenterAt)
                        refitTerrainCollision(fxz);
                }
                skids.update(*physics); // lay tyre marks where wheels slip (post-step)
                // Soft bodies: their particles are the shape, so they come back as
                // a mesh + a centre rather than as a transform.
                softBodies.sync(entities, *physics,
                    [&](Entity& e, const glm::vec3& p, const glm::vec3& r) {
                        const glm::mat4 pw = parentWorldMat(e);
                        setWorld(e, p, r, e.parent >= 0 ? &pw : nullptr);
                    });
                for (Entity& e : entities) {
                    const auto* pc = e.components.get<PhysicsComponent>();
                    if (!pc || !pc->dynamic) continue; // only dynamic bodies move
                    auto it = physicsBody.find(e.id);
                    if (it == physicsBody.end()) continue;
                    glm::vec3 p; glm::quat q;
                    if (!physics->getTransform(it->second, p, q)) continue;
                    // Decompose via ImGuizmo so the Euler angles match how the
                    // renderer recomposes the transform (composeModel).
                    const glm::mat4 mm =
                        glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(q);
                    float t[3], r[3], s[3];
                    ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(mm), t, r, s);
                    // Jolt is world-space -> convert back to the entity's local.
                    const glm::mat4 pw = parentWorldMat(e);
                    setWorld(e, glm::vec3(t[0], t[1], t[2]), glm::vec3(r[0], r[1], r[2]),
                             e.parent >= 0 ? &pw : nullptr);
                }

                // Scene vehicle: stream the Jolt chassis + wheel transforms back
                // into the driven model and its wheel children, so the actual
                // imported car drives (the primitive test car renders itself
                // from Jolt directly and needs none of this).
                if (physics->hasVehicle() && driveVehicleId >= 0) {
                    Entity* ve = document.find(driveVehicleId);
                    auto*   vc = ve ? ve->components.get<VehicleComponent>() : nullptr;
                    glm::vec3 cp; glm::quat cq;
                    if (ve && vc && physics->getTransform(physCarId, cp, cq)) {
                        glm::quat q = cq;
                        if (vc->forward == 1) // chassis frame is yawed 180
                            q = q * glm::angleAxis(glm::pi<float>(),
                                                   glm::vec3(0.0f, 1.0f, 0.0f));
                        const glm::vec3 p =
                            cp + cq * glm::vec3(0.0f, vehicleVisualY(*vc), 0.0f);
                        const glm::mat4 mm =
                            glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(q);
                        float t[3], r[3], s[3];
                        ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(mm), t, r, s);
                        const glm::mat4 pw = parentWorldMat(*ve);
                        setWorld(*ve, glm::vec3(t[0], t[1], t[2]),
                                 glm::vec3(r[0], r[1], r[2]),
                                 ve->parent >= 0 ? &pw : nullptr);
                        // Jolt's wheel frame is one and the same for all four;
                        // put on the modelled wheels as it was, a left wheel
                        // built as the right one turned half round came out
                        // with its rim facing in. So only the CHANGE since the
                        // start (steer and spin, in the chassis frame) goes on,
                        // over the wheel as modelled and the author's
                        // correction (VehicleComponent::wheelTurn).
                        const glm::quat yawFix = (vc->forward == 1)
                            ? glm::angleAxis(glm::pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f))
                            : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
                        for (int i = 0; i < 4; ++i) {
                            Entity* we = document.find(vc->wheelId[i]);
                            glm::vec3 wp; glm::quat wq;
                            if (!we || !physics->getWheelTransform(i, wp, wq)) continue;
                            const glm::quat rel = glm::inverse(cq) * wq;
                            if (!joltWheelStartSet[i]) {
                                joltWheelStart[i]    = rel;
                                joltWheelStartSet[i] = true;
                            }
                            const glm::quat turned = rel * glm::inverse(joltWheelStart[i]);
                            const glm::quat fix = glm::quat_cast(scenegraph::compose(
                                glm::vec3(0.0f), vc->wheelTurn[i], glm::vec3(1.0f)));
                            const glm::quat wheelQ =
                                cq * turned * yawFix * fix * joltWheelRest[i];
                            const glm::mat4 wm =
                                glm::translate(glm::mat4(1.0f), wp) * glm::mat4_cast(wheelQ);
                            ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(wm), t, r, s);
                            const glm::mat4 pww = parentWorldMat(*we);
                            setWorld(*we, glm::vec3(t[0], t[1], t[2]),
                                     glm::vec3(r[0], r[1], r[2]),
                                     we->parent >= 0 ? &pww : nullptr);
                        }
                    }
                }
            }

            // --- Town traffic: advance it, then move its CPU drivers ------------
            // BEFORE the cameras. A camera shooting a CPU car has to see where
            // the car is this frame: stepped after the cameras had their turn,
            // every one of them aimed at where the car was a frame ago while it
            // was drawn where it is -- off by speed x the last frame time, which
            // swings with every frame, so the car jittered in shot. After the
            // physics step, because the drivers' crash test reads the knock that
            // step just gave them. Hit hard, they crash; the traffic stops for
            // the wrecks and for the player's car.
            // The trams are in the street like a wreck is: the traffic brakes for
            // them (one frame behind, which a tram's pace does not notice).
            {
                std::vector<traffic::Obstacle> tramBoxes;
                for (const tramsim::Box& b : trams.boxes()) {
                    traffic::Obstacle o;
                    o.center = b.center;
                    for (int a = 0; a < 3; ++a) o.axes[a] = b.axes[a];
                    o.vel = glm::vec2(b.vel.x, b.vel.z);
                    o.giveWay = b.giveWay;
                    tramBoxes.push_back(o);
                }
                // ...and so is whoever crosses between the pavement and a tram.
                for (const glm::vec3& f : tramRiders.inStreet()) {
                    traffic::Obstacle o;
                    o.center  = f + glm::vec3(0.0f, 0.9f, 0.0f);
                    o.axes[0] = glm::vec3(0.45f, 0.0f, 0.0f);
                    o.axes[1] = glm::vec3(0.0f, 0.9f, 0.0f);
                    o.axes[2] = glm::vec3(0.0f, 0.0f, 0.45f);
                    tramBoxes.push_back(o);
                }
                townTraffic.setTrams(std::move(tramBoxes));
            }
            townTraffic.advance(dt, now);
            if (playMode) {
                townTraffic.setPlayerCar(physics && physics->hasVehicle() ? physCarId : 0,
                                         physCarHalf);
                {
                    // The figures scripts walk (game.moveCharacter): the traffic
                    // stops for them. Only those with a capsule -- one sitting in
                    // a car has given it up, and the car is the obstacle then.
                    std::vector<glm::vec3> people;
                    for (const auto& [fid, handle] : scriptFigures)
                        if (const Entity* fe = document.find(fid))
                            people.push_back(fe->center - glm::vec3(0.0f, fe->half.y, 0.0f));
                    townTraffic.setPeople(std::move(people));
                }
                townTraffic.playTick(entities, physics.get(), dt,
                    [&](Entity& e, const glm::vec3& p, const glm::vec3& r) {
                        const glm::mat4 pw = parentWorldMat(e);
                        setWorld(e, p, r, e.parent >= 0 ? &pw : nullptr);
                    });
            }

            // --- Scripts: tick each scripted entity's Lua update while playing --
            if (playMode) {
                host.camPos = camera.position();
                host.camDir = camera.front();
                host.screen = glm::vec2(static_cast<float>(viewW),
                                        static_cast<float>(viewH));
                host.hudCmds.clear(); // this frame's HUD is what the scripts draw now
                scripts.beginFrame(); // a hold on the keys lasts while it is asked for
                // Scripts and behaviours just write the entity's world transform;
                // children follow via resolveHierarchy (below), no propagation.
                // EVERY script component on the object, not the first.
                // `get<ScriptComponent>()` returns one, and for a long time that
                // was the whole rule: a second script card could be attached, it
                // looked complete, and it never ran. Each one now has its own
                // environment (ScriptSystem keys them by entity AND file), so
                // two scripts on one object are two independent behaviours --
                // which is how an object ends up with both "what it does" and
                // "what it looks like" without either being folded into the
                // other. They run in the order the components were added, and
                // the last write to a shared field wins, as it does between two
                // objects' scripts today.
                for (Entity& e : entities) {
                    if (e.type == EntityType::Sun || !e.activeInHierarchy) continue;
                    for (const auto& comp : e.components.items)
                        if (auto* sc = dynamic_cast<ScriptComponent*>(comp.get());
                            sc && !sc->file.empty())
                            scripts.update(e, *sc, scriptPath(sc->file), dt,
                                           static_cast<float>(now));
                }

                // Built-in component behaviours (data-authored, no code): Spin.
                // Writes LOCAL rotation; the scene-graph derives world (so a
                // spinning child of a spinning parent orbits AND spins).
                for (Entity& e : entities)
                    if (e.activeInHierarchy)
                        for (const auto& c : e.components.items)
                            if (auto* sp = dynamic_cast<SpinComponent*>(c.get()))
                                e.localRotation += sp->axis * sp->speed * dt;

                // Missile and energy pickups counting back to their respawn.
                // Deliberately OUTSIDE the activeInHierarchy loop below: a taken
                // pickup deactivates itself, and a deactivated entity is skipped
                // there, so ticking it in that loop would freeze its timer and it
                // would never come back.
                for (Entity& e : entities) {
                    auto* mp = e.components.get<MissilePickupComponent>();
                    auto* ep = e.components.get<EnergyPickupComponent>();
                    float* cd = mp && mp->cooldown > 0.0f ? &mp->cooldown
                              : ep && ep->cooldown > 0.0f ? &ep->cooldown
                                                          : nullptr;
                    if (!cd) continue;
                    *cd -= dt;
                    if (*cd <= 0.0f) {
                        *cd      = 0.0f;
                        e.active = true; // back on the track for the next lap
                    }
                }

                // Player-proximity behaviours (Collectible, Trigger). A mid-body
                // reference point keeps low objects reachable. While flying the
                // glider (or driving), the "player" is the CRAFT, not the chase
                // camera behind it -- so proximity triggers fire where the craft
                // actually passes, not ~9 m back and up.
                {
                    glm::vec3 playerC = camera.position();
                    playerC.y -= eyeHeight * 0.5f;
                    if (gliderMode && driveGliderId >= 0) playerC = gliderPos;
                    else if (vehicleMode && driveVehicleId >= 0)
                        if (const Entity* dv = document.find(driveVehicleId)) playerC = dv->center;
                    // The physics bodies a Trigger may react to, gathered on the
                    // first trigger that asks -- most scenes have none that do.
                    std::vector<triggerreach::Body> triggerBodies;
                    bool triggerBodiesReady = false;
                    for (Entity& e : entities) {
                        if (!e.activeInHierarchy) continue;  // deactivated: inert
                        // Collectible: on reach, award points, play sound, remove
                        // (destroy is deferred to the queue processed below). An
                        // inventory item waits for the figure's script to pick it
                        // up instead (game.collectibles).
                        if (const auto* col = e.components.get<CollectibleComponent>();
                            col && !col->inventory) {
                            if (glm::distance(playerC, e.center) <= col->radius) {
                                host.score += static_cast<int>(std::lround(col->points));
                                if (!col->sound.empty()) host.playSound(col->sound);
                                pendingDestroy.push_back(e.id);
                            }
                        }
                        // Missile pickup: on reach, put rounds on the rail and
                        // take the pickup off the track for `respawn` seconds.
                        // Deactivated rather than destroyed, so it can come back
                        // for the next lap; with respawn 0 it stays gone for the
                        // rest of the race.
                        //
                        // A full rack takes nothing and leaves the pickup
                        // standing -- flying over one with no room is not how a
                        // player should lose it.
                        if (auto* mp = e.components.get<MissilePickupComponent>()) {
                            // Whoever gets there first. Player two flies over the
                            // same rounds, and a pickup that only ever fed seat
                            // one would make half the track pointless to it.
                            const bool p1 = mp->cooldown <= 0.0f &&
                                            glm::distance(playerC, e.center) <= mp->radius &&
                                            weapons.addAmmo(mp->count) > 0;
                            const bool p2 = !p1 && mp->cooldown <= 0.0f &&
                                            driveGliderId2 >= 0 &&
                                            glm::distance(race2.gliderPos, e.center) <= mp->radius &&
                                            weapons2.addAmmo(mp->count) > 0;
                            if (p1 || p2) {
                                if (!mp->sound.empty()) host.playSound(mp->sound);
                                mp->cooldown = mp->respawn;
                                e.active     = false;
                            }
                        }
                        // Energy pickup: on reach, mend the hull and take the
                        // pickup off the track for `respawn` seconds -- the same
                        // deactivate-and-return dance the missile pickup does,
                        // for the same reason (lap two has to be the same track
                        // as lap one).
                        //
                        // A FULL hull takes nothing and leaves it standing, the
                        // way a full rack leaves the rounds standing. So does a
                        // craft whose hull is already gone: that run is settled
                        // and a pickup must not quietly fly it back out of the
                        // loss.
                        if (auto* ep = e.components.get<EnergyPickupComponent>()) {
                            auto take = [&](racesim::RaceState& st) {
                                if (st.energyOut) return false;
                                if (st.energy >= st.energyCapacity - 0.01f) return false;
                                st.energy = glm::min(st.energyCapacity,
                                                     st.energy + glm::max(ep->amount, 0.0f));
                                return true;
                            };
                            // Whoever gets there first, exactly like the rounds:
                            // both seats fly the same track. Gated on actually
                            // flying a glider -- the hull is the craft's, and on
                            // foot or in the car there is nothing to mend.
                            const bool p1 = ep->cooldown <= 0.0f &&
                                            gliderMode && driveGliderId >= 0 &&
                                            glm::distance(playerC, e.center) <= ep->radius &&
                                            take(race);
                            const bool p2 = !p1 && ep->cooldown <= 0.0f &&
                                            driveGliderId2 >= 0 &&
                                            glm::distance(race2.gliderPos, e.center) <= ep->radius &&
                                            take(race2);
                            if (p1 || p2) {
                                if (!ep->sound.empty()) host.playSound(ep->sound);
                                ep->cooldown = ep->respawn;
                                e.active     = false;
                            }
                        }
                        // Trigger: on entry (edge), set the HUD message / play the
                        // sound / open the Synth's gate. `once` latches via the
                        // transient `fired` flag. The gate closes on exit, even
                        // for a `once` trigger, so no note is left hanging.
                        // "Inside" is the player, the physics bodies, or either,
                        // as the trigger's `reactsTo` says.
                        if (auto* tr = e.components.get<TriggerComponent>()) {
                            bool inside = tr->reactsTo != TriggerComponent::Physics &&
                                          glm::distance(playerC, e.center) <= tr->radius;
                            if (!inside && tr->reactsTo != TriggerComponent::Player) {
                                if (!triggerBodiesReady) {
                                    triggerBodies = triggerreach::collect(
                                        entities, vehicleMode ? driveVehicleId : -1,
                                        gliderMode ? driveGliderId : -1);
                                    triggerBodiesReady = true;
                                }
                                inside = triggerreach::anyInside(triggerBodies, e.center,
                                                                 tr->radius, e.id);
                            }
                            if (inside && !tr->insideLast && !(tr->once && tr->fired)) {
                                tr->fired = true;
                                if (!tr->message.empty()) host.hud = tr->message;
                                if (!tr->sound.empty()) host.playSound(tr->sound);
                                if (tr->synthTarget >= 0 && !tr->gateOpen)
                                    tr->gateOpen = synths.noteOn(tr->synthTarget,
                                                                 std::clamp(tr->synthNote, 0, 127),
                                                                 tr->synthVelocity);
                            }
                            if (!inside && tr->gateOpen) {
                                synths.noteOff(tr->synthTarget, std::clamp(tr->synthNote, 0, 127));
                                tr->gateOpen = false;
                            }
                            tr->insideLast = inside;
                        }
                        // SceneTrigger: on entry (edge), request a load of another
                        // scene. Deferred to after the play tick (pendingSceneLoad),
                        // so the entity list is never swapped mid-iteration.
                        if (auto* stc = e.components.get<SceneTriggerComponent>()) {
                            const bool inside = glm::distance(playerC, e.center) <= stc->radius;
                            if (inside && !stc->insideLast &&
                                !(stc->once && stc->fired) && !stc->scene.empty()) {
                                stc->fired = true;
                                pendingSceneLoad = stc->scene;
                            }
                            stc->insideLast = inside;
                        }
                        // TriggerSound: a looping ambient zone (volume fades with
                        // distance) or a one-shot on entry. The looping voice lives
                        // in zoneSounds, created lazily and stopped when out of range.
                        if (auto* ts = e.components.get<TriggerSoundComponent>()) {
                            const float dist   = glm::distance(playerC, e.center);
                            const bool  inside = dist <= ts->radius;
                            if (ts->oneShot) {
                                // Fire-and-forget on every entry: a full one-shot
                                // that a fast fly-through can't cut off, and that
                                // replays on each pass (edge-triggered).
                                if (inside && !ts->insideLast && !ts->sound.empty())
                                    host.playSound(ts->sound);
                            } else if (ts->loop) {
                                Sound& voice = zoneSounds[e.id];
                                if (inside && !ts->sound.empty()) {
                                    if (!voice.isValid())
                                        voice = Sound::fromFile(
                                            audio, resolveSoundPath(ts->sound), true);
                                    if (!ts->insideLast) voice.play(); // (re)start on entry
                                    const float fall = glm::clamp(1.0f - dist / glm::max(ts->radius, 0.01f), 0.0f, 1.0f);
                                    voice.setVolume(ts->volume * fall * mix.ambientGain());
                                } else if (voice.isValid()) {
                                    voice.stop();
                                }
                            } else if (inside && !ts->insideLast &&
                                       !(ts->once && ts->fired) && !ts->sound.empty()) {
                                ts->fired = true;
                                host.playSound(ts->sound); // one-shot (no per-voice volume)
                            }
                            ts->insideLast = inside;
                        }
                        // AudioSource: keep a playing voice's level live -- track
                        // volume/mix changes and, when spatial, fade with distance
                        // from the player. Start/stop is driven by playOnStart and
                        // game.playAudio/stopAudio, not proximity.
                        if (const auto* as = e.components.get<AudioSourceComponent>()) {
                            auto it = audioVoices.find(e.id);
                            if (it != audioVoices.end() && it->second.isValid()) {
                                float vol = as->volume * mix.ambientGain();
                                if (as->spatial) {
                                    const float dist = glm::distance(playerC, e.center);
                                    vol *= glm::clamp(1.0f - dist / glm::max(as->radius, 0.01f),
                                                      0.0f, 1.0f);
                                }
                                it->second.setVolume(vol);
                            }
                        }
                        // AnimationTrigger: on entry, (re)start the target entity's
                        // Animation from its range start (the anim tick honours restart).
                        if (auto* at = e.components.get<AnimationTriggerComponent>()) {
                            const bool inside = glm::distance(playerC, e.center) <= at->radius;
                            if (inside && !at->insideLast && !(at->once && at->fired))
                                if (Entity* tgt = document.find(at->target))
                                    if (auto* ac = tgt->components.get<AnimationComponent>()) {
                                        ac->restart = true;
                                        at->fired = true;
                                    }
                            at->insideLast = inside;
                        }
                        // DoorOpener: the target Door (or self, target<0) is open
                        // while the player is in range; `stayOpen` latches it.
                        if (auto* dop = e.components.get<DoorOpenerComponent>()) {
                            const bool inside = glm::distance(playerC, e.center) <= dop->radius;
                            Entity* doorEnt = dop->target >= 0 ? document.find(dop->target) : &e;
                            if (doorEnt)
                                if (auto* door = doorEnt->components.get<DoorComponent>()) {
                                    if (dop->stayOpen) {
                                        if (inside) dop->opened = true;
                                        door->open = dop->opened;
                                    } else {
                                        door->open = inside;
                                    }
                                }
                            dop->insideLast = inside;
                        }
                        // Lift: rise while the player is within range, descend when
                        // they leave, between the start (bottom) and start+offset
                        // (top) at `speed`. Writes LOCAL position; a kinematic
                        // collider (created lazily) follows it so it carries the
                        // player and any crates. (World == local for an unparented
                        // lift, the normal case.)
                        if (auto* lf = e.components.get<LiftComponent>()) {
                            if (!lf->homeSet) { lf->home = e.localCenter; lf->homeSet = true; }
                            const bool called = glm::distance(playerC, e.center) <= lf->radius;
                            const float travel = glm::max(glm::length(lf->offset), 0.001f);
                            lf->t = glm::clamp(
                                lf->t + (called ? 1.0f : -1.0f) * (lf->speed / travel) * dt,
                                0.0f, 1.0f);
                            e.localCenter = lf->home + lf->offset * lf->t;
                            if (physics) {
                                const glm::quat q = glm::quat(glm::radians(e.rotation));
                                if (lf->bodyId == 0)
                                    lf->bodyId = physics->addKinematicBox(e.half, e.localCenter, q);
                                else
                                    physics->setKinematicTarget(lf->bodyId, e.localCenter, q, dt);
                            }
                        }
                        // CameraSwitcher: entering the zone makes `target` the
                        // active camera (-1 = back to the player view).
                        if (auto* cs = e.components.get<CameraSwitcherComponent>()) {
                            if (glm::distance(playerC, e.center) <= cs->radius)
                                activeCam = cs->target;
                        }
                    }
                }

                // Mover: oscillate from the start position to start+offset and
                // back (one cycle per `duration`). Writes LOCAL position; the
                // scene graph carries children along. `home` is captured lazily on
                // the first tick so spawned movers work too; both it and `phase`
                // reset for free when Play stops (scene restored from backup).
                for (Entity& e : entities)
                    if (auto* mv = e.components.get<MoverComponent>()) {
                        if (!mv->homeSet) { mv->home = e.localCenter; mv->homeSet = true; }
                        mv->phase += dt / glm::max(mv->duration, 0.05f);
                        const float s = 0.5f - 0.5f * std::cos(6.2831853f * mv->phase);
                        e.localCenter = mv->home + mv->offset * s;
                    }

                // Opponents: AI racers lapping the road centreline, slowing for
                // corners and banking into them. (racesim::updateOpponents.)
                // Player two joins the field when there is one, so both panes
                // show one running order instead of player one's list and an
                // empty box next to it.
                racesim::updateOpponents(race, raceEnv,
                                         driveGliderId2 >= 0 ? &race2 : nullptr);

                // Vapour contrails behind every racer -- the driven craft (keyed by
                // its entity id) and each opponent (at its just-placed centre). The
                // ribbons billboard toward the chase camera and fade out on their
                // own, so a stopped racer's trail dissolves.
                if (trails.enabled) {
                    if (vehicleMode || gliderMode) {
                        const int pid = gliderMode ? driveGliderId : driveVehicleId;
                        if (pid >= 0) trails.emit(pid, gliderMode ? gliderPos : carPos);
                    }
                    // Player two draws one too, from the flown position rather
                    // than the entity centre -- that is the interpolated pose,
                    // so its ribbon is laid down as smoothly as player one's.
                    if (driveGliderId2 >= 0)
                        trails.emit(driveGliderId2, race2.gliderPos);
                    for (Entity& te : entities)
                        if (te.activeInHierarchy && te.id != driveGliderId2 &&
                            te.components.get<OpponentComponent>())
                            trails.emit(te.id, te.center);
                }
                trails.update(dt, camera.position());

                // --- Lock-on missiles ---------------------------------------
                // The weapon itself lives in WeaponSystem: acquisition, flight,
                // effects, HUD, and even which button fires. What belongs HERE
                // is the only part it cannot know -- who counts as a rival in
                // this scene, and what a hit does to one. For an AI racer that
                // is a spin and a lost half-second, not a health bar: it flies
                // the slip-up the opponent sim already knows how to fly, so a
                // missile reads as having thrown the rival off its line.
                {
                    // Player two shoots the same authored weapon from its own
                    // launcher, so the panel keeps tuning one thing.
                    weapons2.adoptSettings(weapons);
                    WeaponSystem::Frame wf;
                    wf.dt    = dt;
                    wf.armed = gliderMode && driveGliderId >= 0;
                    // Nothing leaves the rail on the grid or after the flag:
                    // the countdown holds the field still, and a finished race
                    // flies itself home.
                    wf.mayFire = !race.onGrid && raceCountdown <= 0.0f &&
                                 !raceFinished && !race.energyOut;
                    wf.pos = gliderPos;
                    // Riding a loop the craft has a full 3D frame; everywhere
                    // else its heading is the yaw the flight sim integrates.
                    wf.fwd = race.loopIndex >= 0
                                 ? race.loopFwd
                                 : glm::vec3(std::sin(gliderYaw), 0.0f, std::cos(gliderYaw));
                    wf.up  = race.loopIndex >= 0 ? race.loopUp
                                                 : glm::vec3(0.0f, 1.0f, 0.0f);
                    wf.vel    = gliderVel;
                    wf.camPos = camera.position();
                    // Seat one's buttons: F / pad X fires, T / pad Y steps the
                    // target. Resolved here, like the flight controls, so the
                    // two seats cannot end up sharing a trigger finger.
                    wf.fire = input.isKeyDown(GLFW_KEY_F) ||
                              (input.hasGamepad() &&
                               input.gamepadButton(GLFW_GAMEPAD_BUTTON_X));
                    wf.cycleTarget = input.isKeyDown(GLFW_KEY_T) ||
                                     (input.hasGamepad() &&
                                      input.gamepadButton(GLFW_GAMEPAD_BUTTON_Y));

                    // Who a shooter may lock onto: every AI racer, plus the OTHER
                    // player's craft. A two-player race in which the missiles
                    // only ever go to the computer would be missing the point.
                    auto fieldFor = [&](int shooterId, int rivalId) {
                        std::vector<WeaponSystem::Racer> field;
                        for (const Entity& te : entities) {
                            if (!te.activeInHierarchy || te.id == shooterId) continue;
                            const bool rival =
                                te.components.get<OpponentComponent>() != nullptr ||
                                (rivalId >= 0 && te.id == rivalId);
                            if (!rival) continue;
                            WeaponSystem::Racer r;
                            r.id     = te.id;
                            r.pos    = te.center;
                            r.radius = glm::max(glm::max(te.half.x, te.half.z), 1.0f);
                            r.name   = te.name;
                            field.push_back(std::move(r));
                        }
                        return field;
                    };
                    weapons.update(wf, fieldFor(driveGliderId, driveGliderId2));

                    // What a hit costs depends on WHO took it, which is the part
                    // the weapon cannot know. An AI racer loses its line (it
                    // flies the slip-up the opponent sim already knows); a human
                    // loses hull, on the same bookkeeping a crash uses, and gets
                    // shoved the way the missile was travelling.
                    auto applyHits = [&](const WeaponSystem& w) {
                        for (const WeaponSystem::Hit& h : w.hits()) {
                            racesim::RaceState* victim =
                                (h.targetId == driveGliderId)  ? &race
                              : (h.targetId == driveGliderId2) ? &race2 : nullptr;
                            if (victim) {
                                // A direct hit takes about a third of a full
                                // hull: enough that being shot at matters, far
                                // enough from lethal that one missile cannot end
                                // someone's race outright.
                                racesim::applyDamage(
                                    *victim, victim->energyCapacity * 0.35f * h.damage);
                                victim->gliderVel +=
                                    glm::vec3(h.dir.x, 0.0f, h.dir.z) * (12.0f * h.damage);
                                continue;
                            }
                            Entity* he = document.find(h.targetId);
                            auto* op = he ? he->components.get<OpponentComponent>() : nullptr;
                            if (!op) continue;
                            op->curSpeed *= glm::mix(0.90f, 0.40f, h.damage);
                            op->mistakeT  = glm::max(op->mistakeT, 0.6f + 1.5f * h.damage);
                            op->mistakeCd = glm::max(op->mistakeCd, 3.0f); // no double punish
                            // Shoved across the track the way it was hit. laneCur is
                            // eased back toward the racing line every tick, so this
                            // is a lurch, not a permanent detour.
                            const float yr = sceneHeading(he->rotation);
                            const glm::vec3 fr(std::sin(yr), 0.0f, std::cos(yr));
                            const glm::vec3 sr = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), fr);
                            const float push =
                                glm::dot(sr, glm::vec3(h.dir.x, 0.0f, h.dir.z));
                            op->laneCur += glm::clamp(push, -1.0f, 1.0f) * 2.6f * h.damage;
                        }
                    };
                    applyHits(weapons);

                    // Seat two, same weapon, its own launcher and its own eye.
                    // Its buttons sit next to the arrow keys it flies with.
                    if (driveGliderId2 >= 0) {
                        WeaponSystem::Frame wf2;
                        wf2.dt    = dt;
                        wf2.armed = true;
                        wf2.mayFire = !race2.onGrid && race2.raceCountdown <= 0.0f &&
                                      !race2.raceFinished && !race2.energyOut;
                        wf2.pos = race2.gliderPos;
                        wf2.fwd = race2.loopIndex >= 0
                                      ? race2.loopFwd
                                      : glm::vec3(std::sin(race2.gliderYaw), 0.0f,
                                                  std::cos(race2.gliderYaw));
                        wf2.up  = race2.loopIndex >= 0 ? race2.loopUp
                                                       : glm::vec3(0.0f, 1.0f, 0.0f);
                        wf2.vel    = race2.gliderVel;
                        wf2.camPos = camera2.position();
                        wf2.fire        = input.isKeyDown(GLFW_KEY_PERIOD);
                        wf2.cycleTarget = input.isKeyDown(GLFW_KEY_COMMA);
                        weapons2.update(wf2, fieldFor(driveGliderId2, driveGliderId));
                        applyHits(weapons2);
                    }
                }

                // Door: ease toward open/closed (open set by a DoorOpener), swing
                // or slide from the captured closed pose. A kinematic collider
                // follows it so a shut door blocks and an open one clears. Writes
                // LOCAL transform (children ride along). World == local for an
                // unparented door (the normal case).
                for (Entity& e : entities)
                    if (auto* d = e.components.get<DoorComponent>()) {
                        if (!d->started) {
                            d->started = true;
                            d->home = e.localCenter; d->homeRot = e.localRotation;
                            d->open = d->startOpen; d->t = d->startOpen ? 1.0f : 0.0f;
                        }
                        const float target = d->open ? 1.0f : 0.0f;
                        const float step   = d->speed * dt;
                        if (d->t < target) d->t = glm::min(d->t + step, target);
                        else               d->t = glm::max(d->t - step, target);
                        if (d->slide) {
                            e.localCenter   = d->home + d->offset * d->t;
                            e.localRotation = d->homeRot;
                        } else {
                            e.localCenter   = d->home;
                            e.localRotation = d->homeRot + glm::vec3(0.0f, d->angle * d->t, 0.0f);
                        }
                        if (physics) {
                            const glm::quat q = glm::quat(glm::radians(e.localRotation));
                            if (d->bodyId == 0)
                                d->bodyId = physics->addKinematicBox(e.half, e.localCenter, q);
                            else
                                physics->setKinematicTarget(d->bodyId, e.localCenter, q, dt);
                        }
                    }

                // Spawner: emit a dynamic solid -- or a whole prefab instance --
                // above itself every `interval`, up to `maxCount`, through the
                // same deferred spawn queue as scripts.
                for (Entity& e : entities) {
                    auto* sw = e.components.get<SpawnerComponent>();
                    if (!sw || sw->spawned >= static_cast<int>(sw->maxCount)) continue;
                    sw->timer += dt;
                    if (sw->timer < glm::max(sw->interval, 0.05f)) continue;
                    sw->timer = 0.0f;
                    const glm::vec3 from = e.center + glm::vec3(0.0f, e.half.y + 0.4f, 0.0f);
                    // Launch direction: random within a cone of half-angle `spread`
                    // (deg) around +Y. Sampling cos(theta) uniformly over the cap
                    // gives an even spread; spread 0 -> straight up, 180 -> any dir.
                    const float spreadRad =
                        glm::radians(glm::clamp(sw->spread, 0.0f, 180.0f));
                    const float ct = glm::mix(std::cos(spreadRad), 1.0f, spawnU(spawnRng));
                    const float st = std::sqrt(glm::max(0.0f, 1.0f - ct * ct));
                    const float ph = 6.2831853f * spawnU(spawnRng);
                    const glm::vec3 dir(st * std::cos(ph), ct, st * std::sin(ph));
                    if (!sw->prefab.empty()) {
                        // Prefab: the whole subtree, turned to the spawner's yaw.
                        // The launch velocity lands on the instance root, so it
                        // flies only if the prefab's root carries a dynamic body.
                        const int rootId = host.spawnPrefab
                            ? host.spawnPrefab(sw->prefab, from, e.rotation.y) : 0;
                        if (rootId && sw->speed > 0.0f)
                            pendingSpawnVel[rootId] = dir * sw->speed;
                        // A missing/broken prefab still counts as an attempt, so a
                        // typo stops after `maxCount` instead of logging forever.
                        ++sw->spawned;
                        continue;
                    }
                    ScriptSpawn s;
                    s.type    = sw->spawnType;
                    s.pos     = from;
                    s.half    = glm::vec3(0.3f);
                    s.physics = 2; // dynamic
                    s.vel     = dir * sw->speed;
                    s.name    = "spawned";
                    host.spawn(s);
                    ++sw->spawned;
                }

                // Pusher: shove dynamic bodies in range along `direction` -- a
                // steady force (continuous) or one impulse on entry. O(n^2) over
                // entities, fine for editor scenes.
                if (physics)
                    for (Entity& e : entities) {
                        auto* pu = e.components.get<PusherComponent>();
                        if (!pu) continue;
                        const float len = glm::length(pu->direction);
                        const glm::vec3 dir = len > 1e-4f ? pu->direction / len
                                                          : glm::vec3(0.0f, 1.0f, 0.0f);
                        for (Entity& t : entities) {
                            if (t.id == e.id) continue;
                            const auto* pc = t.components.get<PhysicsComponent>();
                            if (!pc || !pc->dynamic) continue;
                            auto bit = physicsBody.find(t.id);
                            if (bit == physicsBody.end()) continue;
                            const bool inside = glm::distance(e.center, t.center) <= pu->radius;
                            if (pu->continuous) {
                                if (inside)
                                    physics->applyImpulse(bit->second, dir * pu->strength * dt);
                            } else {
                                const bool was = pu->insideBodies.count(t.id) != 0;
                                if (inside && !was)
                                    physics->applyImpulse(bit->second, dir * pu->strength);
                                if (inside) pu->insideBodies.insert(t.id);
                                else        pu->insideBodies.erase(t.id);
                            }
                        }
                    }

                // Apply entity spawns/destroys the scripts requested this frame
                // (deferred so the tick loop above kept stable references).
                for (int did : pendingDestroy) {
                    auto bit = physicsBody.find(did);
                    if (bit != physicsBody.end()) {
                        if (physics) physics->removeBody(bit->second);
                        physicsBody.erase(bit);
                    }
                    if (physics) softBodies.remove(did, *physics);
                    scripts.removeEntity(did);
                    entities.erase(std::remove_if(entities.begin(), entities.end(),
                        [did](const Entity& e){ return e.id == did; }), entities.end());
                }
                pendingDestroy.clear();
                for (const Entity& ne : pendingSpawns) {
                    entities.push_back(ne);
                    const Entity& e = entities.back();
                    const auto* pc = e.components.get<PhysicsComponent>();
                    if (physics && pc && e.type != EntityType::Light &&
                        e.type != EntityType::Sun) {
                        const float m = pc->dynamic ? glm::max(pc->mass, 0.01f) : 0.0f;
                        const auto* mdl = e.components.get<ModelComponent>();
                        const PhysicsBodyId id = addEntityBody(
                            *physics, e, m, mdl ? models.byId(mdl->modelId) : nullptr);
                        if (id) {
                            physicsBody[e.id] = id;
                            auto vit = pendingSpawnVel.find(e.id);
                            if (vit != pendingSpawnVel.end() &&
                                vit->second != glm::vec3(0.0f))
                                physics->setLinearVelocity(id, vit->second);
                        }
                    }
                    pendingSpawnVel.erase(e.id);
                }
                pendingSpawns.clear();

                // Commit input edges so *Pressed fire once per press.
                for (int kc : keyQ)  keyPrev[kc]  = input.isKeyDown(kc) ? 1 : 0;
                for (int b  : mouseQ) mousePrev[b] = input.isMouseButtonDown(b) ? 1 : 0;
                keyQ.clear(); mouseQ.clear();
                scriptTyped.clear();

                // A finished race goes into the circuit records. Edge-detected
                // on raceFinished so the flag writes one row rather than one per
                // frame of the slowing-down lap -- and taken HERE rather than
                // off the classification board, because a player who closes the
                // game on the results screen has still driven the time.
                //
                // Ordered by best lap (see leaderboard::Entry): that is the one
                // figure in a row that means the same thing whatever distance it
                // was set over. The total is kept beside it.
                if (race.raceFinished && !prevRaceFinished && !currentProject.empty()) {
                    leaderboard::Entry rec;
                    rec.bestLap = race.bestLap;
                    rec.total   = race.raceClock;
                    rec.laps    = race.raceLaps;
                    rec.level   = sessionRaceLevel;
                    rec.date    = leaderboard::today();
                    const std::string track =
                        std::filesystem::path(currentProject).stem().string();
                    if (leaderboard::record(raceRecords, track, rec) > 0)
                        leaderboard::save(kScoresFile, raceRecords);
                }
                prevRaceFinished = race.raceFinished;

                // The scene's cameras, resolved after everything that moved this
                // tick -- the sim, the movers, the opponents, the scripts. Done
                // here so the frame renders from where things ENDED UP, and so a
                // script that cut to another camera a few lines ago is obeyed
                // this frame rather than the next.
                applyViewCamera();
            }
            // Same outside Play: a craft can be flown in the editor too, and its
            // camera has to follow there or a test flight is done blind.
            if (!playMode) applyViewCamera();
#ifndef FITZEL_PLAYER
            // --shots: the listed view wins over every camera the scene has.
            // The walking player must not pull the eye back to the capsule: the
            // next frame streams terrain, grass and trees around wherever the
            // camera is when it starts, and that has to be the shot.
            if (playMode && shotRunner.active()) fpsMode = false;
            if (playMode && shotRunner.active())
                shotRunner.applyCamera(
                    camera, [&](float x, float z) { return streamer.heightAt(x, z); },
                    timeOfDay);
#endif
            hudCamera.emplace(camera);   // the eye this frame is drawn from (game.worldToHud)

            // One row of the trace, taken HERE: the sim has written the craft's
            // interpolated pose for this frame and applyViewCamera has just
            // placed the eye from it, so the two numbers are the same frame's --
            // which is the whole point. Sampled anywhere later and a difference
            // could be the sampling rather than the fault.
            if (camTraceLeft > 0) {
                const int driven = gliderMode ? driveGliderId
                                 : vehicleMode ? driveVehicleId : -1;
                const Entity* dce = (driven >= 0) ? document.find(driven) : nullptr;
                const glm::vec3 craft = dce ? dce->center : glm::vec3(0.0f);
                const glm::vec3 simP  = gliderMode ? race.gliderPos : carPos;
                const glm::vec3 eye   = camera.position();
                char row[256];
                std::snprintf(row, sizeof row,
                    "%d,%.3f,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
                    "%.4f,%.4f,%.4f,%.4f,%d",
                    301 - camTraceLeft, dt * 1000.0f, simSteps, simAlpha,
                    craft.x, craft.y, craft.z, simP.x, simP.y, simP.z,
                    eye.x, eye.y, eye.z, glm::length(eye - craft), activeCam);
                camTraceRows.push_back(row);
                if (--camTraceLeft == 0) {
                    const std::string out =
                        (currentProject.empty()
                             ? std::string("camtrace.csv")
                             : std::filesystem::path(currentProject)
                                   .parent_path().generic_string() + "/camtrace.csv");
                    std::ofstream f(out);
                    for (const std::string& r : camTraceRows) f << r << '\n';
                    // ...and where the frame went, in a file beside it. The
                    // positions say WHETHER the motion is smooth; they cannot say
                    // why a frame took 19 ms -- and that is the other half of
                    // judder, because motion that is perfect on paper still
                    // judders when it is presented at the wrong cadence.
                    const std::string zout =
                        out.substr(0, out.size() - 4) + "_zones.csv";
                    std::ofstream zf(zout);
                    const prof::FrameStats fs = prof::frameStats();
                    zf << "# frame ms: last " << fs.last << " avg " << fs.avg
                       << " worst " << fs.worst << " low1_fps " << fs.low1
                       << " spikes " << fs.spikes << '\n';
                    zf << "zone,last_ms,avg_ms,worst_ms\n";
                    for (const prof::ZoneStat& z : prof::zones())
                        zf << (z.name ? z.name : "?") << ',' << z.last << ','
                           << z.avg << ',' << z.worst << '\n';
                    exportStatus = "Camera trace written: " + out;
                    host.hud     = "Camera trace written: " + out;
                }
            }

            // Deferred scene load a SceneTrigger asked for this frame. Done here,
            // outside the play tick, so the entity list is swapped between frames --
            // never mid-iteration. The name resolves to a .fitzel in the current
            // project folder; if the game was playing we re-enter Play in the new
            // scene, so walking through the trigger reads as a seamless level change.
            // Deferred level restart (an overlay Restart button). Leaving and
            // re-entering Play rewinds the scene to the snapshot Play started
            // from -- physics, scripts and the player start included -- without
            // touching the file, so unsaved editor edits survive it.
            if (pendingRestart) {
                pendingRestart = false;
                if (playMode) {
                    stopPlay(); startPlay();
                    // The race was launched from the start screen: send its craft
                    // (and the rest of its setup) in again. The snapshot Play just
                    // restored is the circuit as it sits on disk -- the chosen
                    // craft was never part of it -- so a restart that did not do
                    // this would put the player back in the scene's own glider.
                    // The arrival block a few lines below picks these up in the
                    // same frame, exactly as it does after a launch.
                    if (!sessionCraftJson.empty()) {
                        pendingFromShowroom = false;   // a restart, not an arrival
                        pendingCraftJson   = sessionCraftJson;
                        pendingCraftName   = sessionCraftName;
                        pendingCraftJson2  = sessionCraftJson2;
                        pendingCraftName2  = sessionCraftName2;
                        pendingCraftLaps   = sessionCraftLaps;
                        pendingRaceMode    = sessionRaceMode;
                        pendingRaceField   = sessionRaceField;
                        pendingRaceLevel   = sessionRaceLevel;
                    }
                }
            }

            // "Back to the start screen" from the end-of-race question. The start
            // scene is the one the game boots into (game.json's startScene): in
            // the player that is `bootScene`, in the editor the project's own
            // setting. With none configured there is nothing to go back TO, so
            // the player quits and the editor drops out of Play.
            if (pendingQuit) {
                pendingQuit = false;
                pendingSceneLoad.clear();
                if (playerMode)    window.requestClose();
                else if (playMode) stopPlay();
            }

            if (pendingStartScreen) {
                pendingStartScreen = false;
                std::string startScene = bootScene;
                if (startScene.empty() && !currentProject.empty())
                    startScene = game::load(std::filesystem::path(currentProject)
                                                .parent_path().generic_string())
                                     .startScene;
                if (!startScene.empty())      pendingSceneLoad = startScene;
                else if (playerMode)          window.requestClose();
                else if (playMode)            stopPlay();
            }

            if (!pendingSceneLoad.empty()) {
                const std::filesystem::path folder =
                    std::filesystem::path(currentProject).parent_path();
                const std::filesystem::path target = folder / (pendingSceneLoad + ".fitzel");
                const std::string want = pendingSceneLoad;
                pendingSceneLoad.clear();
                // A different scene is a different session: the craft the start
                // screen chose was chosen for the circuit being left, so it is
                // not re-sent into the next one. (A launch fills this back in
                // when its craft arrives, a few lines below, in this same frame.)
                sessionCraftJson = nlohmann::json();
                sessionCraftJson2 = nlohmann::json();
                sessionCraftName.clear(); sessionCraftName2.clear();
                sessionCraftLaps   = 0;
                sessionRaceMode    = -1;   sessionRaceField   = -1;
                sessionRaceLevel   = gameDifficulty.level;
                std::fprintf(stderr, "[navdbg] sceneload want='%s' cur='%s' target='%s'\n",
                             want.c_str(), currentProject.c_str(),
                             target.generic_string().c_str());
                // Through the VFS: a level change in a packed game asks for a
                // scene that exists only inside the archive.
                if (fitzel::vfs::exists(target.generic_string())) {
                    const bool wasPlaying = playMode;
                    if (playMode) stopPlay();
                    if (loadSceneShowing(target.generic_string(), want) && wasPlaying)
                        startPlay();
                } else {
                    host.hud = "Scene not found: " + want;
                }
            }

            // --- The showroom's craft arrives on the grid ---------------------
            // The circuit is loaded and Play has restarted; now the chosen craft
            // is rebuilt from its JSON (which re-imports its models against the
            // fresh model library), dropped onto the scene's grid slot, and flown.
            // Everything here happens AFTER startPlay took its snapshot, so
            // stopping Play removes the craft again -- the circuit scene on disk
            // never learns that a race was run in it.
            if (!pendingCraftJson.empty()) {
                const nlohmann::json craftJson = std::move(pendingCraftJson);
                pendingCraftJson = nlohmann::json();
                const nlohmann::json craftJson2 = std::move(pendingCraftJson2);
                pendingCraftJson2 = nlohmann::json();
                const int laps = pendingCraftLaps;
                pendingCraftLaps = 0;
                const bool fromShowroom = pendingFromShowroom;
                pendingFromShowroom = false;
                const int   raceMode    = pendingRaceMode;
                const int   raceField   = pendingRaceField;
                const int   raceLevel   = pendingRaceLevel;
                pendingRaceMode  = -1;   pendingRaceField = -1;
                pendingRaceLevel = gameDifficulty.level;
                if (playMode) {
                    // Keep the whole launch for this circuit, so a restart can be
                    // flown in the craft that was chosen rather than in the one
                    // the scene parks on the grid (see pendingRestart above).
                    sessionCraftJson   = craftJson;
                    sessionCraftName   = pendingCraftName;
                    sessionCraftJson2  = craftJson2;
                    sessionCraftName2  = pendingCraftName2;
                    sessionCraftLaps   = laps;
                    sessionRaceMode    = raceMode;
                    sessionRaceField   = raceField;
                    sessionRaceLevel   = raceLevel;

                    // The circuit may be flagged "start in glider mode", in which
                    // case startPlay already put us in its own craft. Let that go
                    // first (restoring its transform) -- the showroom's choice is
                    // what gets flown.
                    endGliderDrive();
                    gliderMode = false;

                    prefab::Prefab p;
                    p.name = pendingCraftName;
                    p.guid = fitzel::AssetId::generate();
                    for (const auto& ej : craftJson)
                        p.entities.push_back(projectio::readEntityJson(pio, ej));

                    // The grid slot: the circuit's own glider if it has one (that
                    // is where a craft is meant to stand), else the start/finish
                    // line, else the player start.
                    glm::vec3 pos(0.0f);
                    int       slot = -1;
                    // Where the craft must POINT. Taken from the grid slot's world
                    // orientation, never from its rotation.y: a decomposed Euler
                    // triple can express the same facing as [-180, y, -180], where
                    // the y component is not the heading at all (two circuits here
                    // are authored exactly that way), and reading .y off one aims
                    // the craft into the scenery. The world matrix is what the
                    // renderer uses, so its axes are the facing by definition.
                    bool  haveHeading = false;
                    float headRad     = 0.0f;
                    // `nose` is the craft's forward in world space, flattened to
                    // the ground plane; the sim's heading convention is
                    // dir = (sin H, 0, cos H) (see updateGlider), hence atan2(x, z).
                    auto headingFrom = [&](const glm::vec3& nose) {
                        glm::vec3 n(nose.x, 0.0f, nose.z);
                        if (glm::length(n) < 1e-4f) return;
                        n = glm::normalize(n);
                        headRad     = std::atan2(n.x, n.z);
                        haveHeading = true;
                    };
                    // The circuit's own chase-camera tuning, lifted off the craft
                    // that was parked on the grid. The arriving craft brings its
                    // flight model -- that is what picking a craft MEANS -- but
                    // not its camera: how close the view sits and how hard it
                    // follows is a property of the track (a tight technical
                    // circuit wants a different camera from an open one), and
                    // every circuit here tunes it separately. Without this, the
                    // showroom's podium craft silently reframes every race.
                    for (const Entity& e : entities)
                        if (const auto* sg = e.components.get<GliderComponent>()) {
                            slot = e.id; pos = e.center;
                            // The model's nose: +Z, or -Z for a craft authored the
                            // other way round (the Glider's own "Model nose").
                            headingFrom(glm::vec3(worldOf(e)[2]) *
                                        (sg->forward == 1 ? -1.0f : 1.0f));
                            break;
                        }
                    // No craft on the grid: line up on the start/finish gate
                    // instead. Its direction of travel is its local +Z after its
                    // own `yaw` offset -- the same axis the sim tests a crossing
                    // along (see overGate), so the craft faces the way a lap runs.
                    if (slot < 0)
                        for (const Entity& e : entities)
                            if (const auto* fl = e.components.get<FinishLineComponent>()) {
                                pos = e.center;
                                const glm::quat gq =
                                    glm::quat(glm::radians(e.rotation)) *
                                    glm::angleAxis(glm::radians(fl->yaw), glm::vec3(0, 1, 0));
                                headingFrom(gq * glm::vec3(0.0f, 0.0f, 1.0f));
                                break;
                            }
                    if (slot < 0 && !haveHeading)
                        for (const Entity& e : entities)
                            if (e.components.get<PlayerStartComponent>()) {
                                pos = e.center;
                                headingFrom(glm::vec3(worldOf(e)[2]));
                                break;
                            }
                    // The slot's own craft steps aside, so the field isn't two
                    // gliders deep on the same square.
                    if (slot >= 0)
                        if (Entity* se = document.find(slot)) se->active = false;

                    // Yaw offset 0, deliberately: instantiate ADDS its yaw to the
                    // root's own, and the root's own is whatever pose the craft
                    // was authored in on the podium (one of these is turned 71
                    // degrees to face the showroom camera). Adding that to the
                    // grid's heading points the craft somewhere between the two.
                    // The heading is set outright below instead.
                    std::vector<Entity> spawn =
                        prefab::instantiate(p, entityCounter, pos, 0.0f);
                    const int rootId = spawn.empty() ? -1 : spawn.front().id;
                    for (Entity& e : spawn) entities.push_back(std::move(e));
                    if (rootId >= 0)
                        if (Entity* re = document.find(rootId))
                            if (auto* rg = re->components.get<GliderComponent>()) {
                                // Point it down the track, level. The offset
                                // mirrors beginGliderDrive's own reading of
                                // rotation.y, so the flight heading comes out as
                                // exactly `headRad` whichever way this craft's
                                // model nose is authored. Level because the sim
                                // computes bank and pitch itself every frame --
                                // an authored tilt would only fight it.
                                if (haveHeading) {
                                    const float rotY = glm::degrees(headRad) -
                                                       (rg->forward == 1 ? 180.0f : 0.0f);
                                    re->localRotation = glm::vec3(0.0f, rotY, 0.0f);
                                    re->rotation      = re->localRotation;
                                }
                            }
                    // Player two's craft, from the same catalogue and by the same
                    // route. It is spawned on top of player one's slot on
                    // purpose: the grid below gives it the place beside it, and
                    // standing them both on the slot first means neither depends
                    // on the scene having authored a second one.
                    int rootId2 = -1;
                    if (!craftJson2.empty()) {
                        prefab::Prefab p2;
                        p2.name = pendingCraftName2;
                        p2.guid = fitzel::AssetId::generate();
                        for (const auto& ej : craftJson2)
                            p2.entities.push_back(projectio::readEntityJson(pio, ej));
                        std::vector<Entity> spawn2 =
                            prefab::instantiate(p2, entityCounter, pos, 0.0f);
                        rootId2 = spawn2.empty() ? -1 : spawn2.front().id;
                        for (Entity& e : spawn2) entities.push_back(std::move(e));
                        if (rootId2 >= 0 && haveHeading)
                            if (Entity* re2 = document.find(rootId2))
                                if (auto* rg2 = re2->components.get<GliderComponent>()) {
                                    const float rotY = glm::degrees(headRad) -
                                                       (rg2->forward == 1 ? 180.0f : 0.0f);
                                    re2->localRotation = glm::vec3(0.0f, rotY, 0.0f);
                                    re2->rotation      = re2->localRotation;
                                }
                    }

                    // The circuit's camera goes with the seat, not with the model
                    // that happened to be standing in it: hand the grid slot's
                    // camera child to the craft that replaced it. A track frames
                    // its own racing (a tunnel circuit wants a closer eye than an
                    // open one), and the slot craft is about to be deactivated
                    // with its camera inside it. A chosen craft that brings its
                    // own camera keeps it -- then the author has said what they
                    // want and nobody should overrule it.
                    if (rootId >= 0 && slot >= 0) {
                        // ALL of them, not the first: a craft that offers a chase
                        // view and a cockpit view is two camera children, and
                        // handing over one of them would leave the C key stepping
                        // between a view and nothing.
                        const auto childCamIds = [&](int parentId) {
                            std::vector<int> ids;
                            for (const Entity& e : entities)
                                if (e.parent == parentId &&
                                    e.components.get<CameraComponent>())
                                    ids.push_back(e.id);
                            return ids;
                        };
                        if (childCamIds(rootId).empty())
                            for (int id : childCamIds(slot))
                                if (Entity* sc = document.find(id)) sc->parent = rootId;
                        // Player two needs eyes of its own, and there is only one
                        // set to hand: copy player one's onto its craft. Without
                        // this the second pane has nothing to draw from and the
                        // screen quietly stays whole -- which would look like the
                        // start screen's two-player choice being ignored.
                        if (rootId2 >= 0 && childCamIds(rootId2).empty())
                            for (int id : childCamIds(rootId)) {
                                const Entity* src = document.find(id);
                                if (!src) continue;
                                Entity cam2 = *src;          // components deep-copy
                                cam2.id     = entityCounter++;
                                cam2.parent = rootId2;
                                cam2.name   = src->name + " P2";
                                entities.push_back(std::move(cam2));
                            }
                    }
                    resolveHierarchy();

                    // The circuit is run over the laps its card promised.
                    if (laps > 0)
                        for (Entity& e : entities)
                            if (auto* fl = e.components.get<FinishLineComponent>())
                                fl->laps = static_cast<float>(laps);
                    // ...and as the kind of session the start screen asked for.
                    // Race or time trial lives on the start/finish line, so that
                    // is where the override goes; racegrid reads it a few lines
                    // below when it lines the grid up, and a time trial sits the
                    // whole field out by itself.
                    if (raceMode >= 0)
                        for (Entity& e : entities)
                            if (auto* fl = e.components.get<FinishLineComponent>())
                                fl->mode = raceMode;
                    // The field: how many of the circuit's rivals take part, and
                    // how hard they push. Everything here happens AFTER
                    // startPlay's snapshot, so a race run against two rookies
                    // never reaches the circuit's .fitzel on disk.
                    // The field: how many of the circuit's rivals take part...
                    if (raceField >= 0) {
                        int kept = 0;
                        for (Entity& e : entities) {
                            auto* op = e.components.get<OpponentComponent>();
                            if (!op || !op->entered) continue;
                            // The craft in the seats are not rivals. Player two's
                            // often carries an Opponent component (that is how a
                            // two-craft track is authored), and trimming the grid
                            // must not count it.
                            if (e.id == rootId || e.id == rootId2) continue;
                            // Scene order decides who stays -- the same order
                            // racegrid builds the grid in -- so a smaller field is
                            // the front of the one the author entered rather than
                            // a random cut of it.
                            if (kept < raceField) ++kept;
                            else                  op->entered = false;
                        }
                    }
                    // ...and the rest of it is BUILT, where the circuit's grid
                    // says so. This is what makes the start screen's field size a
                    // number rather than a ceiling: a marker carrying a prefab
                    // makes its own rival, so a circuit no longer has to be
                    // authored with the biggest field anyone might ask for.
                    //
                    // After the trim (its craft count the same as hand-placed
                    // ones) and BEFORE the difficulty ladder, or a built rival
                    // would race at the circuit's tuning while the rest of the
                    // field ran at the step the player chose.
                    buildGridField(rootId, rootId2, raceField);

                    // ...and how hard they push, which is the whole ladder in one
                    // call (see Difficulty.hpp). Unconditional, unlike the
                    // overrides above: every race is run at SOME step, and PRO is
                    // the one that leaves the circuit exactly as it was tuned.
                    difficulty::applyToField(entities, raceLevel, rootId, rootId2);

                    if (rootId >= 0) {
                        // Fly THIS craft, not "the nearest glider": the grid slot
                        // it replaced is still in the scene, only deactivated.
                        fpsMode = false;
                        input.setCursorLocked(false);
                        gliderMode = true;
                        // Player two gets its craft and its grid slot here too,
                        // for the same reason as in enterGliderMode: a craft
                        // nobody lines up starts in the paddock.
                        race2 = racesim::RaceState{};
                        // The craft the start screen chose for the second seat
                        // wins over the automatic pick: someone answered that
                        // question by hand a moment ago.
                        driveGliderId2 = (rootId2 >= 0) ? rootId2
                                       : (splitScreen ? pickPlayerTwo(rootId) : -1);
                        racegrid::lineUp(entities, roads.active(), rootId,
                                         /*applyParticipation=*/true, driveGliderId2);
                        beginGliderDrive(rootId);
                        if (driveGliderId2 >= 0) seatGliderState(race2, driveGliderId2);
                        bool hasRace = false;
                        for (const Entity& e : entities)
                            if (e.components.get<OpponentComponent>() ||
                                e.components.get<FinishLineComponent>()) {
                                hasRace = true; break;
                            }
                        // Arrived from the start screen: hold the whole field
                        // here, lined up and still, and circle the craft until
                        // the player asks for the race (see RaceState::onGrid).
                        // Only a real race -- a time trial has no field to look
                        // at, and there is nothing to introduce.
                        const bool holdOnGrid = fromShowroom && hasRace &&
                                                racegrid::isRace(entities);
                        race.onGrid   = holdOnGrid;
                        race.gridTime = 0.0f;
                        raceCountdown = (hasRace && !holdOnGrid) ? 3.0f : 0.0f;
                        goFlash   = 0.0f;
                        endPrompt = racehud::EndPrompt{};
                        // Player two waits with player one and leaves with them.
                        race2.onGrid   = holdOnGrid;
                        race2.gridTime = 0.0f;
                        race2.raceCountdown = raceCountdown;  // held on the grid too
                        race2.goFlash = 0.0f;
                        endPrompt2 = racehud::EndPrompt{};
                        // The craft's camera stands itself up on its first frame
                        // (CameraSystem), so the race no longer opens on a swoop
                        // in from wherever the showroom left the editor's eye.
                        cams.reset();
                    } else {
                        host.hud = "Could not place the craft on the grid.";
                    }
                }
            }

            // Drive a non-blocking project/scene load a slice at a time. Kept to a
            // few ms per frame so the editor keeps rendering (and the progress modal
            // below stays live) instead of freezing on a big scene.
            if (sceneLoad.active) projectio::stepLoad(pio, sceneLoad, 8.0);

            // --- Keyframe animation ---------------------------------------
            // BEFORE the UI, not after, and that ordering is the whole of how
            // editing an animated property works. The clip writes its pose here;
            // the Inspector's widget then draws that value, and an edit made in
            // it stands for the rest of the frame -- so dragging a keyed
            // position is something you can see happening. Next frame the clip
            // writes again: with Auto-key on that is the value you just set
            // (it was recorded), and without it the property springs back, which
            // is the true answer to "who owns this number".
            //
            // A running clip advances; a parked one is still applied, so moving
            // the playhead shows the pose at that moment rather than waiting for
            // Play. resolveHierarchy() further down turns the animated LOCAL
            // transforms into world ones, so a keyed parent carries its children.
            for (anim::Playback& pb : animRuntime) {
                if (!pb.player.playing) continue;
                if (pb.clip < 0 || pb.clip >= static_cast<int>(animClips.size())) continue;
                anim::Clip run = animClips[pb.clip];
                // An Animator's Loop overrides the clip's own: whether a walk
                // cycle repeats belongs to the thing walking, not to the walk.
                for (const Entity& e : entities)
                    if (const auto* an = e.components.get<AnimatorComponent>())
                        if (an->clip == run.name && an->playOnStart) { run.loop = an->loop; break; }
                if (!anim::advance(run, entities, pb.player, dt)) pb.player.playing = false;
            }
            // The state machines. Each one asks its graph what should be playing
            // and how far in, then the named clip is applied -- the graph never
            // touches an entity itself, which is what keeps it a machine over
            // clip NAMES and testable without a scene.
            if (playMode || playerMode) {
                // A state's model animation on this object's own model: its
                // index there (-1 = the model has no clip of that name) and its
                // length. By name, so it survives the model's clips reordering.
                auto modelClipOf = [&](const Entity& e, const std::string& name,
                                       float& length) -> int {
                    length = 0.0f;
                    if (name.empty()) return -1;
                    const auto* mc = e.components.get<ModelComponent>();
                    LoadedModel* lm = mc ? models.byId(mc->modelId) : nullptr;
                    if (!lm || !lm->animData) return -1;
                    const auto& clips = lm->animData->animations;
                    for (int i = 0; i < static_cast<int>(clips.size()); ++i)
                        if (clips[i].name == name) { length = clips[i].duration; return i; }
                    return -1;
                };
                for (Entity& e : entities) {
                    auto* ag = e.components.get<AnimGraphComponent>();
                    if (!ag) continue;
                    ag->skinClip = ag->skinFromClip = -1;
                    const int gi = animgraph::findGraph(animGraphs, ag->graph);
                    if (gi < 0) continue;
                    const animgraph::Graph& g = animGraphs[gi];
                    if (ag->runtime.state < 0) animgraph::start(g, ag->runtime);
                    // The length of the clip the CURRENT state names, which is
                    // what loops it and what exit time is measured against. A
                    // model animation's length outranks a Timeline clip's: it
                    // is the one the eye sees loop, and a one-key clip beside it
                    // has no length to loop on at all.
                    float len = 0.0f;
                    if (ag->runtime.state >= 0 &&
                        ag->runtime.state < static_cast<int>(g.states.size())) {
                        const animgraph::State& cur = g.states[ag->runtime.state];
                        const int ci = anim::findClip(animClips, cur.clip);
                        if (ci >= 0) len = animClips[ci].lastKeyTime();
                        float modelLen = 0.0f;
                        if (modelClipOf(e, cur.modelClip, modelLen) >= 0) len = modelLen;
                    }
                    std::string clipName;
                    float clipTime = 0.0f;
                    animgraph::step(g, ag->runtime, dt, len, clipName, clipTime);
                    const int ci = anim::findClip(animClips, clipName);
                    if (ci >= 0) anim::apply(animClips[ci], entities, clipTime);
                    // The skeleton: step() may just have changed state, so this
                    // asks the state it is in NOW. The skinning pass further
                    // down poses the figure from it.
                    if (ag->runtime.state >= 0 &&
                        ag->runtime.state < static_cast<int>(g.states.size())) {
                        float unusedLen = 0.0f;
                        ag->skinClip = modelClipOf(
                            e, g.states[ag->runtime.state].modelClip, unusedLen);
                        ag->skinTime = clipTime;
                    }
                    // A fade: the state being left keeps playing on its own
                    // clock and hands over its share of the pose. Only between
                    // two model animations -- with either side missing there is
                    // no second pose to mix, and the switch is a cut.
                    const animgraph::Instance& in = ag->runtime;
                    if (ag->skinClip >= 0 && in.fadeFrom >= 0 &&
                        in.fadeFrom < static_cast<int>(g.states.size())) {
                        const animgraph::State& from = g.states[in.fadeFrom];
                        float fromLen = 0.0f;
                        const int fc = modelClipOf(e, from.modelClip, fromLen);
                        if (fc >= 0) {
                            ag->skinFromClip  = fc;
                            ag->skinFromTime  = animgraph::clipTime(from, in.fadeFromPhase, fromLen);
                            ag->skinFromShare = animgraph::fadeWeight(in);
                        }
                    }
                }
            }
            if (animPlay.playing) {
                if (!anim::advance(animEdited(), entities, animPlay, dt))
                    animPlay.playing = false;
            } else if (animPlay.preview) {
                anim::apply(animEdited(), entities, animPlay.time);
            }

            // --- UI ------------------------------------------------------
            const long long fzUiMark = prof::mark();
            gui.beginFrame();
            ImGuizmo::BeginFrame();
            if (playMode) {
                // The characters typed this frame, for game.textInput (UTF-8).
                const ImGuiIO& tio = ImGui::GetIO();
                for (int i = 0; i < tio.InputQueueCharacters.Size && scriptTyped.size() < 512; ++i) {
                    const unsigned c = static_cast<unsigned>(tio.InputQueueCharacters[i]);
                    if (c < 0x20 || c == 0x7f) continue;
                    if (c < 0x80) {
                        scriptTyped += static_cast<char>(c);
                    } else if (c < 0x800) {
                        scriptTyped += static_cast<char>(0xC0 | (c >> 6));
                        scriptTyped += static_cast<char>(0x80 | (c & 0x3F));
                    } else {
                        scriptTyped += static_cast<char>(0xE0 | (c >> 12));
                        scriptTyped += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                        scriptTyped += static_cast<char>(0x80 | (c & 0x3F));
                    }
                }
            }
            if (presentMode) {
                // Presentation: hide the editor UI, render the scene full-window.
                window.framebufferSize(viewW, viewH);
                viewportHovered = true;
            }
#ifndef FITZEL_PLAYER
            else {
            // --- Main menu bar (File / Scene / Edit / View / Help) -------
            if (ImGui::BeginMainMenuBar()) {
                editormenu::drawFileMenu(fileMenu);
                editormenu::drawSceneMenu(sceneMenu);
                editormenu::drawEditMenu(editMenu);
                editormenu::drawViewMenu(gui, viewPanels, viewNav, prefsDirty, requestDockRebuild);
                if (ImGui::BeginMenu("Help")) {
                    if (ImGui::MenuItem("Lua API")) luaapi::show(luaApi);
                    ImGui::Separator();
                    if (ImGui::MenuItem("About Fitzel...")) showAbout = true;
                    ImGui::EndMenu();
                }
                // Play / Stop: run the scene as a game (first-person), Stop (or
                // Esc) restores the edited scene and camera exactly.
                ImGui::Separator();
                if (playMode) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
                    if (ImGui::MenuItem("[  Stop  ]")) stopPlay();
                    ImGui::PopStyleColor();
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 1.0f, 0.55f, 1.0f));
                    if (ImGui::MenuItem("|>  Play")) startPlay();
                    ImGui::PopStyleColor();
                }
                ImGui::EndMainMenuBar();
            }

            // The prefab-editing banner, above the toolbar so it is the first
            // thing the eye lands on: while it is up the document is not the
            // scene, every save is refused, and Play will not start. A mode this
            // consequential is not allowed to be a subtle one.
            switch (prefabedit::banner(prefabEdit, entities)) {
                case prefabedit::Action::Save:      savePrefabEdit(); break;
                case prefabedit::Action::SaveClose: savePrefabEdit();
                                                    closePrefabEdit(); break;
                case prefabedit::Action::Discard:   closePrefabEdit();
                                                    exportStatus =
                                                        "Prefab left as it was on disk.";
                                                    break;
                case prefabedit::Action::None:      break;
            }

            // --- Toolbar strip under the menu bar (Toolbar.cpp) ---------------
            {
                using T = icon::Tool;
                toolbar::draw({placeMode, entityNewType,
                               [&](EntityType t) { addEntity(spawnPoint(6.0f), t); },
                               terrainOn, [&] { addTerrainEntity(); },
                               gizmoOp, gizmoMode, playMode, viewShade,
                               [&] { viewtrace::refresh(viewTrace); },
                               viewTool, showRoads,
                               [&] { saveCurrent(); }, !currentProject.empty() && !playMode,
                               {{T::Sculpt, "Terrain sculpt -- raise, lower, pull, erode", &showSculpt},
                                {T::Paint, "Terrain paint -- paint the ground's layers", &showPaint},
                                {T::Rivers, "Rivers & brooks", &showRivers},
                                {T::Water, "Water -- lakes and the sea", &showWater},
                                {T::None, nullptr, nullptr},
                                {T::Road, nullptr, nullptr},
                                {T::Vegetation, "Vegetation -- trees, forests, grass", &showVegetation},
                                {T::Splines, "Splines & bridges -- fences, walls, tracks, bridges", &showSplines},
                                {T::Town, "Town generator", &showTowns},
                                {T::None, nullptr, nullptr},
                                {T::Sky, "Sky & atmosphere -- cloud layers, haze", &showSky},
                                {T::Weather, "Weather & audio -- presets, rain, storm", &showWeather},
                                {T::Environment, "Environment -- time of day, sun, sky image", &showEnv},
                                {T::None, nullptr, nullptr},
                                {T::Materials, "Materials", &showMaterials},
                                {T::Prefabs, "Prefabs", &showPrefabs},
                                {T::Assets, "Assets", &showAssets},
                                {T::Modeling, "Modeling -- the selected object's vertices, edges, faces (Tab)", &showModeling}}});
            }

            // --- New Project / Save As wizard (EditorMenus.cpp) ---------------
            editormenu::drawProjectWizard(fileMenu, [&] { newProject(); },
                                          [&](const std::string& t) { saveProjectTo(t); });

            // --- Crash recovery ----------------------------------------------
            // A snapshot outlived its session, so the editor comes up asking about
            // it before anything else. Restoring loads it as the document without
            // writing a byte into the project: the user looks at what came back
            // and decides with Ctrl+S, which is the only place work becomes real.
            if (pendingSnapshot.valid()) {
                switch (autosave::drawRecoveryModal(pendingSnapshot)) {
                case autosave::Choice::Restore: {
                    const autosave::Snapshot snap = pendingSnapshot;
                    pendingSnapshot = autosave::Snapshot{};
                    if (!restoreSnapshot(snap))
                        std::fprintf(stderr, "Could not restore the snapshot %s\n",
                                     snap.file.c_str());
                    // Consumed either way: the file has been read into the
                    // document, and one left on disk would be offered again at
                    // the next start as though it were still missing work.
                    autosave::discard(autoSave.dir());
                    break;
                }
                case autosave::Choice::Discard:
                    autosave::discard(autoSave.dir());
                    pendingSnapshot = autosave::Snapshot{};
                    break;
                case autosave::Choice::None:
                    break;
                }
            }

            // --- Game Settings dialog ----------------------------------------
            if (gameSettingsOpen) { ImGui::OpenPopup("Game Settings"); gameSettingsOpen = false; }
            if (!currentProject.empty()) {
                const std::string gsFolder =
                    std::filesystem::path(currentProject).parent_path().generic_string();
                std::vector<std::string> sceneStems;
                for (const auto& sc : projectio::listScenesIn(gsFolder))
                    sceneStems.push_back(sc.first);
                if (game::drawSettingsModal("Game Settings", gameSettings,
                                            sceneStems, gsFolder))
                    game::save(gsFolder, gameSettings);
            }

            // --- Scene manager dialogs: New / Rename / Delete (EditorMenus.cpp)
            editormenu::drawSceneDialogs(sceneMenu, {
                [&](const std::string& folder, const std::string& name) {
                    saveSceneFile(currentProject);   // keep the scene we leave
                    resetWorldForNewScene();         // blank terrain/road/vegetation
                    newSceneInProject(folder, name);
                },
                [&](const std::string& name) { renameScene(currentProject, name); },
                [&](const std::string& next, const std::string& gone) {
                    loadSceneFile(next);
                    deleteSceneFile(gone);
                },
            });

            // Non-blocking project/scene load: a modal over the (still-rendering)
            // editor shows progress while stepLoad streams the scene in over the
            // next frames. Being modal, it also stops the half-built scene from
            // being clicked/edited mid-load. It closes itself the frame the loader
            // finishes (stepLoad clears sceneLoad.active before this runs).
            if (sceneLoad.active && !ImGui::IsPopupOpen("Loading project"))
                ImGui::OpenPopup("Loading project");
            if (ImGui::BeginPopupModal("Loading project", nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
                ImGui::TextUnformatted(sceneLoad.label.empty() ? "Loading..."
                                                              : sceneLoad.label.c_str());
                ImGui::Spacing();
                ImGui::ProgressBar(sceneLoad.progress, ImVec2(360.0f, 0.0f));
                if (!sceneLoad.active) ImGui::CloseCurrentPopup(); // finished this frame
                ImGui::EndPopup();
            }

            const ImGuiID dockId = gui.dockspace();

            if (requestDockRebuild || ImGui::DockBuilderGetNode(dockId) == nullptr) {
                requestDockRebuild = false;
                editormenu::buildDefaultDockLayout(dockId);
            }

            // Central scene viewport: shows the composited render texture. Its
            // content size drives the render resolution (set below for next pass).
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            if (ImGui::Begin("Scene", nullptr, ImGuiWindowFlags_NoScrollbar
                                             | ImGuiWindowFlags_NoScrollWithMouse)) {
                const ImVec2 avail = ImGui::GetContentRegionAvail();
                viewW = std::max(1, static_cast<int>(avail.x));
                viewH = std::max(1, static_cast<int>(avail.y));
                // In the Pathtraced mode the picture comes from the tracer
                // instead -- until it has one, which is why the raster frame is
                // still drawn every frame and stands in here. Its image is
                // top-down (the tracer hands over rows, not a GL target), so it
                // is the one image in the editor NOT drawn flipped.
                const unsigned int traced =
                    (viewShade == kShadePathTraced && !playMode)
                        ? viewtrace::texture(viewTrace) : 0u;
                if (traced) {
                    ImGui::Image((ImTextureID)(intptr_t)traced,
                                 ImVec2(static_cast<float>(viewW),
                                        static_cast<float>(viewH)));
                } else {
                    // GL textures are bottom-up: flip V (uv0.y=1, uv1.y=0).
                    ImGui::Image((ImTextureID)(intptr_t)viewportRT.colorTexture(),
                                 ImVec2(static_cast<float>(viewW),
                                        static_cast<float>(viewH)),
                                 ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
                }
                // THE IMAGE'S OWN rect and hover, taken the moment it is drawn.
                //
                // Everything below draws over it, and "the last item" stops being
                // the picture the instant one of those overlays is a real widget
                // rather than a draw-list call. Reading IsItemHovered() at the
                // bottom then asks whether the mouse is over that widget -- so the
                // viewport answers "no" nearly everywhere and the scene cannot be
                // navigated or picked at all. That is exactly what the Play-as
                // picker did the day it was added.
                //
                // Held in named values instead, so an overlay added later is a
                // drawing decision and not a trap.
                const ImVec2 sceneMin = ImGui::GetItemRectMin();
                const ImVec2 sceneMax = ImGui::GetItemRectMax();
                bool sceneHovered = ImGui::IsItemHovered();
                // The selected camera's own view, bottom right, over the scene.
                if (camPreviewId >= 0 && showCamPreview && !traced)
                    viewhud::cameraPreview(sceneMin, sceneMax,
                                           (ImTextureID)(intptr_t)camPreviewRT.colorTexture(),
                                           camPreviewRT.width(), camPreviewRT.height(),
                                           camPreviewName);

                // Top right: what Play will start as (see ViewportHud.hpp). While
                // the pointer is on it, it is not on the scene.
                if (!playMode && viewhud::playAsPicker(sceneMin, sceneMax, sceneStartMode, history))
                    sceneHovered = false;

                // What the tracer is doing, over its own picture: it restarts
                // whenever the camera moves, so "waiting for the view to settle"
                // is a thing it has to be able to say.
                if (viewShade == kShadePathTraced && !playMode)
                    viewhud::traceStatus(sceneMin, viewTrace.status);
                viewportHovered = sceneHovered;
                // Cursor position inside the image, mapped to NDC (for picking).
                // The IMAGE's rect again, not the last item's -- see above: this
                // is what turns a mouse position into a ray, so an overlay's rect
                // here would aim every pick at whatever the overlay covers.
                const ImVec2 rmin = sceneMin;
                const ImVec2 rsz(sceneMax.x - sceneMin.x, sceneMax.y - sceneMin.y);
                viewportRectMin  = glm::vec2(rmin.x, rmin.y); // for the play crosshair
                viewportRectSize = glm::vec2(rsz.x, rsz.y);
                // ...and all of it, as the tools outside main() take it.
                ViewportFrame sceneView = ViewportFrame::looking(
                    camera, rmin, static_cast<float>(viewW), static_cast<float>(viewH),
                    ImGui::GetIO().MousePos, viewportHovered);
                sceneView.pickTerrain = roadPickTerrain;
                sceneView.groundAt    = [&streamer](float x, float z) {
                    return streamer.heightAt(x, z);
                };
                viewportMouseNdc = sceneView.mouseNdc;
                // Keep the multi-selection consistent with the active object before
                // any panel/viewport consumes it this frame.
                sel.normalize();
                viewportClicked = viewportHovered &&
                                  ImGui::IsMouseClicked(ImGuiMouseButton_Left);

                // A camera preview owns the viewport: say which one, and give it
                // a way out right where it took the view from. The pick test
                // above already latched a click -- one on the button must not
                // also select whatever is behind it.
                if (!playMode && activeCam >= 0) {
                    const Entity* pcam = document.find(activeCam);
                    if (viewhud::exitCamera(rmin, pcam ? pcam->name : std::string("(gone)"),
                                            activeCam))
                        viewportClicked = false;
                }

                // Which way we are looking, when it is a standard view, and the
                // lens when it is orthographic (ViewportHud.hpp).
                if (!playMode)
                    viewhud::viewLabel(rmin, viewnav::label(viewNav.current()), camera.orthographic());

                // UI overlay authoring preview: while the overlay editor is open and
                // we're not playing, draw the 2D elements over the viewport (clipped
                // to the Scene window) with the selected one outlined, so placement
                // is visible without pressing Play.
                if (showUiOverlay && !playMode && !uiOverlay.empty()) {
                    uiOverlay.drawAuthoring(ImGui::GetWindowDrawList(),
                                            glm::vec2(rmin.x, rmin.y),
                                            glm::vec2(rsz.x, rsz.y), assetDb, uiSel);
                }

                // Drag an asset from the Assets browser into the viewport: a Model
                // drops onto the terrain; a Texture drops onto the object under the
                // cursor, making a fresh material that uses it and assigning it.
                //
                // The target is the IMAGE's rect, named explicitly: the plain
                // BeginDragDropTarget() binds to the last item, and over the
                // scene that is whichever overlay widget was drawn last (the
                // Play-as picker, the modelling toolbar), not the picture.
                if (ImGui::BeginDragDropTargetCustom(ImRect(sceneMin, sceneMax),
                                                     ImGui::GetID("##sceneDrop"))) {
                    if (const ImGuiPayload* pl =
                            ImGui::AcceptDragDropPayload("ASSET_GUID"))
                        scenedrop::dropOnScene(editorCtx, sceneView,
                                               AssetId::fromString(std::string(
                                                   static_cast<const char*>(pl->Data),
                                                   pl->DataSize)));
                    ImGui::EndDragDropTarget();
                }

                // --- Road edit handles: the tool is in RoadEdit.cpp; main only
                //     hands it the viewport and the undo bracket, like the
                //     spline handles below.
                if (viewTool == ViewTool::Road) {
                    roadedit::Context rc{roads, roadSel, roadSel2, roadDragging, roadDragHeight};
                    rc.view        = sceneView;
                    rc.beginEdit   = beginRoadEdit;
                    rc.endEdit     = commitRoadEdit;
                    rc.editOpen    = [&roadUndoOpen] { return roadUndoOpen; };
                    rc.addPoint    = addRoadPoint;
                    rc.deletePoint = deleteRoadPoint;
                    roadedit::handle(rc);
                }

                // --- Spline handles: the same gesture the road editor uses, for
                //     fences, walls and track. The tool itself is in SplineEdit.cpp
                //     -- main only hands it the viewport and the undo bracket.
                // Where "Place along path" would put its copies, while the
                // panel's placement section is open (see SplinePlace.hpp).
                std::vector<splineplace::Spot> splinePreview;
                if (showSplines && splinePlaceCfg.preview)
                    splinePreview = splineplace::spots(splines, splineSel, splinePlaceCfg);
                auto splineContext = [&]() {
                    splineedit::Context sc{splines, splineSel, splinePtSel,
                                           splineDragging, splineDragHeight};
                    sc.view      = sceneView;
                    sc.beginEdit = beginSplineEdit;
                    sc.endEdit   = commitSplineEdit;
                    sc.editOpen  = [&splineUndoOpen] { return splineUndoOpen; };
                    sc.preview   = splinePreview.empty() ? nullptr : &splinePreview;
                    return sc;
                };
                if (viewTool == ViewTool::Spline) {
                    splineedit::handle(splineContext());
                } else if (showSplines) {
                    // Panel open, edit mode off: still show the paths -- a bare
                    // one has nothing else to be seen by -- and the preview.
                    splineedit::draw(splineContext());
                }

                // --- Water handles: the same gesture again, for brooks, rivers
                //     and canals. The tool is in RiverEdit.cpp -- main only hands
                //     it the viewport and the undo bracket, and the bracket is
                //     what cuts the bed when the gesture ends.
                if (viewTool == ViewTool::River) {
                    riveredit::Context rc{rivers, riverSel, riverPtSel,
                                          riverDragging, riverDragHeight};
                    rc.view      = sceneView;
                    rc.beginEdit = beginRiverEdit;
                    rc.endEdit   = commitRiverEdit;
                    rc.editOpen  = [&riverUndoOpen] { return riverUndoOpen; };
                    riveredit::handle(rc);
                }

                // --- Vehicle setup handles: the tuning geometry drawn where it
                //     actually is, and draggable. Only for the selected vehicle,
                //     and only DRAWN unless the panel's toggle is on -- seeing the
                //     shape costs nothing and is most of the value, while taking
                //     the left button off the transform gizmo has to be asked for.
                //     The tool is in VehicleGizmo.cpp.
                vehGizmoOwnsMouse = false;
                if (!playMode && sel.valid()) {
                    Entity& ve = entities[sel.index()];
                    if (auto* gvc = ve.components.get<VehicleComponent>()) {
                        vehiclegizmo::Context gc{*gvc, worldOf(ve),
                                                 vehGizmoSel, vehGizmoDrag};
                        gc.editable  = vehGizmoEdit;
                        gc.view      = sceneView;
                        // Where the collision box sits is main's relation (it is
                        // what places the Jolt body at Play), so the gizmo asks
                        // rather than repeating it -- a box drawn a hand's width
                        // from where physics puts it would be worse than no box.
                        gc.boxCenterY = [&](const VehicleComponent& v) {
                            return -vehicleVisualY(v);
                        };
                        const int vid = ve.id;
                        gc.beginEdit = [&, vid] { beginVehicleEdit(vid); };
                        gc.endEdit   = commitVehicleEdit;
                        gc.editOpen  = [&] { return vehGizmoUndoOpen; };
                        vehGizmoOwnsMouse = vehiclegizmo::handle(gc) || vehGizmoEdit;
                    }
                }

                // --- Grass brush: stamp/erase instanced blades under a circular
                //     3D brush that hugs the terrain. Hold LMB and drag to paint;
                //     hold Alt (or toggle Erase) to rub grass out. -------------
                if (viewTool == ViewTool::Grass) {
                    const groundbrush::Aim at = groundbrush::aim(sceneView, brushErase);
                    // Throttled so a slow drag doesn't pile blades up: a stamp
                    // every ~0.4 radius makes an even trail.
                    groundbrush::drag(
                        at, lastStampPos, brushRadius * 0.4f,
                        [&](glm::vec2 p) {
                            veg.stampGrass(p, brushRadius, brushRng, brushDensity, waterLevel,
                                           look.snowLevel);
                        },
                        [&](glm::vec2 p) { veg.eraseGrass(p, brushRadius); });
                    groundbrush::ring(sceneView, at, brushRadius, IM_COL32(120, 235, 120, 220));
                }

                // --- Tree brush: scatter/erase hand-placed trees under a circular
                //     3D brush. Drag LMB to plant; hold Alt (or Erase) to remove.
                if (viewTool == ViewTool::Trees) {
                    const groundbrush::Aim at = groundbrush::aim(sceneView, brushErase);
                    groundbrush::drag(
                        at, lastStampPos, veg.treeBrushRadius * 0.5f,
                        [&](glm::vec2 p) {
                            veg.stampTree(p, veg.treeBrushRadius, brushRng, waterLevel,
                                          look.snowLevel);
                        },
                        [&](glm::vec2 p) { veg.eraseTree(p, veg.treeBrushRadius); });
                    groundbrush::ring(sceneView, at, veg.treeBrushRadius,
                                      IM_COL32(90, 200, 120, 220));
                }

                // --- Flower brush: scatter/erase hand-placed blooms under a
                //     circular 3D brush. Drag LMB to plant; Alt (or Erase) removes.
                if (viewTool == ViewTool::Flowers) {
                    const groundbrush::Aim at = groundbrush::aim(sceneView, brushErase);
                    groundbrush::drag(
                        at, lastStampPos, veg.flowerBrushRadius * 0.4f,
                        [&](glm::vec2 p) {
                            veg.stampFlower(p, veg.flowerBrushRadius, brushRng, waterLevel,
                                            look.snowLevel);
                        },
                        [&](glm::vec2 p) { veg.eraseFlower(p, veg.flowerBrushRadius); });
                    groundbrush::ring(sceneView, at, veg.flowerBrushRadius,
                                      IM_COL32(240, 150, 210, 220));
                }

                // --- Object scatter brush: sprinkle weighted random models under
                //     a circular 3D brush (one stamp = one undo step). Drag LMB
                //     to scatter; hold Alt (or Erase) to remove scattered objects.
                if (viewTool == ViewTool::Scatter) {
                    const groundbrush::Aim at = groundbrush::aim(sceneView, brushErase);
                    // Throttled so a slow drag doesn't pile objects up: a stamp
                    // every ~0.6 radius makes an even trail.
                    groundbrush::drag(at, lastStampPos, scatterCfg.radius * 0.6f,
                                      [&](glm::vec2 p) { scatterStamp(p); },
                                      [&](glm::vec2 p) { scatterErase(p); });
                    groundbrush::ring(sceneView, at, scatterCfg.radius, IM_COL32(255, 190, 90, 220));
                }

                // --- Terrain sculpt brush (SculptPanel.cpp): raise/lower/smooth/
                //     flatten the ground under a 3D disc that hugs the surface. --
                if (viewTool == ViewTool::Sculpt)
                    sculptui::brushViewport(sculpt, sceneView, sculptWork, streamer, publishSculpt,
                                            veg.grassDirty, dt);

                // --- Terrain texture paint brush: paint the chosen layer onto the
                //     ground under a 3D disc. Hold LMB to paint; Alt (or Erase)
                //     reverts toward the automatic height/slope blend. ----------
                if (viewTool == ViewTool::Paint) {
                    const groundbrush::Aim at = groundbrush::aim(sceneView, paintErase);
                    if (at.onGround && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        const glm::vec2 c(at.center.x, at.center.z);
                        const float rate = glm::clamp(paintStrength * 4.0f * dt, 0.0f, 1.0f);
                        if (at.erasing) paintWork.erase(c, paintRadius, rate);
                        else            paintWork.paint(c, paintRadius, paintLayer, rate);
                        // Republish + rebuild the touched chunks (paint is baked into
                        // the mesh, so it rides the same edit-rebuild path as sculpt).
                        publishPaint();
                        const float m = paintRadius + 2.0f * paintWork.cell;
                        streamer.editsChanged(glm::vec2(c.x - m, c.y - m),
                                              glm::vec2(c.x + m, c.y + m));
                    }
                    // Brush cursor: teal paint / grey erase.
                    groundbrush::ring(sceneView, at, paintRadius, IM_COL32(90, 230, 210, 225),
                                      IM_COL32(205, 205, 215, 225), 56);
                }

                // --- Mesh texture paint brush: the terrain's layers, brushed
                //     onto the selected modelled object. Hold LMB over the mesh;
                //     Alt (or Erase) takes the paint back off. --------------
                //     Also while a stroke is still open after the button went
                //     to another tool: that call only closes it.
                if (viewTool == ViewTool::MeshPaint || meshBrush.stroking) {
                    bool on = viewTool == ViewTool::MeshPaint;
                    meshpaintui::brushViewport(editorCtx, sceneView, meshBrush, on, dt);
                    takeTool(viewTool, ViewTool::MeshPaint, on);
                }

                // --- Volumetric fog volume: a wireframe box while it is being
                //     placed -------------------------------------------------
                // A body of mist has no edges to see, which makes its box the one
                // thing in the scene you cannot aim at by looking at the result:
                // turn the density up far enough to find the boundary and you are
                // no longer looking at the fog you are trying to author. So the
                // box is drawn, from the same helper the march is fed by -- what
                // is outlined here IS what is marched, follow-camera included.
                if (volFogSet.showVolume && !playMode) {
                    glm::vec3 lo, hi;
                    VolumetricFog::worldBox(volFogSet, camera.position(), lo, hi);
                    sceneView.wireBox(glm::mat4(1.0f), lo, hi,
                                      volFogSet.enabled ? IM_COL32(150, 200, 255, 190)
                                                        : IM_COL32(150, 200, 255, 80),
                                      1.5f);
                }

                // --- Solid blocks: click to select an existing box or place a
                //     new one on the terrain; Del removes the selected block. ----
                {   // Viewport interaction: selecting works in both modes; the
                    // transform gizmo and click-to-place are Edit-mode only.
                    const glm::mat4& vp = sceneView.viewProj;

                    // --- Blender-style 3D cursor -----------------------------
                    // Shift+Right-click places it (the look control ignores
                    // right-drag while Shift is held, see above); its mark.
                    cursor3d::viewport(sceneView, cursor, !playMode);
                    // The mesh being modelled: its keys, and its wireframe,
                    // corners, the element under the pointer, the selection and
                    // the flash of the last edit as a 2D overlay (ModelMode.cpp).
                    if (showModeling && !playMode) {
                        modelmode::ViewportHost mh;
                        mh.gizmoOut    = entityEditMode;
                        mh.faceDragged = gizmoDrag.faceActive;
                        mh.frame       = frameSphere;
                        modelmode::viewport(editorCtx, sceneView, modelSess, mh);
                    }

                    // Shift+S opens the Blender-style snap menu (Ctrl+S stays Save).
                    cursor3d::snapMenu(editorCtx, sceneView, cursor, !playMode);

                    // The picked Curve's points as handles (ProcGraphPanel), ahead
                    // of the gizmo: a point under the pointer takes the left
                    // button from it and from click-to-select.
                    const bool procHandles = !playMode && !modelling && viewTool == ViewTool::None &&
                                             !vehGizmoOwnsMouse && !ImGuizmo::IsUsing() &&
                                             procPanel.handles(sceneView);

                    // The transform gizmo: on the selected object, or while
                    // modelling on the picked face (TransformGizmo.hpp).
                    {
                        gizmo::Settings gs;
                        gs.op         = gizmoOp;
                        gs.mode       = gizmoMode;
                        gs.editMode   = entityEditMode;
                        gs.objectFree = !vehGizmoOwnsMouse && !meshBusy && !procHandles;
                        gs.faceMode   = showModeling && !meshBusy;
                        gs.modelSel   = &modelSess.sel;
                        gs.grid       = cursor.grid;
                        gs.snapAngle  = cursor.snapAngle;
                        gs.snapScale  = cursor.snapScale;
                        gizmo::frame(editorCtx, sceneView, gizmoDrag, gs);
                    }
                    // The selection's wire boxes and its component gizmos -- after
                    // the gizmo, so they show where it put things this frame.
                    overlay::selection(editorCtx, sceneView);
                    // The procedural graph's curves and selected points (ProcGraphPanel).
                    if (!playMode) procPanel.viewport(sceneView);

                    // Empties have no mesh: an icon at each (see ViewportOverlay.hpp).
                    if (!playMode) overlay::empties(entities, sceneView);

                    // Click to select/place, but not while grabbing the gizmo or
                    // running a viewport tool -- the active tool owns the left
                    // button then. One predicate rather than a negation list at
                    // the `if`: a tool missing from that list silently gets both
                    // actions on one click (road points used to drop a primitive
                    // under every waypoint placed in Create mode).
                    const bool toolOwnsClick =
                        viewTool != ViewTool::None || vehGizmoOwnsMouse || modelling || procHandles;
                    viewpick::Host pickHost;
                    pickHost.canPick   = !ImGuizmo::IsOver() && !ImGuizmo::IsUsing() &&
                                         !toolOwnsClick && viewportHovered;
                    pickHost.placeMode = placeMode;
                    pickHost.place     = [&](const glm::vec3& at) { addEntity(at, entityNewType); };
                    // While modelling, a click that lands on the selected mesh
                    // picks one of its faces (or corners, or edges).
                    pickHost.meshClick = [&] {
                        return showModeling && modelmode::click(editorCtx, sceneView, modelSess);
                    };
                    viewpick::click(editorCtx, sceneView, scenePick, pickHost);
                    // While modelling, Del is the mesh's (the modelling mode's
                    // delete menu) -- never the whole object.
                    // ...nor while the Procedural window has the keyboard: there,
                    // the key removes nodes of the graph. The Image editor's Del
                    // clears what is selected in the picture.
                    if (sel.valid() && !modelling && !procPanel.ownsKeys() &&
                        !imageEditor.hasKeyboard() && ImGui::IsKeyPressed(ImGuiKey_Delete))
                        deleteSelection();
                }
            } else {
                viewportHovered = false;
                viewportClicked = false;
            }
            ImGui::End();
            ImGui::PopStyleVar();

            luaapi::draw(luaApi, gui.monoFont());

            editormenu::drawAbout(showAbout);

            viewui::drawStats({showStats, scene, applyScene, camera, streamer, renderer,
                               static_cast<int>(entities.size()), static_cast<int>(sel.count()),
                               viewRadius, farPlaneAuto, farPlaneManual, requestDockRebuild});

            viewui::drawCamera({showCamera, camera, fpsMode, [&](bool on) {
                input.setCursorLocked(on);
                fpsVelY = 0.0f;
                if (on) {
                    const glm::vec3 p = camera.position();
                    camera.setPosition({p.x, streamer.heightAt(p.x, p.z) + eyeHeight, p.z});
                }
            }});

            // The audio mixer. The desk is drawn in MixerPanel.cpp; what it
            // needs from here is the state, the frame time (the meters have
            // ballistics) and whether anything is playing at all.
            if (showMixer)
                mixerui::drawPanel({showMixer, mix, static_cast<float>(dt),
                                    audio.ok(), playMode});

            // The landscape past the terrain: horizon, forest, wind, sun, life.
            natureui::drawPanel({showNature, farTerrainOn, farTerrain.snowLevel,
                                 farTerrain.treeLine, meadowTint, veg.grassDryGrowth,
                                 veg.eco, veg.impostorStart, veg.impostorShadowDist,
                                 veg.forestRadius,
                                 forestFloorLayer, windStrength, windAngle, windGust,
                                 sunLatitude, sunDeclination, cloudShadowsOn,
                                 wildlifeOn, motesOn, soundscapeOn, timeFlows});

            if (showWeather) {
                // The presets belong to the PROJECT, so the list is re-read
                // whenever the open one changes. Here rather than at each of the
                // four places a project can be opened from (open, recent, crash
                // recovery, the player's boot): this runs after all of them, and
                // a string compare per frame is cheaper than four hooks that can
                // fall out of step.
                const std::string wxFolder =
                    currentProject.empty()
                        ? std::string()
                        : std::filesystem::path(currentProject)
                              .parent_path().generic_string();
                if (wxFolder != weatherPresetsFolder) {
                    weatherPresetsFolder = wxFolder;
                    weatherPresets       = weather::load(wxFolder);
                }
                const glm::vec3 wxEye = camera.position();
                weatherui::drawPanel({showWeather, liveWeather(), weatherPresets,
                                      weatherCurrent, weatherSavesTime,
                                      weatherSavesMist, wxFolder,
                                      weatherNameBuf, sizeof(weatherNameBuf),
                                      rainIntensity, roadWetness,
                                      streamer.heightAt(wxEye.x, wxEye.z),
                                      mix.master.mute, mix.master.level,
                                      audio.ok()});
            }

            lookui::drawSkyPanel({showSky, timeOfDay, timePaused, dayLength, skySet, volFogSet,
                                  postLook, splitScreen, renderer, post.autoExposureScale(),
                                  camera.position(),
                                  [&streamer](float x, float z) {
                                      return streamer.heightAt(x, z);
                                  }});

            lookui::drawGradePanel(showColorGrade, postLook);

            lookui::drawWaterPanel({showWater, waterLevel, waveHeight, waveChoppy, waveStrength,
                                    waveScale, foamWidth, waterReflectivity, waterClarity,
                                    waterIor, waterColor});

            // Serve finished texture thumbnails to every panel drawn this frame
            // (materials, terrain, assets) from the shared cache.
            pumpThumbnails();

            terrainui::drawPanel({
                showTerrain, uiSettings, streamer, camera, look,
                texScale, normalStrength, veg.grassDirty, veg.treeCenter, roadsDirty,
                assetDb, thumbFor,
                sculptWork, publishSculpt, paintWork, publishPaint,
                terrainOn, [&]{ addTerrainEntity(); },
            });

            // The terrain panel only ever SETS that flag; hand it on to every
            // road, since regenerating the ground moved all of them.
            if (roadsDirty) { roads.markNeedsBuild(); roadsDirty = false; }

            // Each tool panel shows its own on/off; switching one on takes the
            // viewport's left button from whichever tool had it (ViewTool.hpp).
            {
                bool on = viewTool == ViewTool::Sculpt;
                sculptui::drawPanel({
                    showSculpt, on,
                    sculpt,
                    sculptWork, streamer, veg.grassDirty, publishSculpt,
                });
                takeTool(viewTool, ViewTool::Sculpt, on);
            }
            {
                bool on = viewTool == ViewTool::Paint;
                paintui::drawPanel({
                    showPaint, on,
                    look, paintLayer, paintRadius, paintStrength, paintErase,
                    paintWork, streamer, publishPaint,
                });
                takeTool(viewTool, ViewTool::Paint, on);
            }

            {
                // Children of the "Scattered" group, for the panel's counter.
                int scatteredCount = 0;
                const int sg = findScatterGroup();
                if (sg >= 0)
                    for (const Entity& e : entities)
                        if (e.parent == sg) ++scatteredCount;
                bool on = viewTool == ViewTool::Scatter;
                scatterui::drawPanel({
                    showScatter, on,
                    brushErase, scatterCfg, models, scatteredCount,
                    roads.active().roadPts.size() >= 2,
                    scatterRoadside, scatterClearAll,
                });
                takeTool(viewTool, ViewTool::Scatter, on);
            }

            if (showBuildings) {
                buildingui::drawPanel({
                    showBuildings, buildingCfg,
                    buildings::objectCount(buildingCfg),
                    !currentProject.empty(),
                    document.indexOf(buildingLiveId) >= 0,
                    buildingNameBuf, sizeof(buildingNameBuf),
                    buildingAuto, buildingPending,
                    generateBuilding, rebuildBuilding, saveBuildingPrefab,
                    exportStatus,
                });
            }

            if (showSigns) signTool.panel(showSigns);
            if (showTreeGen) treeGen.panel(showTreeGen);
            if (showRetarget) retargetTool.panel(showRetarget);
            if (showImageEditor) imageEditor.panel(showImageEditor);
            procPanel.draw(showProcedural);

            if (showHouses) {
                houseui::drawPanel({
                    showHouses, houseCfg, houseLevel,
                    !currentProject.empty(),
                    document.indexOf(houseLiveId) >= 0,
                    selectedHouseId() >= 0,
                    houseNameBuf, sizeof(houseNameBuf),
                    houseAuto, housePending,
                    generateHouse, rebuildHouse, loadSelectedHouse, saveHousePrefab,
                    exportStatus,
                });
            }

            // The Vegetation window (VegetationSystem.cpp). Its three brushes'
            // switches come back here; only the one clicked is written back, so a
            // brush still showing "on" this frame cannot take the button back
            // from the one just switched on.
            if (showVegetation) {
                const bool grass0 = viewTool == ViewTool::Grass;
                const bool trees0 = viewTool == ViewTool::Trees;
                const bool flow0  = viewTool == ViewTool::Flowers;
                bool grass = grass0, trees = trees0, flowers = flow0;
                veg.panel(showVegetation,
                          {grass, trees, flowers, brushErase, brushRadius, brushDensity});
                if (grass != grass0)   takeTool(viewTool, ViewTool::Grass, grass);
                if (trees != trees0)   takeTool(viewTool, ViewTool::Trees, trees);
                if (flowers != flow0)  takeTool(viewTool, ViewTool::Flowers, flowers);
            }

            if (showCamPath) { if (ImGui::Begin("Camera path", &showCamPath)) {
                camPathRec.panel(camera);
            }
            ImGui::End(); }

            // The keyframe timeline. Open, it previews the clip (the scene shows
            // the pose at the playhead); closed, the scene goes back to the
            // values the author typed -- which is why the close is handled here
            // rather than inside the panel, whose own code does not run once it
            // is shut.
            timelineui::drawPanel({showTimeline, animClips, animEditClip, animPlay,
                                   entities, sel, animAutoKey,
                                   [&]{ history.touch(); }});
            if (!showTimeline && animPlay.preview)
                anim::endPreview(animEdited(), entities, animPlay);

            // The state machines, drawn. It reads the clip library (a state IS a
            // clip) and the scene (to light the node the selected object is in),
            // and it is the only place a graph is authored.
            graphui::drawPanel({showGraphEditor, animGraphs, animEditGraph, animClips,
                                entities, sel, playMode || playerMode,
                                [&]{ history.touch(); },
                                [&](const Entity& e) {
                                    std::vector<std::string> names;
                                    const auto* mc = e.components.get<ModelComponent>();
                                    LoadedModel* lm = mc ? models.byId(mc->modelId) : nullptr;
                                    if (lm && lm->animData)
                                        for (const auto& c : lm->animData->animations)
                                            names.push_back(c.name);
                                    return names;
                                }});

            // Roads + bridges: the whole panel lives in RoadPanel.cpp; main only
            // hands it the state it may touch (see roadui::PanelState).
            bool roadOn = viewTool == ViewTool::Road;
            roadui::drawPanel({showRoads, roads, roadOn, roadSel, roadSel2, assetDb,
                buildRoad, deleteRoadPoint,
                addRoad, deleteRoad, selectRoad,
                roadPrefabCfg,
                [&]{ const std::string d = prefabDir();
                     return d.empty()
                         ? std::vector<std::pair<std::string, std::string>>()
                         : prefab::list(d); },
                placeRoadPrefabs,
                beginRoadEdit, commitRoadEdit});
            takeTool(viewTool, ViewTool::Road, roadOn);

            // Fences, walls and railway track: the paths live in SplineSystem
            // (saved + undoable on their own timeline), the panel only edits them.
            // See SplinePanel.cpp.
            bool splineOn = viewTool == ViewTool::Spline;
            splineui::drawPanel({showSplines, splines, splineOn, splineSel,
                splinePtSel, materials,
                [&](fitzel::AssetId id) {
                    // Jump to the material the author just pointed an element at,
                    // so giving it a texture is one click from the picker.
                    const int mi = document.materialIndex(id);
                    if (mi >= 0) matSel = mi;
                    showMaterials = true;
                },
                beginSplineEdit, commitSplineEdit,
                splinePlaceCfg,
                [&]{ return sel.valid() ? entities[sel.index()].name : std::string(); },
                [&]{ const std::string d = prefabDir();
                     return d.empty()
                         ? std::vector<std::pair<std::string, std::string>>()
                         : prefab::list(d); },
                placeAlongSpline});
            takeTool(viewTool, ViewTool::Spline, splineOn);

            // Brooks, rivers and canals: the courses live in RiverSystem (saved +
            // undoable on their own timeline), the panel only edits them. See
            // RiverPanel.cpp.
            bool riverOn = viewTool == ViewTool::River;
            riverui::drawPanel({showRivers, rivers, riverOn, riverSel,
                riverPtSel,
                beginRiverEdit, commitRiverEdit});
            takeTool(viewTool, ViewTool::River, riverOn);

            // Roadside city: the biome rules live on the road (saved + undoable
            // with it), the panel only edits them. See CityPanel.cpp.
            if (showCity)
                cityui::drawPanel({showCity, roads.active(), roads.active().built(),
                                   [&]{ roads.active().rebuildCity(); },
                                   bakeNearestBuilding,
                                   beginRoadEdit, commitRoadEdit,
                                   exportStatus});

            // Whole towns: the rules live in CitySystem, the streets in `roads`;
            // the panel edits both through one undo step each. See CityPlanPanel.cpp.
            citygenui::drawPanel({showTowns, towns, roads, townSel, cursor.pos,
                                  townUndoBefore, townEditing,
                                  [&](std::unique_ptr<Command> c) {
                                      history.pushApplied(std::move(c));
                                  },
                                  townStatus,
                                  [&] {
                                      std::vector<std::string> out;
                                      if (currentProject.empty()) return out;
                                      for (const auto& np : prefab::list(prefab::prefabsDirIn(
                                               std::filesystem::path(currentProject)
                                                   .parent_path().generic_string())))
                                          out.push_back(np.first);
                                      return out;
                                  }});

            // A whole race scene from a seed (see LevelGen.hpp). Pumped whether
            // the panel is open or not: a circuit already being put into the
            // world has to finish, and closing the window is not a way to stop
            // it half way.
            pumpLevel();
            if (showLevelGen)
                levelui::drawPanel({showLevelGen, levelParams, levelReport,
                                    levelReportValid, levelDiscardPaint,
                                    static_cast<int>(sculptWork.deltas.size()),
                                    static_cast<int>(paintWork.weights.size()),
                                    levelJobRunning, levelStep >= 0,
                                    previewLevel, applyLevel});
            // Painted BETWEEN the phases, which is what the slicing is for -- and
            // being modal it also stops a half-built world from being clicked at.
            if (levelStep >= 0 && !ImGui::IsPopupOpen("Generating level"))
                ImGui::OpenPopup("Generating level");
            if (ImGui::BeginPopupModal("Generating level", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoTitleBar)) {
                ImGui::TextUnformatted(levelLabel);
                ImGui::ProgressBar(levelProgress, ImVec2(360.0f, 0.0f));
                if (levelStep < 0) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

            // Modelling: one floating window over the viewport, with the
            // picking modes, the operations, their amounts and the face's
            // material in it. The viewport half of it -- what the pointer is
            // over, the wireframe, the preview and the flash -- is drawn up in
            // the Scene window (modeltools::drawOverlay).
            modelmode::modelingPanel(
                editorCtx, modelSess,
                {showModeling, showMaterials, ImVec2(viewportRectMin.x, viewportRectMin.y),
                 ImVec2(viewportRectMin.x + viewportRectSize.x,
                        viewportRectMin.y + viewportRectSize.y),
                 cursor.pos, &splines});

            // Where the selected face's texture sits. Shares the Modeling
            // panel's face selection and its one-undo-step edit callback: this
            // is the same mesh being shaped, looked at from the texture's side.
            modelmode::uvPanel(editorCtx, showUv);

            // The modular synth: building a patch and hearing it. Everything it
            // needs is its own (see SynthPanel.hpp); it takes the engine to play
            // through and the open project to keep its patches in.
            if (showSynth) synthPanel.draw(showSynth, audio, currentProject);

            if (showMeshPaint) {
                bool on = viewTool == ViewTool::MeshPaint;
                meshpaintui::panel(editorCtx, meshBrush,
                                   {showMeshPaint, on, showMaterials, [&] { convertToMesh(); }});
                takeTool(viewTool, ViewTool::MeshPaint, on);
            }

            cursor3d::panel(editorCtx, cursor,
                            {showCursor, showGrid, gridFade,
                             [&](float x, float z) { return streamer.heightAt(x, z); },
                             [&](const glm::vec3& at) { addEntity(at, entityNewType); }});

            // The scene tree: selection, inline rename, drag-to-reparent and the
            // create/duplicate/delete menu (see HierarchyPanel.cpp).
            hierarchyui::drawPanel({entities, document, sel,
                                    renameId, renameBuf, sizeof(renameBuf), renameFocus,
                                    duplicateEntity, deleteEntity,
                                    duplicateSelection, deleteSelection,
                                    addEmptyParent, addEmptyChild, addPrimitiveChild,
                                    addClothChild,
                                    addShotCamera, addCockpitCamera,
                                    addVehicleLights, setMainCamera,
                                    isUnderId, worldOf, rebaseLocal,
                                    prefabNameBuf, sizeof(prefabNameBuf), showPrefabs,
                                    unpackPrefab});

            // The Inspector: the selected entity's fields and its components,
            // each card rendering from its own metadata (see InspectorPanel.cpp).
            inspectorui::drawPanel({entities, document, history, materials, sel,
                                    models, particles, scripts, cams, roads, streamer,
                                    timeOfDay, timePaused,
                                    parentWorldMat, setWorld, rebaseLocal, modelHalf,
                                    deleteEntity, setMainCamera,
                                    collectSubtreeIds, snapshotEntities,
                                    inspEditId, inspEditIds, inspEditBefore,
                                    soundPickerCombo, texturePickerCombo,
                                    listScripts, openScript, scanScriptParams,
                                    currentProject, listScenesIn,
                                    playCue, playBoostPunch,
                                    startAudioSource, stopAudioSource,
                                    matSel, matPickFilter, sizeof(matPickFilter),
                                    showMaterials, showModels, activeCam,
                                    entityNewHalf,
                                    animClips, animEditClip, animPlay, animAutoKey,
                                    animGraphs, showGraphEditor, &synths, unpackPrefab,
                                    &showProcedural, imagePickerCombo});

            // Material library: create/edit reusable surface materials. Solids are
            // assigned one via the Inspector; edits here update every mesh using it.
            materialsui::drawPanel({showMaterials, materials, document, entities,
                                    matSel, matFilter, sizeof(matFilter),
                                    assetDb, videos, texSwatch});

            // Model import: list glTF/GLB files under models/ and drop one into
            // the scene in front of the camera as a Model entity.
            modelsui::drawPanel({showModels, modelDir, modelFile,
                                 models, assetDb, materials,
                                 [&] { return spawnPoint(8.0f); },
                                 isStructuredModel, addModelHierarchy,
                                 addModelEntity});

            // Prefabs: reusable object templates saved in the project's prefabs/
            // folder. Make one from the current selection, or click a saved prefab
            // to drop an instance into the scene (on the ground in front of the
            // camera). See PrefabSystem.hpp.
            prefabsui::drawPanel({showPrefabs, prefabDir, entities, sel,
                                  prefabNameBuf, sizeof(prefabNameBuf),
                                  createPrefabFromSelection,
                                  instantiatePrefabFile,
                                  openPrefabForEdit,
                                  [&]{ return prefabEdit.active; },
                                  renamePrefabFile, deletePrefabFile,
                                  prefabRenameBuf, sizeof(prefabRenameBuf),
                                  prefabSelPath, prefabSelName,
                                  exportPrefabZip, importPrefabZip,
                                  prefabImportPending ? &prefabImportPlan : nullptr,
                                  confirmPrefabImport,
                                  [&] { prefabImportPending = false; },
                                  &prefabPackageNotes});

            // Import Unity asset: browse an asset folder, preview which textures
            // map by Unity naming convention, then import the FBX as a hierarchy
            // with those maps auto-assigned (the matching also runs on reload).
            unityimportui::panel(unityImport,
                                 {showUnityImport, modelDir, assetDb,
                                  [&] { return spawnPoint(8.0f); },
                                  [&](glm::vec3 at, const std::string& path, bool flipV) {
                                      addModelHierarchy(at, path, flipV);
                                  }});

            // Asset browser: every asset in the database, grouped by source
            // (Engine vs Project) and labelled by type. Drag a Model onto the
            // viewport to place it, or a Texture onto a material's Base texture
            // slot. Double-click a Model to drop it ahead of the camera.
            if (showAssets) {
                const std::string projDir =
                    currentProject.empty()
                        ? std::string()
                        : std::filesystem::path(currentProject).parent_path().generic_string();
                assetsui::panel(editorCtx, assetsBrowser,
                                {showAssets, projDir,
                                 // A cached preview, or a decode started for a tile on screen.
                                 [&](AssetId id, bool onScreen) -> unsigned {
                                     const auto it = assetThumbs.find(id);
                                     if (it != assetThumbs.end())
                                         return it->second ? it->second->id() : 0u;
                                     return onScreen ? thumbFor(id) : 0u;
                                 },
                                 g_fileDrop.paths, g_fileDrop.x, g_fileDrop.y,
                                 [&] { return spawnPoint(8.0f); },
                                 [&](const std::string& file) {
                                     showImageEditor = true;
                                     imageEditor.open(file);
                                 }});
            }


            scriptEditor.panel(scripts, gui);

            // The offline renderer's panel. Draws itself, including the
            // preview of whatever the running render has reached so far;
            // pressing Render in it only raises a flag, which service()
            // above acts on at the one point in the frame where it can.
            // The scene FILE, not its folder: renders land beside it in
            // renders/ and its baked light in lightgrids/, both named after it,
            // so two scenes in one project cannot overwrite each other's. Empty
            // means nothing is open, and the panel says so rather than writing
            // next to the executable.
            pathpanel::draw(pathRender, lightGrid,
                            currentProject.empty()
                                ? std::filesystem::path()
                                : std::filesystem::path(currentProject),
                            now);

            // HDRI environment lighting (image-based lighting).
            lookui::drawEnvironmentPanel({showEnv, environment, assetDb, hdriLoaded, hdriAbsPath,
                                          iblEnabled, iblSkybox, iblIntensity});

            // The Vehicle and Glider windows live with their tools (VehicleTool.cpp,
            // GliderTool.cpp); the drive and flight themselves are main's.
            vehicleui::window(editorCtx, {
                showVehiclePanel, vehicleMode,
                [&](bool on) { if (on) enterVehicleMode(); else endEditorDrive(); },
                showCrosshair, skids, trails,
                [&] { weapons.settingsPanel(soundPickerCombo); },
                vehGizmoEdit, showVehicle, [&] { placeCar(); }, carPlaced, carSpeed,
            });

            gliderui::window(editorCtx, {
                showGliderPanel, gliderMode,
                [&](bool on) {
                    if (on) {
                        if (vehicleMode) { vehicleMode = false; endEditorDrive(); }
                        enterGliderMode();
                    } else {
                        endGliderDrive();
                    }
                },
                driveGliderId, gliderVel,
            });

            // Scene UI overlay editor: author the per-scene 2D HUD (text, buttons,
            // images). The list edit is bracketed into one undo step -- opened when
            // a field is first touched, committed when nothing is active -- exactly
            // like the Inspector and the road edits.
            if (showUiOverlay)   // the sound list walks the asset library: only when open
                uioverlayui::panel(uiOverlay, uiEditBracket,
                                   {showUiOverlay, uiSel, assetDb, currentProject, listSounds(),
                                    history, document});

            } // end editor UI (skipped in presentation mode)
#endif // !FITZEL_PLAYER

            // Push the (possibly edited) terrain params into the material, plus
            // the texture layers: bind each layer with a texture to its own unit
            // (terrainLayerUnit -- NOT a plain run, it steps over the cascade
            // array) and upload its height/slope band + tiling. Layers without a
            // texture are skipped, so uLayerCount is the bound count.
            //
            // The gap is tied to the renderer's own constant here, where both are
            // visible. A layer landing on the cascade array is a sampler2D over a
            // sampler2DArray: no error, no crash, just the ground losing its
            // shadows or that layer -- so it is worth a compile-time answer rather
            // than a comment asking people to remember.
            static_assert([] {
                for (int i = 0; i < kMaxTerrainLayers; ++i)
                    if (terrainLayerUnit(i) == Renderer::kShadowMapUnit) return false;
                return true;
            }(), "a terrain layer would bind over the shadow cascade array");
            static_assert(terrainLayerNormUnit(kMaxTerrainLayers - 1) <
                              Renderer::kLightGridUnit,
                          "terrain layer normal maps have grown into the light grid");
            terrainMat.set("uDetailScale", look.detailScale)
                      .set("uDetailStrength", look.detailStrength)
                      .set("uTerrainSpec", look.gloss)
                      .set("uHeightBlend", look.heightBlend)
                      .set("uTexScale", texScale)
                      .set("uNormalStrength", normalStrength)
                      .set("uWaterLevel", waterLevel)
                      .set("uWetness", roadWetness)
                      .set("uMeshPaint", 0)             // object paint is not the terrain's
                      .set("uAlbedo", glm::vec3(0.5f)); // neutral grey where no layer covers
            // The field's colour past its blades (meadow.glsl). Starts well inside
            // the streamed radius -- the blades thin out towards it -- and is
            // complete where the last of them has shrunk away.
            {
                const bool meadow = veg.grassEnabled && meadowTint > 0.0f;
                terrainMat.set("uMeadowNear", veg.grassRadius * 0.3f)
                          .set("uMeadowFar", meadow ? veg.grassRadius * 0.95f : 0.0f)
                          .set("uMeadowAmount", glm::clamp(meadowTint, 0.0f, 1.0f))
                          .set("uMeadowLush", 0.62f)
                          .set("uGrassTint", veg.grassTint)
                          .set("uGrassTop", look.snowLevel - 1.5f);
                // The forest floor under the ecology's woods (ecology.glsl).
                // The meadow's moisture from the horizon's finest ring, on a unit
                // of its own (30: nothing else binds it).
                const unsigned moistTex = farTerrain.ready() ? farTerrain.fineTexture() : 0u;
                glActiveTexture(GL_TEXTURE30);
                glBindTexture(GL_TEXTURE_2D, moistTex);
                glActiveTexture(GL_TEXTURE0);
                terrainMat.set("uMoistTex", 30)
                          .set("uMoistRect", moistTex ? farTerrain.fineRect() : glm::vec4(0.0f))
                          .set("uGrassDry", veg.grassDryGrowth);
                terrainMat.set("uForestLayer", veg.eco.enabled ? forestFloorLayer : -1)
                          .set("uCanopy", veg.canopyColour() * veg.treeBrightness);
                ecology::forEachUniform(
                    veg.eco, [&](const char* n, int v) { terrainMat.set(n, v); },
                    [&](const char* n, float v) { terrainMat.set(n, v); });
            }
#ifdef __EMSCRIPTEN__
            // The browser samples the layers through two texture arrays (see
            // lit.frag, FITZEL_WEB): built here from the layers' own textures,
            // and again only when one of them changes. 1024 per layer: six of
            // them twice over is what a browser tab can spare.
            {
                static std::vector<const fitzel::Texture*> webKey;
                static fitzel::Texture webLayerArr, webLayerNormArr;
                std::vector<const fitzel::Texture*> cols, norms;
                int size = 0;
                for (const TerrainLayer& L : look.layers) {
                    if (!L.tex || static_cast<int>(cols.size()) >= kMaxTerrainLayers) continue;
                    cols.push_back(L.tex.get());
                    norms.push_back(L.norm.get());
                    size = std::max({size, L.tex->width(), L.tex->height()});
                }
                std::vector<const fitzel::Texture*> key = cols;
                key.insert(key.end(), norms.begin(), norms.end());
                if (!cols.empty() && (key != webKey || !webLayerArr.isValid())) {
                    size = std::clamp(size, 16, 1024);
                    const unsigned char grey[4] = {128, 128, 128, 255};
                    const unsigned char flat[4] = {128, 128, 255, 255};
                    webLayerArr     = fitzel::Texture::arrayOf(cols, size, grey);
                    webLayerNormArr = fitzel::Texture::arrayOf(norms, size, flat);
                    webKey = key;
                }
                if (webLayerArr.isValid()) {
                    terrainMat.setTexture("uLayerArr", webLayerArr, terrainLayerUnit(0))
                              .setTexture("uLayerNormArr", webLayerNormArr,
                                          terrainLayerNormUnit(0));
                }
            }
#endif
            {
                int bound = 0;
                for (const TerrainLayer& L : look.layers) {
                    if (!L.tex || bound >= kMaxTerrainLayers) continue;
                    const std::string ix = std::to_string(bound);
#ifndef __EMSCRIPTEN__
                    terrainMat.setTexture("uLayerTex[" + ix + "]", *L.tex,
                                          terrainLayerUnit(bound));
#endif
                    terrainMat
                              .set("uLayerBand[" + ix + "]",
                                   glm::vec4(L.heightStart, L.heightEnd,
                                             L.slopeStart, L.slopeEnd))
                              .set("uLayerScale[" + ix + "]", L.scale);
                    // Optional normal map, kept high so it clears the
                    // shadow/env/IBL samplers the renderer binds lower down.
                    if (L.norm) {
#ifndef __EMSCRIPTEN__
                        terrainMat.setTexture("uLayerNorm[" + ix + "]", *L.norm,
                                              terrainLayerNormUnit(bound));
#endif
                        terrainMat.set("uLayerHasNorm[" + ix + "]", 1);
                    } else {
                        terrainMat.set("uLayerHasNorm[" + ix + "]", 0);
                    }
                    ++bound;
                }
                terrainMat.set("uLayerCount", bound);
            }

            // --- Submit the opaque scene once ---------------------------
            // Render at the docked viewport panel's size, not the whole window.
            // Clamp to >= 1: an exclusive-fullscreen window that gets minimized
            // (e.g. a screenshot/overlay tool grabbing focus) reports a 0x0
            // framebuffer, which would recreate the render targets at 0x0 --
            // an incomplete FBO -- and make `aspect` NaN.
            const int   fbW = std::max(1, viewW), fbH = std::max(1, viewH);
            // Split screen draws the world once per pane, so everything sized
            // per-render -- the HDR buffer, the post chain, the projection --
            // follows the PANE, not the window. Only the final blit knows about
            // the full width, because that is the one image both panes land in.
            // Two panes only when there is a second eye to fill one (see
            // haveView2): the checkbox asks for split screen, the scene decides
            // whether it can deliver it.
            const int   views = haveView2 ? 2 : 1;
            const int   paneW = std::max(1, fbW / views);
            const float aspect = static_cast<float>(paneW) / static_cast<float>(fbH);
            const glm::mat4 proj = camera.projectionMatrix(aspect);
            // The size the 3D scene is drawn at: the pane, times the player's
            // render scale (Graphics menu) -- in Play only. The editor's viewport
            // stays full size: its grid and its picking read the scene's depth
            // pixel for pixel. The finished image is stretched back up in
            // PostChain::present, so everything between here and there -- the
            // HDR target, the post chain, TAA's jitter -- is at this size.
            const float rScale = playMode ? std::clamp(renderScale, 0.5f, 1.0f) : 1.0f;
            const int   rw = std::max(1, static_cast<int>(std::lround(paneW * rScale)));
            const int   rh = std::max(1, static_cast<int>(std::lround(fbH * rScale)));

            renderer.setViewport(rw, rh);
            // Shadow cascades are fitted to player one's frustum and shared by
            // both panes: they are built once per frame, before either pane is
            // drawn. Good enough while the two are racing the same stretch of
            // track; a player who drives far away from the other gets cascades
            // sized for someone else's view.
            renderer.begin(camera, aspect, light);

            // Coarser far off (see TerrainChunk::mesh(int)): every vertex out to
            // 160 m, every second to 320 m, every fourth beyond -- the full grid
            // everywhere was three million triangles a pass, in the main view,
            // every shadow cascade, the probe and the water's mirror alike.
            {
                const float cs = streamer.settings().chunkSize;
                const glm::vec2 eye(camera.position().x, camera.position().z);
                for (const TerrainChunk* chunk : streamer.visibleChunks()) {
                    const glm::vec2 lo = glm::vec2(chunk->coord()) * cs;
                    const glm::vec2 d  = glm::max(glm::max(lo - eye, eye - (lo + cs)), glm::vec2(0.0f));
                    const float dist = glm::length(d);
                    const int lod = dist < 160.0f ? 0 : dist < 320.0f ? 1 : 2;
                    renderer.submit(chunk->mesh(lod), terrainMat, glm::mat4(1.0f), false);
                }
            }
            herd.submit(renderer);   // the grazing herd, lit and shadowed like any object

            // Every road in the scene, each drawn with its own surface, its own
            // wetness and its own glow -- which is the whole point of them being
            // separate objects rather than one ribbon with a fork in it.
            //
            // `anyWetMirror` comes out of this loop for the probe below: a probe
            // is worth capturing if ANY road is going to sample it.
            bool anyWetMirror = false;
            for (RoadSystem* rp : roads) {
                RoadSystem& road = *rp;
                // What the carriageway (and its bridge decks and loops) is wet with:
                // the weather's puddles or the road's own authored sheen, whichever is
                // wetter. Only the road's surfaces read this -- the terrain, the craft
                // and every other material stay on the weather's value alone, which is
                // the whole point of the road having its own.
                const float surfaceWet = glm::max(roadWetness,
                                                  glm::clamp(road.wetness, 0.0f, 1.0f));
                // Wet enough, and asked to mirror at all: this is what makes the road
                // sample the probe -- and therefore what makes the probe worth
                // capturing (see step 0 below) and the road worth keeping out of it.
                const bool wetMirror = surfaceWet > 0.02f && road.wetReflect > 0.001f;
                // Drop impacts: the weather's rings scaled by this road's own strength.
                const float ringAmount = ringWeather * road.rainRings;
                if (wetMirror && road.enabled && road.verts() > 0) anyWetMirror = true;

                // The committed road mesh only changes on Build (see the Roads panel);
                // editing shows a live preview instead (drawn in the viewport overlay).
                if (road.enabled && road.verts() > 0) {
                    road.material().set("uWaterLevel", waterLevel); // wet-darken submerged
                    // Wet sheen: the wetter of the weather and the road's own setting.
                    // The road's is a floor, not an override -- an authored-wet track
                    // stays wet in the sun, and rain can still soak a dry one further.
                    // (The loop meshes below share this material, so they follow.)
                    road.material().set("uWetness", surfaceWet);
                    // Drop impacts: rings while it is actually coming down, not while the
                    // tarmac is merely still wet -- so they stop with the rain, not with
                    // the puddles. Every other material gets 0 from the Renderer's
                    // baseline, so the effect can't leak off the road.
                    road.material().set("uRainRings", ringAmount);
                    road.material().set("uRainDensity", rainDensity);
                    road.material().set("uTime", static_cast<float>(now));
                    // Edge fade: pass the fade band + the UV-to-metres mapping, and route
                    // the road through the transparent (alpha-blended) queue when it's on.
                    const bool roadFades = road.fadeWidth > 0.0f;
                    road.material().set("uRoadFade",  roadFades ? road.fadeWidth : 0.0f);
                    // Measured across the WHOLE section (raised edges included), which
                    // is what the ribbon's u now spans -- against the bare width the
                    // fade would find its edge halfway up the lip and dissolve it.
                    const float roadSpan = road.surfaceHalf() * 2.0f;
                    road.material().set("uRoadWidth", roadSpan);
                    road.material().set("uRoadUMax",  road.texTile > 1e-4f
                                                          ? roadSpan / road.texTile : 0.0f);
                    // Glow: colour/strength/map plus the UV scale that keeps the map
                    // spanning the carriageway. Re-applied per frame because it is
                    // derived from width/texTile, which the panel edits live.
                    road.applyEmission();
                    // Puddles: map + tiling, live-edited in the panel like the glow.
                    road.applyWetness();
                    // Flagged reflective while it is wet, which keeps it OUT of the
                    // probe capture. Left in, the carriageway is drawn into the cube
                    // it is about to sample -- last frame's reflection reflected
                    // again, every frame, and any garbage in it (a probe face that
                    // was never rendered) never washes out.
                    renderer.submit(road.mesh(), road.material(), glm::mat4(1.0f), false,
                                    /*reflective=*/wetMirror, 1.0f,
                                    /*forceTransparent=*/roadFades);
                }

                // The road's concrete -- bridge decks and tunnel bores, one mesh --
                // built by the same Build as the road it carries. Unlike the ribbon it
                // casts shadows: there is ground under a deck for them to fall on, and
                // a bore wants the hill over it to keep the sun out.
                if (road.enabled && road.hasBridges()) {
                    road.bridgeMaterial().set("uWaterLevel", waterLevel);
                    // A deck is carriageway: it takes the road's own wetness too.
                    road.bridgeMaterial().set("uWetness", surfaceWet);
                    // A deck is carriageway too: rain hits it like the rest of the road.
                    road.bridgeMaterial().set("uRainRings", ringAmount);
                    road.bridgeMaterial().set("uRainDensity", rainDensity);
                    road.bridgeMaterial().set("uTime", static_cast<float>(now));
                    renderer.submit(road.bridgeMesh(), road.bridgeMaterial(),
                                    glm::mat4(1.0f), true, /*reflective=*/wetMirror);
                }

                // Vertical loops. Drawn with the road's OWN surface material -- a loop
                // is carriageway, not structure -- but as a separate mesh, because its
                // geometry cannot live in a ribbon that has one height per ground
                // position (see RoadLoop.hpp).
                if (road.enabled && road.hasLoops())
                    renderer.submit(road.loopMesh(), road.material(), glm::mat4(1.0f),
                                    true, /*reflective=*/wetMirror);

                // Junction aprons: the flat plates where this road meets another,
                // or itself, on the level (see RoadJunction.hpp). Their own
                // material rather than the road's, because the carriageway's edge
                // fade is a function of the across-road U and an apron's U is
                // world space -- sharing would dissolve it in stripes.
                if (road.enabled && road.hasJunctions())
                    renderer.submit(road.junctionMesh(), road.junctionMaterial(),
                                    glm::mat4(1.0f), true, /*reflective=*/wetMirror);

                // Decals painted ON the carriageway -- start grids, arrows, boost
                // pads, oil stains -- lofted onto the road's own surface from the
                // rules saved with it (see RoadDecal.hpp). Cut-out and opaque ones
                // ride the ordinary opaque queue; only a blended one pays for
                // sorting, which is why it is a per-decal choice and not a global
                // one.
                if (road.enabled && !road.decalBatches().empty()) {
                    road.applyDecalWetness(surfaceWet, waterLevel);
                    for (const RoadSystem::DecalBatch& d : road.decalBatches())
                        renderer.submit(d.mesh, d.mat, glm::mat4(1.0f),
                                        /*castsPointShadow=*/false,
                                        /*reflective=*/false, d.opacity,
                                        /*forceTransparent=*/d.transparent);
                }
            } // every road

            // Tyre skid marks accumulated while driving (alpha-blended, on ground).
            skids.render(renderer);
            trails.render(renderer);
            // Missiles, their trails, the blasts, and the marker cage around
            // whichever rival is locked.
            weapons.render(renderer);
            weapons2.render(renderer);   // player two's are in the same world

            // Rain wets the (primitive) test car too. Set every frame so the shared
            // lit program never inherits another material's wetness.
            carBodyMat.set("uWetness", roadWetness);
            carCabinMat.set("uWetness", roadWetness);
            carWheelMat.set("uWetness", roadWetness);

            // --- Physics car: draw the chassis + wheels from Jolt transforms.
            //     (Only the primitive test car -- a driven scene model renders
            //     itself through the entity pass, synced from Jolt above.)
            if (vehicleMode && playMode && physics && physics->hasVehicle() &&
                driveVehicleId < 0) {
                glm::vec3 cp; glm::quat cq;
                if (physics->getTransform(physCarId, cp, cq)) {
                    const glm::mat4 chassis =
                        glm::translate(glm::mat4(1.0f), cp) * glm::mat4_cast(cq);
                    renderer.submit(carCube, carBodyMat, chassis *
                        glm::scale(glm::mat4(1.0f), glm::vec3(1.8f, 0.7f, 4.0f)));
                    renderer.submit(carCube, carCabinMat, chassis *
                        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.5f, -0.3f)) *
                        glm::scale(glm::mat4(1.0f), glm::vec3(1.5f, 0.6f, 1.8f)));
                    for (int i = 0; i < 4; ++i) {
                        glm::vec3 wp; glm::quat wq;
                        if (physics->getWheelTransform(i, wp, wq))
                            renderer.submit(carWheel, carWheelMat,
                                glm::translate(glm::mat4(1.0f), wp) * glm::mat4_cast(wq));
                    }
                }
            }
            // --- Arcade vehicle (editor): terrain-aligned body + rolling wheels --
            else if (showVehicle && carPlaced && !playMode && driveVehicleId < 0) {
                const float e = 1.2f;
                const glm::vec3 N = glm::normalize(glm::vec3(
                    streamer.heightAt(carPos.x - e, carPos.z) - streamer.heightAt(carPos.x + e, carPos.z),
                    2.0f * e,
                    streamer.heightAt(carPos.x, carPos.z - e) - streamer.heightAt(carPos.x, carPos.z + e)));
                const glm::vec3 fwd0(std::sin(carYaw), 0.0f, std::cos(carYaw));
                const glm::vec3 fwd   = glm::normalize(fwd0 - N * glm::dot(fwd0, N));
                const glm::vec3 right = glm::normalize(glm::cross(N, fwd));
                glm::mat4 basis(1.0f);
                basis[0] = glm::vec4(right, 0.0f);
                basis[1] = glm::vec4(N, 0.0f);
                basis[2] = glm::vec4(fwd, 0.0f);
                basis[3] = glm::vec4(carPos, 1.0f);

                const glm::mat4 body = basis
                    * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, wheelR + bodyH * 0.5f, 0.0f))
                    * glm::scale(glm::mat4(1.0f), glm::vec3(bodyW, bodyH, bodyL));
                renderer.submit(carCube, carBodyMat, body);
                const glm::mat4 cabin = basis
                    * glm::translate(glm::mat4(1.0f),
                                     glm::vec3(0.0f, wheelR + bodyH + cabH * 0.5f, -0.25f))
                    * glm::scale(glm::mat4(1.0f), glm::vec3(cabW, cabH, cabL));
                renderer.submit(carCube, carCabinMat, cabin);

                const glm::vec3 wl[4] = {
                    { halfTrack, wheelR,  halfBase}, {-halfTrack, wheelR,  halfBase},
                    { halfTrack, wheelR, -halfBase}, {-halfTrack, wheelR, -halfBase}};
                for (int i = 0; i < 4; ++i) {
                    glm::mat4 w = basis * glm::translate(glm::mat4(1.0f), wl[i]);
                    if (i < 2) w = w * glm::rotate(glm::mat4(1.0f), steerAngle, glm::vec3(0, 1, 0));
                    w = w * glm::rotate(glm::mat4(1.0f), wheelSpin, glm::vec3(1, 0, 0));
                    renderer.submit(carWheel, carWheelMat, w);
                }
            }

            // Resolve the scene-graph so every entity's world center/rotation
            // reflects this frame's edits/scripts/physics and its parent chain.
            resolveHierarchy();

            // --- Skeletal animation (CPU skinning). For each entity carrying an
            //     Animation component (or an Animation Graph whose state names a
            //     model animation) on an animated model, advance its clock and
            //     re-skin the model's meshes so the shared static render path shows
            //     the deformed pose. (Meshes are shared per model; a second
            //     figure of the same model is posed in its own copy, SkinCopies.)
            {
                std::vector<Vertex> skinScratch;
                skinCopies.beginFrame();
                // The ground under a foot, for the IK: game.groundHeight's
                // (roads, bridges, steps, the drawn terrain), with the slope
                // from two more looks a hand's breadth away.
                const ik::GroundFn ikGround = [&](const glm::vec3& from, float maxDist, float& y,
                                                  glm::vec3& n) {
                    if (!host.groundHeight || !host.groundHeight(from, maxDist, y)) return false;
                    const float e = 0.12f;
                    float yx = y, yz = y;
                    host.groundHeight(from + glm::vec3(e, 0.0f, 0.0f), maxDist, yx);
                    host.groundHeight(from + glm::vec3(0.0f, 0.0f, e), maxDist, yz);
                    n = glm::normalize(glm::vec3(y - yx, e, y - yz));
                    return true;
                };
                for (Entity& e : entities) {
                    if (!e.activeInHierarchy) continue;   // deactivated: don't skin
                    auto* ac = e.components.get<AnimationComponent>();
                    const auto* mc = e.components.get<ModelComponent>();
                    // A graph state that names one of the model's animations
                    // poses the figure itself -- over the Animation component,
                    // which only takes over again in a state that names none.
                    const auto* ag = e.components.get<AnimGraphComponent>();
                    const bool byGraph = ag && ag->skinClip >= 0;
                    if ((!ac && !byGraph) || !mc) continue;
                    LoadedModel* lm = models.byId(mc->modelId);
                    if (!lm || !lm->animated || !lm->animData) continue;
                    const auto& clips = lm->animData->animations;
                    if (clips.empty()) continue;
                    int   ci   = 0;
                    float time = 0.0f;
                    // A graph fade: the pose of the state being left, and its share.
                    int   fromCi    = -1;
                    float fromTime  = 0.0f;
                    float fromShare = 0.0f;
                    if (byGraph) {
                        ci   = glm::clamp(ag->skinClip, 0, static_cast<int>(clips.size()) - 1);
                        time = ag->skinTime;   // looped and scaled by the graph
                        if (ag->skinFromClip >= 0 && ag->skinFromShare > 0.0f) {
                            fromCi    = glm::clamp(ag->skinFromClip, 0,
                                                   static_cast<int>(clips.size()) - 1);
                            fromTime  = ag->skinFromTime;
                            fromShare = ag->skinFromShare;
                        }
                    } else {
                        ci = glm::clamp(ac->clip, 0, static_cast<int>(clips.size()) - 1);
                        const float dur = clips[ci].duration;
                        // Playback sub-range [rStart, rEnd] (end <= start -> whole clip).
                        const float rStart = glm::clamp(ac->start, 0.0f, dur);
                        float rEnd = (ac->end > ac->start) ? glm::clamp(ac->end, 0.0f, dur) : dur;
                        if (rEnd <= rStart) rEnd = dur;
                        const float span = rEnd - rStart;
                        // First tick this Play: apply autostart; a trigger sets restart.
                        if (!ac->started) {
                            ac->started = true;
                            ac->playing = ac->autostart;
                            ac->time    = ac->reverse ? rEnd : rStart;
                        }
                        if (ac->restart) {
                            ac->restart = false;
                            ac->playing = true;
                            ac->time    = ac->reverse ? rEnd : rStart;
                        }
                        if (ac->playing && span > 1e-4f) {
                            ac->time += dt * ac->speed * (ac->reverse ? -1.0f : 1.0f);
                            if (ac->loop) {
                                float rel = ac->time - rStart;
                                rel -= std::floor(rel / span) * span; // wrap into [0, span)
                                ac->time = rStart + rel;
                            } else if (ac->reverse) {
                                if (ac->time <= rStart) { ac->time = rStart; ac->playing = false; }
                            } else {
                                if (ac->time >= rEnd)   { ac->time = rEnd;   ac->playing = false; }
                            }
                        }
                        time = ac->time;
                    }
                    auto palette = fromCi >= 0
                        ? sampleSkeletonBlend(*lm->animData, fromCi, fromTime,
                                              ci, time, 1.0f - fromShare)
                        : sampleSkeleton(*lm->animData, ci, time);
                    if (palette.empty()) continue;
                    // The limbs, bent onto the ground and to where scripts send
                    // the hands -- after the clip, before the skin (and before
                    // the pose is stored, so what a hand carries follows it).
                    const auto* ikc = e.components.get<IKComponent>();
                    if (ikc || limbIk.reaching(e.id)) {
                        ik::FeetOptions fo;
                        if (ikc) {
                            fo.feet     = ikc->feet;
                            fo.align    = ikc->align;
                            fo.maxStep  = ikc->maxStep;
                            fo.response = ikc->response;
                            fo.leftLeg  = ikc->leftLeg;  fo.rightLeg = ikc->rightLeg;
                            fo.leftArm  = ikc->leftArm;  fo.rightArm = ikc->rightArm;
                        }
                        // Model space to the world as SceneSubmit draws it (and
                        // BoneAttach places what a figure carries).
                        const glm::vec3 sz = glm::max(lm->size(), glm::vec3(1e-4f));
                        const glm::mat4 toWorld =
                            scenegraph::compose(e.center, e.rotation, (e.half * 2.0f) / sz) *
                            glm::translate(glm::mat4(1.0f), -lm->center());
                        limbIk.apply(e.id, ikc ? &fo : nullptr, *lm->animData, toWorld,
                                     e.center.y - e.half.y, palette, ikGround, dt);
                    }
                    if (playMode) boneAttach.storePose(e.id, palette);
                    const auto& prims = lm->animData->primitives;
                    std::vector<fitzel::Mesh>* own = skinCopies.target(e.id, lm);
                    for (std::size_t p = 0;
                         p < lm->meshes.size() && p < prims.size(); ++p) {
                        skinPrimitive(prims[p], palette, skinScratch);
                        if (own) SkinCopies::put(*own, p, skinScratch);
                        else     lm->meshes[p].update(skinScratch);
                    }
                }
                skinCopies.endFrame();
            }

            limbIk.endFrame();   // the hands were sent for this frame only

            // Whatever the figures carry, onto the bones they were just posed
            // with -- before anything is drawn (see BoneAttach.hpp).
            if (playMode) boneAttach.apply(entities, models, dt);

            // --- Scene entities through the renderer (shadows, lighting, water).
            // Fences, walls and track: regenerate whatever an edit dirtied, HERE
            // rather than at the edit site, so a path is rebuilt at most once per
            // frame however many sliders moved -- and before gpuMats is built,
            // because the generator find-or-creates its palette materials in the
            // library and a batch drawn with a material that isn't in gpuMats yet
            // would be skipped for a frame.
            splines.update(materials);

            // The trams, on the tracks just rebuilt: what stands in their way is
            // the town's traffic, the figures scripts walk and the player's car.
            {
                std::vector<tramsim::Blocker> inWay;
                const traffic::Sim& ts = townTraffic.sim();
                for (const traffic::Vehicle& v : ts.vehicles()) {
                    const traffic::Pose p = ts.pose(v);
                    const glm::vec3 fwd(p.heading.x, 0.0f, p.heading.y);
                    const float reach = std::max(v.length * 0.5f - 0.9f, 0.0f);
                    inWay.push_back({p.pos, 1.0f});
                    inWay.push_back({p.pos + fwd * reach, 1.0f});
                    inWay.push_back({p.pos - fwd * reach, 1.0f});
                }
                // Who walks on their own -- the figures scripts walk, the player
                // on foot -- holds a tram's doors by standing in them.
                std::vector<glm::vec3> walking;
                if (playMode) {
                    for (const auto& [fid, handle] : scriptFigures)
                        if (const Entity* fe = document.find(fid)) {
                            const glm::vec3 feet = fe->center - glm::vec3(0.0f, fe->half.y, 0.0f);
                            inWay.push_back({feet, 0.4f});
                            walking.push_back(feet);
                        }
                    if (fpsMode)
                        walking.push_back(camera.position() - glm::vec3(0.0f, eyeHeight, 0.0f));
                    glm::vec3 cp;
                    glm::quat cq;
                    if (physics && physics->hasVehicle() && physics->getTransform(physCarId, cp, cq))
                        inWay.push_back({cp, 2.2f});
                }
                // The riders first, on the trams as they stand now -- which is
                // where they are drawn this frame.
                tramRiders.step(dt, trams.sim(), townTraffic.sim());
                trams.update(splines, dt, materials, inWay, walking);
                // In Play the cars are platforms to walk into and ride on.
                if (playMode && physics) trams.syncBodies(physics.get(), dt);
            }

            // Water: re-solve whatever an edit dirtied, here for the same reason
            // -- once per frame however many sliders moved. Only the SURFACE is
            // rebuilt; the bed waits for the gesture to end (see carveRivers),
            // which is why a drag shows the water moving live and the ground
            // catching up when the mouse comes back up.
            rivers.update();   // ...and before gpuMats, for the same reason

            // Towns: streets laid (or brought back by an undo) want the roads
            // built; buildings re-derive whatever an edit or a Build dirtied.
            // Before gpuMats too -- the town's palettes are find-or-created.
            if (towns.consumeBuildRequest()) {
                // Streets that just left the scene give their corridor back
                // first (keeping any river bed cut there), so the Build after
                // it re-cuts only what is still standing.
                glm::vec2 mn, mx;
                if (towns.releaseRetired(roads, sculptWork,
                        [&](std::int64_t k) { return rivers.mineAt(k); }, mn, mx)) {
                    publishSculpt();
                    streamer.editsChanged(mn, mx);
                }
                buildRoad();
            }
            towns.update(materials);
            {
                const long long fzTraffic = prof::mark();
                townTraffic.setView(camera.position(), proj * camera.viewMatrix(), views == 1);
                townTraffic.update(towns, dt, materials);
                prof::addSince("traffic update", fzTraffic);
            }
            townLamps.update(towns, materials);
            // The street lamps are on from dusk: their glass and their light.
            const float lampsOn = TownLamps::nightFactor(light.direction);
            // A script may switch the street lamps by hand (game.setStreetLamps);
            // what else lights up after dark -- headlights, windows, the trams --
            // still goes by the dusk.
            const float streetLamps =
                streetLampMode < 0 ? lampsOn : static_cast<float>(streetLampMode);
            streetLampsLit = streetLamps > 0.5f;

            // Handing them over is one function, in SceneSubmit.cpp, so that the
            // editor is a CALLER of it rather than the only place it exists --
            // see the header there for why that matters the moment anything else
            // has to draw a scene. The scratch lives here because the render
            // queue points into it and is replayed several times per frame.
            // Decals: every Decal object's image cut from what is in its box,
            // again wherever the box or what is in it moved (Decals.hpp).
            decalSys.update(entities, models,
                            (terrainOn && host.terrainHeight)
                                ? decals::HeightFn([&](float x, float z) { return host.terrainHeight(x, z); })
                                : decals::HeightFn{});
            scenesubmit::Scratch submitScratch;
            scenesubmit::submit({entities, materials, document, models, meshCache,
                                 lit, renderer,
                                 carCube, rampMesh, cylMesh, sphereMesh, planeMesh,
                                 composeModel, roadWetness, playMode,
                                 [&](int id) { return decalSys.meshFor(id); },
                                 [&](int id, std::size_t prim) { return shatterSys.leftOf(id, prim); },
                                 [&](int id) { return shatterSys.vanished(id); },
                                 [&](int id, std::size_t prim) { return skinCopies.meshOf(id, prim); }},
                                submitScratch);
            // ...and the holes scripts threw, in the same frame's materials.
            decalSys.submitThrown(renderer, submitScratch.gpuMats, materials, document);
            // ...and the shards of the glass that was shot out.
            shatterSys.submit(renderer, submitScratch.gpuMats, materials, document);
            // One GPU material per library asset -- and the batched geometry
            // below (the roadside city, the side objects, the splines) wears
            // the same library materials the entities do, so it draws from that
            // same table instead of building a second one that could disagree.
            const std::vector<Material>& gpuMats = submitScratch.gpuMats;
            // The towns' traffic lights switch on this frame's copy of their
            // lamp materials (see CitySystem::forEachSignalLamp).
            towns.forEachSignalLamp(now, [&](const fitzel::AssetId& id, float glow) {
                const int mi = document.materialIndex(id);
                if (mi >= 0 && mi < static_cast<int>(submitScratch.gpuMats.size()))
                    submitScratch.gpuMats[static_cast<std::size_t>(mi)].set("uEmissionStrength", glow);
            });
            townLamps.forEachGlow([&](const fitzel::AssetId& id, float glow) {
                const int mi = document.materialIndex(id);
                if (mi >= 0 && mi < static_cast<int>(submitScratch.gpuMats.size()))
                    submitScratch.gpuMats[static_cast<std::size_t>(mi)].set("uEmissionStrength",
                                                                             glow * streetLamps);
            });
            // The traffic's headlights and tail lights come on with the street
            // lamps; brake lights and indicators glow whenever they are on.
            townTraffic.forEachGlow([&](const fitzel::AssetId& id, float night, float day) {
                const int mi = document.materialIndex(id);
                if (mi >= 0 && mi < static_cast<int>(submitScratch.gpuMats.size()))
                    submitScratch.gpuMats[static_cast<std::size_t>(mi)].set(
                        "uEmissionStrength", glm::mix(day, night, lampsOn));
            });
            // So do the light strips in the trams.
            trams.forEachGlow([&](const fitzel::AssetId& id, float night, float day) {
                const int mi = document.materialIndex(id);
                if (mi >= 0 && mi < static_cast<int>(submitScratch.gpuMats.size()))
                    submitScratch.gpuMats[static_cast<std::size_t>(mi)].set(
                        "uEmissionStrength", glm::mix(day, night, lampsOn));
            });
            // Lit windows after dark: the houses' lit panes, and every facade's
            // window grid (towers, blocks, civic buildings, the roadside city),
            // which by day are only a hint of the light inside.
            for (const fitzel::AssetId& id : towns.litWindows()) {
                const int mi = document.materialIndex(id);
                if (mi >= 0 && mi < static_cast<int>(submitScratch.gpuMats.size()))
                    submitScratch.gpuMats[static_cast<std::size_t>(mi)].set(
                        "uEmissionStrength",
                        materials[static_cast<std::size_t>(mi)].emissionStrength * lampsOn);
            }
            for (std::size_t mi = 0; mi < materials.size() && mi < submitScratch.gpuMats.size(); ++mi)
                if (materials[mi].windowGrid)
                    submitScratch.gpuMats[mi].set("uWindowGlow",
                                                  materials[mi].windowGlow * (0.15f + 0.85f * lampsOn));

            // --- Roadside city (see CityGen.hpp) -------------------------------
            // Drawn as a handful of MERGED meshes, not as twenty thousand loose
            // primitives. The renderer replays its queue about a dozen times a
            // frame (four shadow cascades, six probe faces, the water mirror, the
            // main pass) and re-uploads ~25 uniforms per mesh per pass, so the
            // cost of a city is its DRAW count, not its triangle count -- and
            // merging is what collapses one into the other. Geometry is already in
            // world space, hence the identity model matrix.
            //
            // Distance culling stays on this side because submission is shared by
            // every pass; a frustum test against the main camera would wrongly
            // empty the reflection. The renderer frustum-culls per pass itself.
            // The buildings are never entities either, so they cost no hierarchy
            // row, no undo snapshot and no line in the scene file. Their materials
            // are the shared "Building A..H" library slots, so they ride in
            // gpuMats like anything else.
            for (const RoadSystem* rp : roads) {
                const RoadSystem& road = *rp;
                if (!road.enabled || !road.cityEnabled || road.district().empty())
                    continue;
                const city::District& dist = road.district();
                const glm::vec3 eye = camera.position();
                // Pixels that one metre of height covers at one metre from the
                // eye, so a chunk's worth on screen is (its height / its distance)
                // times this. The main camera's framebuffer is the yardstick even
                // though the queue is shared by every pass: the point is to drop
                // what nobody would see in the pane they are looking at, and a
                // reflection or a shadow cast by a four-pixel block is not worth a
                // draw either.
                int subW = 0, subH = 0;
                window.framebufferSize(subW, subH);
                const float pxPerMetre =
                    static_cast<float>(subH)
                    / (2.0f * std::tan(glm::radians(camera.fov()) * 0.5f));
                const float minPx = std::max(road.cityMinPixels, 0.0f);
                for (std::size_t i = 0; i < dist.batches.size(); ++i) {
                    const city::Batch& b = dist.batches[i];
                    // Nearest point of the chunk's box to the eye, so a long chunk
                    // is not dropped because its centre happens to be far.
                    const glm::vec3 d = glm::max(glm::max(b.lo - eye, eye - b.hi),
                                                 glm::vec3(0.0f));
                    const float d2 = glm::dot(d, d);
                    if (d2 > road.cityRange * road.cityRange) continue;
                    // ...and what it is worth on screen at that distance. Only
                    // past a metre: closer than that the ratio blows up, and
                    // nothing that near is ever small anyway.
                    if (minPx > 0.0f && d2 > 1.0f) {
                        const float px =
                            (b.hi.y - b.lo.y) * pxPerMetre / std::sqrt(d2);
                        if (px < minPx) continue;
                    }
                    const int mi = document.materialIndex(b.material);
                    if (mi < 0 || mi >= static_cast<int>(gpuMats.size())) continue;
                    renderer.submit(road.cityMesh(i), gpuMats[mi], glm::mat4(1.0f),
                                    true, isMirror(materials[mi]),
                                    materials[mi].opacity,
                                    materials[mi].alphaMode == AlphaMode::Blend);
                }
            }

            // --- Towns (see CitySystem) ------------------------------------------
            // The same merged, world-space batches as the roadside city, culled
            // by their own draw range.
            towns.forEachDraw(camera.position(),
                [&](const Mesh& mesh, const fitzel::AssetId& mat, bool shadow) {
                    const int mi = document.materialIndex(mat);
                    if (mi < 0 || mi >= static_cast<int>(gpuMats.size())) return;
                    renderer.submit(mesh, gpuMats[mi], glm::mat4(1.0f), shadow,
                                    isMirror(materials[mi]), materials[mi].opacity,
                                    materials[mi].alphaMode == AlphaMode::Blend, shadow);
                });
            townTraffic.forEachDraw([&](const Mesh& mesh, const fitzel::AssetId& mat) {
                const int mi = document.materialIndex(mat);
                if (mi < 0 || mi >= static_cast<int>(gpuMats.size())) return;
                renderer.submit(mesh, gpuMats[mi], glm::mat4(1.0f), false);
            });
            townTraffic.forEachPrefabDraw(
                [&](const Mesh& mesh, const fitzel::AssetId& mat, const glm::mat4& m, bool detail) {
                    const int mi = document.materialIndex(mat);
                    if (mi < 0 || mi >= static_cast<int>(gpuMats.size())) return;
                    // The near ones cast into the street lamps' shadows too: a
                    // car passing under a lamp throws its shadow on the road.
                    renderer.submit(mesh, gpuMats[mi], m, detail, isMirror(materials[mi]),
                                    materials[mi].opacity,
                                    materials[mi].alphaMode == AlphaMode::Blend, detail, detail);
                });
            townLamps.forEachDraw(camera.position(),
                [&](const Mesh& mesh, const fitzel::AssetId& mat, const glm::mat4& m, bool nearEye) {
                    const int mi = document.materialIndex(mat);
                    if (mi < 0 || mi >= static_cast<int>(gpuMats.size())) return;
                    renderer.submit(mesh, gpuMats[mi], m, false, isMirror(materials[mi]),
                                    materials[mi].opacity,
                                    materials[mi].alphaMode == AlphaMode::Blend, nearEye);
                });

            // --- Road side objects (guard rails, curbs, posts) -----------------
            // Derived from the road's side lines and drawn as instanced models: one
            // model resolved per batch, then its baked meshes submitted at every
            // placement transform. Their materials ride in gpuMats already (the
            // model import registered them into the library like any other model).
            auto drawSideModel = [&](LoadedModel* lm, const glm::mat4& mm) {
                for (std::size_t i = 0; i < lm->meshes.size(); ++i) {
                    const int mi = document.materialIndex(lm->primMaterialId[i]);
                    // A model resolved (imported) this very frame appended its
                    // materials AFTER gpuMats was built -> its index is out of range
                    // for one frame. Skip; next frame gpuMats has it.
                    if (mi < 0 || mi >= static_cast<int>(gpuMats.size())) continue;
                    renderer.submit(lm->meshes[i], gpuMats[mi], mm, true,
                                    isMirror(materials[mi]),
                                    materials[mi].opacity,
                                    materials[mi].alphaMode == AlphaMode::Blend);
                }
            };
            for (const RoadSystem* rp : roads) {
                if (!rp->enabled) continue;
                for (const RoadSystem::SideBatch& batch : rp->sideBatches()) {
                    // Knockable instances are dynamic bodies in Play -- drawn from
                    // their live physics transform below, not their static seat.
                    if (playMode && batch.knockable) continue;
                    LoadedModel* lm = resolveSideModel(batch.model);
                    if (!lm) continue;
                    const glm::vec3 halfSz = glm::max(lm->size(), glm::vec3(1e-4f));
                    for (const roadside::Instance& in : batch.instances) {
                        // Rest the model's base on the placement point: centre its
                        // AABB half a (scaled) height above the ground it stands on.
                        const glm::vec3 c =
                            in.pos + glm::vec3(0.0f, halfSz.y * 0.5f * in.scale, 0.0f);
                        const glm::mat4 mm =
                            composeModel(c, glm::vec3(0.0f, glm::degrees(in.yaw), 0.0f),
                                         glm::vec3(in.scale)) *
                            glm::translate(glm::mat4(1.0f), -lm->center());
                        drawSideModel(lm, mm);
                    }
                }
            }
            // Imported models the towns stand on blocks in place of a generated
            // building (CivicSlot::useModel), drawn like the side objects.
            towns.forEachModel(camera.position(), [&](const std::string& ref, const glm::mat4& mm) {
                if (LoadedModel* lm = resolveSideModel(ref)) drawSideModel(lm, mm);
            });
            // Knockable posts follow their rigid body: read the box's world
            // transform (its centre == the model's AABB centre, as placed) and draw
            // the model there, so a clipped post tumbles and flies off.
            if (playMode && physics)
                for (const playworld::SidePost& p : sidePosts) {
                    LoadedModel* lm = models.byId(p.modelId);
                    glm::vec3 pos; glm::quat rot;
                    if (!lm || !physics->getTransform(p.body, pos, rot)) continue;
                    const glm::mat4 mm =
                        glm::translate(glm::mat4(1.0f), pos) * glm::mat4_cast(rot) *
                        glm::scale(glm::mat4(1.0f), glm::vec3(p.scale)) *
                        glm::translate(glm::mat4(1.0f), -lm->center());
                    drawSideModel(lm, mm);
                }

            // --- Stones and reeds along the watercourses -----------------------
            // Derived geometry, merged per material and already in world space --
            // hence the identity model matrix, exactly like the roadside city and
            // the fences below. Opaque and lit: a boulder is a boulder, and only
            // the water itself needs a shader of its own.
            for (const RiverSystem::Run& run : rivers.runs()) {
                for (std::size_t i = 0; i < run.dress.size() &&
                                        i < run.dressMeshes.size(); ++i) {
                    const int mi = document.materialIndex(run.dress[i].material);
                    if (mi < 0 || mi >= static_cast<int>(gpuMats.size())) continue;
                    renderer.submit(run.dressMeshes[i], gpuMats[mi], glm::mat4(1.0f),
                                    true, isMirror(materials[mi]),
                                    materials[mi].opacity,
                                    materials[mi].alphaMode == AlphaMode::Blend);
                }
            }

            // --- Splines (fences, walls, railway track) ------------------------
            // Derived geometry, merged per material and already in world space --
            // hence the identity model matrix, exactly like the roadside city
            // above. Never entities, so they cost no hierarchy row, no undo
            // snapshot and no line in the scene file.
            for (const SplineSystem::Run& run : splines.runs()) {
                for (std::size_t i = 0; i < run.geo.batches.size() &&
                                        i < run.meshes.size(); ++i) {
                    const int mi = document.materialIndex(run.geo.batches[i].material);
                    if (mi < 0 || mi >= static_cast<int>(gpuMats.size())) continue;
                    renderer.submit(run.meshes[i], gpuMats[mi], glm::mat4(1.0f),
                                    true, isMirror(materials[mi]),
                                    materials[mi].opacity,
                                    materials[mi].alphaMode == AlphaMode::Blend);
                }
            }
            trams.forEachDraw([&](const Mesh& mesh, const fitzel::AssetId& mat, const glm::mat4& m) {
                const int mi = document.materialIndex(mat);
                if (mi < 0 || mi >= static_cast<int>(gpuMats.size())) return;
                renderer.submit(mesh, gpuMats[mi], m, true, isMirror(materials[mi]),
                                materials[mi].opacity,
                                materials[mi].alphaMode == AlphaMode::Blend);
            });

            // Any entity carrying a LightComponent becomes a real light -- decoupled
            // from EntityType, so a box can glow too. Point lights radiate omni;
            // spot lights (type 1) shine a cone down the entity's forward (+Z), so
            // parenting one to a car turns it into a headlight.
            std::vector<PointLight> pointLights;
            std::vector<SpotLight>  spotLights;
            for (const Entity& b : entities) {
                if (!b.activeInHierarchy) continue;          // deactivated: no light
                const auto* lc = b.components.get<LightComponent>();
                if (!lc) continue;
                if (lc->type == 1) {                          // spot
                    if (static_cast<int>(spotLights.size()) >= Renderer::kMaxSpotLights)
                        continue;
                    SpotLight sl;
                    sl.position  = b.center;
                    sl.direction = glm::normalize(glm::quat(glm::radians(b.rotation)) *
                                                  glm::vec3(0.0f, 0.0f, 1.0f));
                    sl.color     = lc->color * lc->intensity; // HDR radiance
                    sl.range     = lc->range;
                    const float outer = glm::radians(glm::clamp(lc->spotAngle, 1.0f, 89.0f));
                    const float inner = outer * (1.0f - glm::clamp(lc->spotBlend, 0.0f, 1.0f));
                    sl.cosOuter  = std::cos(outer);
                    sl.cosInner  = std::cos(inner);
                    spotLights.push_back(sl);
                } else {                                      // point
                    if (static_cast<int>(pointLights.size()) >= Renderer::kMaxPointLights)
                        continue;
                    PointLight pl;
                    pl.position    = b.center;
                    pl.color       = lc->color * lc->intensity; // HDR radiance
                    pl.range       = lc->range;
                    pl.castShadows = lc->castShadows;
                    pl.shadowBias  = lc->shadowBias;
                    pointLights.push_back(pl);
                }
            }
            // Everything from gui.beginFrame() down to here is scene assembly:
            // the editor's panels plus walking the entities and submitting them.
            // Measured as one span because it is one cost -- CPU work before a
            // single GL draw has been issued.
            // Drawn last so it reports the frame that just happened, and inside
            // the UI span so its own cost is honestly counted rather than hidden.
            debugoverlay::draw(&showPerf);
            prof::addSince("ui + submit", fzUiMark);

            // After dark the cars of the nearest tram light their inside.
            trams.collectLights(camera.position(), lampsOn, pointLights,
                                std::min(3, Renderer::kMaxPointLights - 2 -
                                                static_cast<int>(pointLights.size())));
            // The street lamps nearest the eye, after the scene's own lights and
            // before the missiles, with two points left for those.
            townLamps.collectLights(camera.position(), streetLamps,
                                    pointLights, Renderer::kMaxPointLights - 2,
                                    spotLights, Renderer::kMaxSpotLights,
                                    Renderer::kMaxShadowedPoints);
            // Missile motors and detonations are lights too -- a blast that does
            // not light the corner it goes off in reads as a decal pasted over
            // the scene. Appended last so authored scene lights keep priority
            // when the renderer's budget runs out.
            weapons.collectLights(pointLights);
            weapons2.collectLights(pointLights);

            std::optional<gputime::Scope> fzGpuFrame;   // the frame's whole GPU bill
            fzGpuFrame.emplace("GPU frame");
            const long long fzShadowMark = prof::mark();
            // Play mode is the game, and the game is always Textured -- a
            // viewport mode is a way of looking at the scene while building it.
            const int  shade = playMode ? kShadeTextured : viewShade;
            // Pathtraced still rasters the frame underneath: it is what the
            // viewport shows until the first trace arrives, and what it falls
            // back to while the view is moving. The raster frame is the cheap
            // half of that pair by a wide margin.
            const int  rasterShade = (shade == kShadePathTraced) ? kShadeTextured
                                                                 : shade;
            const bool shadeFull   = (rasterShade == kShadeTextured);
            renderer.setShadingMode(rasterShade);
            renderer.setPointLights(pointLights);
            renderer.setSpotLights(spotLights);
            // Baked light for this frame: loads the grid belonging to the open
            // scene the first time it is seen, then hands it to the renderer.
            // Before the shadow and lit passes, which is where it is read.
            lightGrid.syncTo(currentProject.empty()
                                 ? std::filesystem::path()
                                 : std::filesystem::path(currentProject),
                             renderer);
            renderer.preparePointShadows(); // omni shadow cubemaps (opt-in lights)

            // --- Multi-pass render with sky and planar water ------------
            // Trees cast shadows: drawn into every cascade via this callback.
            // Which eye the cascades are being fitted to. Split screen runs the
            // pass twice with two different cameras, and the tree cull measures
            // distance from the eye -- fed player one's position both times, the
            // second pane loses the shadows around itself.
            glm::vec2 shadowEyeXZ(camera.position().x, camera.position().z);
            float cascadeNear = 0.0f;   // where the slice being cast starts
            auto treeShadowCaster = [&](const glm::mat4& lightSpace, int cascade,
                                        float cascadeFar) {
                if (cascade == 0) cascadeNear = 0.0f;
                // Timed apart from the rest of the cascade pass: "GPU shadows"
                // is the sum of two very different costs -- the queue (terrain
                // and objects, plain depth) and the forest (alpha-cutout leaves,
                // LOD0, once per cascade) -- and which of the two is the bill is
                // the first thing you need to know when the number is large.
                // Summed over every cascade, like the zone it sits inside.
                FZ_GPU_ZONE("GPU shadow trees");
                // How far past its own reach a cascade still needs casters: a
                // tree standing between the sun and the slice shades into it,
                // and the length of that reach is the tree's height over the
                // tangent of the sun's elevation. Low sun, long shadows, more
                // trees; noon, almost none. Capped, because at sunrise the
                // formula runs to the horizon and the shadow it asks for is a
                // grey smear no one can point at.
                const float sunY  = std::max(0.05f, light.direction.y);
                const float sunXZ = std::sqrt(std::max(0.0f, 1.0f - sunY * sunY));
                const float runs  = std::min(25.0f * sunXZ / sunY, 60.0f);
                const float reach = cascadeFar + runs;
                // ...and the other end: a tree nearer than the slice's start by
                // more than its shadow runs shades nothing in it. The far cascade
                // starts past the tree-shadow limit's worth of forest, and drew
                // the whole disc of it again.
                const float from = std::max(0.0f, cascadeNear - runs - 5.0f);
                cascadeNear = cascadeFar;
                // The author's own limit still wins: it is the one that decides
                // whether the far cascades get a forest at all.
                const float limit = veg.treeShadowDistance > 0.0f
                                        ? std::min(veg.treeShadowDistance, reach)
                                        : reach;
                veg.drawTreeShadow(lightSpace, now, storm, shadowEyeXZ, limit, from,
                                   cascade >= 2, renderer.cascadeCullFace());
            };
            // Cascades for player one. With two panes up the second pane fits
            // its own set inside the loop below -- shadows are cut to a view
            // frustum, so they cannot be shared between two people looking at
            // different places.
            // The clouds' shadow on the ground (CloudShadow.hpp): the sky's own
            // cumulus, marched towards the sun from a 16 km ground map. Before
            // any pass, so every receiver this frame reads the same map.
            {
                bool cumulus = false;
                for (const skylayers::Packed& L :
                     skylayers::order(skySet, effCloudBot, skySet.top))
                    if (L.kind == 0) cumulus = true;
                if (cumulus && cloudShadowsOn && shadeFull) {
                    FZ_GPU_ZONE("GPU cloud shadows");
                    CloudShadow::Params cp;
                    cp.time     = static_cast<float>(now);
                    cp.coverage = glm::mix(0.86f, 0.46f, effCoverage);
                    cp.density  = effDensity;
                    cp.scale    = skySet.scale;
                    cp.speed    = effWind;
                    cp.bottom   = effCloudBot;
                    cp.top      = skySet.top;
                    cp.sunDir   = light.direction;
                    cp.eye      = camera.position();
                    cp.groundY  = waterLevel;
                    cloudShadow.render(cp, [&] { fsQuad.draw(); });
                } else {
                    cloudShadow.disable();
                }
                // ...and the mountains' (FarTerrain::renderSunShadow): the valley
                // goes into shade while the summits still catch the sun.
                if (farTerrainOn && shadeFull)
                    farTerrain.renderSunShadow(light.direction, camera.position(),
                                               [&] { fsQuad.draw(); });
                else
                    cloudShadowInfo().mtnOn = false;
                applyCloudShadow(lit);
            }
            {
                FZ_GPU_ZONE("GPU shadows");
                renderer.prepareShadows(treeShadowCaster); // shadows from the real camera
            }
            prof::addSince("shadows", fzShadowMark);
            const long long fzSceneMark = prof::mark();

            // Fullscreen sky + volumetric clouds for a given view.
            auto drawSky = [&](const glm::mat4& invViewProj, const glm::vec3& eye,
                               bool tonemap) {
                glDisable(GL_DEPTH_TEST);
                glDepthMask(GL_FALSE);
                glDisable(GL_CULL_FACE);
                sky.bind();
                sky.setMat4("uInvViewProj", invViewProj);
                sky.setVec3("uCameraPos", eye);
                sky.setVec3("uSunDir", light.direction);
                sky.setVec3("uSunColor", light.color);
                sky.setFloat("uTime", static_cast<float>(now));
                sky.setFloat("uCoverage", glm::mix(0.86f, 0.46f, effCoverage));
                sky.setFloat("uCloudDensity", effDensity);
                sky.setFloat("uCloudScale", skySet.scale);
                sky.setFloat("uCloudSpeed", effWind);
                sky.setFloat("uCloudBottom", effCloudBot);
                sky.setFloat("uCloudTop", skySet.top);
                sky.setFloat("uOvercast", fog.overcast);
                sky.setVec3("uOvercastColor", fog.overcastColor);
                // The layer stack, highest first, with the cumulus already in
                // its place in the order -- see SkyLayers.hpp. The march is one
                // entry in this list rather than a step after it, which is what
                // lets a stratus deck sit UNDER the cumulus and hide it.
                const std::vector<skylayers::Packed> layers =
                    skylayers::order(skySet, effCloudBot, skySet.top);
                sky.setInt("uLayerCount", static_cast<int>(layers.size()));
                for (std::size_t li = 0; li < layers.size(); ++li) {
                    const skylayers::Packed& L = layers[li];
                    char nm[40];
                    std::snprintf(nm, sizeof(nm), "uLayerKind[%zu]", li);
                    sky.setInt(nm, L.kind);
                    std::snprintf(nm, sizeof(nm), "uLayerA[%zu]", li);
                    sky.setVec4(nm, glm::vec4(L.amount, L.height, L.scale, L.wind));
                    std::snprintf(nm, sizeof(nm), "uLayerDir[%zu]", li);
                    sky.setVec2(nm, glm::vec2(L.dirX, L.dirZ));
                }
                sky.setFloat("uExposure", postLook.exposure);
                sky.setInt("uTonemap", tonemap ? 1 : 0);
                fsQuad.draw();
                glDepthMask(GL_TRUE);
                glEnable(GL_DEPTH_TEST);
                glEnable(GL_CULL_FACE);
            };

            // Background: the HDRI panorama when it is the active sky, else the
            // procedural sky. Same signature as drawSky so it drops in everywhere.
            auto drawBackground = [&](const glm::mat4& invViewProj,
                                      const glm::vec3& eye, bool tonemap) {
                if (!(iblSkybox && environment.valid())) {
                    drawSky(invViewProj, eye, tonemap);
                    return;
                }
                glDisable(GL_DEPTH_TEST);
                glDepthMask(GL_FALSE);
                glDisable(GL_CULL_FACE);
                skybox.bind();
                environment.bindEnvCube(0);
                skybox.setInt("uEnv", 0);
                skybox.setMat4("uInvViewProj", invViewProj);
                skybox.setVec3("uCameraPos", eye);
                skybox.setFloat("uIntensity", iblIntensity);
                skybox.setFloat("uExposure", postLook.exposure);
                skybox.setInt("uTonemap", tonemap ? 1 : 0);
                fsQuad.draw();
                glDepthMask(GL_TRUE);
                glEnable(GL_DEPTH_TEST);
                glEnable(GL_CULL_FACE);
            };

#ifndef FITZEL_PLAYER
            // The offline renderer harvests HERE: every system has submitted,
            // the lights are set, begin() has not yet cleared the queue -- and
            // the sky (drawBackground, just above) and this frame's cloud
            // shadow exist, which the tracer captures too. Costs a bool test
            // unless somebody has actually pressed Render.
            pathpanel::SceneLook ptLook;
            ptLook.hdriPath      = hdriAbsPath;
            ptLook.hdriIntensity = iblEnabled ? iblIntensity : 0.0f;
            // The grade the post chain will put on this very frame. Without it a
            // render comes out flat and cool beside the viewport, because the
            // viewport never shows an ungraded image -- not even in a project
            // nobody has touched the Colour grade panel in.
            ptLook.grade.hueShift   = postLook.hueShift;
            ptLook.grade.saturation = postLook.saturation;
            ptLook.grade.value      = postLook.valueGain;
            ptLook.grade.warmth     = postLook.warmth;
            ptLook.grade.contrast   = postLook.contrast;
            ptLook.grade.curve      = postLook.tonemapCurve;
            ptLook.grade.vignette   = postLook.vignette;
            // The grass, which the harvest cannot see: the tracer regenerates it
            // from the same parameters the streamed field was built from.
            if (veg.grassEnabled) {
                ptLook.grass       = &veg.traceField();
                ptLook.grassRadius = veg.grassRadius;
                ptLook.grassWindTime = static_cast<float>(glfwGetTime());
                ptLook.paintedGrass  = &veg.paintedBlades;
                ptLook.grassHeight   = veg.grassHeight;
            }
            // Everything else the queue does not carry, rebuilt from the
            // systems' own data (WorldTrace.hpp). Called synchronously from
            // inside service(), so capturing this frame's locals is safe.
            ptLook.world = [&](pathtrace::Scene& sc, const worldtrace::Options& wo,
                               bool preview, std::vector<std::string>& notes) {
                worldtrace::Sources ws;
                ws.veg       = &veg;
                ws.rivers    = &rivers;
                ws.wildlife  = (wildlifeOn && veg.birdsEnabled) ? &wildlife : nullptr;
                ws.particles = &particles;
                ws.spray     = &spray;
                ws.motes     = (shadeFull && motesOn) ? &motes : nullptr;
                ws.motesRadius = motes.radius;
                ws.rain      = &rain;
                ws.clouds    = &cloudShadow;
                ws.terrain   = &streamer.settings();
                ws.farTerrainOn = farTerrainOn && veg.terrainPresent;
                const glm::vec4 nr = farTerrain.nearRect();
                ws.nearMin = glm::vec2(nr.x, nr.y);
                ws.nearMax = glm::vec2(nr.z, nr.w);
                ws.farLook   = &farTerrain;
                ws.moistTex  = farTerrain.ready() ? farTerrain.fineTexture() : 0u;
                ws.moistRect = farTerrain.fineRect();
                ws.waterLevel   = waterLevel;
                ws.waterColor   = waterColor;
                ws.waterClarity = waterClarity;
                ws.waterIor     = waterIor;
                ws.time     = static_cast<float>(now);
                ws.weather  = storm;
                ws.ambient  = light.ambient;
                ws.sunColor = light.color;
                ws.sunDir   = light.direction;
                ws.drawSky  = [&](const glm::mat4& ivp, const glm::vec3& eye) {
                    drawBackground(ivp, eye, false);
                };
                ws.skyIsLightingHdri = iblSkybox && environment.valid() && iblEnabled;
                worldtrace::append(sc, ws, wo, camera.position(), camera.right(),
                                   camera.up(), camera.fov(), preview, notes);
            };
            // The preview shows what the still would: the same switches.
            viewTrace.world = pathRender.world;
            pathpanel::service(pathRender, lightGrid, renderer, camera, ptLook,
                               currentProject.empty()
                                   ? std::filesystem::path()
                                   : std::filesystem::path(currentProject));
            // The path-traced viewport harvests from the same window and for
            // the same reason. Off, this is one early return.
            viewtrace::service(viewTrace, shade == kShadePathTraced, renderer,
                               camera, ptLook, viewW, viewH, now);
#endif

            // Instanced 3D trees for a given view (used by the main pass and the
            // water reflection, so trees mirror in the water). Two-sided.

            // 0) Environment probe: capture the scene into a cubemap so reflective
            //    materials mirror the surrounding world. One shared probe (v1); its
            //    parallax is only exact at its capture point. The trigger is that a
            //    reflective material EXISTS in the library -- not that a placed
            //    object already uses one -- so a surface reflects the instant its
            //    material is made reflective, without first having to drop in an
            //    object with a reflective material. Still skipped entirely when
            //    nothing in the scene is reflective (the common case), so the
            //    cubemap render is not paid for a matte scene.
            bool wantProbe = false;
            for (const MaterialDef& md : materials)
                // ANY reflectivity, deliberately -- not isMirror(). A glossy
                // facade samples the probe just like a mirror does; it is only
                // being kept OUT of the capture, and being the capture point,
                // that a mirror is special about.
                if (md.reflectivity > 0.0f) { wantProbe = true; break; }
            // A wet carriageway mirrors the world through the same probe (see
            // lit.frag: uWetReflect raises the surface's reflectance), so it has
            // to ask for one too -- otherwise it samples a stale cube, or one
            // that was never rendered at all. This is what makes rain and the
            // road's Wet-reflection slider cost a cubemap render; with that
            // slider at 0, or a dry road, nothing here is paid for.
            wantProbe = wantProbe || anyWetMirror;
            if (wantProbe) {
                FZ_GPU_ZONE("GPU env probe");
                // Capture at the first MIRROR-like object if there is one (best
                // parallax there); otherwise around the camera, so reflective
                // terrain / not-yet-placed materials still get a sensible probe.
                // Merely glossy surfaces do not count, or the first glazed
                // building mass in the scene would capture the whole city's
                // reflections from inside a tower.
                // Player one's eye: the probe is captured once and shared by
                // both panes, like the shadows above.
                // Only a shown one: a game's pool of hidden objects parks them
                // anywhere, under the ground included, and a probe captured
                // down there mirrors the underside of the world into every river.
                // The NEAREST mirror within reach of the eye, not the first in the
                // list: a mirror-painted car the traffic drives round the town was
                // "the first", and the probe followed it -- three faces a frame
                // while it drove, captured somewhere the camera could not even see.
                glm::vec3 probePos = camera.position();
                float bestD2 = 60.0f * 60.0f;
                for (const Entity& b : entities) {
                    const auto* mc = b.components.get<MaterialComponent>();
                    if (b.activeInHierarchy &&
                        b.type != EntityType::Light && b.type != EntityType::Sun && mc &&
                        isMirror(materials[document.materialIndex(mc->material)])) {
                        const glm::vec3 d = b.center - camera.position();
                        const float d2 = glm::dot(d, d);
                        if (d2 < bestD2) { bestD2 = d2; probePos = b.center; }
                    }
                }
                renderer.prepareEnvProbe(probePos,
                    [&](const glm::mat4& ivp, const glm::vec3& eye) {
                        drawBackground(ivp, eye, false);
                    });
            }

            // === Per-pane rendering ===========================================
            // Everything above this point is the frame's shared work: shadow
            // cascades, the environment probe, the terrain material. Everything
            // below is drawn once per player, into render targets sized to one
            // pane, and blitted into its half of the final image.
            //
            // The water mirror sits inside the loop rather than above it because
            // a reflection is a function of where the eye is: shared between two
            // panes it would mirror player one's view into player two's water.
            for (int vi = 0; vi < views; ++vi) {
            const Camera&    vcam   = (vi == 0) ? camera : camera2;
            const glm::vec3  camPos = vcam.position();
            const glm::mat4  view   = vcam.viewMatrix();
            // Each pane projects with ITS OWN camera: field of view is a property
            // of the camera entity now, so two players can be looking through
            // different lenses. (The outer `proj` stays what the shared work --
            // shadow fitting, the probe -- was sized against.)
            // Temporal AA: this frame is drawn a sub-pixel off, a different
            // offset every frame, and the resolve gathers them. Only in a single
            // full-shading pane -- one history cannot serve two cameras, and a
            // wireframe gathered over frames is a smear.
            const bool useTaa = postLook.taaEnabled && views == 1 && shadeFull;
            const glm::mat4  projUnjittered = vcam.projectionMatrix(aspect);
            glm::vec2 taaJitter(0.0f);
            glm::mat4 projJ = projUnjittered;
            if (useTaa) {
                taaJitter = PostChain::jitter(taaFrame, rw, rh);
                if (projJ[2][3] != 0.0f) {
                    // Perspective: subtracting from the z column moves NDC by
                    // +jitter, since w = -z.
                    projJ[2][0] -= taaJitter.x;
                    projJ[2][1] -= taaJitter.y;
                } else {
                    // Orthographic (w = 1): the translation column does it.
                    projJ[3][0] += taaJitter.x;
                    projJ[3][1] += taaJitter.y;
                }
            }
            const glm::mat4  proj   = projJ;
            const glm::mat4  mainVP = proj * view;
            const glm::mat4  mainVPUnjittered = projUnjittered * view;

            // This pane's shadow cascades. Pane 0 already has them from the
            // shared pass above; the second pane re-fits them to its own eye,
            // which costs one more cascade pass over the same queue. Everything
            // this pane draws afterwards -- the water passes included -- samples
            // what is bound here, so it has to happen before any of it.
            if (vi > 0) {
                shadowEyeXZ = glm::vec2(vcam.position().x, vcam.position().z);
                renderer.prepareShadowsFor(vcam, aspect, treeShadowCaster);
            }

            // Is the water plane in shot at all? Both passes below push the
            // ENTIRE scene through renderScene a second and third time, purely
            // to feed the water surface -- so on a track that never comes near
            // water, or in the sandbox preset that parks the level at -1000,
            // two thirds of the frame's draw submission went into a surface
            // nobody can see.
            //
            // The plane is treated as infinite: if every corner of the view
            // frustum lands on the same side of it, it cannot be on screen.
            // Terrain occlusion is only asked about when looking steeply down
            // (below); otherwise it would want an occlusion query, and erring
            // that way only costs a pass that could have been skipped, never a
            // missing reflection.
            const bool waterVisible = shadeFull && [&] {
                const glm::mat4 invVP = glm::inverse(mainVP);
                bool above = false, below = false;
                for (int i = 0; i < 8; ++i) {
                    const glm::vec4 h =
                        invVP * glm::vec4((i & 1) ? 1.0f : -1.0f,
                                          (i & 2) ? 1.0f : -1.0f,
                                          (i & 4) ? 1.0f : -1.0f, 1.0f);
                    if (std::abs(h.w) < 1e-6f) return true; // degenerate: don't gamble
                    ((h.y / h.w > waterLevel) ? above : below) = true;
                }
                if (!(above && below)) return false;
                // The frustum straddles the plane. Looking steeply down on
                // ground -- every corner ray of the view meets the plane short of
                // the far plane, as from an aircraft -- the water can only show
                // where the ground under a ray's crossing point lies below the
                // level: anywhere else the ray met the ground first. So ask the
                // terrain along a grid of rays. A view that sees the horizon
                // keeps the plain answer: a lake far off is narrower than any
                // grid worth sampling.
                if (!terrainOn || camera.position().y <= waterLevel) return true;
                const auto crossing = [&](float nx, float ny, glm::vec2& at) {
                    const glm::vec4 n = invVP * glm::vec4(nx, ny, -1.0f, 1.0f);
                    const glm::vec4 f = invVP * glm::vec4(nx, ny, 1.0f, 1.0f);
                    const glm::vec3 a = glm::vec3(n) / n.w, b = glm::vec3(f) / f.w;
                    if ((a.y - waterLevel) * (b.y - waterLevel) > 0.0f) return false;
                    const float t = (a.y - waterLevel) / (a.y - b.y);
                    at = glm::vec2(a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t);
                    return true;
                };
                glm::vec2 at;
                for (int i = 0; i < 4; ++i)
                    if (!crossing((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, at))
                        return true;
                constexpr int kGx = 16, kGy = 10;
                for (int gy = 0; gy <= kGy; ++gy)
                    for (int gx = 0; gx <= kGx; ++gx) {
                        if (!crossing(-1.0f + 2.0f * gx / kGx, -1.0f + 2.0f * gy / kGy, at))
                            continue;
                        if (streamer.heightAt(at.x, at.y) < waterLevel + 0.5f) return true;
                    }
                return false;
            }();

            waterGate.poll();
            if (waterVisible && waterGate.seen()) {
                // 1) Reflection: sky + scene mirrored across the water plane,
                //    clipping everything below the surface.
                const glm::mat4 mirror =
                    glm::translate(glm::mat4(1.0f), {0.0f, 2.0f * waterLevel, 0.0f}) *
                    glm::scale(glm::mat4(1.0f), {1.0f, -1.0f, 1.0f});
                const glm::mat4 reflView = view * mirror;
                const glm::vec3 reflEye{camPos.x, 2.0f * waterLevel - camPos.y, camPos.z};

                // Reflection/refraction render LINEAR (tonemap=false) so the water
                // shader can sample and tonemap them once at the end.
                FZ_GPU_ZONE("GPU water reflect/refract");
                reflectRT.bind();
                glClear(GL_DEPTH_BUFFER_BIT);
                drawBackground(glm::inverse(proj * reflView), reflEye, false);
                // The ranges mirror in the lake -- half of why a mountain lake
                // looks like one.
                if (farTerrain.ready()) {
                    farTerrain.draw(makeFrameContext(proj * reflView, reflEye, now, storm,
                                                     light, fog),
                                    reflView, farProjection(vcam, aspect, taaJitter), true);
                    glClear(GL_DEPTH_BUFFER_BIT);
                }
                glCullFace(GL_FRONT); // mirroring flips winding
                renderer.renderScene(reflView, proj, reflEye,
                                     glm::vec4(0, 1, 0, -waterLevel + 0.1f), false);
                glCullFace(GL_BACK);
                {   // trees mirror in the water (reflected view/eye)
                    veg.drawTrees(makeFrameContext(proj * reflView, reflEye, now, storm,
                                                   light, fog));
                }

                // 2) Refraction: scene only, clipping above water (deep-water clear).
                refractRT.bind();
                glClearColor(waterColor.r * 0.5f, waterColor.g * 0.5f,
                             waterColor.b * 0.5f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                renderer.renderScene(view, proj, camPos,
                                     glm::vec4(0, -1, 0, waterLevel + 0.1f), false);
            }

            // 3) Main pass: sky + full scene rendered LINEAR into the HDR buffer
            //    (tonemapping happens in the composite pass).
            // Sized to ONE pane: both players are drawn through the same set of
            // targets, one after the other, so a second view costs no extra
            // video memory -- only the time to fill them twice.
            if (hdrRT.width() != rw || hdrRT.height() != rh)
                hdrRT = RenderTarget(rw, rh, RenderTarget::Format::RGBA16F, true, true);
            post.resize(rw, rh);   // no-op unless the pane actually changed
            // The one target that stays FULL width: it is the finished image
            // both panes are blitted into (and what the editor shows in its
            // viewport panel).
            if (viewportRT.width() != fbW || viewportRT.height() != fbH) {
                retiredRT  = std::move(viewportRT);   // still named in this frame's draw list
                viewportRT = RenderTarget(fbW, fbH, RenderTarget::Format::RGBA8);
            }
            hdrRT.bind();
            // A plain mode gets a flat ground to stand on rather than a sky: the
            // sky is a material too, and a wireframe read against a sunset is
            // exactly the reading these modes exist to avoid. Linear, because
            // the composite tonemaps afterwards.
            if (!shadeFull) glClearColor(0.055f, 0.060f, 0.070f, 1.0f);
            glClearStencil(0);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            // The horizon and the sky come AFTER the near scene and its
            // vegetation (below), each shaded only where what was drawn before
            // it left the view open. Both used to be drawn first, and every
            // pixel paid -- the far terrain's ridge shadows marched across the
            // ranges, the sky's marched cumulus -- even where a house, a tree or
            // the near ground was about to cover it, which from a street is most
            // of the screen. Everything drawn until then marks stencil bit 0.
            const bool farLate = shadeFull;
            if (farLate) {
                glEnable(GL_STENCIL_TEST);
                glStencilMask(0xFF);
                glStencilFunc(GL_ALWAYS, 1, 0xFF);
                glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            }
            if (shade == kShadeWireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            // Screen-space reflections out of last frame's picture -- for the
            // main pass only: the probe faces and the water mirror above look
            // from elsewhere. Not on the very first frame, nor with the player's
            // Reflections off, nor in split screen (the history is one pane's).
            // Contact shadows read the same history, for short rays towards the
            // sun; they go with the player's Shadows setting instead.
            // Not through an orthographic lens either: both traces compare a
            // ray's clip w against linearised depth, and w is 1 there.
            const bool history = views == 1 && shadeFull && post.historyColor() != 0 &&
                                 !vcam.orthographic() &&
                                 glm::distance(camPos, taaPrevEye[vi]) < 25.0f; // not across a cut
            const bool ssr     = history && postLook.ssrEnabled && gfxSet.reflections > 0;
            const bool contact = history && postLook.contactShadows && gfxSet.shadows > 0 &&
                                 renderer.shadowsEnabled();
            // The scene in two halves: the solid objects now, the see-through
            // ones after the vegetation and the water (below).
            const auto sceneHalf = [&](Renderer::ScenePart part) {
                if (ssr || contact)
                    renderer.setScreenHistory(post.historyColor(), post.historyDepth(),
                                              taaPrevVP[vi], vcam.nearPlane(), vcam.farPlane(),
                                              ssr, contact);
                renderer.setScenePart(part);
                renderer.renderScene(view, proj, camPos, Renderer::kNoClip, false);
                renderer.setScenePart(Renderer::ScenePart::All);
                renderer.clearScreenHistory();
            };
            {
                FZ_GPU_ZONE("GPU terrain + objects");
                sceneHalf(Renderer::ScenePart::Opaque);
            }
            if (shade == kShadeWireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

            // Shared draw context for the lit vegetation (grass, trees, billboards)
            // in this HDR pass.
            FrameContext gctx =
                makeFrameContext(mainVP, camPos, now, storm, light, fog);
            // This pane's cascades were fitted to this camera (above), so the
            // grass and trees drawn with it can receive the sun's shadow.
            gctx.shadows     = &renderer.shadows();
            gctx.viewForward = vcam.front();

            // Vegetation and birds only in Textured. Grass and trees are
            // vertex-shader geometry with shaders of their own, none of which
            // has a clay mode -- and a hundred thousand blades of grass drawn
            // as wireframe is a white screen, not a view of the scene.
            if (shadeFull) {
                { FZ_GPU_ZONE("GPU grass");
                  veg.drawGrass(gctx); } // grass into the HDR buffer, lit + fogged

                // Flowers into the HDR buffer, lit + fogged like grass.
                { FZ_GPU_ZONE("GPU flowers");
                  veg.drawFlowers(gctx); }

                // Trees (instanced, per-material) + distant billboards into the HDR.
                { FZ_GPU_ZONE("GPU trees");
                  veg.drawTrees(gctx);
                  veg.drawTreeBillboards(gctx, vcam.right()); }

                // Birds: a flock wheeling above the camera, two-sided into the HDR.
                if (wildlifeOn && veg.birdsEnabled) wildlife.draw(gctx);
                else veg.drawBirds(mainVP, now, camPos);
            }
            // The horizon, in its own depth range (see FarTerrain.hpp), where
            // nothing near was drawn (stencil bit 0 clear), marking bit 7 where it
            // lands; then the sky into what is left (a zero stencil); then the
            // far terrain's depth goes back out of the buffer, so what follows
            // (water, glass, fog, the post chain) sees sky there as before.
            // Jittered like the scene, or TAA would smear every ridgeline it
            // resolves.
            const bool farDrawn = farLate && farTerrain.ready();
            if (farDrawn) {
                FZ_GPU_ZONE("GPU far terrain");
                glStencilMask(0x80);
                glStencilFunc(GL_EQUAL, 0x80, 0x01);   // (0x80 & 0x01) == (stencil & 0x01)
                glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                farTerrain.draw(makeFrameContext(mainVP, camPos, now, storm, light, fog),
                                view, farProjection(vcam, aspect, taaJitter));
            }
            if (farLate) {
                FZ_GPU_ZONE("GPU sky");
                halfSky.renderBehind(hdrRT, fsQuad,
                                     [&] { drawBackground(glm::inverse(mainVP), camPos, false); });
            }
            if (farDrawn) {
                glEnable(GL_STENCIL_TEST);
                glStencilMask(0x00);
                glStencilFunc(GL_EQUAL, 0x80, 0x80);   // the far terrain's pixels
                glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
                // No glGet here, on purpose: under the driver's threaded
                // optimisation a state query waits for everything queued so far.
                // FarTerrain::draw leaves GL_LESS behind; the quad faces the eye,
                // so culling (back faces, in this pass) never touches it.
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_ALWAYS);
                glDepthMask(GL_TRUE);
                glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                depthToFar.bind();
                fsQuad.draw();
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glDepthFunc(GL_LESS);
            }
            if (farLate) {
                glStencilMask(0xFF);
                glDisable(GL_STENCIL_TEST);
            }

            // 4) The water surface: a large quad following the camera, sampling
            //    the reflection/refraction targets with Fresnel + ripples. Drawn
            //    only when those targets were filled this frame -- otherwise it
            //    would mirror whatever the camera was looking at last time it
            //    saw water.
            if (waterVisible) {
                FZ_GPU_ZONE("GPU water surface");
                glm::mat4 waterModel =
                    glm::translate(glm::mat4(1.0f), {camPos.x, waterLevel, camPos.z});
                waterModel = glm::scale(waterModel, glm::vec3(1400.0f, 1.0f, 1400.0f));

                water.bind();
                water.setMat4("uModel", waterModel);
                water.setMat4("uViewProj", mainVP);
                water.setVec3("uCameraPos", camPos);
                water.setVec3("uLightDir", light.direction);
                water.setVec3("uLightColor", light.color);
                water.setFloat("uTime", static_cast<float>(now));
                water.setVec3("uWaterColor", waterColor);
                water.setFloat("uWaveStrength", waveStrength);
                water.setFloat("uWaveScale", waveScale);
                water.setFloat("uReflectivity", waterReflectivity);
                water.setFloat("uClarity", waterClarity);
                water.setFloat("uIor", waterIor);
                water.setFloat("uWaveHeight", effWaveH);
                water.setFloat("uChoppy", effWaveC);
                water.setVec3("uAmbient", light.ambient);
                water.setVec3("uFogColor", fog.color);
                water.setVec3("uFogSunColor", fog.sunColor);
                water.setFloat("uFogDensity", fog.density);
                water.setFloat("uFogHeightFalloff", fog.heightFalloff);
                water.setFloat("uFogHeight", fog.height);
                water.setFloat("uExposure", postLook.exposure);
                water.setInt("uTonemap", 0); // linear into HDR; composite tonemaps
                water.setInt("uReflection", 0);
                water.setInt("uRefraction", 1);
                water.setInt("uRefractionDepth", 2);
                water.setFloat("uNear", camera.nearPlane());
                water.setFloat("uFar", camera.farPlane());
                water.setInt("uOrtho", camera.orthographic() ? 1 : 0);
                water.setFloat("uFoamWidth", foamWidth);
                // Rain dimpling the lake. Full strength, with none of the road's
                // wetness gate -- open water is wet whether or not it has been
                // raining, and a drop hits it just as hard in a drizzle as in a
                // downpour. What the weather changes is how MANY land.
                water.setFloat("uRainRings", 1.0f);
                water.setFloat("uRainDensity", rainDensity);
                // Rings where the fish rose (Wildlife.hpp).
                wildlife.applyRipples(water, wildlifeOn && veg.birdsEnabled);
                water.setVec4("uWaterClip", farTerrain.ready()
                                                ? farTerrain.nearRect()
                                                : glm::vec4(-1e9f, -1e9f, 1e9f, 1e9f));
                reflectRT.bindColorTexture(0);
                refractRT.bindColorTexture(1);
                refractRT.bindDepthTexture(2);
                waterGate.begin();
                waterMesh.draw();
                waterGate.end();
            }

            // 5) Brooks, rivers and canals, into the same HDR buffer.
            //
            // After the lake, so a stream running into it lays over the surface
            // it is joining. Blended with depth writes OFF: the surface is
            // transparent, and one that wrote depth would punch a hole in the
            // water behind it wherever a bend doubles back into view. Two-sided,
            // because a fall's curtain is routinely seen from behind.
            //
            // Its own pass rather than a submission to the renderer, because a
            // river is not a material on a mesh -- it is a shader with one
            // channel's numbers in it, and there are as many sets of those as
            // there are watercourses.
            if (!rivers.runs().empty()) {
                FZ_GPU_ZONE("GPU rivers");
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);
                glDisable(GL_CULL_FACE);
                river.bind();
                river.setMat4("uViewProj", mainVP);
                // Nothing to clip in the main pass: this plane passes everything,
                // so a clip distance left enabled by an earlier pass cannot eat
                // the water.
                river.setVec4("uClipPlane", glm::vec4(0.0f, 1.0f, 0.0f, 1.0e6f));
                river.setVec3("uCameraPos", camPos);
                river.setVec3("uLightDir", light.direction);
                river.setVec3("uLightColor", light.color);
                river.setVec3("uAmbient", light.ambient);
                river.setFloat("uTime", static_cast<float>(now));
                river.setVec3("uFogColor", fog.color);
                river.setVec3("uFogSunColor", fog.sunColor);
                river.setFloat("uFogDensity", fog.density);
                river.setFloat("uFogHeightFalloff", fog.heightFalloff);
                river.setFloat("uFogHeight", fog.height);
                river.setFloat("uExposure", postLook.exposure);
                river.setInt("uTonemap", 0); // linear into HDR; composite tonemaps
                // The scene probe, for the reflection. Unit 2 is the renderer's
                // own probe unit and it rebinds it every lit pass, so borrowing
                // it here costs nothing and cannot alias a 2D sampler: this
                // shader reads the cube and nothing else from it.
                renderer.bindEnvProbe(Renderer::kEnvProbeUnit);
                river.setInt("uEnvProbe", Renderer::kEnvProbeUnit);
                river.setFloat("uEnvMaxLod", renderer.envProbeMaxLod());
                // Rain on running water. Same two values the lake gets and for
                // the same reason -- a brook is already wet, so there is nothing
                // to soak first. The shader decides where a drop can land; water
                // in mid air off a fall has no surface for a ring.
                river.setFloat("uRainRings", 1.0f);
                river.setFloat("uRainDensity", rainDensity);
                wildlife.applyRipples(river, wildlifeOn && veg.birdsEnabled);
                for (std::size_t i = 0; i < rivers.runs().size() &&
                                        i < rivers.paths.size(); ++i) {
                    if (!rivers.paths[i].enabled) continue;
                    const RiverSystem::Run& run = rivers.runs()[i];
                    if (run.mesh.vertexCount() == 0) continue;
                    const rivergen::Style& rst = rivers.paths[i].style;
                    river.setVec3("uShallow", rst.shallow);
                    river.setVec3("uDeep", rst.deep);
                    river.setFloat("uClarity", rst.clarity);
                    river.setFloat("uReflect", rst.reflect);
                    river.setFloat("uRippleScale", rst.rippleScale);
                    river.setFloat("uRipple", rst.ripple);
                    river.setFloat("uFlowSpeed", rst.flowSpeed);
                    river.setFloat("uFoamWidth", rst.foamWidth);
                    river.setFloat("uSparkle", rst.sparkle);
                    run.mesh.draw();
                }
                glEnable(GL_CULL_FACE);
                glDepthMask(GL_TRUE);
                glDisable(GL_BLEND);
            }

            // --- Rain streaks (storm) + boat foam, into the HDR buffer --------
            rain.draw(gctx);
            // Mist off the falls and rapids. The emitters come out of the
            // PROFILE (rivergen::spray), so the spray is wherever the water is
            // actually falling rather than wherever somebody remembered to place
            // a puff -- and it moves with the course when the course is dragged.
            //
            // Near the camera only: a kilometre of gorge would otherwise fill the
            // pool with particles nobody can see, and the pool is shared with the
            // boat wake. Airborne droplets only; the pool's flat foam snaps to the
            // LAKE's level, which is not where a brook is.
            if (spray.ready() && !rivers.runs().empty()) {
                std::uniform_real_distribution<float> u01(0.0f, 1.0f);
                auto rnd = [&] { return u01(sprayRng); };
                for (std::size_t ri = 0; ri < rivers.runs().size() &&
                                         ri < rivers.paths.size(); ++ri) {
                    if (!rivers.paths[ri].enabled) continue;
                    for (const rivergen::SprayPoint& sp : rivers.runs()[ri].spray) {
                        const float d = glm::distance(camPos, sp.pos);
                        if (d > 110.0f) continue;
                        const float fade = 1.0f - glm::smoothstep(60.0f, 110.0f, d);
                        // Poisson-ish: one draw per emitter per frame, so a
                        // hundred of them cost a hundred comparisons and not a
                        // hundred accumulators.
                        if (rnd() > sp.strength * fade * 16.0f * dt) continue;
                        const glm::vec3 side(sp.dir.z, 0.0f, -sp.dir.x);
                        SprayP p;
                        p.pos  = sp.pos + side * ((rnd() - 0.5f) * sp.width * 1.4f);
                        p.pos.y += rnd() * 0.25f;
                        p.vel  = sp.dir * (1.2f + rnd() * 2.8f) +
                                 glm::vec3(0.0f, 1.0f + rnd() * 2.6f, 0.0f) +
                                 side * ((rnd() - 0.5f) * 1.6f);
                        p.life = p.life0 = 0.55f + rnd() * 0.85f;
                        p.size = 0.5f + rnd() * 1.1f;
                        p.flat = 0.0f;
                        spray.add(p);
                    }
                }
            }
            spray.update(dt, waterLevel); // age the pool, then draw what survived
            spray.draw(gctx);
            // The scene's see-through objects, over everything solid there is:
            // the trees, the lake, the rivers. Before them, an explosion over a
            // forest came out beneath its crowns.
            {
                FZ_GPU_ZONE("GPU transparent");
                sceneHalf(Renderer::ScenePart::Transparent);
            }
            // Authored emitters. Stepped with the frame clock rather than the
            // sim's fixed one: these are decoration, and a puff of smoke that
            // resolves one frame late is worth less than the code to avoid it.
            // Updated HERE, next to the draw, so a paused editor still shows the
            // effect it is being tuned against.
            particles.update(entities, dt, assetDb);
            particles.draw(gctx);

            // --- Pollen and dust in the light, additive into HDR ---------------
            if (shadeFull && motesOn)
                motes.draw(gctx, static_cast<float>(rh) * 0.5f * proj[1][1]);

            // --- Fireflies: night-only glowing wanderers, additive into HDR ---
            // Out once the sun is down, not at half daylight: dayF is still only
            // one half with the sun a hand above the ridge, and a meadow full of
            // lit specks under a golden sunset is a screensaver, not a dusk.
            veg.drawFireflies(mainVP, now,
                              1.0f - glm::smoothstep(-0.10f, 0.0f, light.direction.y),
                              camPos);

            // --- Volumetric fog: the marched mist volume, blended into the HDR
            //     buffer ------------------------------------------------------
            // Last thing INTO the buffer and before anything that reads it: the
            // fog stands in front of everything the scene drew (it is a volume
            // between them and the eye), and it belongs to the picture the
            // composite tonemaps -- painted on afterwards it would neither bloom
            // around the sun nor take the frame's exposure.
            {
                FZ_ZONE("volumetric fog");
                FZ_GPU_ZONE("GPU volumetric fog");
                // Every entity carrying a VolumetricFogComponent is a volume, and
                // its BOX is the entity's own -- the same transform the gizmo
                // edits and the selection outline draws, built through the same
                // composeModel so the three cannot disagree about a rotation.
                // Deactivating the entity (or an ancestor) puts the mist out,
                // like it does a light.
                volFogVolumes.clear();
                for (const Entity& b : entities) {
                    if (!b.activeInHierarchy) continue;
                    const auto* fc = b.components.get<VolumetricFogComponent>();
                    if (!fc) continue;
                    VolumetricFog::Volume v;
                    // half-extents -> full size, because the proxy is a UNIT cube.
                    v.model  = composeModel(b.center, b.rotation,
                                            glm::max(b.half * 2.0f, glm::vec3(0.01f)));
                    v.medium = fc->fog;
                    volFogVolumes.push_back(v);
                }

                VolumetricFog::Params vp;
                vp.viewProj = mainVP;
                vp.camPos   = camPos;
                vp.camFwd   = vcam.front();
                vp.time     = static_cast<float>(now);
                vp.sunDir   = sunDir;
                // The sun's HDR radiance and the haze colour, not the raw tint
                // and the surface ambient: the mist is lit by the frame's own
                // light, so it goes out with the sun and takes the horizon's
                // colour in shadow -- which is the colour the aerial haze is
                // already painting the distance with.
                vp.sunColor = light.color;
                vp.ambient  = fog.color;
                volFog.render(hdrRT, volFogVolumes, volFogSet, vp, fsQuad,
                              renderer.shadowsEnabled() ? &renderer.shadows() : nullptr);
            }

            // --- Post: SSAO, bloom, tonemap, colour grade, speed blur -------
            // All of it lives in PostChain, which owns the shaders and the
            // intermediate targets it needs. What this pane has to say is only
            // what changes per frame and per view.
            {
                PostChain::Params pp;
                pp.proj      = proj;
                pp.viewProj  = mainVP;
                pp.camPos    = camPos;
                pp.nearPlane = vcam.nearPlane();
                pp.farPlane  = vcam.farPlane();
                pp.ortho     = vcam.orthographic();
                pp.aspect    = aspect;
                pp.sunDir    = sunDir;
                pp.sunCol    = sunCol;
                // Whose speed streaks THIS pane: the craft this pane follows.
                const racesim::RaceState& blurSt = (vi == 0) ? race : race2;
                // The player's graphics choices gate the effects HERE, where they
                // are consumed, rather than by writing into the values above:
                // those belong to the scene's author, and switching an effect back
                // on has to return the look that was authored, not a default.
                const gfxmenu::PostGate gate = gfxmenu::gatePost(
                    gfxSet, postLook.ssaoStrength, postLook.bloomIntensity, postLook.rayIntensity, postLook.dofMax,
                    postLook.motionBlurStrength * blurSt.blurSpeed01 * 0.35f);
                pp.ssaoRadius = postLook.ssaoRadius; pp.ssaoBias = postLook.ssaoBias;
                pp.ssaoPower  = postLook.ssaoPower;  pp.ssaoStrength = gate.ssaoStrength;
                // What share of a sunlit, level surface's light is the sky's:
                // that is all the AO may take away where the sun reaches
                // (composite.frag). Floored at 0.3, because the AO also stands
                // in for the contact shadow the cascades are too coarse to cast.
                {
                    const auto lum = [](const glm::vec3& c) {
                        return glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f));
                    };
                    const float amb = lum(light.ambient);
                    const float sun = lum(light.color) *
                                      std::max(glm::normalize(light.direction).y, 0.2f);
                    pp.aoSunlitShare = glm::clamp(amb / std::max(amb + sun, 1e-4f),
                                                  0.3f, 1.0f);
                }
                pp.shadows     = renderer.shadowsEnabled() ? &renderer.shadows() : nullptr;
                pp.viewForward = vcam.front();
                pp.bloomThreshold = postLook.bloomThreshold; pp.bloomKnee = postLook.bloomKnee;
                pp.bloomIntensity = gate.bloomIntensity;
                pp.rayIntensity   = gate.rayIntensity;
                pp.dofNear = postLook.dofNear; pp.dofFar = postLook.dofFar; pp.dofMax = gate.dofMax;
                if (playMode && scriptFocusFar > 0.0f) {
                    pp.dofNear = scriptFocusNear;
                    pp.dofFar  = scriptFocusFar;
                }
                pp.exposure = postLook.exposure;
                pp.hueShift = postLook.hueShift; pp.saturation = postLook.saturation;
                pp.valueGain = postLook.valueGain; pp.warmth = postLook.warmth; pp.contrast = postLook.contrast;
                pp.split = postLook.gradeSplit; pp.vibrance = postLook.gradeVibrance;
                pp.curve        = postLook.tonemapCurve;
                pp.vignette     = postLook.vignette;
                pp.grain        = postLook.filmGrain;
                pp.frame        = taaFrame;
                pp.autoExposure = postLook.autoExposure;
                pp.autoMinEv    = postLook.autoMinEv;
                pp.autoMaxEv    = postLook.autoMaxEv;
                pp.adaptSpeed   = postLook.adaptSpeed;
                pp.dt           = dt;
                pp.blurStrength     = gate.blurStrength;
                pp.blurAnchor       = blurSt.blurAnchorWorld;
                pp.blurAnchorValid  = blurSt.blurAnchorValid;
                if (useTaa) {
                    // A cut -- the camera jumped, or this is the first frame --
                    // leaves a history that belongs to another picture; drop it
                    // rather than let the resolve drag it across the new one.
                    const bool cut = !taaHavePrev[vi] ||
                                     glm::distance(camPos, taaPrevEye[vi]) > 25.0f;
                    pp.taa      = true;
                    pp.curVP    = mainVPUnjittered;
                    pp.prevVP   = cut ? mainVPUnjittered : taaPrevVP[vi];
                    pp.jitterUV = taaJitter * 0.5f;
                    pp.taaReset = cut;
                    // Motion for what moved on its own, depth-tested against
                    // everything the HDR buffer now holds.
                    FZ_GPU_ZONE("GPU motion vectors");
                    post.beginMotion(hdrRT);
                    renderer.renderMotion(mainVP, pp.curVP, pp.prevVP);
                    // ...and the towns' traffic, re-skinned on the CPU every frame.
                    townTraffic.drawMotion(mainVP, pp.curVP, pp.prevVP);
                    // ...and the crowns in the wind (treemotion.vert).
                    if (shadeFull)
                        veg.drawTreeMotion(mainVP, pp.curVP, pp.prevVP, camPos);
                    // ...and where the pollen glitters, which follows nothing.
                    if (shadeFull && motesOn) motes.drawReactive(mainVP);
                }
                taaPrevVP[vi]   = mainVPUnjittered;
                taaPrevEye[vi]  = camPos;
                taaHavePrev[vi] = useTaa;
                FZ_GPU_ZONE("GPU post (bloom/blur)");
                post.run(hdrRT, pp, fsQuad);
            }

            // --- FXAA: filter the (motion-blurred) composite to the viewport
            //     texture (editor) or straight to the screen (presentation) -----
            if (presentMode) {
                int winW = 0, winH = 0;
                window.framebufferSize(winW, winH);
                RenderTarget::unbind(winW, winH);
            } else {
                viewportRT.bind();
            }
            // Clear only on the first pane: the second one must not wipe the
            // image the first just landed.
            if (vi == 0) glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            // This pane's half of the target. The passes above each bound their
            // own target and set the viewport with it, so this is the only place
            // that has to place anything by hand.
            {
                int dstW = fbW, dstH = fbH;
                if (presentMode) window.framebufferSize(dstW, dstH);
                const int pw = std::max(1, dstW / views);
                glViewport(vi * pw, 0, pw, dstH);
            }
            { FZ_GPU_ZONE("GPU composite");
              // TAA replaces FXAA and brings its sharpening along instead.
              // Upscaled from a reduced render scale, a touch more sharpening
              // puts back what the stretch takes (CAS: it lifts texture, not
              // the edges' aliasing).
              const bool upscaled = rw < paneW;
              const float sharp = useTaa ? postLook.taaSharpen : (upscaled ? 0.35f : 0.0f);
              post.present(fsQuad, postLook.fxaaEnabled && !useTaa,
                           upscaled ? std::min(1.0f, sharp + 0.25f) : sharp, upscaled); }
            } // per-pane loop
            fzGpuFrame.reset();
            ++taaFrame;   // the next jitter offset, once per frame whatever the panes

            // Back to the whole image. Everything after this -- the editor grid,
            // the HUD, ImGui -- addresses the full target, and would otherwise
            // be squeezed into whichever half was drawn last.
            {
                int fullW = fbW, fullH = fbH;
                if (presentMode) window.framebufferSize(fullW, fullH);
                glViewport(0, 0, fullW, fullH);
            }

            // Player one's view, for the overlays drawn on top of the finished
            // image (editor grid, HUD). They are still whole-image things, so
            // with two panes up they follow the left one; giving each pane its
            // own HUD is a later step, and the grid is an editor aid that will
            // not be looking at a split screen in the first place.
            const glm::vec3 camPos = camera.position();
            const glm::mat4 mainVP = proj * camera.viewMatrix();

            // --- The scene from another eye, finished -----------------------
            // One more pass over the scene that was already submitted: sky,
            // terrain, objects and trees, tonemapped into an LDR target that
            // never reaches the post chain. What the camera preview and the
            // camera textures draw (see the preview below for what is left out).
            auto drawViewLDR = [&](const glm::mat4& v, const glm::mat4& p, const camerasys::Pose& pv) {
                drawBackground(glm::inverse(p * v), pv.position, true);
                renderer.renderScene(v, p, pv.position, Renderer::kNoClip, true);
                const FrameContext pctx = makeFrameContext(p * v, pv.position, now, storm, light, fog);
                veg.drawTrees(pctx);
                // The billboards' right vector. Taken from the pose's own up
                // rather than from world up: a camera looking straight down --
                // which a trackside shot may well be -- has no right axis
                // against the sky, and normalizing that zero would turn every
                // distant tree into a NaN.
                glm::vec3 pRight = glm::cross(pv.front, pv.up);
                if (glm::length(pRight) < 1e-4f)
                    pRight = glm::cross(pv.front, glm::vec3(0.0f, 0.0f, 1.0f));
                if (glm::length(pRight) > 1e-4f)
                    veg.drawTreeBillboards(pctx, glm::normalize(pRight));
            };

            // --- What cameras see, on the surfaces that show it -------------
            // Monitors, security screens, mirrors (CameraTexture.hpp): every
            // camera a material names, at most 30 times a second each. Shown
            // by the surfaces from the next frame on, like everything drawn
            // after the main image.
            {
                FZ_ZONE("camera textures");
                FZ_GPU_ZONE("GPU camera textures");
                camTextures.update(materials, entities,
                                   [&](int id, camerasys::Pose& out) { return cams.pose(id, out); },
                                   camera.nearPlane(), camera.farPlane(), now, drawViewLDR);
            }

#ifndef FITZEL_PLAYER
            // --- What the selected camera sees ------------------------------
            // One more pass over the scene that was already submitted, from
            // somewhere else. Cheap for that reason: the draw list, the shadow
            // map and the environment are the frame's, and only the view changes
            // -- the same trick the water reflection plays.
            //
            // Sky, terrain, objects and trees. NOT grass, water or the post
            // chain: grass and water are streamed and composited around the MAIN
            // camera and would show that camera's neighbourhood from this one's
            // angle, which is worse than leaving them out. The corner says so.
            camPreviewId = -1;
            if (showCamPreview && !playMode && !presentMode && sel.valid()) {
                const Entity& se = entities[sel.index()];
                camerasys::Pose pv;
                if (se.components.get<CameraComponent>() && cams.pose(se.id, pv)) {
                    camPreviewId   = se.id;
                    camPreviewName = se.name;
                }
                // Redrawn at 20 Hz -- but AT ONCE when the selection moved to a
                // different camera: a corner that went on showing the last one
                // for another twentieth of a second reads as the wrong camera
                // being previewed, which is worse than a stutter in the corner.
                const bool camPreviewDue =
                    camPreviewId >= 0 &&
                    (camPreviewId != camPreviewLast || now >= camPreviewNext);
                if (camPreviewDue) {
                    camPreviewLast = camPreviewId;
                    camPreviewNext = now + 0.05;
                    const float pa = static_cast<float>(camPreviewRT.width()) /
                                     static_cast<float>(camPreviewRT.height());
                    const glm::mat4 pvProj =
                        glm::perspective(glm::radians(glm::clamp(pv.fov, 5.0f, 150.0f)),
                                         pa, camera.nearPlane(), camera.farPlane());
                    const glm::mat4 pvView =
                        glm::lookAt(pv.position, pv.position + pv.front, pv.up);
                    FZ_ZONE("camera preview");           // the draw calls (CPU)
                    FZ_GPU_ZONE("GPU camera preview");   // ...and the card's share
                    camPreviewRT.bind();
                    glClearColor(0.05f, 0.06f, 0.07f, 1.0f);
                    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    // Tonemapped in the shader: this target is LDR and never
                    // reaches the post chain, so it has to arrive finished.
                    drawViewLDR(pvView, pvProj, pv);
                    // ...and back to the image everything after this draws into.
                    viewportRT.bind();
                    int fullW = fbW, fullH = fbH;
                    glViewport(0, 0, fullW, fullH);
                }
            }
#endif

#ifndef FITZEL_PLAYER
            // --- The construction grid, onto the finished image ---------------
            // Last of all, and deliberately so: it is a drawing aid, not part of
            // the world, so nothing that happens to the world may happen to it.
            // Here it is past tonemapping, exposure, bloom, SSAO, motion blur and
            // FXAA -- its colour is the colour authored, at dawn as at midnight.
            // It is still hidden by whatever stands in front of it: the shader
            // reads the scene's depth (bound here as a texture, since the buffer
            // itself is long unbound) and drops the fragments behind it.
            //
            // Editor only, three times over: compiled out of the player, skipped
            // in Play, and skipped in presentation mode -- which draws the game
            // straight to the screen and has no viewport image to draw onto.
            if (showGrid && !playMode && !presentMode) {
                grid.cell   = cursor.grid;   // what you see is what you snap to
                grid.plane  = cursor.pos.y;   // ...on the plane the cursor is on
                grid.cursor = cursor.pos;
                // Never fade beyond what the camera can see: the grid's quad ends
                // at its fade distance, so a fade further out than the far plane
                // would be sliced off mid-strength by the clip instead of easing
                // away. View distance is the knob for seeing further.
                grid.fade   = std::min(gridFade, camera.farPlane() * 0.7f);
                grid.highlightCursorCell = cursor.visible;
                grid.viewportPx    = glm::vec2(fbW, fbH);
                grid.sceneDepthUnit = 0;
                hdrRT.bindDepthTexture(0);
                grid.draw(makeFrameContext(mainVP, camPos, now, storm, light, fog));
            }
#endif

            // Editor: return to the window framebuffer and clear a dark backdrop
            // for the dock panels. Presentation mode already drew to the screen.
            if (!presentMode) {
                int winW = 0, winH = 0;
                window.framebufferSize(winW, winH);
                RenderTarget::unbind(winW, winH);
                glClearColor(0.07f, 0.07f, 0.08f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            }

            // --- Play HUD: crosshair, score, and the script's HUD line -------
            if (playMode) {
                ImDrawList* dl = ImGui::GetForegroundDrawList();
                // The HUD is anchored to the rendered viewport image, which is the
                // whole window in presentation mode but an inset dock panel in the
                // editor. Anchoring here keeps the crosshair on the aim point and
                // the text inside the view (not off in a side panel).
                ImVec2 vmin, vsize;
                if (presentMode || viewportRectSize.x < 1.0f) {
                    vmin  = ImVec2(0.0f, 0.0f);
                    vsize = ImGui::GetIO().DisplaySize;
                } else {
                    vmin  = ImVec2(viewportRectMin.x, viewportRectMin.y);
                    vsize = ImVec2(viewportRectSize.x, viewportRectSize.y);
                }
                const ImVec2 c(vmin.x + vsize.x * 0.5f, vmin.y + vsize.y * 0.5f);

                // Water wash: a blue-green tint over the view when the car is in the
                // water, deepening to a full underwater tint if the chase camera
                // itself dips below the surface. Sells the plunge optically.
                {
                    float waterFx = carWaterSub * 0.4f;
                    const float camDepth = waterLevel - camera.position().y;
                    if (camDepth > 0.0f)
                        waterFx = glm::max(waterFx, glm::clamp(camDepth * 0.6f, 0.0f, 0.72f));
                    if (waterFx > 0.003f) {
                        const int a = static_cast<int>(waterFx * 255.0f);
                        dl->AddRectFilled(vmin, ImVec2(vmin.x + vsize.x, vmin.y + vsize.y),
                                          IM_COL32(18, 74, 92, a));
                    }
                }

                // Crosshair, sized to the view. Hidden when disabled, and always
                // hidden while driving (you aim on foot, not from the car).
                if (showCrosshair && host.crosshair && !vehicleMode && !gliderMode &&
                    !showroomUi.active()) {
                    const float ch = std::max(10.0f, vsize.y * 0.018f);
                    const ImU32 white = IM_COL32(255, 255, 255, 220);
                    dl->AddLine(ImVec2(c.x - ch, c.y), ImVec2(c.x + ch, c.y), white, 2.0f);
                    dl->AddLine(ImVec2(c.x, c.y - ch), ImVec2(c.x, c.y + ch), white, 2.0f);
                }

                // Script HUD line, scaled to the view height and drawn with a dark
                // shadow so it stays legible over any scene. (The old fixed "Score:"
                // readout was a game leftover -- a script that wants to show a score
                // does it via game.setHud.)
                ImFont* font = ImGui::GetFont();
                const float fs  = glm::clamp(vsize.y * 0.04f, 22.0f, 48.0f);
                const float pad = fs * 0.6f;
                auto shadowText = [&](float x, float y, ImU32 col, const char* s){
                    dl->AddText(font, fs, ImVec2(x + 2.0f, y + 2.0f),
                                IM_COL32(0, 0, 0, 190), s);
                    dl->AddText(font, fs, ImVec2(x, y), col, s);
                };
                if (!host.hud.empty())
                    shadowText(vmin.x + pad, vmin.y + pad,
                               IM_COL32(235, 235, 240, 235), host.hud.c_str());
                // What the scripts drew this frame (game.hudRect & co.), from
                // their 1080-high canvas onto the view. Clipped to the view so a
                // HUD laid out to the edge stays out of the editor's panels.
                if (!host.hudCmds.empty()) {
                    const float k = vsize.y / 1080.0f;
                    auto P = [&](float x, float y) {
                        return ImVec2(vmin.x + x * k, vmin.y + y * k);
                    };
                    dl->PushClipRect(vmin, ImVec2(vmin.x + vsize.x, vmin.y + vsize.y), true);
                    for (const ScriptHudCmd& hc : host.hudCmds) {
                        const float* a = hc.a;
                        switch (hc.kind) {
                        case ScriptHudCmd::Kind::Rect:
                            dl->AddRectFilled(P(a[0], a[1]), P(a[0] + a[2], a[1] + a[3]),
                                              hc.col, a[4] * k);
                            break;
                        case ScriptHudCmd::Kind::Gradient:
                            dl->AddRectFilledMultiColor(P(a[0], a[1]),
                                                        P(a[0] + a[2], a[1] + a[3]),
                                                        hc.col, hc.col, hc.col2, hc.col2);
                            break;
                        case ScriptHudCmd::Kind::Frame:
                            dl->AddRect(P(a[0], a[1]), P(a[0] + a[2], a[1] + a[3]),
                                        hc.col, a[4] * k, 0, std::max(1.0f, hc.size * k));
                            break;
                        case ScriptHudCmd::Kind::Line:
                            dl->AddLine(P(a[0], a[1]), P(a[2], a[3]), hc.col,
                                        std::max(1.0f, hc.size * k));
                            break;
                        case ScriptHudCmd::Kind::Circle:
                            dl->AddCircleFilled(P(a[0], a[1]), a[2] * k, hc.col);
                            break;
                        case ScriptHudCmd::Kind::Ring:
                            dl->AddCircle(P(a[0], a[1]), a[2] * k, hc.col, 0,
                                          std::max(1.0f, hc.size * k));
                            break;
                        case ScriptHudCmd::Kind::Tri:
                            dl->AddTriangleFilled(P(a[0], a[1]), P(a[2], a[3]),
                                                  P(a[4], a[5]), hc.col);
                            break;
                        case ScriptHudCmd::Kind::Image:
                            // The host keeps the texture alive for the session
                            // (ScriptBridge), so the name is still good here.
                            dl->AddImage(static_cast<ImTextureID>(
                                             static_cast<std::intptr_t>(hc.tex)),
                                         P(a[0], a[1]), P(a[0] + a[2], a[1] + a[3]),
                                         ImVec2(hc.uv[0], hc.uv[1]),
                                         ImVec2(hc.uv[2], hc.uv[3]), hc.col);
                            break;
                        case ScriptHudCmd::Kind::Text: {
                            ImFont* f = (hc.bold && ui::boldFont()) ? ui::boldFont() : font;
                            const float px = hc.size * k;
                            const ImVec2 sz = f->CalcTextSizeA(px, FLT_MAX, 0.0f,
                                                               hc.text.c_str());
                            const ImVec2 at(P(a[0], a[1]).x - sz.x * hc.align,
                                            P(a[0], a[1]).y);
                            // The drop shadow keeps its strength with the text's own
                            // alpha, so a fading line fades as one piece.
                            const float off = std::max(1.0f, px * 0.06f);
                            const unsigned alpha = (hc.col >> 24) & 0xFF;
                            dl->AddText(f, px, ImVec2(at.x + off, at.y + off),
                                        IM_COL32(0, 0, 0, alpha * 3 / 4), hc.text.c_str());
                            dl->AddText(f, px, at, hc.col, hc.text.c_str());
                            break;
                        }
                        }
                    }
                    dl->PopClipRect();
                }
                // Boat-mode banner while afloat: centred near the top of the view.
                if (vehicleMode && boatMode) {
                    const char* bm = "~ BOAT MODE ~";
                    const ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, bm);
                    shadowText(c.x - sz.x * 0.5f, vmin.y + pad,
                               IM_COL32(130, 210, 255, 255), bm);
                }
                // The racing HUD (speed, lap + times, the field, countdown, final
                // classification) lives in RaceHud.cpp and reads the race state
                // directly. `topInset` keeps it clear of the script's HUD line.
                if (gliderMode) {
                    // Each player's instruments go in that player's pane. The HUD
                    // was already written to take a rect and one RaceState, so a
                    // second one is a second call -- what it must NOT be is one
                    // HUD across the whole image, which is player one's lap and
                    // player one's energy stretched over a view that is half
                    // somebody else's.
                    const ImVec2 hmin  = vmin;
                    const ImVec2 hsize = haveView2
                        ? ImVec2(vsize.x * 0.5f, vsize.y) : vsize;
                    // The weapon's own HUD first: the reticle sits on the world,
                    // so the race panels (which own the screen corners) draw over
                    // it rather than under it. Weapons belong to player one for
                    // now, so it stays in player one's pane.
                    weapons.drawHud(dl, hmin, hsize, mainVP);
                    const racehud::EndAction act =
                        racehud::draw(dl, hmin, hsize, race,
                                      host.hud.empty() ? 0.0f : fs * 1.5f,
                                      endPrompt, endIn);
                    // Whose view this is. The instruments below still read YOUR
                    // race -- your lap, your energy -- so a view that quietly
                    // showed somebody else's craft with your dials around it
                    // would be a lie. It says the name and it says the key.
                    if (spectateId >= 0) {
                        const Entity* w = document.find(spectateId);
                        char wb[96];
                        std::snprintf(wb, sizeof(wb), "Watching %s  -  V",
                                      (w && !w->name.empty()) ? w->name.c_str()
                                                              : "rival");
                        const ImVec2 ts = ImGui::CalcTextSize(wb);
                        const ImVec2 at(hmin.x + (hsize.x - ts.x) * 0.5f,
                                        hmin.y + hsize.y - ts.y - fs * 2.0f);
                        dl->AddRectFilled(ImVec2(at.x - fs * 0.5f, at.y - fs * 0.25f),
                                          ImVec2(at.x + ts.x + fs * 0.5f,
                                                 at.y + ts.y + fs * 0.25f),
                                          IM_COL32(0, 0, 0, 110), fs * 0.25f);
                        dl->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f),
                                    IM_COL32(0, 0, 0, 200), wb);
                        dl->AddText(at, IM_COL32(255, 225, 140, 240), wb);
                    }
                    // Player two's instruments, in the right-hand pane, from its
                    // own race state. It gets no menu input: the end-of-race
                    // question is one decision about one scene, and two people
                    // answering it from two panes is a race condition with a
                    // steering wheel. Player one answers; this only reports.
                    if (haveView2) {
                        const ImVec2 h2min(vmin.x + vsize.x * 0.5f, vmin.y);
                        // Player two's reticle and lock brackets belong to its
                        // own launcher and its own view -- the world markers
                        // billboard toward camera2, so they are drawn with that
                        // pane's view-projection.
                        weapons2.drawHud(dl, h2min, hsize,
                                         camera2.projectionMatrix(
                                             hsize.x / glm::max(hsize.y, 1.0f)) *
                                         camera2.viewMatrix());
                        racehud::draw(dl, h2min, hsize, race2, 0.0f,
                                      endPrompt2, racehud::EndInput{});
                    }
                    // Both answers are deferred to the top of the next frame --
                    // they tear down and rebuild the scene, which must not happen
                    // underneath the draw call that asked the question.
                    if (act == racehud::EndAction::RaceAgain)         pendingRestart = true;
                    else if (act == racehud::EndAction::StartScreen)  pendingStartScreen = true;
                }

                // Scene 2D UI overlay: authored text/buttons/images, drawn last so
                // it sits above the rest of the HUD. Buttons fire their data-authored
                // action through this sink (so this stays free of the overlay code).
                if (!uiOverlay.empty()) {
                    UiActionSink sink;
                    sink.loadScene   = [&](const std::string& s){ pendingSceneLoad = s; };
                    sink.showMessage = [&](const std::string& s){ host.hud = s; };
                    sink.addScore    = [&](float n){ host.score += static_cast<int>(n); };
                    sink.playSound   = [&](const std::string& s){ if (host.playSound) host.playSound(s); };
                    sink.quit        = [&](){ window.requestClose(); };
                    // Resume: close the menu and give the cursor back to the game,
                    // exactly like pressing the menu's key again.
                    sink.resume      = [&](){
                        uiOverlay.setRuntimeVisible(false);
                        input.setCursorLocked(fpsMode && !scriptCursorFree);
                    };
                    // Restart: deferred, so the entity list is swapped between
                    // frames rather than underneath this draw call.
                    sink.restart     = [&](){ pendingRestart = true; };
                    // Deferred by a frame like every other menu action: the
                    // overlay is mid-draw here, and opening a screen that covers
                    // it from inside its own button handler is the one way to
                    // have two menus believe they own the cursor.
                    sink.graphics    = [&](){ gfxOpenRequest = true; };
                    // Keyboard / gamepad activation, fired here because this is
                    // where the sink exists. Consumed either way, so a press
                    // can't queue up for the next frame.
                    if (uiActivate) {
                        const bool ok = uiOverlay.activateFocus(sink);
                        std::fprintf(stderr, "[navdbg] activate -> %d\n", ok ? 1 : 0);
                        uiActivate = false;
                    }
                    uiOverlay.drawRuntime(dl, glm::vec2(vmin.x, vmin.y),
                                          glm::vec2(vsize.x, vsize.y), assetDb, sink);
                }

                // --- The showroom, drawn last: it is the whole screen --------
                // `mainVP` lets it project the podium, so the stage effects sit
                // on the craft rather than on a guessed point.
                if (showroomUi.active()) {
                    const showroom::Launch go =
                        showroomUi.draw(dl, vmin, vsize, assetDb, mainVP);
                    if (go.back) {
                        // Same exit as Esc: quit the game, or drop the editor
                        // out of Play.
                        if (playerMode) window.requestClose();
                        else            stopPlay();
                    } else if (go.start && !go.scene.empty()) {
                        // Hand the scene and the camera back first: the craft
                        // must be captured in its authored pose (not floating
                        // over the podium), and the race must not inherit the
                        // showroom's lens.
                        showroomUi.end(entities, camera);
                        resolveHierarchy();
                        pendingCraftJson = nlohmann::json();
                        pendingCraftName = go.craftName;
                        pendingCraftName2 = go.craftName2;
                        pendingCraftLaps = go.laps;
                        pendingRaceMode    = go.mode;
                        pendingRaceField   = go.opponents;
                        // The step chosen on the start screen is also the one
                        // the player keeps: the SKILL row is the profile editor,
                        // so leaving the screen is what commits it.
                        pendingRaceLevel   = go.difficulty;
                        if (gameDifficulty.level != go.difficulty) {
                            gameDifficulty.level = go.difficulty;
                            difficulty::save(kDifficultyFile, gameDifficulty);
                        }
                        pendingCraftJson2 = nlohmann::json();
                        const auto modelGuidOf = [&](int mid) -> std::string {
                            LoadedModel* lm = models.byId(mid);
                            return (lm && lm->assetId.valid())
                                       ? lm->assetId.toString() : std::string();
                        };
                        // Both seats' picks travel the same way. They may well be
                        // the same craft twice -- two of the same machine is a
                        // legitimate race -- so each is captured on its own.
                        const auto capture = [&](int id, const std::string& name,
                                                 nlohmann::json& out) {
                            if (id < 0) return;
                            auto p = prefab::fromSubtree(entities, id, name);
                            if (!p) return;
                            // A craft that was CHOSEN is in the race, whatever
                            // flag the start screen's scene keeps it under. A
                            // showroom parks the machines it is not showing by
                            // deactivating them, and some are saved that way --
                            // end() has just faithfully put that flag back a few
                            // lines above, exactly as it should. Carried into the
                            // circuit unchanged it spawns a craft that is never
                            // drawn, whose camera child is dead with it, and the
                            // race opens on an empty track with the player
                            // nowhere: the same picture as the start screen being
                            // ignored outright. Only the root -- a child that is
                            // authored off (an effect, a spare part) stays off.
                            if (!p->entities.empty()) p->entities.front().active = true;
                            nlohmann::json ents = nlohmann::json::array();
                            for (const Entity& pe : p->entities)
                                ents.push_back(projectio::writeEntityJson(pe, modelGuidOf));
                            out = std::move(ents);
                        };
                        capture(go.craftId,  go.craftName,  pendingCraftJson);
                        capture(go.craftId2, go.craftName2, pendingCraftJson2);
                        pendingFromShowroom = true;   // this one gets the orbit
                        // Two seats chosen on the start screen IS the split: the
                        // pane count follows from someone being in the second one.
                        splitScreen = go.craftId2 >= 0;
                        // Deferred, like every other scene change: the entity
                        // list is swapped between frames, never underneath the
                        // draw call that asked for it.
                        pendingSceneLoad = go.scene;
                    }
                }
            }

            // --- Graphics menu, over everything ------------------------------
            // Outside the HUD block above on purpose: that one only runs in Play,
            // and a screen for fitting the game to the machine has to be reachable
            // wherever the frame rate went wrong -- including the editor.
            //
            // It is a scrim over the running picture rather than a screen of its
            // own because every row changes what is behind it, and a setting
            // nobody can see move is a setting nobody can judge.
            if (gfxUi.open()) {
                ImDrawList* gdl = ImGui::GetForegroundDrawList();
                ImVec2 gmin, gsize;
                if (presentMode || viewportRectSize.x < 1.0f) {
                    gmin  = ImVec2(0.0f, 0.0f);
                    gsize = ImGui::GetIO().DisplaySize;
                } else {   // docked editor: the menu belongs to the viewport pane
                    gmin  = ImVec2(viewportRectMin.x, viewportRectMin.y);
                    gsize = ImVec2(viewportRectSize.x, viewportRectSize.y);
                }
                bool gfxChanged = false;
                const gfxmenu::Settings before = gfxSet;
                const bool stay =
                    gfxUi.draw(gdl, gmin, gsize, gfxSet, gfxIn, &gfxChanged);
                if (gfxChanged) applyGfx(before);
                if (!stay) {
                    // Saved on the way out rather than per keystroke: the file
                    // records what was settled on, and a row stepped through five
                    // values is one decision, not five.
                    gfxmenu::save("graphics.json", gfxSet);
                    input.setCursorLocked(fpsMode && !scriptCursorFree);
                }
            }

            // UI comfort settings changed this frame: write them once, after the
            // widget is released, rather than on every slider tick.
            if (prefsDirty && !ImGui::IsAnyItemActive()) {
                uiFontSize   = gui.fontSize();
                uiFontFamily = gui.fontFamilyName(gui.fontFamily());
                projectio::savePrefs(pio);
                prefsDirty = false;
            }

            // --- Idle throttle: decide whether the NEXT frame runs full-rate ---
            // Anything that needs continuous redraws counts as activity: mouse
            // movement/wheel/buttons, an active ImGui widget or text field, a
            // gizmo drag, an in-progress vehicle drive, or a held camera/tool key
            // (held keys emit no repeat events, so poll them explicitly). A short
            // grace after the last activity keeps easing/hover smooth.
            const ImGuiIO& io = ImGui::GetIO();
            const bool mouseActive =
                io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f ||
                io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f ||
                io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2];
            const bool keyHeld =
                input.isKeyDown(GLFW_KEY_W) || input.isKeyDown(GLFW_KEY_A) ||
                input.isKeyDown(GLFW_KEY_S) || input.isKeyDown(GLFW_KEY_D) ||
                input.isKeyDown(GLFW_KEY_Q) || input.isKeyDown(GLFW_KEY_E) ||
                input.isKeyDown(GLFW_KEY_R) || input.isKeyDown(GLFW_KEY_F) ||
                input.isKeyDown(GLFW_KEY_SPACE) ||
                input.isKeyDown(GLFW_KEY_LEFT_SHIFT) ||
                input.isKeyDown(GLFW_KEY_LEFT_CONTROL) ||
                input.isKeyDown(GLFW_KEY_UP) || input.isKeyDown(GLFW_KEY_DOWN) ||
                input.isKeyDown(GLFW_KEY_LEFT) || input.isKeyDown(GLFW_KEY_RIGHT);
            const bool interacting =
                mouseActive || keyHeld || io.WantTextInput || camAnimating ||
                ImGui::IsAnyItemActive() || ImGuizmo::IsUsing() || vehicleMode || gliderMode;
            if (interacting) lastActive = now;
            activeFrame = (now - lastActive) < kIdleGrace;

#ifndef FITZEL_PLAYER
            // Whatever the Assets panel didn't claim was dropped somewhere else (or
            // while it was closed). Drop it on the floor rather than let it queue up
            // and ride along with the next drop, which would import the wrong files
            // at the wrong moment.
            g_fileDrop.paths.clear();
#endif
            prof::addSince("scene (GPU submit)", fzSceneMark);

            {   // Dear ImGui's own draw: building its vertex buffers and issuing
                // the UI draw calls, separate from the panels' logic above.
                FZ_ZONE("imgui draw");
                gui.endFrame();
            }
            // The draw list that named it has been issued: the target the
            // viewport was resized away from can go now.
            retiredRT.reset();
            {   // The buffer swap. With vsync on, this is where the wait for the
                // display lands -- so it is normally the largest number here and
                // that is healthy. What matters is whether it *spikes* while
                // every other section stays flat: that means the stall is on the
                // GPU or in the driver, not in our frame.
                FZ_ZONE("present (swap)");
                window.swapBuffers();
            }
            // Whatever the GPU finished while we were busy: read the timestamps
            // back now, two frames after they were issued, and post them to the
            // profiler beside the CPU zones (see GpuTimer.hpp).
            gputime::collect();

#ifndef FITZEL_PLAYER
            if (playMode && shotRunner.active()) {
                int sw = 0, sh = 0;
                window.framebufferSize(sw, sh);
                if (shotRunner.afterFrame(window.time(), sw, sh))
                    window.requestClose();
            }
#endif

            // --- Benchmark mode (--profile) ----------------------------------
            // Measure for a few seconds, write the breakdown, quit. The window
            // starts on the first frame AFTER the project is up and the profiler
            // has been reset: a scene load leaves a half-second frame in the
            // history that would otherwise be the "worst" number for the whole
            // run and drag the average with it.
            if (!boot.profilePath.empty()) {
                if (profileStart <= 0.0) {
                    profileStart = window.time();
                    prof::reset();
                } else if (window.time() - profileStart > boot.profileSeconds) {
                    writeProfileReport();
#ifndef FITZEL_PLAYER
                    if (!boot.profileShot.empty()) {
                        // After the swap: the front buffer holds the finished
                        // frame, post chain and all, which is what a comparison
                        // wants to look at. Flipped, because GL counts rows from
                        // the bottom and every image format here does not.
                        int sw = 0, sh = 0;
                        window.framebufferSize(sw, sh);
                        std::vector<unsigned char> px(
                            static_cast<std::size_t>(sw) * sh * 4);
                        glReadBuffer(GL_FRONT);
                        glPixelStorei(GL_PACK_ALIGNMENT, 1);
                        glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE,
                                     px.data());
                        std::vector<unsigned char> flipped(px.size());
                        const std::size_t row = static_cast<std::size_t>(sw) * 4;
                        for (int y = 0; y < sh; ++y)
                            std::memcpy(&flipped[static_cast<std::size_t>(y) * row],
                                        &px[static_cast<std::size_t>(sh - 1 - y) * row],
                                        row);
                        stbi_write_png(boot.profileShot.c_str(), sw, sh, 4,
                                       flipped.data(), static_cast<int>(row));
                    }
#endif
                    window.requestClose();
                }
            }
        };

#ifdef __EMSCRIPTEN__
        // In the browser the page owns the loop: a frame is a callback from its
        // animation timer, and code that never returns to it freezes the tab.
        // So here the loop SUSPENDS after each frame: this function is a
        // coroutine on the web (see AppMain below), every local above lives in
        // its heap frame, and main() resumes it once per browser frame.
        while (window.isOpen()) {
            frame();
            co_await std::suspend_always{};
        }
#else
        while (window.isOpen()) frame();
#endif

#ifndef FITZEL_PLAYER
        // Out of the loop under our own power. That is the whole definition of a
        // clean shutdown here, so this session's snapshot goes: the next start
        // finding one is what tells it the session before ended badly, and one
        // left after a normal quit would make that signal lie.
        //
        // Unless the offer on screen was never answered. Closing the editor is not
        // a decision about work from a session that crashed -- and it is exactly
        // the thing someone does by reflex when a dialog they did not expect comes
        // up. Then the snapshot stays, and is offered again next time.
        if (!pendingSnapshot.valid()) autoSave.clear();
#endif

    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        FZ_MAIN_RETURN 1;
    }

    FZ_MAIN_RETURN 0;
}

int main(int argc, char** argv) {
#ifdef __EMSCRIPTEN__
    // Runs the start-up and the first frame, then suspends. Static: the task
    // and the coroutine frame it owns live as long as the page. main() then
    // returns normally -- the runtime stays up (EXIT_RUNTIME=0) and the browser
    // calls the loop below once per frame, which resumes the program for one.
    static AppMain app = appMain(argc, argv);
    if (app.handle.done()) return app.handle.promise().result;
    emscripten_set_main_loop_arg(
        [](void*) {
            if (app.handle.done()) {
                emscripten_cancel_main_loop();
                return;
            }
            app.handle.resume();
            // Frames shown so far, for the page (and for measuring: the
            // engine's own profiler sees only the CPU half of a browser frame).
            EM_ASM({ Module.fzFrames = (Module.fzFrames | 0) + 1; });
        },
        nullptr, 0, false);
    // `?unthrottled` on the page's address: frames on the event loop instead
    // of the display's refresh -- they keep coming in a hidden tab, which is
    // what a test drives the game from.
    if (EM_ASM_INT({ return /[?&]unthrottled/.test(location.search) ? 1 : 0; }))
        emscripten_set_main_loop_timing(EM_TIMING_SETIMMEDIATE, 0);
    return 0;
#else
    return appMain(argc, argv);
#endif
}
