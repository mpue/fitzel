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
#include <sstream>
#include <future>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
#include "VideoLibrary.hpp"
#include "GpuTimer.hpp"
#include "Profiler.hpp"
#include "DebugOverlay.hpp"
#include "SandboxMath.hpp"
#include "CameraPath.hpp"
#include "ScriptSystem.hpp"
#include "ScriptBridge.hpp"
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
#include "HierarchyPanel.hpp"
#include "InspectorPanel.hpp"
#include "MaterialsPanel.hpp"
#include "MeshPaintPanel.hpp"
#include "ModelsPanel.hpp"
#include "PrefabsPanel.hpp"
#ifndef FITZEL_PLAYER
#include "Autosave.hpp"
#include "GridRenderer.hpp"
#include "ModelingPanel.hpp"
#include "UvPanel.hpp"
#include "ViewportNav.hpp"
#include "PathTracePanel.hpp"
#include "ViewportTrace.hpp"
#endif
#include "SpraySystem.hpp"
#include "ParticleSystem.hpp"
#include "TerrainPanel.hpp"
#include "FolderDialog.hpp"
#include "GameSettingsPanel.hpp"
#include "LoadingScreen.hpp"
#include "LightGrid.hpp"
#include "VegetationSystem.hpp"
#include "RoadSet.hpp"
#include "RoadSystem.hpp"
#include "SplineSystem.hpp"
#include "RiverSystem.hpp"
#include "RaceSim.hpp"
#include "RaceGrid.hpp"
#include "CameraSystem.hpp"
#include "PostChain.hpp"
#include "VolumetricFog.hpp"
#include "RaceHud.hpp"
#include "Showroom.hpp"
#include "Difficulty.hpp"
#include "Leaderboard.hpp"
#include "GraphicsMenu.hpp"
#include "RoadPanel.hpp"
#include "RoadPrefab.hpp"
#include "SplinePanel.hpp"
#include "SplineEdit.hpp"
#include "RiverPanel.hpp"
#include "RiverEdit.hpp"
#include "SkidSystem.hpp"
#include "SoftBodySystem.hpp"
#include "TrailSystem.hpp"
#include "WeaponSystem.hpp"
#include "WorldAudio.hpp"
#include "ScatterTool.hpp"
#include "BuildingGen.hpp"
#include "BuildingPanel.hpp"
#include "CityPanel.hpp"
#include "VehicleGizmo.hpp"
#include "VehicleTool.hpp"
#include "GliderTool.hpp"
#include "CarAudio.hpp"
#include "GliderAudio.hpp"
#include "UiOverlay.hpp"
#include "UiOverlayCommand.hpp"
#include "UiStyle.hpp"

using namespace fitzel;

// On laptops with hybrid graphics (NVIDIA Optimus / AMD PowerXpress), ask the
// driver to run us on the discrete high-performance GPU instead of the iGPU.
#if defined(_WIN32)
extern "C" {
    __declspec(dllexport) unsigned long NvOptimusEnablement = 1;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
// AttachConsole, for the windowed Release build (see main()). GLFW has already
// defined APIENTRY by this point and windows.h defines it again, identically --
// drop it first so the duplicate doesn't warn.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#undef APIENTRY
#include <windows.h>
#endif

#ifndef FITZEL_PLAYER
namespace {

// --- Thumbnail disk cache --------------------------------------------------
// Decoding a 4K texture (or a huge EXR) down to a 128px preview is expensive and
// hammers the disk -- opening the Assets browser would otherwise re-read every
// source texture in full. We cache each decoded preview to a tiny file keyed by
// the asset GUID and tagged with the source's last-write time (so edits
// invalidate it); the thumbnail worker loads these instead of re-decoding.
std::filesystem::path thumbCacheDir() {
    std::error_code ec;
    std::filesystem::path d =
        std::filesystem::temp_directory_path(ec) / "fitzel_thumbs";
    std::filesystem::create_directories(d, ec);
    return d;
}

long long sourceMtime(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    return ec ? 0 : static_cast<long long>(t.time_since_epoch().count());
}

// Cache file layout: magic 'FTH1' | int64 srcMtime | int32 w,h,ch | raw pixels.
bool loadThumbCache(const std::filesystem::path& file, long long srcMtime,
                    fitzel::ImagePixels& out) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    char magic[4] = {};
    f.read(magic, 4);
    if (!f || magic[0] != 'F' || magic[1] != 'T' ||
        magic[2] != 'H' || magic[3] != '1') return false;
    long long mt = 0; int w = 0, h = 0, ch = 0;
    f.read(reinterpret_cast<char*>(&mt), sizeof mt);
    f.read(reinterpret_cast<char*>(&w),  sizeof w);
    f.read(reinterpret_cast<char*>(&h),  sizeof h);
    f.read(reinterpret_cast<char*>(&ch), sizeof ch);
    if (!f || mt != srcMtime ||
        w < 0 || h < 0 || ch < 0 || w > 4096 || h > 4096 || ch > 4) return false;
    const std::size_t n = static_cast<std::size_t>(w) * h * ch;
    if (n == 0) { out = {}; return true; } // negative cache: source has no usable preview
    out.pixels.resize(n);
    f.read(reinterpret_cast<char*>(out.pixels.data()),
           static_cast<std::streamsize>(n));
    if (!f) { out.pixels.clear(); return false; }
    out.width = w; out.height = h; out.channels = ch;
    return true;
}

// Always writes -- an invalid image is stored as a zero-size "negative" entry so a
// source that can't produce a preview is not re-decoded on every session.
void saveThumbCache(const std::filesystem::path& file, long long srcMtime,
                    const fitzel::ImagePixels& img) {
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f.write("FTH1", 4);
    f.write(reinterpret_cast<const char*>(&srcMtime), sizeof srcMtime);
    const int w = img.width, h = img.height, ch = img.channels;
    f.write(reinterpret_cast<const char*>(&w),  sizeof w);
    f.write(reinterpret_cast<const char*>(&h),  sizeof h);
    f.write(reinterpret_cast<const char*>(&ch), sizeof ch);
    f.write(reinterpret_cast<const char*>(img.pixels.data()),
            static_cast<std::streamsize>(img.pixels.size()));
}

// One entry in the Lua editor's code-completion list: the identifier to insert
// plus a short signature/description shown greyed after it.
struct Completion { const char* text; const char* hint; };

// Top-level identifiers: Lua keywords + the stdlib bits scripts use + the script
// lifecycle functions and the `e` entity fields. Offered when the word being
// typed is not a `game.` member.
const Completion kTopLevel[] = {
    {"function", "def"}, {"local", "scope"}, {"return", ""}, {"end", ""},
    {"then", ""}, {"else", ""}, {"elseif", ""}, {"for", ""}, {"while", ""},
    {"repeat", ""}, {"until", ""}, {"break", ""}, {"true", ""}, {"false", ""},
    {"nil", ""}, {"and", ""}, {"or", ""}, {"not", ""}, {"in", ""},
    {"start", "start(e)  -- called once on spawn"},
    {"update", "update(e, dt, t)  -- called each frame"},
    {"game", "engine API table"},
    {"print", "print(...)"}, {"pairs", "pairs(t)"}, {"ipairs", "ipairs(t)"},
    {"tostring", "tostring(v)"}, {"tonumber", "tonumber(v)"}, {"type", "type(v)"},
    {"math", "math.*"}, {"string", "string.*"}, {"table", "table.*"},
};

// Members of the `game` table (functions + constants), offered after "game.".
// Signatures mirror ScriptSystem.cpp's C bindings.
const Completion kGameMembers[] = {
    {"keyDown", "keyDown(KEY) -> bool  (held)"},
    {"keyPressed", "keyPressed(KEY) -> bool  (this frame)"},
    {"mouseDown", "mouseDown(btn) -> bool"},
    {"mousePressed", "mousePressed(btn) -> bool"},
    {"cameraPos", "cameraPos() -> x, y, z"},
    {"cameraDir", "cameraDir() -> x, y, z"},
    {"spawn", "spawn{type=,x=,y=,z=,...} -> id"},
    {"destroy", "destroy(id)"},
    {"getPos", "getPos(id) -> x, y, z"},
    {"setPos", "setPos(id, x, y, z)"},
    {"setVelocity", "setVelocity(id, x, y, z)"},
    {"applyImpulse", "applyImpulse(id, x, y, z)"},
    {"playSound", "playSound(name)"},
    {"addScore", "addScore(n)"}, {"getScore", "getScore() -> n"},
    {"setHud", "setHud(text)"},
    {"BOX", "type 0"}, {"RAMP", "type 1"}, {"CYLINDER", "type 2"}, {"SPHERE", "type 3"},
    {"MOUSE_LEFT", "0"}, {"MOUSE_RIGHT", "1"}, {"MOUSE_MIDDLE", "2"},
    {"KEY_SPACE", "32"}, {"KEY_ENTER", "257"}, {"KEY_ESCAPE", "256"},
    {"KEY_LSHIFT", "340"}, {"KEY_LCTRL", "341"},
    {"KEY_LEFT", "263"}, {"KEY_RIGHT", "262"}, {"KEY_UP", "265"}, {"KEY_DOWN", "264"},
    {"KEY_W", "87"}, {"KEY_A", "65"}, {"KEY_S", "83"}, {"KEY_D", "68"},
};

// New-script templates, offered in the "New Script" dialog. An "empty component"
// is just the two lifecycle stubs; the documented one lists the entity fields
// and the game API as a starting reference.
const char* kTemplateEmpty =
    "-- %s : entity component (runs in Play)\n\n"
    "function start(e)\n"
    "end\n\n"
    "function update(e, dt, t)\n"
    "end\n";

const char* kTemplateDocumented =
    "-- %s : entity component (runs in Play)\n"
    "-- e fields: x/y/z pos, rx/ry/rz rot(deg), sx/sy/sz half-size, name, id\n"
    "--           (mutate them to move/rotate/scale this entity)\n"
    "-- API: game.keyDown/keyPressed(KEY_*), game.mouseDown/mousePressed(MOUSE_*),\n"
    "--      game.spawn{...}, game.destroy(id), game.setPos/getPos(id,...),\n"
    "--      game.setVelocity/applyImpulse(id,...), game.playSound(name),\n"
    "--      game.addScore(n)/getScore(), game.setHud(text), game.cameraPos/Dir()\n\n"
    "function start(e)\n"
    "    -- called once when the entity enters Play\n"
    "end\n\n"
    "function update(e, dt, t)\n"
    "    -- dt = seconds since last frame, t = seconds since Play started\n"
    "end\n";

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

// --- Startup ---------------------------------------------------------------
// The handful of things that must happen before anything is loaded, hoisted out
// of main() so the top of the function reads as a list of what booting means
// rather than fifty lines of how.

// Release builds link as a GUI app (see sandbox/CMakeLists.txt), so
// double-clicking the exe no longer flashes up a console window. That would also
// throw away every fprintf(stderr) log line -- so if we WERE started from a
// terminal, adopt it and point the C streams back at it. Started from Explorer
// there is no parent console, AttachConsole fails, and the streams stay where
// they were (nowhere). Nothing else changes.
void adoptParentConsole() {
#if defined(_WIN32) && defined(FITZEL_WINDOWED)
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
#endif
}

// Resolve all relative paths (assets/, content/, scripts/, game.json, project/)
// against the executable's own directory, so the app behaves the same whether
// launched from a shell, a shortcut, or a double-click.
void setWorkingDirToExe(int argc, char** argv) {
    if (argc <= 0) return;
    std::error_code ec;
    const auto exePath = std::filesystem::absolute(argv[0], ec);
    if (!ec && exePath.has_parent_path())
        std::filesystem::current_path(exePath.parent_path(), ec);
}

// An exported game ships one encrypted archive next to the exe instead of loose
// content/, project/ and assets/ folders. Mounted before anything is read, so
// every load after this point -- textures, models, sounds, shaders, scenes,
// scripts -- resolves against it. With no archive present (dev runs, the editor)
// nothing changes: the VFS falls straight through to disk.
void mountGameArchive() {
    std::error_code ec;
    if (std::filesystem::exists("game.fpak", ec))
        fitzel::vfs::mount("game.fpak", std::filesystem::current_path(ec));
}

// How this run starts. An exported/player build ships a game.json next to the exe
// that boots straight into the game with the editor hidden; `--play <project>`
// does the same from a command line. An empty project means the editor.
struct BootConfig {
    std::string project;
    std::string scene;             // start scene stem ("" = default scene)
    bool        fullscreen = true;
    // --- Benchmark mode ------------------------------------------------------
    // `--profile <file>` plays the project for a few seconds, writes what the
    // frame cost -- every CPU and GPU zone, and what the vegetation submitted --
    // to that file, and quits.
    //
    // It exists because the alternative is reading numbers off a screen: the
    // Performance window is the right tool while you are in there working, and
    // the wrong one for "is this change faster than that one", which needs the
    // same scene measured twice under the same conditions and the two numbers
    // side by side. This is that.
    std::string profilePath;
    // Where to drop a PNG of the last measured frame. A benchmark that only
    // reports milliseconds cannot tell you whether the change that bought them
    // also removed a shadow -- which, for anything in this area, is the more
    // likely outcome of the two. Same run, same camera, one picture.
    //
    // Editor build only: the PNG writer's implementation lives in the editor's
    // Render panel, and the player has no reason to carry an image encoder for
    // a development flag.
    std::string profileShot;
    double      profileSeconds = 8.0;
};

BootConfig loadBootConfig(int argc, char** argv) {
    BootConfig cfg;
    std::error_code ec;
    if (std::filesystem::exists("game.json", ec)) {
        std::ifstream gin("game.json");
        try {
            nlohmann::json gj; gin >> gj;
            cfg.project    = gj.value("project", std::string{});
            cfg.scene      = gj.value("startScene", std::string{});
            cfg.fullscreen = gj.value("fullscreen", true);
        } catch (...) {}
    }
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--play")            cfg.project     = argv[i + 1];
        else if (a == "--scene")      cfg.scene       = argv[i + 1];
        else if (a == "--profile")    cfg.profilePath = argv[i + 1];
        else if (a == "--profile-shot") cfg.profileShot = argv[i + 1];
        else if (a == "--profile-seconds")
            cfg.profileSeconds = std::max(1.0, std::atof(argv[i + 1]));
    }
    return cfg;
}

// --- Content roots ---------------------------------------------------------

// Where this build's content lives. A portable/exported build ships a `content/`
// next to the exe; a dev run falls back to the compile-time tree CMake injected.
struct ContentRoots {
    std::string content;
    std::string models;
    std::string textures;
    std::string sounds;
};

ContentRoots resolveContentRoots() {
    const bool local = fitzel::vfs::isDirectory("content");
    ContentRoots r;
    r.content  = local ? std::filesystem::absolute("content").generic_string()
                       : std::string(FITZEL_CONTENT_DIR);
    r.models   = local ? r.content + "/models"   : std::string(FITZEL_MODEL_DIR);
    r.textures = local ? r.content + "/textures" : std::string(FITZEL_TEXTURE_DIR);
    r.sounds   = local ? r.content + "/sounds"   : std::string(FITZEL_SOUND_DIR);
    return r;
}

// --- Core shaders ----------------------------------------------------------

// The engine's own shader programs -- the ones every frame goes through.
struct CoreShaders {
    Shader lit;     // scene geometry + terrain
    Shader water;   // the water surface
    // Running water (brooks, rivers, canals). Its own program rather than the
    // one above because the lake's planar reflection is rendered for ONE height
    // and a river is at a different one every ten metres -- see river.frag.
    Shader river;
    Shader sky;     // sky + volumetric clouds (fullscreen raymarch pass)
    Shader skybox;  // HDRI background (reuses the fullscreen sky vertex shader)
};

// All four in one go, so a program that failed to compile is reported here by
// name instead of turning up as a black screen halfway through the first frame.
// Returns false if a REQUIRED one failed. The skybox is not one of them: an HDRI
// background is optional, so losing it costs the background, not the session.
bool loadCoreShaders(CoreShaders& out) {
    const auto load = [](Shader& dst, const char* vert, const char* frag,
                         const char* name) {
        dst = Shader::fromFiles(vert, frag);
        if (!dst.isValid())
            std::fprintf(stderr, "Failed to load %s shader\n", name);
        return dst.isValid();
    };
    bool ok = true;
    ok = load(out.lit,   "assets/shaders/lit.vert",   "assets/shaders/lit.frag",   "lit")   && ok;
    ok = load(out.water, "assets/shaders/water.vert", "assets/shaders/water.frag", "water") && ok;
    ok = load(out.river, "assets/shaders/river.vert", "assets/shaders/river.frag", "river") && ok;
    ok = load(out.sky,   "assets/shaders/sky.vert",   "assets/shaders/sky.frag",   "sky")   && ok;
    load(out.skybox,     "assets/shaders/sky.vert",   "assets/shaders/skybox.frag", "skybox");
    return ok;
}

// --- Startup geometry ------------------------------------------------------

// A tessellated water grid so Gerstner waves can displace its vertices. Unit
// sized in XZ around the origin; the water pass scales it to the world.
Mesh makeWaterGrid(int n) {
    std::vector<Vertex>        verts;
    std::vector<std::uint32_t> idx;
    verts.reserve(static_cast<std::size_t>(n) * n);
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            const float fx = static_cast<float>(x) / (n - 1) - 0.5f;
            const float fz = static_cast<float>(z) / (n - 1) - 0.5f;
            verts.push_back({{fx, 0.0f, fz}, {0, 1, 0},
                             {static_cast<float>(x) / (n - 1),
                              static_cast<float>(z) / (n - 1)}});
        }
    }
    for (int z = 0; z < n - 1; ++z) {
        for (int x = 0; x < n - 1; ++x) {
            const std::uint32_t i0 = static_cast<std::uint32_t>(z * n + x);
            const std::uint32_t i1 = i0 + 1;
            const std::uint32_t i2 = i0 + static_cast<std::uint32_t>(n);
            const std::uint32_t i3 = i2 + 1;
            idx.insert(idx.end(), {i0, i2, i1, i1, i2, i3});
        }
    }
    return Mesh::create(verts, idx);
}

// The quad every fullscreen pass is drawn through: sky, HDRI skybox, post chain.
Mesh makeFullscreenQuad() {
    const std::vector<Vertex> verts = {
        {{-1.0f, -1.0f, 0.0f}, {0, 0, 1}, {0, 0}},
        {{ 1.0f, -1.0f, 0.0f}, {0, 0, 1}, {1, 0}},
        {{ 1.0f,  1.0f, 0.0f}, {0, 0, 1}, {1, 1}},
        {{-1.0f,  1.0f, 0.0f}, {0, 0, 1}, {0, 1}},
    };
    return Mesh::create(verts, {0, 1, 2, 0, 2, 3});
}

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

#ifndef FITZEL_PLAYER

// --- Lua code completion ---------------------------------------------------

// The completion popup's state: the matches for the identifier under the cursor
// (the popup shows while this is non-empty and the editor is focused), the word
// being completed, and what Esc last did about it.
struct Completions {
    std::vector<Completion> items;
    std::string             prefix;             // the partial word being completed
    int                     sel  = 0;           // highlighted match
    bool                    open = false;
    bool                    gameMember  = false; // completing after "game."
    bool                    manualClose = false; // Esc: stay closed until
    std::string             closedPrefix;        // the prefix changes
};

// Refresh the candidates from the identifier under the cursor. Called each frame
// after the editor renders, so it sees the latest edit.
void refreshCompletion(TextEditor& ed, Completions& c) {
    c.items.clear();
    const auto        cur  = ed.GetCursorPosition();
    const std::string line = ed.GetCurrentLineText();
    const int         tab  = ed.GetTabSize();
    // Map the tab-expanded cursor column back to a byte index in the line.
    int idx = 0, col = 0;
    while (idx < static_cast<int>(line.size()) && col < cur.mColumn) {
        col += (line[idx] == '\t') ? (tab - (col % tab)) : 1;
        ++idx;
    }
    auto isIdent = [](char ch){
        return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; };
    int start = idx;
    while (start > 0 && isIdent(line[start - 1])) --start;
    c.prefix = line.substr(start, idx - start);
    // "game." member context: a '.' right before the word, and the token before
    // the dot is exactly "game".
    c.gameMember = false;
    if (start > 0 && line[start - 1] == '.') {
        int ws = start - 1;
        while (ws > 0 && isIdent(line[ws - 1])) --ws;
        c.gameMember = (line.substr(ws, (start - 1) - ws) == "game");
    }
    if (c.prefix.empty() && !c.gameMember) {
        c.open = false; c.manualClose = false; return;
    }
    // Esc keeps the popup closed until the prefix actually changes.
    if (c.manualClose) {
        if (c.prefix == c.closedPrefix) { c.open = false; return; }
        c.manualClose = false;
    }
    auto lower = [](std::string s){
        for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s; };
    const std::string pfx = lower(c.prefix);
    auto consider = [&](const Completion* arr, std::size_t count){
        for (std::size_t i = 0; i < count; ++i)
            if (lower(arr[i].text).rfind(pfx, 0) == 0) c.items.push_back(arr[i]);
    };
    if (c.gameMember)
        consider(kGameMembers, sizeof(kGameMembers) / sizeof(kGameMembers[0]));
    else
        consider(kTopLevel, sizeof(kTopLevel) / sizeof(kTopLevel[0]));
    // Nothing useful to offer (no match, or the sole match is already typed).
    if (c.items.empty() || (c.items.size() == 1 && lower(c.items[0].text) == pfx)) {
        c.open = false; return;
    }
    if (c.sel >= static_cast<int>(c.items.size())) c.sel = 0;
    c.open = true;
}

// --- The toolbar strip's icons ---------------------------------------------
// Every button in the strip paints its own picture into the window's draw list.
// No icon font: nothing extra to ship, nothing to fall back to when a glyph is
// missing, and a 26 px symbol built from a handful of lines stays sharp where a
// scaled bitmap would not. It is all pure painting -- draw list, centre, radius,
// colour -- which is why it lives out here, and why the strip itself is left
// holding only what a click does.
namespace icon {

constexpr ImU32 kOn  = IM_COL32(255, 205,  70, 255);  // this tool/shape is active
constexpr ImU32 kOff = IM_COL32(215, 215, 220, 255);
constexpr ImU32 kDim = IM_COL32(130, 132, 140, 255);  // offered but not available

// The primitive shapes, drawn as themselves.
void shape(ImDrawList* dl, EntityType t, ImVec2 c, float r, ImU32 col) {
    switch (t) {
        case EntityType::Box:
            dl->AddRect({c.x - r, c.y - r}, {c.x + r, c.y + r}, col, 0.0f, 0, 2.0f);
            break;
        case EntityType::Ramp:
            dl->AddTriangle({c.x - r, c.y + r}, {c.x + r, c.y + r},
                            {c.x + r, c.y - r}, col, 2.0f);
            break;
        case EntityType::Cylinder:
            dl->AddRect({c.x - r * 0.7f, c.y - r}, {c.x + r * 0.7f, c.y + r},
                        col, 4.0f, 0, 2.0f);
            dl->AddLine({c.x - r * 0.7f, c.y - r}, {c.x + r * 0.7f, c.y - r}, col, 2.0f);
            break;
        case EntityType::Sphere:
            dl->AddCircle(c, r, col, 0, 2.0f);
            break;
        case EntityType::Plane: {
            // A quad seen at a shallow angle: the flat thing it is, told apart
            // from the Box beside it by being flat rather than by a label.
            const ImVec2 p[4] = {{c.x - r, c.y + r * 0.45f}, {c.x - r * 0.45f, c.y - r * 0.45f},
                                 {c.x + r, c.y - r * 0.45f}, {c.x + r * 0.45f, c.y + r * 0.45f}};
            dl->AddPolyline(p, 4, col, ImDrawFlags_Closed, 2.0f);
            break;
        }
        case EntityType::Light:
            dl->AddCircleFilled(c, r * 0.45f, col);
            for (int a = 0; a < 8; ++a) {
                const float  ang = a * 0.7853982f;
                const ImVec2 d(std::cos(ang), std::sin(ang));
                dl->AddLine({c.x + d.x * r * 0.7f, c.y + d.y * r * 0.7f},
                            {c.x + d.x * r, c.y + d.y * r}, col, 1.5f);
            }
            break;
        case EntityType::Empty:  // small dashed cross = transform node
            dl->AddLine({c.x - r, c.y}, {c.x + r, c.y}, col, 1.5f);
            dl->AddLine({c.x, c.y - r}, {c.x, c.y + r}, col, 1.5f);
            dl->AddCircle(c, r * 0.4f, col, 0, 1.5f);
            break;
        default: break;
    }
}

// A mouse arrow for Select; the same arrow with a plus next to it for Create.
void pointer(ImDrawList* dl, bool create, ImVec2 c, float r, ImU32 col) {
    const ImVec2 a(c.x - r * (create ? 0.9f : 0.45f), c.y - r);
    dl->AddTriangleFilled(a, {a.x, a.y + r * 1.7f},
                          {a.x + r * 1.15f, a.y + r * 1.15f}, col);
    if (create) {
        const ImVec2 q(c.x + r * 0.6f, c.y - r * 0.3f);
        dl->AddLine({q.x - r * 0.5f, q.y}, {q.x + r * 0.5f, q.y}, col, 2.0f);
        dl->AddLine({q.x, q.y - r * 0.5f}, {q.x, q.y + r * 0.5f}, col, 2.0f);
    }
}

// A little horizon with a hill on it.
void terrain(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    dl->AddLine({c.x - r, c.y + r * 0.6f}, {c.x + r, c.y + r * 0.6f}, col, 1.5f);
    dl->AddTriangle({c.x - r * 0.8f, c.y + r * 0.6f}, {c.x, c.y - r * 0.7f},
                    {c.x + r * 0.8f, c.y + r * 0.6f}, col, 1.8f);
}

// The three gizmo operations: a 4-way arrow, a circular arrow, a diagonal
// between a filled and an open handle.
void gizmo(ImDrawList* dl, ImGuizmo::OPERATION op, ImVec2 c, float r, ImU32 col) {
    const float a = 3.5f;
    if (op == ImGuizmo::TRANSLATE) {
        dl->AddLine({c.x - r, c.y}, {c.x + r, c.y}, col, 1.6f);
        dl->AddLine({c.x, c.y - r}, {c.x, c.y + r}, col, 1.6f);
        dl->AddTriangleFilled({c.x + r, c.y}, {c.x + r - a, c.y - a}, {c.x + r - a, c.y + a}, col);
        dl->AddTriangleFilled({c.x - r, c.y}, {c.x - r + a, c.y - a}, {c.x - r + a, c.y + a}, col);
        dl->AddTriangleFilled({c.x, c.y - r}, {c.x - a, c.y - r + a}, {c.x + a, c.y - r + a}, col);
        dl->AddTriangleFilled({c.x, c.y + r}, {c.x - a, c.y + r - a}, {c.x + a, c.y + r - a}, col);
    } else if (op == ImGuizmo::ROTATE) {
        dl->PathArcTo(c, r, 0.6f, 5.4f, 20);
        dl->PathStroke(col, 0, 1.8f);
        const ImVec2 e(c.x + std::cos(5.4f) * r, c.y + std::sin(5.4f) * r);
        const ImVec2 tg(-std::sin(5.4f), std::cos(5.4f));
        const ImVec2 no(std::cos(5.4f), std::sin(5.4f));
        dl->AddTriangleFilled({e.x + tg.x * a, e.y + tg.y * a},
                              {e.x - no.x * a * 0.7f, e.y - no.y * a * 0.7f},
                              {e.x + no.x * a * 0.7f, e.y + no.y * a * 0.7f}, col);
    } else {
        dl->AddLine({c.x - r * 0.7f, c.y + r * 0.7f}, {c.x + r * 0.7f, c.y - r * 0.7f}, col, 1.8f);
        dl->AddRectFilled({c.x + r * 0.7f - 3, c.y - r * 0.7f - 3},
                          {c.x + r * 0.7f + 3, c.y - r * 0.7f + 3}, col);
        dl->AddRect({c.x - r * 0.7f - 3, c.y + r * 0.7f - 3},
                    {c.x - r * 0.7f + 3, c.y + r * 0.7f + 3}, col, 0.0f, 0, 1.5f);
    }
}

// Object box with its own tilted axis = local frame; globe with meridian and
// equator = world frame.
void gizmoSpace(ImDrawList* dl, bool local, ImVec2 c, float r, ImU32 col) {
    if (local) {
        dl->AddRect({c.x - r * 0.7f, c.y - r * 0.55f},
                    {c.x + r * 0.35f, c.y + r * 0.7f}, col, 0.0f, 0, 1.6f);
        dl->AddLine({c.x + r * 0.35f, c.y - r * 0.55f}, {c.x + r, c.y - r}, col, 1.6f);
    } else {
        dl->AddCircle(c, r, col, 0, 1.6f);
        dl->AddLine({c.x - r, c.y}, {c.x + r, c.y}, col, 1.2f);
        dl->AddLine({c.x, c.y - r}, {c.x, c.y + r}, col, 1.2f);
        dl->AddBezierQuadratic({c.x, c.y - r}, {c.x - r * 0.9f, c.y},
                               {c.x, c.y + r}, col, 1.1f);
        dl->AddBezierQuadratic({c.x, c.y - r}, {c.x + r * 0.9f, c.y},
                               {c.x, c.y + r}, col, 1.1f);
    }
}

// Two edges converging into the distance plus a dashed centre line: a road,
// readable at 26 px without an icon font.
void road(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    dl->AddLine({c.x - r, c.y + r}, {c.x - r * 0.35f, c.y - r}, col, 1.8f);
    dl->AddLine({c.x + r, c.y + r}, {c.x + r * 0.35f, c.y - r}, col, 1.8f);
    dl->AddLine({c.x, c.y + r * 0.9f}, {c.x, c.y + r * 0.2f}, col, 1.4f);
    dl->AddLine({c.x, c.y - r * 0.2f}, {c.x, c.y - r * 0.8f}, col, 1.4f);
}

// The viewport shading ladder: a wire cube, then the same ball with as much of
// the material as each mode keeps -- nothing, the scene's light, the paintwork.
// One shape across three of the four, because what changes between them is not
// the object.
void shade(ImDrawList* dl, int mode, ImVec2 c, float r, ImU32 col) {
    // The toolbar's own background, for the pattern that has to be cut OUT of a
    // filled ball rather than drawn on top of it -- these icons have one colour
    // to draw with, and a checker needs two.
    constexpr ImU32 kInk = IM_COL32(29, 32, 38, 255);
    if (mode == 3) { // wireframe: a cube with its far edges left in
        const float a = r * 0.78f, o = r * 0.42f;
        dl->AddRect({c.x - a, c.y - a + o}, {c.x + a - o, c.y + a}, col, 0.0f, 0, 1.5f);
        dl->AddRect({c.x - a + o, c.y - a}, {c.x + a, c.y + a - o}, col, 0.0f, 0, 1.1f);
        dl->AddLine({c.x - a, c.y - a + o}, {c.x - a + o, c.y - a}, col, 1.1f);
        dl->AddLine({c.x + a - o, c.y + a}, {c.x + a, c.y + a - o}, col, 1.1f);
        return;
    }
    if (mode == 4) {                       // pathtraced: a ray bouncing off it
        dl->AddCircleFilled({c.x + r * 0.25f, c.y + r * 0.3f}, r * 0.55f, col);
        dl->AddLine({c.x - r, c.y - r}, {c.x - r * 0.1f, c.y - r * 0.15f}, col, 1.4f);
        dl->AddLine({c.x - r * 0.1f, c.y - r * 0.15f}, {c.x + r * 0.35f, c.y - r}, col, 1.4f);
        dl->AddLine({c.x + r * 0.35f, c.y - r}, {c.x + r, c.y - r * 0.35f}, col, 1.4f);
        return;
    }
    dl->AddCircleFilled(c, r * 0.85f, col);
    if (mode == 2) {                       // solid lit: the scene's sun on it
        for (int i = 0; i < 5; ++i) {
            const float ang = 3.4f + i * 0.30f;
            const ImVec2 d(std::cos(ang), std::sin(ang));
            dl->AddLine({c.x + d.x * r * 1.15f, c.y + d.y * r * 1.15f},
                        {c.x + d.x * r * 1.55f, c.y + d.y * r * 1.55f}, col, 1.3f);
        }
    } else if (mode == 0) {                // textured: a pattern, cut in
        const float q = r * 0.42f;
        dl->AddRectFilled({c.x - q, c.y - q}, {c.x, c.y}, kInk);
        dl->AddRectFilled({c.x, c.y}, {c.x + q, c.y + q}, kInk);
    }
}

} // namespace icon

// One button in the strip: a blank fixed-size button with a tooltip, whose
// picture the caller paints afterwards at `center` -- afterwards, so it lands on
// top of the button rather than under it. A disabled button still draws itself:
// greyed out is a state worth showing, missing is not.
bool iconButton(const char* id, ImVec2 size, const char* tip, bool disabled,
                ImVec2& center) {
    ImGui::PushID(id);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(disabled);
    const bool clicked = ImGui::Button("##b", size);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    center = ImVec2(p0.x + size.x * 0.5f, p0.y + size.y * 0.5f);
    ImGui::PopID();
    ImGui::SameLine();
    return clicked;
}

// --- The default panel layout ----------------------------------------------

// First run (or after "Reset layout"): lay the panels out into a tidy right-hand
// column, split top/bottom, so they don't start as a heap of floating windows.
// Once arranged, ImGui persists it in imgui.ini.
void buildDefaultDockLayout(ImGuiID dockId) {
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_PassthruCentralNode
                                    | ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->WorkSize);

    // Hierarchy (left) | Scene (centre) | Inspector over ONE tool dock
    // (right).
    //
    // Every tool panel is pre-docked into that single node, so opening
    // one adds a TAB rather than a window. Left to float, they get
    // dragged out into a tiled wall -- which is exactly what happened:
    // fourteen panels side by side and the viewport reduced to a tab
    // behind one of them. The panels are not the work; the scene is,
    // and it keeps the middle.
    //
    // This does not make the panels fewer, only stop them competing
    // for the same space. Fewer is a different job (merging them by
    // task rather than by source file).
    ImGuiID central = 0;
    ImGuiID left  = ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Left, 0.18f,
                                                nullptr, &central);
    ImGuiID right = ImGui::DockBuilderSplitNode(central, ImGuiDir_Right, 0.30f,
                                                nullptr, &central);
    // Inspector keeps its own strip above the tools: it is the one
    // panel wanted WHILE a tool is open (pick a road point, look at
    // what it is), so making it a peer tab would mean flipping back
    // and forth.
    ImGuiID inspector = 0;
    ImGuiID tools = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.62f,
                                                nullptr, &inspector);

    ImGui::DockBuilderDockWindow("Scene", central);
    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", inspector);
    // Titles must match the ImGui::Begin() strings exactly -- a typo
    // costs a floating window and nothing else, which is why they are
    // in one list rather than scattered over the call sites.
    for (const char* w : {
            "Terrain", "Terrain Sculpt", "Terrain Paint", "Water",
            "Sky & atmosphere", "Weather & audio", "Colour grade",
            "Environment",
            "Vegetation", "Scatter",
            "Roads", "City", "Buildings",
            "Materials", "Models", "Prefabs", "Assets",
            "UI Overlay", "Camera path", "Camera", "3D Cursor", "Render",
            "Vehicle", "Glider", "Voxels", "Mixer", "Scripts",
            "Performance", "Stats"})
        ImGui::DockBuilderDockWindow(w, tools);
    ImGui::DockBuilderFinish(dockId);
}

// --- The menu bar ----------------------------------------------------------
// Each menu gets exactly the slice of main's state it touches, gathered once
// into a context of references and callbacks -- the same shape projectio's
// Context already uses -- and handed back every frame. That lets the menu bodies
// move out of main() unchanged, which is the point: a menu that had to be
// rewritten in order to be moved is a menu whose behaviour you can no longer
// diff against the one that worked.

using NameAndPath = std::vector<std::pair<std::string, std::string>>;

struct FileMenuCtx {
    Window&                         window;
    const std::string&              currentProject;
    const std::string&              prefLocation;
    const std::vector<std::string>& recentProjects;
    const std::string&              exportStatus;
    const std::string&              autosaveStatus;
    const char*                     projNameBuf;
    char*                           wizName;
    std::size_t                     wizNameCap;
    char*                           wizLocation;
    std::size_t                     wizLocationCap;
    bool&                           wizardOpen;
    bool&                           wizardIsNew;
    game::Settings&                 gameSettings;
    bool&                           gameSettingsOpen;
    std::function<void()>                          saveCurrent;
    std::function<void(const std::string&)>        exportGame;
    std::function<bool(const std::string&)>        openProjectAsync;
    std::function<NameAndPath(const std::string&)> listProjectsIn;
};

struct SceneMenuCtx {
    const std::string& currentProject;
    char*              sceneNameBuf;
    std::size_t        sceneNameCap;
    bool&              sceneNewOpen;
    bool&              sceneRenameOpen;
    bool&              sceneDeleteOpen;
    std::function<void(const std::string&)>        saveSceneFile;
    std::function<bool(const std::string&)>        loadSceneAsync;
    std::function<NameAndPath(const std::string&)> listScenesIn;
};

struct EditMenuCtx {
    CommandStack&        history;
    Document&            document;
    std::vector<Entity>& entities;
    Selection&           sel;
    char*                prefabNameBuf;
    std::size_t          prefabNameCap;
    bool&                showPrefabs;
    std::function<void()>             clampRoadSel;
    std::function<void()>             clampSplineSel;
    // Also re-cuts the watercourse beds: an undo puts different PATHS back and
    // the terrain has to follow them. See main's clampRiverSel.
    std::function<void()>             clampRiverSel;
    std::function<void()>             duplicateSelection;
    std::function<void()>             deleteSelection;
};

// One row per entry in the View menu: the submenu it sits under, its label
// (nullptr = a separator within that submenu), the shortcut hint, and the bool
// it toggles. A table rather than twenty-eight MenuItem calls, because "Close
// all panels" has to clear exactly the set the menu opens -- kept as two
// hand-written lists they drift apart, and a panel you cannot close is worse
// than one you cannot open. `closeAll = false` holds an entry out of that sweep.
struct PanelEntry {
    const char* group;
    const char* label;
    const char* shortcut;
    bool*       flag;
    bool        closeAll = true;
};


void drawFileMenu(const FileMenuCtx& c) {
    if (!ImGui::BeginMenu("File")) return;
    if (ImGui::MenuItem("New Project...")) {
        c.wizardIsNew = true;
        c.wizName[0] = '\0';
        std::snprintf(c.wizLocation, c.wizLocationCap, "%s",
                      c.prefLocation.c_str());
        c.wizardOpen = true;
    }
    if (ImGui::MenuItem("Save Project", nullptr, false,
                        !c.currentProject.empty()))
        c.saveCurrent();
    if (ImGui::MenuItem("Save Project As...")) {
        c.wizardIsNew = false;
        std::snprintf(c.wizName, c.wizNameCap, "%s", c.projNameBuf);
        std::snprintf(c.wizLocation, c.wizLocationCap, "%s",
                      c.prefLocation.c_str());
        c.wizardOpen = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Game Settings...", nullptr, false,
                        !c.currentProject.empty())) {
        // Load the project's current settings, then open the modal.
        c.gameSettings = game::load(
            std::filesystem::path(c.currentProject)
                .parent_path().generic_string());
        c.gameSettingsOpen = true;
    }
    if (ImGui::MenuItem("Export Game...", nullptr, false,
                        !c.currentProject.empty())) {
        std::string picked;
        if (ed::pickFolder(picked, c.prefLocation)) c.exportGame(picked);
    }
    if (!c.exportStatus.empty())
        ImGui::TextDisabled("%s", c.exportStatus.c_str());
    // When the last crash snapshot was taken. Not an action, just proof the
    // safety net is there -- which is the only thing anyone wants to know about
    // it until the day they need it.
    if (!c.autosaveStatus.empty())
        ImGui::TextDisabled("%s", c.autosaveStatus.c_str());
    ImGui::Separator();
    if (ImGui::BeginMenu("Open Project")) {
        if (ImGui::MenuItem("Browse folder...")) {
            std::string picked;
            if (ed::pickFolder(picked, c.prefLocation) &&
                !c.openProjectAsync(picked))
                std::fprintf(stderr,
                    "No project (.fitzel) in %s\n", picked.c_str());
        }
        if (!c.recentProjects.empty()) {
            ui::sectionText("Recent");
            int ri = 0;
            for (const std::string& folder : c.recentProjects) {
                // Scope each item by index so two entries can never
                // share an ImGui id, even if a duplicate path slips
                // into the list.
                ImGui::PushID(ri++);
                const std::string lbl =
                    std::filesystem::path(folder).filename().string();
                if (ImGui::MenuItem(lbl.c_str()))
                    c.openProjectAsync(folder);
                ImGui::PopID();
            }
        }
        ui::sectionText("In default location");
        const auto projs = c.listProjectsIn(c.prefLocation);
        if (projs.empty()) ImGui::TextDisabled("(none)");
        for (const auto& [n, folder] : projs)
            if (ImGui::MenuItem((n + "##d" + folder).c_str()))
                c.openProjectAsync(folder);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Exit")) c.window.requestClose();
    ImGui::EndMenu();
}

void drawSceneMenu(const SceneMenuCtx& c) {
    if (!ImGui::BeginMenu("Scene")) return;
    if (c.currentProject.empty()) {
        ImGui::TextDisabled("Open or create a project first.");
    } else {
        const std::string projFolder =
            std::filesystem::path(c.currentProject).parent_path().generic_string();
        if (ImGui::MenuItem("New Scene...")) {
            c.sceneNameBuf[0] = '\0';
            c.sceneNewOpen = true;
        }
        if (ImGui::MenuItem("Save Scene"))
            c.saveSceneFile(c.currentProject);
        if (ImGui::MenuItem("Rename Scene...")) {
            std::snprintf(c.sceneNameBuf, c.sceneNameCap, "%s",
                std::filesystem::path(c.currentProject).stem().string().c_str());
            c.sceneRenameOpen = true;
        }
        const auto scenes = c.listScenesIn(projFolder);
        ImGui::BeginDisabled(scenes.size() < 2); // keep at least one scene
        if (ImGui::MenuItem("Delete Scene..."))
            c.sceneDeleteOpen = true;
        ImGui::EndDisabled();
        ui::sectionText("Switch to");
        for (const auto& [n, path] : scenes) {
            const bool active = (path == c.currentProject);
            if (ImGui::MenuItem((n + "##sc" + path).c_str(), nullptr, active) &&
                !active) {
                c.saveSceneFile(c.currentProject); // don't lose current edits
                c.loadSceneAsync(path);
            }
        }
    }
    ImGui::EndMenu();
}

void drawEditMenu(const EditMenuCtx& c) {
    if (!ImGui::BeginMenu("Edit")) return;
    const std::string undoLbl = c.history.canUndo()
        ? std::string("Undo ") + c.history.undoName() : "Undo";
    const std::string redoLbl = c.history.canRedo()
        ? std::string("Redo ") + c.history.redoName() : "Redo";
    if (ImGui::MenuItem(undoLbl.c_str(), "Ctrl+Z", false, c.history.canUndo())) {
        c.history.undo(c.document); c.sel.clear(); c.clampRoadSel(); c.clampSplineSel();
        c.clampRiverSel();
    }
    if (ImGui::MenuItem(redoLbl.c_str(), "Ctrl+Y", false, c.history.canRedo())) {
        c.history.redo(c.document); c.sel.clear(); c.clampRoadSel(); c.clampSplineSel();
        c.clampRiverSel();
    }
    ImGui::Separator();
    const bool hasSel = c.sel.valid() &&
        c.entities[c.sel.index()].type != EntityType::Sun;
    const int selCount = static_cast<int>(c.sel.count());
    const char* dupLbl = selCount > 1 ? "Duplicate selection" : "Duplicate";
    const char* delLbl = selCount > 1 ? "Delete selection"    : "Delete";
    if (ImGui::MenuItem(dupLbl, nullptr, false, hasSel))
        c.duplicateSelection();
    if (ImGui::MenuItem(delLbl, nullptr, false, hasSel))
        c.deleteSelection();
    if (ImGui::MenuItem("Save as Prefab...", nullptr, false, hasSel)) {
        // Seed the name field from the selection and open the panel;
        // the panel's "Create" button does the actual save.
        const std::string nm = c.entities[c.sel.index()].name;
        std::snprintf(c.prefabNameBuf, c.prefabNameCap, "%s",
                      nm.c_str());
        c.showPrefabs = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Clear objects")) {
        c.entities.erase(std::remove_if(c.entities.begin(), c.entities.end(),
            [](const Entity& e){ return e.type != EntityType::Sun; }),
            c.entities.end());
        c.sel.clear();
        c.history.clear(); // bulk reset -> drop history
    }
    ImGui::EndMenu();
}

void drawViewMenu(Gui& gui, const std::vector<PanelEntry>& panels,
                  viewnav::Nav& viewNav,
                  bool& prefsDirty, bool& requestDockRebuild) {
    if (!ImGui::BeginMenu("View")) return;
    // Where the camera looks from, before which windows are open: it is the one
    // entry here that changes the picture rather than the furniture, and it is
    // the way to reach a standard view without a numpad -- or without holding
    // anything steady, which is the point (see the editor's aims in README).
    viewNav.drawMenu();
    ImGui::Separator();
    // Grouped by the JOB, not by which file draws it. A flat list of twenty-eight
    // entries is a list you read start to finish every time; "where do I set fog"
    // has an obvious answer only once the entries are sorted the way the work is.
    const char* group = nullptr;
    bool        open  = false;
    for (const PanelEntry& e : panels) {
        if (!group || std::strcmp(group, e.group) != 0) {
            if (open) ImGui::EndMenu();
            group = e.group;
            open  = ImGui::BeginMenu(group);
        }
        if (!open)    continue;
        if (!e.label) ImGui::Separator();
        else          ImGui::MenuItem(e.label, e.shortcut, e.flag);
    }
    if (open) ImGui::EndMenu();
    ImGui::Separator();
    // Text size/typeface are a comfort setting, not a scene one: they live in the
    // editor prefs and apply immediately.
    if (ImGui::BeginMenu("Interface")) {
        float px = gui.fontSize();
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderFloat("Text size", &px, 14.0f, 28.0f, "%.0f pt")) {
            gui.setFontSize(px);
            prefsDirty = true;
        }
        if (gui.fontFamilyCount() > 1) {
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::BeginCombo("Typeface",
                                  gui.fontFamilyName(gui.fontFamily()))) {
                for (int i = 0; i < gui.fontFamilyCount(); ++i)
                    if (ImGui::Selectable(gui.fontFamilyName(i),
                                          i == gui.fontFamily())) {
                        gui.setFontFamily(i);
                        ui::setBoldFont(gui.boldFont());
                        prefsDirty = true;
                    }
                ImGui::EndCombo();
            }
        }
        ui::hint("Verdana and Tahoma have the largest x-height "
                 "-- easiest to read at small sizes.");
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Close all panels")) {
        // One click back to scene + hierarchy + inspector. The panels are cheap
        // to reopen and expensive to look past.
        for (const PanelEntry& e : panels)
            if (e.flag && e.closeAll) *e.flag = false;
    }
    if (ImGui::MenuItem("Reset layout")) requestDockRebuild = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Put the panels back into one docked tab\n"
                          "strip on the right, with the scene in the\n"
                          "middle. Needed once after an update: your\n"
                          "own arrangement is remembered in imgui.ini\n"
                          "and wins until you ask for this.");
    ImGui::EndMenu();
}

#endif // !FITZEL_PLAYER

} // namespace

int main(int argc, char** argv) {
    try {
        adoptParentConsole();
        setWorkingDirToExe(argc, argv);
        mountGameArchive();

        const BootConfig   boot           = loadBootConfig(argc, argv);
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
        const ContentRoots roots       = resolveContentRoots();
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

        CoreShaders shaders;
        if (!loadCoreShaders(shaders)) return 1;
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
        Mesh waterMesh = makeWaterGrid(400);
        // Half-resolution reflection/refraction: the water distortion hides it
        // and it roughly quarters the cost of those two textured passes.
        RenderTarget reflectRT(640, 360);
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

        Mesh fsQuad = makeFullscreenQuad();

        // HDR scene buffer + the post chain (SSAO, bloom, tonemap, speed blur,
        // FXAA). The chain owns its shaders and its intermediate targets -- see
        // PostChain.hpp for why that ownership is the whole point of it.
        PostChain post;
        if (!post.init()) return 1;
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
        RenderTarget hdrRT(hdrW, hdrH, RenderTarget::Format::RGBA16F, /*depthTex=*/true);
        // The post chain's knobs stay HERE, not on the chain: they are edited by
        // the Sky & atmosphere and Colour grade panels, saved with the project,
        // and driven by the weather -- all of which is main's business. The chain
        // is handed them per frame.
        float bloomIntensity = 0.35f;
        float rayIntensity   = 0.5f;
        float bloomThreshold = 1.0f;  // luminance where the glow starts
        float bloomKnee      = 0.5f;  // soft-knee width below it

        // The final composited image lives in this target and is shown as the
        // central "Viewport" dock panel (IDE/editor style). Its size tracks the
        // panel's content region, so the scene renders at the viewport's pixels.
        RenderTarget viewportRT(hdrW, hdrH, RenderTarget::Format::RGBA8);
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
        bool fxaaEnabled = true;
        int  viewW = hdrW, viewH = hdrH;
        bool viewportHovered = false;
        glm::vec2 viewportMouseNdc(0.0f); // cursor within the viewport, NDC [-1,1]
        bool viewportClicked = false;     // left-click landed on the viewport image
        glm::vec2 viewportRectMin(0.0f);  // viewport image top-left in screen px
        glm::vec2 viewportRectSize(0.0f); // viewport image size in screen px
        // The horizon-based AO samples along screen-space directions it derives
        // itself, so there is no sample kernel to upload any more.
        float ssaoStrength = 0.7f;
        // Cube-face size of the reflection probe, mirrored here so it can be a
        // scene setting; the renderer owns the actual cubes (see
        // setEnvProbeResolution, which reallocates them).
        int   envProbeRes  = fitzel::Renderer::kDefaultEnvProbeRes;
        // Cap on the probe's cube faces per frame. The default buys back most of
        // the reflection lag at speed; 1 is the old amortized behaviour.
        int   envProbeFaces = 3;
        float ssaoRadius   = 1.5f;
        float ssaoBias     = 0.15f; // radians: horizons below this don't occlude
        float ssaoPower    = 1.6f;

        // Day/night cycle.
        float timeOfDay = 7.3f;    // hours [0,24)
        float dayLength = 240.0f;  // real seconds per full 24h (0 = frozen)
        bool  timePaused = true;   // freeze the time of day where it is

        // Cloud controls.
        float cloudCoverage = 0.5f;
        float cloudDensity  = 1.0f;
        // A cumulus is at least as TALL as it is wide. The old 140..320 slab was
        // 180 m thick under 400 m features, so every cloud came out a pancake --
        // and a field of pancakes seen from underneath is a textured ceiling,
        // which is exactly what it looked like. Base and top now stand roughly
        // where real ones do, and the feature size grew to match.
        float cloudScale    = 0.0009f;
        float cloudSpeed    = 5.0f;
        float cloudBottom   = 700.0f;
        float cloudTop      = 2400.0f;

        // The high layer (see sky.frag): ice, well above the cumulus and well
        // above the weather -- which is why none of this is touched by the storm
        // slider below. A front rolling in does not blow the cirrus away; it
        // slides underneath it.
        float cirrusAmount  = 0.35f;   // 0 = clear, 1 = a milky sheet
        float cirrusHeight  = 1400.0f; // world units; the whole layer scales with it
        float cirrusSpeed   = 2.5f;    // the jet stream, not the surface wind
        float contrailAmount = 0.0f;   // 0 = an empty sky, 1 = four trails

        // Weather: 0 = clear .. 1 = storm. Drives clouds, light, fog, waves, rain.
        float weather     = 0.0f;
        bool  autoWeather = false;
        // Ground wetness 0..1: builds while it rains, dries slowly after, so roads
        // and terrain stay shiny for a while once the rain passes.
        float roadWetness = 0.0f;

        // Rain streaks + boat spray own their own shaders and GL buffers now.
        RainRenderer rain;
        if (!rain.init()) return 1;
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
        if (!veg.init()) return 1;

        bool      grassPaintMode = false;      // grass brush active
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
        bool  sculptMode     = false;
        int   sculptTool     = 0;            // 0 raise 1 lower 2 smooth 3 flatten 4 erode 5 stamp 6 noise
        float sculptRadius   = 8.0f;         // world units
        float sculptStrength = 0.5f;         // 0..1 brush intensity
        float sculptFlattenH = 0.0f;         // flatten target height (grabbed on press)
        int   stampShape     = 0;            // 0 dome 1 cone 2 plateau 3 crater 4 ridge
        float stampHeight    = 12.0f;        // stamp peak height (m); negative digs in
        float stampRot       = 0.0f;         // ridge orientation (radians)
        float noiseFreq      = 0.35f;        // roughen feature size
        float noiseSeed      = 0.0f;         // advanced per dab so detail layers up
        float carveDepth     = 12.0f;        // valley depth (m); Alt raises a ridge

        // --- Proportional pull ------------------------------------------------
        // Press on the ground and the point under the cursor follows it up or
        // down, with the disc around it coming along less and less out to the rim.
        //
        // It is the one sculpt tool here that is not a dab. Every other brush
        // applies a step per frame and the ground ends up wherever holding the
        // button for that long put it -- which means the result depends on the
        // steadiness of a hand, and getting a hill to a particular height means
        // creeping up on it. This one is ABSOLUTE: the height is a function of
        // where the mouse is now, not of how long it has been down, so overshoot
        // costs nothing, wobble leaves nothing behind, and letting go early
        // leaves exactly what was on screen. That is the whole reason it exists
        // next to Raise rather than instead of it.
        float pullFalloff    = 1.0f;         // skirt shape (see TerrainEditField::pull)
        // What one CLICK is worth. The drag is a convenience on top of it, not
        // the way in: press-and-move is exactly the gesture this editor exists to
        // not require (see the Parkinson note in the project's UI rules), so the
        // tool has to be complete without it -- set the number with the panel's
        // steppers, click the ground, done. A drag ends by writing what it
        // arrived at back into here, so the next click repeats it.
        float pullHeight     = 4.0f;         // metres per click; negative pushes in
        bool      pullActive  = false;       // a pull gesture is in progress
        glm::vec2 pullCenter{0.0f};          // where it was anchored
        float     pullRadius  = 8.0f;        // ...and the radius it was anchored with
        float     pullShape   = 1.0f;        // ...and the shape, both frozen for the drag
        float     pullApplied = 0.0f;        // metres already written to the field
        float     pullStartY  = 0.0f;        // mouse Y at the press, in screen pixels
        float     pullScale   = 0.05f;       // world metres per screen pixel, at the anchor

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
        bool  paintMode     = false;
        int   paintLayer    = 0;             // which of the first 4 texture layers to paint
        float paintRadius   = 8.0f;          // world units
        float paintStrength = 0.5f;          // 0..1 brush intensity
        bool  paintErase    = false;         // paint vs revert-to-auto

        // --- Mesh texture painting ------------------------------------------
        // A brush that puts textures on a modelled object. The weights live on
        // the mesh's own corners (EditMesh::paint) and what they MEAN lives there
        // too (MeshComponent::paintSlots), so a painted object is self-contained:
        // it travels, it copies, it becomes a prefab, and none of that depends on
        // what the terrain happens to be textured with. The brush splits the faces
        // it crosses, because paint on four corners is not a stroke. The tool
        // itself is in MeshPaint.cpp -- main only hands it the viewport and
        // brackets the stroke for undo.
        bool  meshPaintMode     = false;
        int   meshPaintSlot     = 0;         // which of the mesh's own four slots
        float meshPaintRadius   = 0.6f;      // world units -- an object-sized brush
        float meshPaintStrength = 0.5f;
        float meshPaintDetail   = 0.25f;     // split faces down to this edge length
        bool  meshPaintErase    = false;
        // One held button = one undo step: the entity as it was when the stroke
        // started, banked when it ends.
        bool   meshPaintStroking = false;
        Entity meshPaintBefore;
        // Where a stroke stops splitting. A brush held down over a wall would
        // otherwise quarter its faces until the editor stops.
        constexpr int kMeshPaintMaxFaces = 4000;

        // --- Object scatter -------------------------------------------------
        // A 3D brush that sprinkles imported models over the terrain as regular
        // Model entities, grouped under a root "Scattered" Empty; one stamp =
        // one undo step. Settings/placement/panel live in ScatterTool.
        bool               scatterMode = false;
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


        // --- Trees: instanced model + billboard LOD (owned by VegetationSystem)
        if (!veg.initTrees(modelDir, texDir)) return 1;

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
        auto applyGfx = [&](const gfxmenu::Settings& prev) {
            gfxmenu::Targets t;
            t.viewRadius    = &viewRadius;
            t.envProbeRes   = &envProbeRes;
            t.envProbeFaces = &envProbeFaces;
            t.fxaa          = &fxaaEnabled;
            t.grassEnabled  = &veg.grassEnabled;
            t.flowerEnabled = &veg.flowerEnabled;
            t.grassDensity  = &veg.grassDensity;
            t.grassRadius   = &veg.grassRadius;
            t.flowerDensity = &veg.flowerDensity;
            t.cityMinPixels = &cityDetailCull;
            t.regrowVegetation = [&] { veg.grassDirty = true; };
            t.setVSync = [&](bool on) { glfwSwapInterval(on ? 1 : 0); };
            gfxmenu::apply(gfxSet, prev, renderer, t);
            for (RoadSystem* r : roads) r->cityMinPixels = cityDetailCull;
        };
        applyGfx(gfxSet);   // the saved choices, before the first frame is drawn

        bool roadEditMode = false;   // edit-mode flag (mutually exclusive brushes)
        int  roadSel      = -1;       // selected control point (-1 = none)
        int  roadSel2     = -1;       // shift-clicked second point (bridge far end)
        bool roadDragging = false;    // dragging the selected handle
        bool roadDragHeight = false;  // ...vertically (Ctrl held on grab) vs across the ground
        // Erase a control point, keeping the bridges that name points by index
        // honest: any bridge ending on it goes with it, and later points shift down.
        auto removeRoadPoint = [&](int k) {
            RoadSystem& road = roads.active();
            if (k < 0 || k >= static_cast<int>(road.roadPts.size())) return;
            road.erasePoint(k);
            std::vector<RoadSystem::BridgeSpec> keep;
            for (RoadSystem::BridgeSpec b : road.bridges) {
                if (b.a == k || b.b == k) continue;
                if (b.a > k) --b.a;
                if (b.b > k) --b.b;
                keep.push_back(b);
            }
            road.bridges.swap(keep);
            // Tunnels name their ends the same way, so they need the same
            // bookkeeping -- see the loop note below.
            std::vector<RoadSystem::BridgeSpec> keepT;
            for (RoadSystem::BridgeSpec t : road.tunnels) {
                if (t.a == k || t.b == k) continue;
                if (t.a > k) --t.a;
                if (t.b > k) --t.b;
                keepT.push_back(t);
            }
            road.tunnels.swap(keepT);
            // Loops name their ends the same way a bridge does, so they need the
            // same bookkeeping -- a loop left pointing at a deleted point would
            // silently move to whatever slid into its place.
            std::vector<roadloop::Spec> keepL;
            for (roadloop::Spec l : road.loops) {
                if (l.a == k || l.b == k) continue;
                if (l.a > k) --l.a;
                if (l.b > k) --l.b;
                keepL.push_back(l);
            }
            road.loops.swap(keepL);
            roadSel = roadSel2 = -1;
            road.needsBuild = true;
        };
        // Insert a control point at index `at`, mirroring removeRoadPoint's bookkeeping:
        // any bridge endpoint at or after `at` shifts up by one so it keeps naming the
        // same point. Selects the new point.
        auto insertRoadPoint = [&](int at, glm::vec2 p) {
            RoadSystem& road = roads.active();
            at = glm::clamp(at, 0, static_cast<int>(road.roadPts.size()));
            // A point dropped between two others inherits their height, so
            // inserting into a raised stretch doesn't punch a hole in it.
            float lift = 0.0f;
            const int n = static_cast<int>(road.roadPts.size());
            if (n > 0) {
                const int a = std::clamp(at - 1, 0, n - 1);
                const int b = std::clamp(at,     0, n - 1);
                lift = 0.5f * (road.liftOf(a) + road.liftOf(b));
            }
            // ...and its cross-fall, for the same reason: dropping a point into a
            // banked corner should not flatten it at that one station.
            float bank = 0.0f;
            if (n > 0) {
                const int a = std::clamp(at - 1, 0, n - 1);
                const int b = std::clamp(at,     0, n - 1);
                bank = 0.5f * (road.bankOf(a) + road.bankOf(b));
            }
            road.insertPoint(at, p, lift, bank);
            for (RoadSystem::BridgeSpec& b : road.bridges) {
                if (b.a >= at) ++b.a;
                if (b.b >= at) ++b.b;
            }
            for (RoadSystem::BridgeSpec& t : road.tunnels) {
                if (t.a >= at) ++t.a;
                if (t.b >= at) ++t.b;
            }
            for (roadloop::Spec& l : road.loops) {
                if (l.a >= at) ++l.a;
                if (l.b >= at) ++l.b;
            }
            roadSel   = at;
            roadSel2  = -1;
            road.needsBuild = true;
        };
        // Best index to insert a new waypoint at world XZ `P`: between the two control
        // points whose segment lies nearest, so clicking on an existing road adds a
        // point in the middle instead of always at the tail. A click that projects
        // past an open end extends the road there instead of splitting the end segment.
        auto roadInsertIndex = [&](glm::vec2 P) -> int {
            const RoadSystem& road = roads.active();
            const int n = static_cast<int>(road.roadPts.size());
            if (n < 2) return n; // 0 or 1 points: nothing to insert between -> append
            float bestD = 1e30f, bestT = 0.0f;
            int   bestSeg = 0;
            const int segs = road.closed ? n : n - 1; // closed loops wrap last->first
            for (int i = 0; i < segs; ++i) {
                const glm::vec2 a = road.roadPts[i];
                const glm::vec2 b = road.roadPts[(i + 1) % n];
                const glm::vec2 ab = b - a;
                const float len2 = glm::dot(ab, ab);
                const float t = len2 > 1e-6f
                    ? glm::clamp(glm::dot(P - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
                const float d = glm::distance(P, a + ab * t);
                if (d < bestD) { bestD = d; bestSeg = i; bestT = t; }
            }
            if (!road.closed) {
                if (bestSeg == 0     && bestT <= 0.0f) return 0; // before the start
                if (bestSeg == n - 2 && bestT >= 1.0f) return n; // past the end
            }
            return bestSeg + 1;
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
        PhysicsBodyId physCarId = 0;   // Jolt vehicle chassis (Play-mode drive)
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
        // Arcade car pose (state lives in `race`; aliased so the loop keeps the
        // old names). physSteer stays a plain local -- it's the Jolt car's input.
        glm::vec3& carPos     = race.carPos;
        float&     carYaw     = race.carYaw;      // heading (radians)
        float&     carSpeed   = race.carSpeed;    // m/s (negative = reverse)
        float&     wheelSpin  = race.wheelSpin;   // rolling angle (radians)
        float&     steerAngle = race.steerAngle;  // front-wheel steer (radians, arcade)
        float physSteer  = 0.0f;   // smoothed steer input -1..1 (Jolt car)
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
            // push(), not pushApplied(): RoadShapeCmd::redo does more than
            // assign -- it flags the rebuild the committed shape needs.
            if (!cmd->trivial()) history.push(std::move(cmd), document);
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
        bool splineEditMode   = false;  // owns the LMB (mutually exclusive brushes)
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
        // The spline editor's five flags again. The one difference is what a
        // commit means: pushing the undo step is also what re-cuts the bed, so
        // every gesture ends in exactly one carve however many frames it took.
        bool riverEditMode   = false;  // owns the LMB (mutually exclusive brushes)
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
        // Document (like the road), so it carries its own selection + undo state:
        // an interaction opens with the list it found and commits the difference,
        // so a slider dragged across many frames is one undo step.
        UiOverlay              uiOverlay;
        int                    uiSel = -1;
        std::vector<UiElement> uiEditBefore;
        bool                   uiEditOpen = false;

        std::vector<Entity>& entities = document.entities();
        // What is selected: the active object plus, when more than one is picked,
        // the whole set -- and the invariant tying them together. See Selection.hpp.
        Selection sel(entities);
        // Box-select (Ctrl + left-drag in the viewport): in-progress rectangle.
        bool      boxSelecting  = false;
        ImVec2    boxStart{0.0f, 0.0f};
        // Round-robin picking: the entity ids the last click's ray passed through
        // (nearest first) and which one is currently selected, so repeated clicks
        // at the same spot cycle to the next overlapping entity (a parent group's
        // bounding box no longer permanently swallows clicks meant for a child).
        std::vector<int> pickStack;
        int       pickIdx        = -1;
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
        // Half-thickness a new Plane gets. Not zero: the pick box would be a
        // sheet nobody can click and the box collider would be degenerate.
        constexpr float kPlaneHalfY = 0.05f;
        EntityType entityNewType = EntityType::Box; // type placed on click
        int       entityCounter = 0; // for unique default names

        // Blender-style 3D cursor: a world-space reference point placed with
        // Shift+Right-click, used as a snap/placement anchor (see the "3D Cursor"
        // panel). cursorGrid is the step for the grid-snap operations.
        glm::vec3 cursor3D{0.0f};
        bool      cursorVisible = true;
        float     cursorGrid    = 1.0f;
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
        float assetThumbSize = 76.0f;
        char  assetFilter[64] = "";
        bool  assetTexturesOnly = false;
        std::string assetDropStatus; // outcome of the last drop from Explorer

        struct ThumbWork {
            std::mutex              mutex;
            std::condition_variable cv;
            std::deque<std::pair<fitzel::AssetId, std::string>> queue;   // to decode
            std::vector<std::pair<fitzel::AssetId, fitzel::ImagePixels>> done; // decoded
            bool stop = false;
        };
        ThumbWork thumbWork;
        std::thread thumbThread([&thumbWork]{
            const std::filesystem::path cacheDir = thumbCacheDir();
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
                const long long mt = sourceMtime(job.second);
                fitzel::ImagePixels img;
                if (!loadThumbCache(cacheFile, mt, img)) {
                    img = Texture::decodeThumbnail(job.second, 128);
                    saveThumbCache(cacheFile, mt, img);
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
        bool showScriptEditor = false;
        bool showAbout       = false;
        bool showStats       = false;
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
        bool showCity        = false;
        bool showCamPath     = false;
        bool showRoads       = false;
        bool showUiOverlay   = false; // scene 2D UI overlay editor
        bool showCursor      = false; // 3D cursor panel
        bool showModeling    = false; // face-modelling panel
        bool showMeshPaint   = false; // painting layers onto a modelled mesh
        bool showUv          = false; // where a face's texture sits on it
        // Which face of the selected mesh the modelling operations act on. Reset
        // whenever the selection moves to another object: a face index means
        // nothing on a different mesh.
        int  meshFaceSel     = -1;
        int  meshFaceOwner   = -1;   // entity id that index belongs to
        // A face-gizmo drag in flight: the entity as it was when the drag began
        // (one undo step for the whole drag) and the scale it applies to its mesh.
        bool      faceGizmoActive = false;
        Entity    faceGizmoBefore;
        glm::vec3 faceGizmoScale{1.0f};
        // The scale the gizmo has reported so far in this drag (see the SCALE
        // branch: ImGuizmo measures that one from the start of the drag, not from
        // the last frame).
        glm::vec3 faceGizmoAccScale{1.0f};
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
        // "Import Unity Asset" panel: a browsed asset folder, the chosen FBX, and
        // a cached texture-match preview (recomputed when the selection changes).
        std::string unityDir;        // asset folder being browsed (default: models/)
        std::string unityFbx;        // selected .fbx (absolute path), "" = none
        std::vector<std::pair<std::string, std::string>> unityFbxList; // (rel, abs)
        std::string unityFbxScanDir; // folder unityFbxList was scanned for ("" = stale)
        std::vector<fitzel::UnityTexMatch> unityPreview;
        std::vector<std::string> unityNearby; // image files near the selected FBX
        std::string unityPreviewFor; // path unityPreview was computed for
        bool        unityFlipV = true;   // mirror V on import (FBX UV convention)
        std::string unityStatus;         // last import result, shown in the panel

        // The audio mixer. Master (masterVolume/muted below)
        // scales everything via the device; Ambient scales the looping weather/
        // zone voices; SFX scales the one-shot bus. Each channel: level + mute.
        struct MixChannel { float level = 1.0f; bool mute = false;
                            float gain() const { return mute ? 0.0f : level; } };
        MixChannel mixAmbient, mixSfx;

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
        // How the viewport draws the scene. The ladder is Blender's, and so is
        // the reason for it: the finished picture is the worst view for most of
        // the work that goes into making one. Wireframe shows what is behind
        // what, Solid shows shape under a fixed studio light that a broken scene
        // cannot take away, Solid lit shows the scene's own light without the
        // paintwork arguing with it, and Textured is the game. Editor only --
        // play mode always draws the game (see viewShade's use below).
        //
        // Not saved: it is a way of LOOKING at the scene for a minute, not a
        // property of it, and a project that reopened in wireframe because
        // somebody once checked a normal would be a puzzle, not a convenience.
        // Pathtraced is the odd one out: the other four are the raster
        // renderer told to show less, this one is a different renderer
        // altogether, running in the background and handing the viewport a
        // picture (see ViewportTrace.hpp).
        enum ViewShade { kShadeTextured = 0, kShadeSolid = 1, kShadeSolidLit = 2,
                         kShadeWireframe = 3, kShadePathTraced = 4 };
        int viewShade = kShadeTextured;
#ifndef FITZEL_PLAYER
        viewtrace::State viewTrace;
#endif

        ImGuizmo::OPERATION gizmoOp = ImGuizmo::TRANSLATE; // Move / Scale (axis-aligned)
        // Gizmo reference frame: WORLD = global axes, LOCAL = the object's own axes.
        // Toggle from the toolbar or with X. (ImGuizmo forces SCALE to local anyway.)
        ImGuizmo::MODE gizmoMode = ImGuizmo::WORLD;
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
            [&]{ models.clear(); prefabCache.clear(); meshCache.clear(); },
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
        auto saveProjectTo        = [&](const std::string& f){ projectio::saveProjectTo(pio, f); noteSaved(); };
        auto saveCurrent          = [&](){ projectio::saveCurrent(pio); noteSaved(); };
        auto exportGame           = [&](const std::string& o){ projectio::exportGame(pio, o); };
        auto listProjectsIn       = [&](const std::string& r){ return projectio::listProjectsIn(r); };
        // Loading/creating a project replaces the document, so the undo history
        // must not survive the boundary.
        // Rescan road-surface textures and tree assets to include the project being
        // opened before the scene loads (loadScene restores the saved surface/trees
        // by name, so the project's files must already be in the lists by then).
        auto newProject           = [&](){ projectio::newProject(pio); history.clear(); prefabCache.clear(); roads.refreshTextures(std::string()); veg.refreshTreeAssets(std::string()); };

        // Non-blocking editor loads: kick off an incremental scene load, then step
        // it each frame (below) so the UI keeps drawing with a progress bar. The
        // player's boot and a scene trigger drive the same loader, but drain it in
        // one go behind the loading screen (openProjectShowing / loadSceneShowing
        // below) -- they need the scene complete before continuing.
        projectio::SceneLoad sceneLoad;
        auto openProjectAsync = [&](const std::string& f){
            roads.refreshTextures(f); veg.refreshTreeAssets(f);
            history.clear(); prefabCache.clear();
            return projectio::beginOpenProject(pio, sceneLoad, f);
        };
        auto loadSceneAsync = [&](const std::string& p){
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
        auto saveSceneFile        = [&](const std::string& p){ projectio::saveScene(pio, p); noteSaved(); };
        auto loadSceneFile        = [&](const std::string& p){ const bool ok = projectio::loadSceneFile(pio, p); history.clear(); prefabCache.clear(); return ok; };
        auto newSceneInProject    = [&](const std::string& f, const std::string& n){ auto p = projectio::newSceneInProject(pio, f, n); history.clear(); return p; };
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
        auto worldOf = [&](const Entity& e) {
            return composeModel(e.center, e.rotation, glm::vec3(1.0f));
        };
        // Convert a world-space edit (gizmo, physics) into the entity's LOCAL
        // transform (the source of truth), given its parent's world matrix (null
        // for a root). Also mirrors into center/rotation for this frame.
        auto setWorld = [&](Entity& e, const glm::vec3& wPos, const glm::vec3& wRot,
                            const glm::mat4* parentWorld) {
            e.center = wPos; e.rotation = wRot;
            if (!parentWorld) { e.localCenter = wPos; e.localRotation = wRot; return; }
            const glm::mat4 lm =
                glm::inverse(*parentWorld) * composeModel(wPos, wRot, glm::vec3(1.0f));
            float t[3], r[3], s[3];
            ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(lm), t, r, s);
            e.localCenter   = glm::vec3(t[0], t[1], t[2]);
            e.localRotation = glm::vec3(r[0], r[1], r[2]);
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
            for (Entity& e : entities) resolveOne(e, done);
        };
        // World matrix of an entity's PARENT (identity for a root) -- for setWorld.
        auto parentWorldMat = [&](const Entity& e) -> glm::mat4 {
            if (e.parent < 0) return glm::mat4(1.0f);
            const Entity* p = document.find(e.parent);
            return p ? worldOf(*p) : glm::mat4(1.0f);
        };

        // --- 3D-cursor snap operations (shared by the panel + the Shift+S popup) --
        auto cursorHaveSel = [&] {
            return sel.valid();
        };
        auto snapToGrid = [&](glm::vec3 p) {
            const float g = cursorGrid;
            if (g <= 0.0f) return p;
            return glm::vec3(std::round(p.x / g) * g, std::round(p.y / g) * g,
                             std::round(p.z / g) * g);
        };
        // Move the selected entity to a world position (via the local source of
        // truth, so it respects any parent -- same path the gizmo/inspector use).
        auto moveSelectionTo = [&](const glm::vec3& wPos) {
            if (!cursorHaveSel()) return;
            Entity& b = entities[sel.index()];
            const glm::mat4 pw = parentWorldMat(b);
            setWorld(b, wPos, b.rotation, b.parent >= 0 ? &pw : nullptr);
        };
        auto snapCursorToOrigin    = [&] { cursor3D = glm::vec3(0.0f); };
        auto snapCursorToGrid      = [&] { cursor3D = snapToGrid(cursor3D); };
        auto snapCursorToTerrain   = [&] { cursor3D.y = streamer.heightAt(cursor3D.x, cursor3D.z); };
        auto snapCursorToSelection = [&] { if (cursorHaveSel()) cursor3D = entities[sel.index()].center; };
        auto snapSelectionToCursor = [&] { moveSelectionTo(cursor3D); };
        auto snapSelectionToGrid   = [&] { if (cursorHaveSel()) moveSelectionTo(snapToGrid(entities[sel.index()].center)); };

#ifndef FITZEL_PLAYER
        // --- Face modelling ---------------------------------------------------
        // The editable mesh on the selected object, if it has one.
        auto selectedMesh = [&]() -> MeshComponent* {
            if (!cursorHaveSel()) return nullptr;
            return entities[sel.index()].components.get<MeshComponent>();
        };
        // Turn the selected box into an editable mesh of exactly the same size --
        // built at the object's real dimensions, so a metre in the modelling
        // panel is a metre in the world rather than a fraction of a unit cube.
        // Nothing else about the object changes: same transform, same material,
        // same collider.
        auto convertToMesh = [&] {
            if (!cursorHaveSel()) return;
            Entity& e = entities[sel.index()];
            if (e.type != EntityType::Box || e.components.get<MeshComponent>()) return;
            const Entity before = e;
            auto mc = std::make_unique<MeshComponent>();
            mc->mesh = EditMesh::box(e.half);
            mc->touch();
            e.components.items.push_back(std::move(mc));
            meshFaceSel = -1;
            history.pushApplied(std::make_unique<ModifyEntityCmd>(before, e));
        };
        // The scale an entity currently applies to its mesh (1 unless someone has
        // dragged the Scale gizmo). Read before an edit and re-applied after, or
        // re-deriving the half-extents from raw bounds would quietly undo it.
        auto meshScaleOf = [](const Entity& e, const MeshComponent& mc) {
            glm::vec3 mn, mx;
            mc.mesh.bounds(mn, mx);
            return (e.half * 2.0f) / glm::max(mx - mn, glm::vec3(1e-4f));
        };
        // What every mesh edit ends with, whichever way it was made -- a panel
        // button or a gizmo drag: re-centre the geometry on the object's origin,
        // move the object by that same shift so nothing appears to jump, and take
        // the new bounds as its half-extents. That invariant is what keeps the
        // pick box, the gizmo and the collider describing the shape that is
        // actually there -- an extruded tower whose AABB still claimed to be the
        // original cube would be unpickable at the top and would collide with air
        // at the bottom.
        auto normalizeMeshEntity = [&](Entity& e, MeshComponent& mc,
                                       const glm::vec3& scale) {
            const glm::vec3 shift = editmesh::recenter(mc.mesh);
            glm::vec3 mn, mx;
            mc.mesh.bounds(mn, mx);
            e.half = glm::max((mx - mn) * 0.5f * scale, glm::vec3(1e-3f));
            if (glm::dot(shift, shift) > 0.0f) {
                const glm::quat q  = glm::quat(glm::radians(e.rotation));
                const glm::mat4 pw = parentWorldMat(e);
                setWorld(e, e.center + q * (shift * scale), e.rotation,
                         e.parent >= 0 ? &pw : nullptr);
            }
            mc.touch();
        };
        // Run one face operation as one undoable step.
        auto applyMeshEdit = [&](const std::function<int(MeshComponent&)>& op,
                                 const char* /*label*/) {
            if (!cursorHaveSel()) return;
            Entity& e = entities[sel.index()];
            MeshComponent* mc = e.components.get<MeshComponent>();
            if (!mc || !op) return;
            const Entity    before = e;
            const glm::vec3 scale  = meshScaleOf(e, *mc);
            meshFaceSel = op(*mc);
            normalizeMeshEntity(e, *mc, scale);
            auto cmd = std::make_unique<ModifyEntityCmd>(before, e);
            if (!cmd->trivial()) history.pushApplied(std::move(cmd));
        };
        // World-space corners of one face of the selected mesh, for picking and
        // for drawing the highlight. Empty when there is no such face.
        auto meshFaceWorld = [&](const Entity& e, const MeshComponent& mc, int face) {
            std::vector<glm::vec3> out;
            if (!mc.mesh.validFace(face)) return out;
            glm::vec3 mn, mx;
            mc.mesh.bounds(mn, mx);
            const glm::vec3 sz = glm::max(mx - mn, glm::vec3(1e-4f));
            const glm::mat4 m = composeModel(e.center, e.rotation, (e.half * 2.0f) / sz);
            out.reserve(mc.mesh.faces[face].size());
            for (int i : mc.mesh.faces[face])
                out.push_back(glm::vec3(m * glm::vec4(mc.mesh.verts[i], 1.0f)));
            return out;
        };
#endif // !FITZEL_PLAYER
        // True if box `a` is `ancestorId` or below it (to reject cyclic reparenting).
        // True if box `a` is `ancestorId` or below it (to reject cyclic reparenting).
        auto isUnderId = [&](int a, int ancestorId) {
            for (int p = a; p >= 0; ) {
                if (p == ancestorId) return true;
                int nextIdx = -1;
                for (int i = 0; i < static_cast<int>(entities.size()); ++i)
                    if (entities[i].id == p) { nextIdx = i; break; }
                p = (nextIdx >= 0) ? entities[nextIdx].parent : -1;
            }
            return false;
        };
        // Delete an entity by index, reparenting its children to its own parent.
        auto deleteEntity = [&](int idx) {
            if (idx < 0 || idx >= static_cast<int>(entities.size())) return;
            if (entities[idx].type == EntityType::Sun) return; // the sun is permanent
            // Delete the whole subtree: the entity plus every descendant, as one
            // undoable step (deleting a parent shouldn't orphan its child parts).
            std::vector<int> ids{entities[idx].id};
            for (std::size_t k = 0; k < ids.size(); ++k)
                for (const Entity& e : entities)
                    if (e.parent == ids[k]) ids.push_back(e.id);
            history.push(std::make_unique<DeleteEntitiesCmd>(document, ids), document);
            sel.clear();
        };
        // Duplicate an entity as one undoable step: an offset copy that KEEPS its
        // parent.
        //
        // It used to unparent the copy, and that moved it. localCenter is relative
        // to the parent and is the source of truth; resolveHierarchy gives a ROOT
        // the world position `center = localCenter`. So a child sitting at local
        // (0, 0, -3) on a craft half a map away had its copy teleported to world
        // (1.1, 0, -3) -- next to the origin. On a visible object you would watch
        // it fly off; on an Empty there is nothing to see, so the copy was simply
        // somewhere else, unclickable where you were looking. Duplicating a
        // thruster mount is exactly that case.
        //
        // The offset is in the parent's frame, which is what "beside the original"
        // means for a child. `center` is left alone: it is derived, and
        // resolveHierarchy fills it from the parent this frame.
        auto duplicateEntity = [&](int idx) {
            if (idx < 0 || idx >= static_cast<int>(entities.size())) return;
            if (entities[idx].type == EntityType::Sun) return;
            Entity nb = entities[idx];
            nb.localCenter.x += nb.half.x * 2.2f;
            nb.id     = entityCounter++;
            nb.name  += " copy";
            history.push(std::make_unique<AddEntityCmd>(nb), document);
            sel.select(nb.id);
        };
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
        // Load a .fprefab and drop an instance into the scene, on the ground in
        // front of the camera, as one undoable step. Selects the new root.
        auto instantiatePrefabFile = [&](const std::string& path) {
            auto p = prefab::load(pio, path);
            if (!p || p->entities.empty()) {
                exportStatus = "Failed to load prefab.";
                return;
            }
            const glm::vec3 f = camera.position() + camera.front() * 8.0f;
            const glm::vec3 g(f.x, streamer.heightAt(f.x, f.z), f.z);
            std::vector<Entity> spawn = prefab::instantiate(*p, entityCounter, g, 0.0f);
            const int rootId = spawn.empty() ? -1 : spawn.front().id;
            history.push(std::make_unique<AddEntitiesCmd>(std::move(spawn), "Prefab"),
                         document);
            if (rootId >= 0) sel.select(rootId);
        };
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
#endif // !FITZEL_PLAYER
        // Ids of an entity and all its descendants (for a parented gizmo drag).
        auto collectSubtreeIds = [&](int rootId) {
            std::vector<int> ids{rootId};
            for (bool grew = true; grew; ) {
                grew = false;
                for (const Entity& e : entities) {
                    const bool have = std::find(ids.begin(), ids.end(), e.id) != ids.end();
                    const bool parentIn =
                        std::find(ids.begin(), ids.end(), e.parent) != ids.end();
                    if (!have && parentIn) { ids.push_back(e.id); grew = true; }
                }
            }
            return ids;
        };
        auto snapshotEntities = [&](const std::vector<int>& ids) {
            std::vector<Entity> out;
            out.reserve(ids.size());
            for (int id : ids) if (const Entity* e = document.find(id)) out.push_back(*e);
            return out;
        };

#ifndef FITZEL_PLAYER
        // --- Procedural buildings (see BuildingGen.hpp) -------------------------
        // Generate a building on the ground in front of the camera as one undoable
        // step, select it, and remember it as the "live" one so it can be re-tuned
        // and saved without hunting for it in the hierarchy.
        auto generateBuilding = [&]() {
            const glm::vec3 f = camera.position() + camera.front() * 60.0f;
            const glm::vec3 g(f.x, streamer.heightAt(f.x, f.z), f.z);
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
#endif // !FITZEL_PLAYER

#ifndef FITZEL_PLAYER
        // --- Selection-wide operations (see Selection.hpp for the set itself) ---
        // The two that stay here: both are one UNDOABLE STEP over the document,
        // and the history and the id counter are main's, not the selection's.
        // Delete every selected object's subtree as one undoable step (falls back
        // to the single-object delete when only one is selected).
        auto deleteSelection = [&]() {
            const std::vector<int> chosen = sel.ids();
            if (chosen.size() <= 1) { deleteEntity(sel.index()); return; }
            std::vector<int> ids;
            for (int rootId : chosen) {
                const Entity* e = document.find(rootId);
                if (!e || e->type == EntityType::Sun) continue;
                for (int id : collectSubtreeIds(rootId))
                    if (std::find(ids.begin(), ids.end(), id) == ids.end())
                        ids.push_back(id);
            }
            if (ids.empty()) return;
            history.push(std::make_unique<DeleteEntitiesCmd>(document, ids), document);
            sel.clear();
        };
        // Duplicate every selected object as one undoable step; the copies become
        // the selection. Parents are kept, exactly as the single Duplicate does
        // and for the same reason (see there).
        //
        // With one wrinkle a single copy cannot have: when a selected object's
        // PARENT was copied too, the copy must hang off the copied parent rather
        // than the original. Otherwise duplicating a craft and its thrusters
        // together gives you a second craft whose thrusters are still bolted to
        // the first one.
        auto duplicateSelection = [&]() {
            const std::vector<int> chosen = sel.ids();
            if (chosen.size() <= 1) { duplicateEntity(sel.index()); return; }
            std::vector<Entity> copies;
            std::vector<int>    newIds;
            std::unordered_map<int, int> remap;   // original id -> copy id
            for (int id : chosen) {
                const Entity* src = document.find(id);
                if (!src || src->type == EntityType::Sun) continue;
                Entity nb = *src;
                nb.localCenter.x += nb.half.x * 2.2f;
                nb.id     = entityCounter++;
                nb.name  += " copy";
                remap[id] = nb.id;
                newIds.push_back(nb.id);
                copies.push_back(std::move(nb));
            }
            for (Entity& c : copies) {
                const auto it = remap.find(c.parent);
                if (it != remap.end()) c.parent = it->second;
            }
            if (copies.empty()) return;
            history.push(std::make_unique<AddEntitiesCmd>(std::move(copies), "Duplicate"),
                         document);
            sel.clear();
            sel.addMany(newIds);
        };
#endif // !FITZEL_PLAYER

        // Spawn a new entity of `type` as a child of `parentId` (-1 = root),
        // placed at world position/rotation (wPos/wRot). Mirrors addEntity's
        // material/light setup but lets the hierarchy context menu build parented
        // nodes. Returns the new entity's id. One undoable step.
        auto spawnChild = [&](int parentId, EntityType type,
                              const glm::vec3& wPos, const glm::vec3& wRot) -> int {
            Entity nb;
            nb.type = type;
            nb.half = (type == EntityType::Light) ? glm::vec3(0.3f)
                    : (type == EntityType::Empty) ? glm::vec3(0.5f)
                    : (type == EntityType::Plane)
                          ? glm::vec3(entityNewHalf.x, kPlaneHalfY, entityNewHalf.z)
                    : entityNewHalf;
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
            nb.parent = parentId;
            nb.name   = std::string(entityTypeName(type)) + " " + std::to_string(nb.id);
            Entity* p = (parentId >= 0) ? document.find(parentId) : nullptr;
            const glm::mat4 pw = p ? worldOf(*p) : glm::mat4(1.0f);
            setWorld(nb, wPos, wRot, p ? &pw : nullptr);
            history.push(std::make_unique<AddEntityCmd>(nb), document);
            return nb.id;
        };
        // Make the Camera on entity `entId` the single Main Camera: the view that
        // Play (and the exported game) starts from. Sets its CameraComponent's
        // activeOnStart and clears it on every other camera, so exactly one is the
        // main camera. Pass -1 to clear all cameras (Play starts from the player
        // view). One undoable step over all camera entities; a no-op if `entId`
        // has no CameraComponent.
        auto setMainCamera = [&](int entId) {
            if (entId >= 0) {
                const Entity* e = document.find(entId);
                if (!e || !e->components.get<CameraComponent>()) return;
            }
            std::vector<int> camIds;
            for (const Entity& e : entities)
                if (e.components.get<CameraComponent>()) camIds.push_back(e.id);
            if (camIds.empty()) return;
            std::vector<Entity> before = snapshotEntities(camIds);
            for (Entity& e : entities)
                if (auto* cc = e.components.get<CameraComponent>())
                    cc->activeOnStart = (e.id == entId);
            auto cmd = std::make_unique<ModifyEntitiesCmd>(before, snapshotEntities(camIds));
            if (!cmd->trivial()) history.pushApplied(std::move(cmd));
        };
        // Context-menu helpers (index-based; capture the id first so the entities
        // vector may safely grow underneath).
        auto addEmptyChild = [&](int idx) {
            if (idx < 0 || idx >= static_cast<int>(entities.size())) return;
            const Entity& n = entities[idx];
            const int id = spawnChild(n.id, EntityType::Empty, n.center, n.rotation);
            sel.select(id);
        };
        auto addPrimitiveChild = [&](int idx, EntityType type) {
            if (idx < 0 || idx >= static_cast<int>(entities.size())) return;
            const Entity& n = entities[idx];
            const int id = spawnChild(n.id, type, n.center, glm::vec3(0.0f));
            sel.select(id);
        };
        // A camera that SHOOTS the picked object: an Empty carrying a Camera in
        // Multishot mode, aimed at that object by id (see MultiShot.hpp).
        //
        // It is deliberately NOT a child of its subject, which is the opposite of
        // how a follow camera is made here. A multishot camera stands off the
        // thing it films -- ahead of it, above it, planted in the road waiting for
        // it -- and a camera parented to a moving car would be fighting that
        // transform in every shot. So the subject is named instead, and this menu
        // item is what saves the author from having to know that.
        //
        // Where it is placed hardly matters (the shots decide where the eye goes),
        // but it is put a sensible framing distance off the subject anyway, so the
        // gizmo's tether is short and readable rather than crossing the map.
        auto addShotCamera = [&](int idx) {
            if (idx < 0 || idx >= static_cast<int>(entities.size())) return;
            const Entity& n = entities[idx];
            const float r = glm::max(glm::length(glm::vec2(n.half.x, n.half.z)), 0.4f);
            Entity cam;
            cam.type        = EntityType::Empty;
            cam.half        = glm::vec3(0.5f);
            cam.id          = entityCounter++;
            cam.name        = n.name + " Cam";
            cam.localCenter = cam.center =
                n.center + glm::vec3(r * 2.6f + 1.5f, n.half.y + 1.0f, 0.0f);
            auto cc = std::make_unique<CameraComponent>();
            cc->mode       = CameraComponent::Multishot;
            cc->shotTarget = n.id;
            cam.components.items.push_back(std::move(cc));
            history.push(std::make_unique<AddEntityCmd>(cam), document);
            sel.select(cam.id);
        };
        // Insert a new Empty between `idx` and its current parent, then reparent
        // `idx` under it -- keeping the node put. Groups the node under a fresh
        // pivot, like Unity's "Create Empty Parent".
        auto addEmptyParent = [&](int idx) {
            if (idx < 0 || idx >= static_cast<int>(entities.size())) return;
            if (entities[idx].type == EntityType::Sun) return; // the sun stays root
            const int       nodeId      = entities[idx].id;
            const int       grandparent = entities[idx].parent;
            const glm::vec3 wPos        = entities[idx].center;
            const glm::vec3 wRot        = entities[idx].rotation;
            const int emptyId = spawnChild(grandparent, EntityType::Empty, wPos, wRot);
            Entity* node = document.find(nodeId);
            Entity* emp  = document.find(emptyId);
            if (node && emp) {
                node->parent = emptyId;
                const glm::mat4 pw = worldOf(*emp);
                rebaseLocal(*node, &pw); // keep the child where it was
            }
            sel.select(emptyId);
        };
        // Attach car lights to a vehicle entity: two forward spot headlights at the
        // nose and two red point taillights (no shadows) at the tail, all parented so
        // they move/steer with the car. One undoable step. No-op without a Vehicle.
        auto addVehicleLights = [&](int idx) {
            if (idx < 0 || idx >= static_cast<int>(entities.size())) return;
            Entity& veh = entities[idx];
            const auto* vc = veh.components.get<VehicleComponent>();
            if (!vc) return;
            const int vehId = veh.id;
            // Body extents: the larger of the model AABB and the chassis box.
            // frontSign maps the model's nose (native -Z when forward==1) to local Z.
            const glm::vec3 h = glm::max(veh.half, vc->chassisHalf);
            const float frontSign = (vc->forward == 1) ? -1.0f : 1.0f;
            const float zx  = h.z * 0.96f * frontSign; // nose Z (tail is -zx)
            const float xo  = h.x * 0.6f;              // left/right inset
            const float yo  = h.y * 0.1f;              // just above centre
            const float yaw = (vc->forward == 1) ? 180.0f : 0.0f; // spot faces the nose
            const glm::mat4 pw = worldOf(veh);
            std::vector<Entity> batch;
            auto makeLight = [&](const char* name, glm::vec3 lpos, glm::vec3 lrot,
                                 bool spot, glm::vec3 col, float inten, float rng) {
                Entity nb;
                nb.type   = EntityType::Light;
                nb.half   = glm::vec3(0.12f);
                nb.id     = entityCounter++;
                nb.parent = vehId;
                nb.name   = name;
                nb.localCenter   = lpos;
                nb.localRotation = lrot;
                auto lc = std::make_unique<LightComponent>();
                lc->type = spot ? 1 : 0;
                lc->color = col; lc->intensity = inten; lc->range = rng;
                lc->castShadows = false;
                if (spot) { lc->spotAngle = 30.0f; lc->spotBlend = 0.25f; }
                nb.components.items.push_back(std::move(lc));
                // Seed the world transform (resolveHierarchy refreshes it each frame).
                const glm::mat4 w =
                    pw * composeModel(nb.localCenter, nb.localRotation, glm::vec3(1.0f));
                float t[3], r[3], s[3];
                ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(w), t, r, s);
                nb.center   = {t[0], t[1], t[2]};
                nb.rotation = {r[0], r[1], r[2]};
                batch.push_back(std::move(nb));
            };
            const glm::vec3 warm(1.0f, 0.96f, 0.85f);
            const glm::vec3 red (1.0f, 0.05f, 0.02f);
            makeLight("Headlight L", { xo, yo,  zx}, {0.0f, yaw, 0.0f}, true,  warm, 12.0f, 28.0f);
            makeLight("Headlight R", {-xo, yo,  zx}, {0.0f, yaw, 0.0f}, true,  warm, 12.0f, 28.0f);
            makeLight("Taillight L", { xo, yo, -zx}, {0.0f, 0.0f, 0.0f}, false, red,   3.0f,  4.0f);
            makeLight("Taillight R", {-xo, yo, -zx}, {0.0f, 0.0f, 0.0f}, false, red,   3.0f,  4.0f);
            history.push(std::make_unique<AddEntitiesCmd>(std::move(batch), "Add headlights"),
                         document);
            sel.select(vehId);
        };


        // --- Flowers (owned by VegetationSystem) -----------------------------
        if (!veg.initFlowers()) return 1;
        bool flowerPaintMode = false; // brush mode flag; rest of flower state in veg

        // Gameplay RNG for spawner launch-direction randomization (persists across
        // spawns so successive emits vary within a Play session).
        std::mt19937 spawnRng(1234u);
        std::uniform_real_distribution<float> spawnU(0.0f, 1.0f);

        // Tree brush mode flag; the rest of the tree state/logic lives in veg.
        bool treePaintMode = false;

        // --- Audio: weather-driven sound layers --------------------------
        showProgress(0.82f, "Loading audio...");
        Audio audio;
        const std::string& soundDir = roots.sounds;
        WeatherSounds wx;
        loadWeatherSounds(audio, soundDir, wx);
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
        float masterVolume = 0.8f;
        bool  muted        = false;
        bool  prevFlashOn  = false;

        // Atmospheric fog (subtle by default; aerial perspective, not haze soup).
        float fogDensity = 0.0045f; // stronger aerial perspective (soft distant haze)
        float fogFalloff = 0.028f;  // fog reaches higher so distant hills recede

        // Depth of field (distance blur). dofMax = 0 disables it.
        float dofMax   = 5.0f;      // max blur radius (pixels)
        float dofNear  = 25.0f;     // sharp up to here (metres)
        float dofFar   = 140.0f;    // fully blurred beyond here

        // Camera motion blur: streaks the scene along per-pixel screen velocity
        // (this frame's camera transform vs last frame's, by depth reprojection).
        // Purely camera motion -- fast turns/flight smear, a static view stays
        // sharp. 0 disables it (like dofMax).
        float motionBlurStrength = 0.6f; // 0 off .. ~2 heavy (exposure fraction)

        // Tonemapping exposure + HSV colour grade.
        float exposure   = 1.0f;
        float hueShift   = 0.0f;
        float saturation = 1.35f; // richer, less milky greens
        float valueGain  = 1.0f;
        float warmth     = 0.18f; // golden-hour white balance
        float contrast   = 0.16f; // lift the flat look

        bool requestDockRebuild = false; // set by "Reset layout" to re-apply the default

        // Camera angle controls.
        float camFov   = camera.fov();
        float camYaw   = camera.yaw();
        float camPitch = camera.pitch();

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
#ifndef FITZEL_PLAYER
        // The viewport's other two ways of moving: the axis-aligned standard
        // views (numpad, Blender's layout) and middle-mouse panning. See
        // ViewportNav.hpp -- both are editor-only, the player has no viewport to
        // navigate.
        viewnav::Nav viewNav;
#endif

        // Undo/redo edge state + gizmo-drag snapshot (a drag is one undoable step).
        bool                prevUndo = false, prevRedo = false;
        bool                gizmoActive = false;
        std::vector<int>    gizmoIds;
        std::vector<Entity> gizmoBefore;
        // Multi-select gizmo drag: the selected roots being moved together and the
        // active object's world transform last frame, so each other root gets the
        // same incremental delta applied (individual-origins style).
        std::vector<int>    gizmoRoots;
        glm::vec3           gizmoPrevT{0.0f}, gizmoPrevR{0.0f}, gizmoPrevS{1.0f};
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
        addB("autoWeather", autoWeather);      addF("weather", weather);
        addB("muted", muted);                  addF("volume", masterVolume);
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
        addF("mixAmbient", mixAmbient.level);   addB("mixAmbientMute", mixAmbient.mute);
        addF("mixSfx", mixSfx.level);           addB("mixSfxMute", mixSfx.mute);
        addF("timeOfDay", timeOfDay);          addF("dayLength", dayLength);
        addF("coverage", cloudCoverage);       addF("cloudDensity", cloudDensity);
        addF("cloudScale", cloudScale);        addF("cloudWind", cloudSpeed);
        addF("cloudBottom", cloudBottom);      addF("cloudTop", cloudTop);
        addF("cirrus", cirrusAmount);          addF("cirrusHeight", cirrusHeight);
        addF("cirrusWind", cirrusSpeed);
        // NOT "contrails": that name belongs to the vehicle trail toggle a few
        // lines up (addB, a bool). Two settings under one key write over each
        // other in the file, and whichever loses gets read back at the other's
        // type -- which is what threw json::type_error out of a scene load.
        addF("skyContrails", contrailAmount);
        addF("fogDensity", fogDensity);        addF("fogFalloff", fogFalloff);
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
        addF("exposure", exposure);            addF("bloom", bloomIntensity);
        addF("rays", rayIntensity);            addF("ssao", ssaoStrength);
        addF("ssaoRadius", ssaoRadius);        addF("ssaoBias", ssaoBias);
        addF("bloomThreshold", bloomThreshold); addF("bloomKnee", bloomKnee);
        addF("cascadeSplit", renderer.shadows().splitLambda);
        addI("envProbeRes", envProbeRes);      addI("envProbeFaces", envProbeFaces);
        addF("hue", hueShift);                 addF("saturation", saturation);
        addF("value", valueGain);              addF("warmth", warmth);
        addF("contrast", contrast);            addF("motionBlur", motionBlurStrength);
        addF("waterLevel", waterLevel);        addF("waveHeight", waveHeight);
        addF("waveChoppy", waveChoppy);        addF("waveStrength", waveStrength);
        addF("waveScale", waveScale);          addF("foamWidth", foamWidth);
        addF("waterColorR", waterColor.x);     addF("waterColorG", waterColor.y);
        addF("waterColorB", waterColor.z);
        addF("waterReflectivity", waterReflectivity); addF("waterClarity", waterClarity);
        addF("waterIor", waterIor);
        addF("cursorX", cursor3D.x); addF("cursorY", cursor3D.y); addF("cursorZ", cursor3D.z);
        addF("cursorGrid", cursorGrid);
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
            // Hand-painted grass: a compact space-separated float blob (7 per
            // blade). Stored as one JSON string so pretty-printing doesn't
            // explode into a line per number.
            std::ostringstream gs;
            gs.precision(7);
            for (float v : veg.paintedBlades) gs << v << ' ';
            j["paintedGrass"] = gs.str();
            // Tree species: name, LOD meshes, billboard config and per-species density.
            veg.serializeTrees(j);
            // Hand-painted trees: compact float blob (6 per tree: pos3, yaw, scale,
            // speciesIdx).
            std::ostringstream ts;
            ts.precision(7);
            for (float v : veg.paintedTrees) ts << v << ' ';
            j["paintedTrees2"] = ts.str();
            // Hand-painted flowers (8 per bloom: pos3, yaw, scale, rgb).
            std::ostringstream fs;
            fs.precision(7);
            for (float v : veg.paintedFlowers) fs << v << ' ';
            j["paintedFlowers"] = fs.str();
            // Terrain sculpt: grid spacing + a compact "ix iz delta ..." blob of
            // every edited cell (one JSON string, same reasoning as the grass).
            j["terrainEditCell"] = sculptWork.cell;
            std::ostringstream es;
            es.precision(7);
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
                es << ix << ' ' << iz << ' ' << own << ' ';
            }
            j["terrainEdits"] = es.str();

            // Terrain texture paint: grid spacing + an "ix iz r g b a ..." blob of
            // every painted cell's four layer weights (same compact-string scheme).
            j["terrainPaintCell"] = paintWork.cell;
            std::ostringstream ps;
            ps.precision(5);
            for (const auto& [k, w0] : paintWork.weights) {
                const glm::vec4 w = glm::max(w0 - rivers.minePaintAt(k),
                                             glm::vec4(0.0f));
                if (glm::all(glm::lessThan(w, glm::vec4(1e-4f)))) continue;
                const int ix = static_cast<int>(k >> 32);
                const int iz = static_cast<int>(
                    static_cast<std::int32_t>(static_cast<std::uint32_t>(k)));
                ps << ix << ' ' << iz << ' '
                   << w.x << ' ' << w.y << ' ' << w.z << ' ' << w.w << ' ';
            }
            j["terrainPaint"] = ps.str();

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
                    slotJson(md.normalTexId, md.normalTex, md.modelNormalTex,
                             "normalMap", ov[key]);
                    slotJson(md.emissionTexId, md.emissionTex, md.modelEmissionTex,
                             "emissionMap", ov[key]);
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
            for (const Setting& s : tunables) s.read(j);
            // The probe size is the one setting that owns GPU memory: push it
            // through, or the scene's value sits in the variable while the
            // renderer keeps the cubes it already had.
            renderer.setEnvProbeResolution(envProbeRes);
            envProbeRes = renderer.envProbeResolution(); // as clamped/rounded
            renderer.setEnvProbeMaxFaces(envProbeFaces);
            envProbeFaces = renderer.envProbeMaxFaces();
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
            // Restore hand-painted grass (empty for scenes saved before it existed).
            veg.paintedBlades.clear();
            if (j.contains("paintedGrass") && j["paintedGrass"].is_string()) {
                std::istringstream gs(j["paintedGrass"].get<std::string>());
                float v;
                while (gs >> v) veg.paintedBlades.push_back(v);
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
                std::istringstream ts(j["paintedTrees2"].get<std::string>());
                float v;
                while (ts >> v) veg.paintedTrees.push_back(v);
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
                std::istringstream fs(j["paintedFlowers"].get<std::string>());
                float v;
                while (fs >> v) veg.paintedFlowers.push_back(v);
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
                std::istringstream es(j["terrainEdits"].get<std::string>());
                int ix, iz; float d;
                while (es >> ix >> iz >> d)
                    sculptWork.deltas[TerrainEditField::cellKey(ix, iz)] = d;
            }
            publishSculpt();

            // Restore terrain texture paint (empty for older scenes). Publish before
            // the rebuild so the streamed chunks bake the weights into their vertices.
            paintWork.weights.clear();
            paintWork.cell = j.value("terrainPaintCell", 1.0f);
            if (j.contains("terrainPaint") && j["terrainPaint"].is_string()) {
                std::istringstream ps(j["terrainPaint"].get<std::string>());
                int ix, iz; glm::vec4 w;
                while (ps >> ix >> iz >> w.x >> w.y >> w.z >> w.w)
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
                        md.reflectivity  = e.value("reflectivity", md.reflectivity);
                        md.roughness     = e.value("roughness", md.roughness);
                        md.opacity       = e.value("opacity", md.opacity);
                        md.glass         = e.value("glass", md.glass);
                        md.ior           = e.value("ior", md.ior);
                        md.thickness     = e.value("thickness", md.thickness);
                        md.alphaMode     = static_cast<AlphaMode>(
                            e.value("alphaMode", static_cast<int>(md.alphaMode)));
                        md.alphaCutoff   = e.value("alphaCutoff", md.alphaCutoff);
                        md.emission      = rd3(e.value("emission", nlohmann::json{}), md.emission);
                        md.emissionStrength = e.value("emissionStrength", md.emissionStrength);
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
                            tex = assetDb.loadTexture(gid);
                        };
                        readSlot("texture", md.texId, md.tex);
                        // Reference only -- the bind pass in the frame loop opens
                        // it, the same as for .fmat materials.
                        if (e.contains("video") && e["video"].is_string())
                            md.videoId =
                                AssetId::fromString(e["video"].get<std::string>());
                        readSlot("normalMap", md.normalTexId, md.normalTex);
                        readSlot("emissionMap", md.emissionTexId, md.emissionTex);
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

        // --- Lua script editor (ImGuiColorTextEdit) --------------------------
#ifndef FITZEL_PLAYER
        TextEditor  luaEditor;
        luaEditor.SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
        luaEditor.SetPalette(TextEditor::GetDarkPalette());
        std::string editorPath;          // "scripts/<file>.lua" open ("" = none)
        bool        editorDirty = false;  // unsaved changes
        char        newScriptName[64] = "";
        int         newScriptTemplate = 0; // 0 = empty component, 1 = documented
        // Code-completion popup state for the Lua editor (see Completions): the
        // popup shows while there are matches and the editor is focused --
        // Tab/Enter accepts, arrows navigate, Esc dismisses. See the editor
        // window below.
        Completions comp;
#endif // !FITZEL_PLAYER
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
        // .lua files currently in the scripts dir (bare names, sorted).
        auto listScripts = [&](){
            std::vector<std::string> out;
            std::error_code ec;
            for (const auto& de :
                 std::filesystem::directory_iterator(scriptsDir(), ec))
                if (de.is_regular_file() && de.path().extension() == ".lua")
                    out.push_back(de.path().filename().string());
            std::sort(out.begin(), out.end());
            return out;
        };
        // Open a script (bare filename under the scripts dir) in the editor.
        auto openScript = [&](const std::string& file){
            if (file.empty()) return;
            const std::string path = scriptPath(file);
            std::ifstream in(path);
            std::stringstream ss; ss << in.rdbuf();
            luaEditor.SetText(ss.str());
            editorPath = path;
            editorDirty = false;
            showScriptEditor = true;
        };
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
        // Write the editor buffer back and reload the VM so Play picks it up.
        auto saveEditor = [&](){
            if (editorPath.empty()) return;
            std::ofstream out(editorPath);
            if (out) { out << luaEditor.GetText(); scripts.reset(); editorDirty = false; }
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
        std::vector<Entity>      playEntities;
        std::vector<MaterialDef> playMaterials;
        std::unique_ptr<PhysicsWorld> physics;      // rigid-body world during Play
        std::map<int, PhysicsBodyId>  physicsBody;  // entity id -> body handle
        // The entities that wobble instead of moving as one piece. Built with the
        // physics world at Play start and thrown away with it (see SoftBodySystem).
        SoftBodySystem                softBodies;
        // Knockable road side objects (posts/bollards): each a dynamic body created
        // at Play start, rendered from its live physics transform so a car bowls it
        // over. Rebuilt every Play; the derived static instances take over in the
        // editor. Holds what rendering needs: the body + which model at what scale.
        struct SidePost { PhysicsBodyId body; int modelId; float scale; };
        std::vector<SidePost> sidePosts;

        // --- Scene-vehicle drive helpers (see VehicleTool for the setup UI) ---
        // The nearest entity carrying a VehicleComponent, or -1.
        auto findNearestVehicle = [&]() -> int {
            int best = -1;
            float bestD = 1e30f;
            const glm::vec3 cp = camera.position();
            for (const Entity& e : entities) {
                if (!e.components.get<VehicleComponent>()) continue;
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
            physCarId = physics->addVehicle(
                glm::max(vc->chassisHalf, glm::vec3(0.05f)), vc->mass, sp, q,
                vc->wheelRadius, vc->wheelWidth, vc->halfTrack,
                vc->frontZ, vc->rearZ, vc->maxSteerDeg, vc->engineTorque, tuning);
            driveVehicleId = (physCarId != 0) ? id : -1;
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
                        physCarId = physics->addVehicle(
                            glm::vec3(0.9f, 0.35f, 2.0f), 1200.0f, sp, q,
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
            if (g >= 0 && playMode)
                racegrid::lineUp(entities, roads.active(), g, /*applyParticipation=*/true,
                                 driveGliderId2);
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
        std::unordered_map<int, unsigned char> keyPrev, mousePrev; // edge state
        std::vector<int>                 keyQ, mouseQ;              // queried this frame
        // Gameplay input, as scripts see it. An open menu overlay swallows it all
        // (the menu's own buttons are handled by the overlay, not by scripts), so
        // a paused game doesn't keep shooting while the player picks an option.
        host.keyDown      = [&](int kc){ return !uiMenuOpen && input.isKeyDown(kc); };
        host.keyPressed   = [&](int kc){ keyQ.push_back(kc);
                                         return !uiMenuOpen &&
                                                input.isKeyDown(kc) && !keyPrev[kc]; };
        host.mouseDown    = [&](int b){ return !uiMenuOpen && input.isMouseButtonDown(b); };
        host.mousePressed = [&](int b){ mouseQ.push_back(b);
                                        return !uiMenuOpen &&
                                               input.isMouseButtonDown(b) && !mousePrev[b]; };
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
            e.id       = entityCounter++;
            pendingSpawnVel[e.id] = s.vel;
            pendingSpawns.push_back(e);
            return e.id;
        };
        host.destroy = [&](int id){ pendingDestroy.push_back(id); };
        // Instantiate a prefab by name at a world position (yaw degrees). Mirrors
        // game.spawn: the whole subtree is queued into pendingSpawns and appears
        // next frame; returns the new root entity's id (0 on failure). The prefab
        // is loaded (and its models imported) once, then cached by name.
        host.spawnPrefab = [&](const std::string& name, glm::vec3 pos,
                               float yaw) -> int {
            if (currentProject.empty() || name.empty()) return 0;
            std::string key = name;
            for (char& c : key)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            auto it = prefabCache.find(key);
            if (it == prefabCache.end()) {
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
                    std::fprintf(stderr,
                        "[Fitzel] game.spawnPrefab: no prefab named '%s'\n", name.c_str());
                    return 0;
                }
                auto loaded = prefab::load(pio, path);
                if (!loaded || loaded->entities.empty()) {
                    std::fprintf(stderr,
                        "[Fitzel] game.spawnPrefab: failed to load '%s'\n", name.c_str());
                    return 0;
                }
                it = prefabCache.emplace(std::move(key), std::move(*loaded)).first;
            }
            std::vector<Entity> inst =
                prefab::instantiate(it->second, entityCounter, pos, yaw);
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
        host.applyImpulse = [&](int id, glm::vec3 j){
            auto it = physicsBody.find(id);
            if (physics && it != physicsBody.end()) physics->applyImpulse(it->second, j);
        };
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
        host.playSound = [&](const std::string& n){ audio.playOneShot(resolveSoundPath(n)); };
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
            voice.setVolume(muted ? 0.0f : masterVolume * glm::clamp(gain, 0.0f, 2.0f));
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
            v.setVolume(a->volume * mixAmbient.gain());
            v.play();
        };
        auto stopAudioSource = [&](int id) {
            auto it = audioVoices.find(id);
            if (it != audioVoices.end() && it->second.isValid()) it->second.stop();
        };
        host.playAudio = [&](int id){ startAudioSource(id); };
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
        host.setCamPos = [&](glm::vec3 p){ camera.setPosition(p); };
        host.setCamDir = [&](glm::vec3 d){
            if (glm::length(d) < 1e-5f) return;
            d = glm::normalize(d);
            camera.setYaw(glm::degrees(std::atan2(d.z, d.x)));
            camera.setPitch(glm::degrees(std::asin(glm::clamp(d.y, -1.0f, 1.0f))));
        };
        host.setCamFov = [&](float f){ camera.setFov(glm::clamp(f, 10.0f, 140.0f)); };
        host.setActiveCamera = [&](int id){ activeCam = id; };
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

        auto startPlay = [&] {
            if (playMode) return;
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

            // Physics: fresh world with the terrain as a static heightfield
            // ground, plus a rigid body per physics-tagged entity.
            physics = std::make_unique<PhysicsWorld>();
            physics->setGravity(glm::vec3(0.0f, -9.81f, 0.0f));
            // Fresh world: the previous collider id is void. Build the terrain
            // heightfield around the start position; it follows the focus below.
            terrainCollId = 0;
            refitTerrainCollision(glm::vec2(camera.position().x, camera.position().z));
            // Roads: every one in the scene, each with its own collider, its own
            // rails and posts and its own city. A hidden road is not there to be
            // driven on either, which is what makes the checkbox in the road list
            // a way to try a layout without deleting the other one.
            sidePosts.clear();
            for (const RoadSystem* rp : roads) {
                const RoadSystem& road = *rp;
                if (!road.enabled) continue;
                // A static triangle-mesh collider (from the last Build, graded
                // into the terrain), so the player and objects can walk/drive on it.
                if (road.collIndices().size() >= 3)
                    physics->addMesh(road.collVerts().data(),
                                     static_cast<int>(road.collVerts().size()),
                                     road.collIndices().data(),
                                     static_cast<int>(road.collIndices().size()));
                // Side objects collide as a box each, sized to the model's AABB. Rails
                // and curbs are static (mass 0) -- they stop a car driving off the edge.
                // Knockable lines (posts, bollards) are DYNAMIC, so the car bowls them
                // over; those are tracked in sidePosts and rendered from their live
                // transform below. The fresh physics world discards all of them when
                // Play stops, like the road mesh.
                for (const RoadSystem::SideBatch& batch : road.sideBatches()) {
                    LoadedModel* lm = resolveSideModel(batch.model);
                    if (!lm) continue;
                    for (const roadside::Instance& in : batch.instances) {
                        const glm::vec3 half =
                            glm::max(lm->size() * 0.5f * in.scale, glm::vec3(0.02f));
                        glm::vec3 c =
                            in.pos + glm::vec3(0.0f, half.y, 0.0f); // base on the ground
                        const glm::quat q = glm::angleAxis(in.yaw, glm::vec3(0, 1, 0));
                        if (batch.knockable) {
                            // Start a hair clear of the ground so the body settles
                            // onto it instead of being ejected out of a penetration
                            // (the physics heightfield is coarser than the terrain).
                            c.y += 0.03f;
                            const PhysicsBodyId id = physics->addBox(
                                half, c, q, glm::max(batch.mass, 0.1f));
                            if (id) sidePosts.push_back({id, lm->id, in.scale});
                        } else {
                            physics->addBox(half, c, q, 0.0f); // static
                        }
                    }
                }
                // The city's facades: a static box per piece the generator flagged as
                // solid (its masses and podium, not the bands, fins or signs), so a
                // vehicle crashes into a building instead of driving through it. That
                // is a handful per tower rather than one per part -- BuildingGen only
                // marks the load-bearing shapes -- which is what keeps a whole
                // district's collision affordable. Discarded with the physics world
                // when Play stops, like the road mesh.
                if (road.cityEnabled)
                    for (const city::Piece& pc : road.district().colliders) {
                        physics->addBox(glm::max(pc.half, glm::vec3(0.05f)), pc.center,
                                        glm::angleAxis(glm::radians(pc.yaw),
                                                       glm::vec3(0, 1, 0)),
                                        0.0f);
                    }
            }
            // Fences, walls and track: one static box per short run of path (see
            // splinegen::Collider). Coarse on purpose -- a car needs the wall to
            // be there, not to be able to thread the gap between two rails -- and
            // discarded with the physics world when Play stops, like the road's.
            for (const SplineSystem::Run& run : splines.runs())
                for (const splinegen::Collider& col : run.geo.colliders)
                    physics->addBox(glm::max(col.half, glm::vec3(0.02f)), col.center,
                                    glm::angleAxis(glm::radians(col.yaw),
                                                   glm::vec3(0, 1, 0)),
                                    0.0f);

            skids.clear(); // no skid marks carry over from a previous Play session
            trails.clear(); // ...nor stale contrails
            particles.clear(); // ...nor a cloud of smoke from the last run
            weapons.reset();   // ...nor a missile still in the air, or a lock
            weapons2.reset();  // ...for either seat
            // Player two starts from scratch too, and gets re-seated: the craft
            // it flew belongs to the scene that is being restarted.
            race2 = racesim::RaceState{};
            driveGliderId2 = -1;
            physicsBody.clear();
            for (Entity& e : entities) {
                const auto* pc = e.components.get<PhysicsComponent>();
                if (!pc || !e.activeInHierarchy ||
                    e.type == EntityType::Light || e.type == EntityType::Sun)
                    continue;
                // Opponents are kinematic (driven along the road each frame), so
                // they must never get a dynamic body -- one would be flung by the
                // solver (e.g. spawning inside the terrain) and fight the tick.
                if (e.components.get<OpponentComponent>()) continue;
                // A soft body IS this entity's physics; a rigid collider beside it
                // would be a second, differently shaped copy fighting the first.
                if (e.components.get<SoftBodyComponent>()) continue;
                const float m = pc->dynamic ? glm::max(pc->mass, 0.01f) : 0.0f;
                const glm::quat q = glm::quat(glm::radians(e.rotation));
                PhysicsBodyId id = 0;
                switch (e.type) {
                    case EntityType::Sphere:
                        id = physics->addSphere(
                            (e.half.x + e.half.y + e.half.z) / 3.0f, e.center, m);
                        break;
                    case EntityType::Cylinder:
                        id = physics->addCylinder(e.half.x, e.half.y, e.center, q, m);
                        break;
                    case EntityType::Ramp: {
                        // Triangular-prism wedge: rises along +Z (front-bottom to
                        // back-top), matching the ramp mesh and the walk collider.
                        const glm::vec3 h = e.half;
                        const glm::vec3 pts[6] = {
                            {-h.x, -h.y, -h.z}, { h.x, -h.y, -h.z},
                            {-h.x, -h.y,  h.z}, { h.x, -h.y,  h.z},
                            {-h.x,  h.y,  h.z}, { h.x,  h.y,  h.z}};
                        id = physics->addConvexHull(pts, 6, e.center, q, m);
                        break;
                    }
                    case EntityType::Model: {
                        // Convex hull of the model's vertices (centred + scaled to
                        // match the render), falling back to the AABB box.
                        const auto* mdl = e.components.get<ModelComponent>();
                        LoadedModel* lm = mdl ? models.byId(mdl->modelId) : nullptr;
                        if (lm && lm->hullPoints.size() >= 4) {
                            const glm::vec3 c = lm->center();
                            std::vector<glm::vec3> pts;
                            pts.reserve(lm->hullPoints.size());
                            for (const glm::vec3& v : lm->hullPoints)
                                pts.push_back((v - c) * mdl->scale);
                            id = physics->addConvexHull(
                                pts.data(), static_cast<int>(pts.size()),
                                e.center, q, m);
                        }
                        if (!id) id = physics->addBox(e.half, e.center, q, m);
                        break;
                    }
                    default: // Box
                        id = physics->addBox(e.half, e.center, q, m);
                        break;
                }
                if (id) physicsBody[e.id] = id;
            }
            // Jelly, balloons and cloth. After the loop above and after the world's
            // static geometry, so a soft body lands ON the ground rather than being
            // squeezed out of it on its first step.
            softBodies.spawn(entities, *physics);
            // The player is a physics capsule (~1.8 m tall). It spawns at the
            // first entity carrying a PlayerStart component (adopting its facing
            // and move speed); otherwise at the edit camera.
            glm::vec3 startPos = camera.position();
            for (const Entity& e : entities)
                if (const auto* ps = e.components.get<PlayerStartComponent>()) {
                    startPos = e.center;
                    camera.setYaw(e.rotation.y);
                    camera.moveSpeed = ps->moveSpeed;
                    break;
                }
            physics->spawnCharacter(0.3f, 0.6f,
                glm::vec3(startPos.x, streamer.heightAt(startPos.x, startPos.z), startPos.z));

            fpsMode        = true; // play as the walking player
            input.setCursorLocked(true);
            fpsVelY = 0.0f;
            camera.setPosition({startPos.x,
                streamer.heightAt(startPos.x, startPos.z) + eyeHeight, startPos.z});

            // Auto-start every AudioSource flagged play-on-start (music/ambient),
            // unless the object (or an ancestor) is deactivated.
            for (const Entity& e : entities)
                if (const auto* a = e.components.get<AudioSourceComponent>();
                    a && a->playOnStart && e.activeInHierarchy)
                    startAudioSource(e.id);

            // --- What the game starts as -------------------------------------
            // The walking player is set up above and is the fallback for
            // everything here, which is deliberate: every other mode needs the
            // scene to provide something (a vehicle, a glider, a camera), and a
            // game that cannot start the way it was configured should start in
            // the way that always works rather than not start at all.
            //
            // Read from the project's game.json rather than from the copy the
            // dialog edits, for the same reason the loading screen is re-read on
            // every level change: it is one small file, and reading it here means
            // a change in the dialog is in effect on the very next Play instead
            // of after a restart.
            game::StartMode startAs = game::StartMode::Fps;
            if (!currentProject.empty())
                startAs = game::load(std::filesystem::path(currentProject)
                                         .parent_path().generic_string()).startMode;
            // A scene from before the setting moved (see legacyStartVehicle).
            // Only while the game says nothing else, so the dialog always wins.
            if (startAs == game::StartMode::Fps) {
                if (legacyStartVehicle)     startAs = game::StartMode::Vehicle;
                else if (legacyStartGlider) startAs = game::StartMode::Glider;
            }

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
                // scene has none, which then leaves the walking player).
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

            // A showroom scene is a start screen, not a level: no walking player
            // and no locked cursor -- the picker owns the frame, and it takes the
            // scene over from here (arranging the craft, driving the camera).
            // Checked last so it overrules whichever start mode ran above.
            if (showroom::Showroom::isShowroomScene(entities)) {
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
            // Hand the craft and the camera back before the snapshot restore.
            showroomUi.end(entities, camera);
            playMode  = false;
            uiMenuOpen = false;     // back to the editor: the scene's menu is gone
            vehicleMode = false;    // the physics car is gone with the world
            driveVehicleId = -1;    // the scene restore below un-drives the model
            gliderMode = false;     // stop flying; the scene restore un-flies the craft
            driveGliderId = -1;
            gliderDriveActive = false; gliderBackup.clear(); // Play restore owns the transform
            skids.clear();          // drop skid marks so they don't linger in the editor
            trails.clear();         // and the contrails
            weapons.reset();        // and anything the launcher still had in the air
            weapons2.reset();
            terrainCollId = 0;      // the collider dies with the world below
            physics.reset();
            physicsBody.clear();
            softBodies.clear();  // the particles died with the world
            zoneSounds.clear(); // stop + free any looping TriggerSound voices
            audioVoices.clear(); // stop + free any AudioSource voices
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

#ifndef FITZEL_PLAYER
        // The menu bar's slice of the state above, gathered once (everything it
        // names lives for the whole loop) and redrawn from every frame.
        FileMenuCtx fileMenu{
            window, currentProject, prefLocation, recentProjects, exportStatus,
            autoSave.status(), projNameBuf,
            wizName, sizeof(wizName), wizLocation, sizeof(wizLocation),
            wizardOpen, wizardIsNew, gameSettings, gameSettingsOpen,
            saveCurrent, exportGame, openProjectAsync, listProjectsIn,
        };
        SceneMenuCtx sceneMenu{
            currentProject, sceneNameBuf, sizeof(sceneNameBuf),
            sceneNewOpen, sceneRenameOpen, sceneDeleteOpen,
            saveSceneFile, loadSceneAsync, listScenesIn,
        };
        EditMenuCtx editMenu{
            history, document, entities, sel,
            prefabNameBuf, sizeof(prefabNameBuf), showPrefabs,
            clampRoadSel, clampSplineSel, clampRiverSel,
            duplicateSelection, deleteSelection,
        };
        // The View menu, as data (see PanelEntry). "Close all panels" walks this
        // same table, so it can no longer fall behind the menu.
        const std::vector<PanelEntry> viewPanels = {
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
            {"Planting", "Vegetation",         nullptr, &showVegetation},
            {"Planting", "Scatter",            nullptr, &showScatter},
            {"Track",    "Roads",              nullptr, &showRoads},
            {"Track",    "Splines",            nullptr, &showSplines},
            {"Track",    "City",               nullptr, &showCity},
            {"Track",    "Buildings",          nullptr, &showBuildings},
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
            {"Objects",  nullptr,              nullptr, nullptr},
            {"Objects",  "3D cursor",          nullptr, &showCursor},
            {"Objects",  "Grid",               nullptr, &showGrid, false},
            {"Assets",   "Materials",          nullptr, &showMaterials},
            {"Assets",   "Models",             nullptr, &showModels},
            {"Assets",   "Prefabs",            nullptr, &showPrefabs},
            {"Assets",   "Assets",             nullptr, &showAssets},
            {"Assets",   "Scripts",            nullptr, &showScriptEditor},
            {"Assets",   nullptr,              nullptr, nullptr},
            {"Assets",   "Import Unity asset", nullptr, &showUnityImport},
            {"Presentation", "UI Overlay",     nullptr, &showUiOverlay},
            {"Presentation", "Camera",         nullptr, &showCamera},
            {"Presentation", "Camera path",    nullptr, &showCamPath},
            {"Presentation", "Render",         nullptr, &pathRender.open},
            {"Presentation", "Mixer",          nullptr, &showMixer},
            {"Inspect",  "Performance",        "F3",    &showPerf},
            {"Inspect",  "Stats",              nullptr, &showStats},
        };
#endif


        // --- Benchmark mode ---------------------------------------------------
        // See BootConfig::profilePath. Vsync goes off for the run: a frame that
        // waits for the display measures the display.
        double profileStart = 0.0;
        if (!boot.profilePath.empty()) glfwSwapInterval(0);
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
                << prof::history().size() << " frames" << "\n" << "\n";
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

        while (window.isOpen()) {
            const bool uncapped = playMode || playerMode || camAnimating;
            if (uncapped) {
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
                              !pendingSnapshot.valid(),
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
                // The camera child of `owner`, or -1. First one wins; a craft with
                // two is an authoring mistake, not a mode.
                const auto childCam = [&](int owner) {
                    if (owner < 0) return -1;
                    for (const Entity& e : entities)
                        if (e.parent == owner && e.activeInHierarchy &&
                            e.components.get<CameraComponent>())
                            return e.id;
                    return -1;
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
                    camera.setFov(playCamFov);
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

            // Hot reload: pick up on-disk asset edits ~twice a second. Textures
            // and models reload in place (existing handles update automatically);
            // edited/added/removed materials refresh the project's library.
            if (now >= nextAssetPoll) {
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

            // --- Input ---------------------------------------------------
            // F frames the selected object; Shift+F toggles first-person walk mode.
            const bool fDown  = input.isKeyDown(GLFW_KEY_F);
            const bool shiftF = input.isKeyDown(GLFW_KEY_LEFT_SHIFT) ||
                                input.isKeyDown(GLFW_KEY_RIGHT_SHIFT);
            if (fDown && !prevF && !vehicleMode && !gliderMode && !ImGui::GetIO().WantTextInput) {
                if (shiftF) { // Shift+F: toggle first-person (cursor locks, mouse-look)
                    fpsMode = !fpsMode;
                    input.setCursorLocked(fpsMode);
                    fpsVelY = 0.0f;
                    if (fpsMode) { // drop to standing height immediately
                        const glm::vec3 p = camera.position();
                        camera.setPosition({p.x, streamer.heightAt(p.x, p.z) + eyeHeight, p.z});
                    }
                } else if (!fpsMode && sel.valid()) {
                    // Focus: keep the view direction, back off to fit the object,
                    // and glide there smoothly (applied each frame below).
                    const Entity& e = entities[sel.index()];
                    const float radius = glm::max(glm::length(e.half), 0.25f);
                    const float fov    = glm::radians(glm::max(camera.fov(), 1.0f));
                    const float dist   = radius / std::max(std::tan(fov * 0.5f), 0.05f) * 1.3f;
                    camFocusTarget = e.center - camera.front() * dist;
                    camFocusing    = true;
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
            if (vDown && !prevV && !ImGui::GetIO().WantTextInput) {
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

            // G toggles the fly-a-glider mode: take the nearest glider (a model
            // with a Glider component) and fly it with the arcade hover sim, in
            // the editor or in Play. Mutually exclusive with the car's drive mode.
            const bool gDown = input.isKeyDown(GLFW_KEY_G);
            if (gDown && !prevG && !ImGui::GetIO().WantTextInput) {
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
                    input.setCursorLocked(uiOverlay.runtimeVisible() ? false : fpsMode);
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
                    input.setCursorLocked(gfxUi.open() ? false : fpsMode);
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
            if (escDown && !prevEsc && !escIsMenuKey && !gfxUi.open()) {
                if (playerMode)          { window.requestClose(); }
                else if (presentMode) {
                    presentMode = false;
                    window.setFullscreen(false);
                } else if (playMode)     { stopPlay(); }
                else if (vehicleMode)    { vehicleMode = false; endEditorDrive(); }
                else if (gliderMode)     { gliderMode = false; endGliderDrive(); }
                else if (fpsMode) { fpsMode = false; input.setCursorLocked(false); }
                // Plain editor: Esc steps back to selection (drop the transform
                // tool), then a second Esc clears the selection. Never quits.
                // A road point selection is the innermost thing to let go of, so
                // it clears first -- the bridge pair with it.
                else if (roadEditMode && roadSel >= 0) { roadSel = roadSel2 = -1; }
                else if (splineEditMode && splinePtSel >= 0) { splinePtSel = -1; }
                else if (riverEditMode && riverPtSel >= 0) { riverPtSel = -1; }
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
                !ImGui::GetIO().WantTextInput &&
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
                    physics->setVehicleInput(fwdIn, physSteer, brake, handBrake);
                    engineThrottle = std::abs(fwdIn);
                }

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
                            0.5f + std::abs(vel.y) * 0.15f, 0.5f, 1.0f) * mixSfx.gain());
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
                auto wallHit = [&](const Entity& b, float px, float pz) {
                    if (b.type != EntityType::Box && b.type != EntityType::Cylinder &&
                        b.type != EntityType::Sphere) return false;
                    if (bodyHi <= b.center.y - b.half.y || bodyLo >= b.center.y + b.half.y) return false;
                    if (px + pr <= b.center.x - b.half.x || px - pr >= b.center.x + b.half.x) return false;
                    if (pz + pr <= b.center.z - b.half.z || pz - pr >= b.center.z + b.half.z) return false;
                    return true;
                };
                float nx = pos.x + mvx; // move X, then Z -> slide along faces
                for (const Entity& b : entities)
                    if (wallHit(b, nx, pos.z))
                        nx = (mvx > 0.0f) ? b.center.x - b.half.x - pr : b.center.x + b.half.x + pr;
                pos.x = nx;
                float nz = pos.z + mvz;
                for (const Entity& b : entities)
                    if (wallHit(b, pos.x, nz))
                        nz = (mvz > 0.0f) ? b.center.z - b.half.z - pr : b.center.z + b.half.z + pr;
                pos.z = nz;

                // Ground = terrain, raised to the top of any block we stand over.
                float groundY = streamer.heightAt(pos.x, pos.z);
                for (const Entity& b : entities) {
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
                                       && !shiftHeld
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
                if (viewportHovered || presentMode) camera.processScroll(input.scrollDelta());
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
                    nav.viewportHovered = viewportHovered || presentMode;
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

            // Regrow grass (async) / trees when the camera has moved far enough.
            {
                const glm::vec2 camXZ(camera.position().x, camera.position().z);
                // Every road's centreline as one polyline, the runs separated by a
                // break marker so nothing mows a strip between two of them; the
                // clearance is the widest road's, which is the only number this
                // interface has room for.
                const std::vector<glm::vec2> cls = roads.centerlines();
                const float roadW = roads.maxWidth();
                if (veg.updateGrass(camXZ, cls, roadW * 0.5f + 1.5f,
                                    waterLevel, look.snowLevel) && veg.flowerEnabled)
                    veg.regenFlowers(veg.grassCenter(), cls, roadW,
                                     waterLevel, look.snowLevel);
                veg.updateFlowers(); // finish + upload a pending async flower regen
                veg.updateTrees(camXZ, cls, roadW, waterLevel, look.snowLevel);
            }

            // --- Weather: drift (auto) and derive storm parameters ----------
            if (autoWeather) {
                const float target = glm::clamp(
                    0.5f + 0.42f * std::sin(static_cast<float>(now) * 0.018f)
                         + 0.18f * std::sin(static_cast<float>(now) * 0.011f + 2.1f),
                    0.0f, 1.0f);
                weather += (target - weather) * std::min(1.0f, dt * 0.3f);
            }
            weather = glm::clamp(weather, 0.0f, 1.0f);

            const float effCoverage  = glm::mix(cloudCoverage, 0.97f, weather);
            const float effDensity   = glm::mix(cloudDensity, 2.7f, weather);
            const float effWind      = glm::mix(cloudSpeed, 26.0f, weather);
            const float effCloudBot  = glm::mix(cloudBottom, 80.0f, weather);
            const float effWaveH     = glm::mix(waveHeight, 2.4f, weather);
            const float effWaveC     = glm::mix(waveChoppy, 0.95f, weather);
            const float effFog       = fogDensity + weather * 0.011f;
            // Same curve the streaks fall on -- shared so the sound and the road's
            // wet sheen can't start before there is anything coming down.
            const float rainIntensity = rainIntensityFor(weather);
            const float lightDim     = glm::mix(1.0f, 0.30f, weather);
            // Drop impacts on the carriageway. Tied to the rain, not to `roadWetness`:
            // the road stays wet for ~20s after a shower and nothing should still be
            // landing on it then. Needs a wet surface too -- rings on dry tarmac read
            // as dents. Scaled by the road's own dial (see the Roads panel).
            // The weather's half of the drop-impact rings. Each road multiplies
            // its own `rainRings` onto this when it is drawn -- the strength is a
            // property of the surface, and two roads in one scene do not have to
            // agree about it.
            const float ringWeather =
                rainIntensity * glm::min(roadWetness * 2.0f, 1.0f);

            // Wetness eases toward the rain intensity: quick to soak (~2s), slow to
            // dry (~20s), so surfaces glisten for a while after the rain stops.
            {
                const float wetTau = (rainIntensity > roadWetness) ? 2.0f : 20.0f;
                roadWetness += (rainIntensity - roadWetness) *
                               (1.0f - std::exp(-dt / wetTau));
                roadWetness = glm::clamp(roadWetness, 0.0f, 1.0f);
            }

            // Lightning: brief flashes once the storm is strong.
            float flash = 0.0f;
            if (weather > 0.5f) {
                const float ft  = static_cast<float>(now) * 0.55f;
                const float rnd = glm::fract(std::sin(std::floor(ft) * 127.1f) * 43758.5f);
                if (rnd > 0.9f) {
                    flash = std::exp(-glm::fract(ft) * 7.0f) * (weather - 0.5f) * 2.0f;
                }
            }

            // Weather audio: cross-fade the looping layers, fire thunder on a
            // fresh lightning flash. Only audible while playing -- the editor
            // stays silent.
            // Mixer routing: Master to the device, SFX to the one-shot bus,
            // Ambient scales the looping weather layers.
            audio.setMasterVolume(muted ? 0.0f : masterVolume);
            audio.setSfxVolume(mixSfx.gain());
            const float amb = mixAmbient.gain();
            rainSnd.setVolume(playMode ? rainIntensity * amb : 0.0f);
            windSnd.setVolume(playMode ? glm::smoothstep(0.15f, 1.0f, weather) * 0.9f * amb : 0.0f);
            breezeSnd.setVolume(playMode ? (1.0f - glm::smoothstep(0.0f, 0.5f, weather)) * 0.5f * amb : 0.0f);
            // Water ambience: louder the deeper the car is submerged (SFX bus).
            waterSnd.setVolume(playMode ? glm::clamp(carWaterSub, 0.0f, 1.0f) * mixSfx.gain() : 0.0f);
            // Storm bed: fades in as the weather peaks (ambient bus).
            stormSnd.setVolume(playMode ? glm::smoothstep(0.5f, 0.95f, weather) * amb : 0.0f);
            const bool flashOn = flash > 0.25f;
            if (playMode && flashOn && !prevFlashOn) {
                thunderSnd.setVolume(glm::clamp(weather, 0.3f, 1.0f) * amb);
                thunderSnd.play();
            }
            prevFlashOn = flashOn;

            // Engine sound: run the RPM-layered loops + auto gearbox while a car
            // is being driven; silence (and reset the box) the moment it stops.
            if (engineDriving) {
                if (!carAudio.running()) carAudio.start();
                carAudio.update(dt, engineSpeedMps, engineThrottle, engineWheelR,
                                mixSfx.gain());
            } else if (carAudio.running()) {
                carAudio.stop();
            }

            // Glider jet thruster: whine + roar layers spooled by speed/throttle
            // while flying; silenced the moment flight ends.
            if (gliderAudioActive) {
                if (!gliderAudio.running()) gliderAudio.start();
                gliderAudio.update(dt, gliderSpeedMps, gliderTopSpeed, gliderThrottle,
                                   mixSfx.gain());
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
                                  driveGliderId, driveGliderId2, mixSfx.gain());
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
                worldAudio.setAmbience(amb, mixSfx.gain());
            } else {
                worldAudio.setAmbience({}, 0.0f);
            }

            // --- Day/night: advance time, derive sun direction and lighting ---
            if (!timePaused && dayLength > 0.1f) {
                timeOfDay += dt * (24.0f / dayLength);
                timeOfDay = std::fmod(timeOfDay, 24.0f);
            }
            const float phi = (timeOfDay / 24.0f) * 6.2831853f - 1.5707963f;
            const glm::vec3 sunDir =
                glm::normalize(glm::vec3(std::cos(phi), std::sin(phi), 0.18f));
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
            light.ambient = glm::mix(glm::vec3(0.015f, 0.02f, 0.04f),
                                     glm::vec3(0.12f, 0.14f, 0.18f), dayF);
            // Overcast: dimmer, greyer, cooler ambient.
            light.ambient = glm::mix(light.ambient,
                                     glm::vec3(0.05f, 0.06f, 0.08f), weather * 0.7f);
            // Lightning flash lights the scene briefly.
            light.color   += glm::vec3(0.8f, 0.85f, 1.0f) * (flash * 6.0f);
            light.ambient += glm::vec3(0.5f, 0.55f, 0.7f) * flash;
            renderer.setExposure(exposure);

            // Atmospheric fog, tinted by time of day to match the sky horizon.
            // Colours are authored in sRGB and linearised for the linear-space
            // blend (tonemapping converts back on output).
            Fog fog;
            fog.height        = waterLevel;
            fog.density       = effFog;
            fog.heightFalloff = fogFalloff;
            // Brighter, slightly warmer daytime haze so the distance reads as soft
            // atmosphere (like the reference) rather than a cool blue wash.
            const glm::vec3 hazeDisp =
                glm::mix(glm::vec3(0.03f, 0.04f, 0.09f), glm::vec3(0.76f, 0.82f, 0.90f), dayF);
            const glm::vec3 sunHazeDisp =
                glm::mix(hazeDisp, glm::vec3(1.0f, 0.66f, 0.38f), 0.7f * dayF);
            fog.color    = glm::pow(hazeDisp, glm::vec3(2.2f));
            fog.sunColor = glm::pow(sunHazeDisp, glm::vec3(2.2f));
            renderer.setFog(fog);
            renderer.setEnvironmentIBL(&environment, iblEnabled, iblIntensity);

            // --- Physics: step the world, sync dynamic bodies back to entities -
            if (playMode && physics) {
                physics->step(dt);
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
                        for (int i = 0; i < 4; ++i) {
                            Entity* we = document.find(vc->wheelId[i]);
                            glm::vec3 wp; glm::quat wq;
                            if (!we || !physics->getWheelTransform(i, wp, wq)) continue;
                            const glm::mat4 wm =
                                glm::translate(glm::mat4(1.0f), wp) * glm::mat4_cast(wq);
                            ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(wm), t, r, s);
                            const glm::mat4 pww = parentWorldMat(*we);
                            setWorld(*we, glm::vec3(t[0], t[1], t[2]),
                                     glm::vec3(r[0], r[1], r[2]),
                                     we->parent >= 0 ? &pww : nullptr);
                        }
                    }
                }
            }

            // --- Scripts: tick each scripted entity's Lua update while playing --
            if (playMode) {
                host.camPos = camera.position();
                host.camDir = camera.front();
                host.screen = glm::vec2(static_cast<float>(viewW),
                                        static_cast<float>(viewH));
                // Scripts and behaviours just write the entity's world transform;
                // children follow via resolveHierarchy (below), no propagation.
                for (Entity& e : entities)
                    if (e.type != EntityType::Sun && e.activeInHierarchy)
                        if (auto* sc = e.components.get<ScriptComponent>();
                            sc && !sc->file.empty())
                            scripts.update(e, scriptPath(sc->file), dt,
                                           static_cast<float>(now));

                // Built-in component behaviours (data-authored, no code): Spin.
                // Writes LOCAL rotation; the scene-graph derives world (so a
                // spinning child of a spinning parent orbits AND spins).
                for (Entity& e : entities)
                    if (e.activeInHierarchy)
                        for (const auto& c : e.components.items)
                            if (auto* sp = dynamic_cast<SpinComponent*>(c.get()))
                                e.localRotation += sp->axis * sp->speed * dt;

                // Missile pickups counting back to their respawn. Deliberately
                // OUTSIDE the activeInHierarchy loop below: a taken pickup
                // deactivates itself, and a deactivated entity is skipped there,
                // so ticking it in that loop would freeze its timer and it would
                // never come back.
                for (Entity& e : entities) {
                    auto* mp = e.components.get<MissilePickupComponent>();
                    if (!mp || mp->cooldown <= 0.0f) continue;
                    mp->cooldown -= dt;
                    if (mp->cooldown <= 0.0f) {
                        mp->cooldown = 0.0f;
                        e.active     = true; // back on the track for the next lap
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
                    for (Entity& e : entities) {
                        if (!e.activeInHierarchy) continue;  // deactivated: inert
                        // Collectible: on reach, award points, play sound, remove
                        // (destroy is deferred to the queue processed below).
                        if (const auto* col = e.components.get<CollectibleComponent>()) {
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
                        // Trigger: on entry (edge), set the HUD message / play the
                        // sound. `once` latches via the transient `fired` flag.
                        if (auto* tr = e.components.get<TriggerComponent>()) {
                            const bool inside = glm::distance(playerC, e.center) <= tr->radius;
                            if (inside && !tr->insideLast && !(tr->once && tr->fired)) {
                                tr->fired = true;
                                if (!tr->message.empty()) host.hud = tr->message;
                                if (!tr->sound.empty()) host.playSound(tr->sound);
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
                                    voice.setVolume(ts->volume * fall * mixAmbient.gain());
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
                                float vol = as->volume * mixAmbient.gain();
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
                        const glm::quat q = glm::quat(glm::radians(e.rotation));
                        PhysicsBodyId id = 0;
                        switch (e.type) {
                            case EntityType::Sphere:
                                id = physics->addSphere(
                                    (e.half.x + e.half.y + e.half.z) / 3.0f, e.center, m);
                                break;
                            case EntityType::Cylinder:
                                id = physics->addCylinder(e.half.x, e.half.y, e.center, q, m);
                                break;
                            default:
                                id = physics->addBox(e.half, e.center, q, m);
                                break;
                        }
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
                        const auto childCam = [&](int parentId) {
                            for (Entity& e : entities)
                                if (e.parent == parentId &&
                                    e.components.get<CameraComponent>())
                                    return &e;
                            return static_cast<Entity*>(nullptr);
                        };
                        if (!childCam(rootId))
                            if (Entity* sc = childCam(slot)) sc->parent = rootId;
                        // Player two needs an eye of its own, and there is only
                        // one camera to hand: copy player one's onto its craft.
                        // Without this the second pane has nothing to draw from
                        // and the screen quietly stays whole -- which would look
                        // like the start screen's two-player choice being
                        // ignored.
                        if (rootId2 >= 0 && !childCam(rootId2))
                            if (const Entity* src = childCam(rootId)) {
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

            // --- UI ------------------------------------------------------
            const long long fzUiMark = prof::mark();
            gui.beginFrame();
            ImGuizmo::BeginFrame();
            if (presentMode) {
                // Presentation: hide the editor UI, render the scene full-window.
                window.framebufferSize(viewW, viewH);
                viewportHovered = true;
            }
#ifndef FITZEL_PLAYER
            else {
            // --- Main menu bar (File / Scene / Edit / View / Help) -------
            if (ImGui::BeginMainMenuBar()) {
                drawFileMenu(fileMenu);
                drawSceneMenu(sceneMenu);
                drawEditMenu(editMenu);
                drawViewMenu(gui, viewPanels, viewNav, prefsDirty, requestDockRebuild);
                if (ImGui::BeginMenu("Help")) {
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

            // --- Toolbar strip under the menu bar: primitive-creation icons.
            //     A viewport side bar reserves space at the top of the work area,
            //     so the dockspace below shifts down automatically. It starts
            //     with the Select/Create pair (what a viewport click does), then
            //     the shapes: clicking one makes that type the active one. The
            //     pictures themselves are painted by icon:: -- see there.
            {
                ImGuiViewport* tvp = ImGui::GetMainViewport();
                const float bh   = 26.0f;
                const float barH = bh + ImGui::GetStyle().WindowPadding.y * 2.0f + 2.0f;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
                const bool barOpen = ImGui::BeginViewportSideBar(
                    "##PrimToolbar", tvp, ImGuiDir_Up, barH,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration);
                if (barOpen) {
                    ImDrawList*  dl = ImGui::GetWindowDrawList();
                    const ImVec2 bs(bh, bh);
                    const float  r  = 8.0f;
                    ImVec2       c;
                    auto gap = [&]{ ImGui::Dummy(ImVec2(10.0f, 1.0f)); ImGui::SameLine(); };

                    // --- Select / Create ---------------------------------
                    // The pair that decides what a left-click on empty ground
                    // does. Select is the default and the harmless one: it can
                    // only pick and deselect. Create is the one that drops
                    // objects, and you have to ask for it -- otherwise every
                    // stray click while looking around litters the scene with
                    // boxes. Esc steps back out of Create.
                    auto modeToggle = [&](bool create, const char* tip) {
                        const bool hit = iconButton(create ? "modeCreate" : "modeSelect",
                                                    bs, tip, false, c);
                        icon::pointer(dl, create, c, r,
                                      placeMode == create ? icon::kOn : icon::kOff);
                        if (hit) placeMode = create;
                    };
                    modeToggle(false, "Select -- a click picks objects and never creates one (Esc)");
                    modeToggle(true,  "Create -- a click on empty ground drops the chosen shape");
                    gap();

                    auto shapeBtn = [&](EntityType t, const char* id, const char* tip) {
                        const bool hit = iconButton(id, bs, tip, false, c);
                        icon::shape(dl, t, c, r,
                                    entityNewType == t ? icon::kOn : icon::kOff);
                        if (hit) {
                            entityNewType = t;
                            // In Select mode the button itself is the create
                            // action, so it drops one in front of the camera. In
                            // Create mode it only arms the shape -- there the
                            // click in the viewport is what places it, and
                            // getting two objects out of one click surprises.
                            if (!placeMode) {
                                const glm::vec3 pp = camera.position() + camera.front() * 6.0f;
                                addEntity(glm::vec3(pp.x, streamer.heightAt(pp.x, pp.z), pp.z), t);
                            }
                        }
                    };
                    shapeBtn(EntityType::Box,      "shapeBox",      "Box");
                    shapeBtn(EntityType::Ramp,     "shapeRamp",     "Ramp");
                    shapeBtn(EntityType::Cylinder, "shapeCylinder", "Cylinder");
                    shapeBtn(EntityType::Sphere,   "shapeSphere",   "Sphere");
                    shapeBtn(EntityType::Plane,    "shapePlane",
                             "Plane -- a flat quad, for floors, walls and backdrops");
                    shapeBtn(EntityType::Light,    "shapeLight",    "Light");
                    shapeBtn(EntityType::Empty,    "shapeEmpty",
                             "Empty (transform-only grouping node)");

                    // Terrain: an object like any other, so it is added like any
                    // other. Not a shapeBtn -- it is a component on an Empty, not
                    // an entity type -- and disabled once the scene has ground,
                    // since a scene has one terrain.
                    if (iconButton("terrainAdd", bs,
                                   terrainOn ? "Terrain (the scene already has one)"
                                             : "Terrain (adds ground to the scene)",
                                   terrainOn, c))
                        addTerrainEntity();
                    icon::terrain(dl, c, r, terrainOn ? icon::kDim : icon::kOff);

                    // Gap, then the transform-gizmo modes (Q/W/E).
                    gap();
                    auto modeBtn = [&](ImGuizmo::OPERATION op, const char* id,
                                       const char* tip) {
                        const bool hit = iconButton(id, bs, tip, false, c);
                        icon::gizmo(dl, op, c, r, gizmoOp == op ? icon::kOn : icon::kOff);
                        if (hit) gizmoOp = op;
                    };
                    modeBtn(ImGuizmo::TRANSLATE, "gizmoMove",   "Move (Q)");
                    modeBtn(ImGuizmo::ROTATE,    "gizmoRotate", "Rotate (W)");
                    modeBtn(ImGuizmo::SCALE,     "gizmoScale",  "Scale (E)");

                    // Gap, then the gizmo reference-frame toggle (local vs world).
                    gap();
                    {
                        const bool isLocal = (gizmoMode == ImGuizmo::LOCAL);
                        char tip[64];
                        std::snprintf(tip, sizeof tip, "Gizmo space: %s  (X to toggle)",
                                      isLocal ? "Local" : "World");
                        const bool hit = iconButton("gizmoSpace", bs, tip, false, c);
                        icon::gizmoSpace(dl, isLocal, c, r, icon::kOn);
                        if (hit) gizmoMode = isLocal ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
                    }

                    // Gap, then how the viewport DRAWS the scene. Not a tool:
                    // nothing here changes the scene, only what is shown of it,
                    // which is why the group sits apart from the ones that edit.
                    gap();
                    {
                        struct ShadeBtn { int mode; const char* id; const char* tip; };
                        static const ShadeBtn kShades[] = {
                            {kShadeWireframe, "shadeWire",
                             "Wireframe -- edges only, and you can see through it"},
                            {kShadeSolid, "shadeSolid",
                             "Solid -- one clay surface under a fixed studio light.\n"
                             "Shape stays readable whatever the scene's lighting does."},
                            {kShadeSolidLit, "shadeSolidLit",
                             "Solid lit -- the scene's own light and shadows,\n"
                             "without the textures arguing with them."},
                            {kShadeTextured, "shadeTextured",
                             "Textured -- the game: materials, sky, water, plants."},
                            {kShadePathTraced, "shadePath",
                             "Pathtraced -- the offline renderer, live in the\n"
                             "viewport. Starts when the camera comes to rest\n"
                             "and refines until it is done.\n"
                             "Click again to pick up an edit."},
                        };
                        for (const ShadeBtn& b : kShades) {
                            const bool hit = iconButton(b.id, bs, b.tip, playMode, c);
                            icon::shade(dl, b.mode, c, r,
                                        playMode ? icon::kDim
                                        : viewShade == b.mode ? icon::kOn : icon::kOff);
                            if (!hit) continue;
                            // Pressing the mode you are already in means "look
                            // again": the trace follows the camera by itself,
                            // and this is the one thing it cannot see coming.
                            if (b.mode == kShadePathTraced && viewShade == b.mode)
                                viewtrace::refresh(viewTrace);
                            viewShade = b.mode;
                        }
                    }

                    // Gap, then the road editor: a toggle, not a one-shot action
                    // like the buttons before it, so it stays lit while it owns
                    // the left mouse button in the viewport.
                    gap();
                    {
                        char tip[160];
                        std::snprintf(tip, sizeof tip,
                                      "Road editor%s\n"
                                      "Click ground = add point, drag = move,\n"
                                      "Ctrl+drag = raise/lower, Del = delete.",
                                      roadEditMode ? " (on)" : "");
                        const bool hit = iconButton("roadTool", bs, tip, false, c);
                        icon::road(dl, c, r, roadEditMode ? icon::kOn : icon::kOff);
                        if (hit) {
                            roadEditMode = !roadEditMode;
                            if (roadEditMode) {
                                // Same hand-off the panel's Edit mode checkbox
                                // does: one tool owns the left button at a time.
                                grassPaintMode = sculptMode = treePaintMode =
                                    flowerPaintMode = paintMode = scatterMode = false;
                                showRoads = true; // the tunables belong with the tool
                            }
                        }
                    }
                }
                ImGui::End();
                ImGui::PopStyleVar();
            }

            // --- New Project / Save As wizard --------------------------------
            if (wizardOpen) { ImGui::OpenPopup("Project Wizard"); wizardOpen = false; }
            ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
            if (ImGui::BeginPopupModal("Project Wizard", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted(wizardIsNew
                    ? "Create a new project" : "Save project as");
                ImGui::Separator();
                const float fieldW = 340.0f;
                ImGui::SetNextItemWidth(fieldW);
                ImGui::InputText("Name", wizName, sizeof(wizName));
                ImGui::SetNextItemWidth(fieldW);
                ImGui::InputText("Location", wizLocation, sizeof(wizLocation));
                ImGui::SameLine();
                if (ImGui::Button("Browse...")) {
                    std::string picked;
                    if (ed::pickFolder(picked,
                            wizLocation[0] ? std::string(wizLocation) : prefLocation))
                        std::snprintf(wizLocation, sizeof(wizLocation), "%s",
                                      picked.c_str());
                }

                const std::string safe = safeName(wizName);
                const std::string loc(wizLocation);
                const std::string target = loc.empty() ? std::string()
                                                       : (loc + "/" + safe);
                std::error_code vec;
                const bool nameOk = wizName[0] != '\0';
                const bool locOk  = !loc.empty() &&
                                    std::filesystem::is_directory(loc, vec);
                const bool exists = nameOk && locOk &&
                                    std::filesystem::exists(target, vec);

                ImGui::Spacing();
                if (!target.empty()) {
                    // Bound the wrap so a long path can't stretch the modal wide.
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 460.0f);
                    ImGui::TextDisabled("Folder: %s", target.c_str());
                    ImGui::PopTextWrapPos();
                }
                const ImVec4 warn(1.0f, 0.55f, 0.3f, 1.0f);
                if (!nameOk)      ImGui::TextColored(warn, "Enter a project name.");
                else if (!locOk)  ImGui::TextColored(warn, "Location does not exist.");
                else if (exists)  ImGui::TextColored(warn,
                                      "A folder with that name already exists here.");
                ImGui::Spacing();

                const bool canGo = nameOk && locOk && !exists;
                ImGui::BeginDisabled(!canGo);
                if (ImGui::Button(wizardIsNew ? "Create" : "Save",
                                  ImVec2(120.0f, 0.0f))) {
                    if (wizardIsNew) newProject();
                    saveProjectTo(target);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

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

            // --- Scene manager dialogs (New / Rename / Delete) ---------------
            if (sceneNewOpen)    { ImGui::OpenPopup("New Scene");    sceneNewOpen = false; }
            if (sceneRenameOpen) { ImGui::OpenPopup("Rename Scene"); sceneRenameOpen = false; }
            if (sceneDeleteOpen) { ImGui::OpenPopup("Delete Scene"); sceneDeleteOpen = false; }
            const std::string sceneFolder = currentProject.empty() ? std::string()
                : std::filesystem::path(currentProject).parent_path().generic_string();
            // 0 = ok, 1 = empty, 2 = a scene with that name already exists. `self`
            // allows the current scene's own file to match (used by Rename).
            auto sceneNameState = [&](bool allowSelf) -> int {
                if (sceneNameBuf[0] == '\0') return 1;
                const std::string target =
                    sceneFolder + "/" + safeName(sceneNameBuf) + ".fitzel";
                std::error_code ec;
                if (std::filesystem::exists(target, ec) &&
                    !(allowSelf && target == currentProject)) return 2;
                return 0;
            };
            const ImVec4 sceneWarn(1.0f, 0.55f, 0.3f, 1.0f);

            ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Appearing);
            if (ImGui::BeginPopupModal("New Scene", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted("New scene in this project");
                ImGui::TextDisabled("Shares the project's materials; starts from the "
                                    "current world with no objects.");
                ImGui::Separator();
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                ImGui::SetNextItemWidth(300.0f);
                ImGui::InputText("Name##newscene", sceneNameBuf, sizeof(sceneNameBuf));
                const int st = sceneNameState(false);
                if (st == 1)      ImGui::TextColored(sceneWarn, "Enter a scene name.");
                else if (st == 2) ImGui::TextColored(sceneWarn,
                                      "A scene with that name already exists.");
                ImGui::Spacing();
                ImGui::BeginDisabled(st != 0);
                if (ImGui::Button("Create", ImVec2(120.0f, 0.0f))) {
                    saveSceneFile(currentProject);          // keep the scene we leave
                    resetWorldForNewScene();                // blank terrain/road/vegetation
                    newSceneInProject(sceneFolder, sceneNameBuf);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

            ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Appearing);
            if (ImGui::BeginPopupModal("Rename Scene", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted("Rename the current scene");
                ImGui::Separator();
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                ImGui::SetNextItemWidth(300.0f);
                ImGui::InputText("Name##renscene", sceneNameBuf, sizeof(sceneNameBuf));
                const int st = sceneNameState(true); // its own file may match
                if (st == 1)      ImGui::TextColored(sceneWarn, "Enter a scene name.");
                else if (st == 2) ImGui::TextColored(sceneWarn,
                                      "A scene with that name already exists.");
                ImGui::Spacing();
                ImGui::BeginDisabled(st != 0);
                if (ImGui::Button("Rename", ImVec2(120.0f, 0.0f))) {
                    renameScene(currentProject, sceneNameBuf);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

            if (ImGui::BeginPopupModal("Delete Scene", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Delete scene \"%s\"?",
                    std::filesystem::path(currentProject).stem().string().c_str());
                ImGui::TextDisabled("This permanently removes the .fitzel file from disk.");
                ImGui::Spacing();
                if (ImGui::Button("Delete", ImVec2(120.0f, 0.0f))) {
                    const std::string gone = currentProject;
                    std::string next; // switch to another scene before removing this one
                    for (const auto& [n, p] : listScenesIn(sceneFolder))
                        if (p != gone) { next = p; break; }
                    if (!next.empty()) {
                        loadSceneFile(next);
                        deleteSceneFile(gone);
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

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
                buildDefaultDockLayout(dockId);
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
                // What the tracer is doing, over its own picture. A progressive
                // render that says nothing is indistinguishable from a stuck
                // one, and this one restarts whenever the camera moves -- so
                // "waiting for the view to settle" is a thing it has to be able
                // to say.
                if (viewShade == kShadePathTraced && !playMode &&
                    !viewTrace.status.empty()) {
                    const ImVec2 rm = ImGui::GetItemRectMin();
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(rm.x + 10.0f, rm.y + 8.0f),
                        IM_COL32(255, 225, 140, 230), viewTrace.status.c_str());
                }
                viewportHovered = ImGui::IsItemHovered();
                // Cursor position inside the image, mapped to NDC (for picking).
                const ImVec2 rmin = ImGui::GetItemRectMin();
                const ImVec2 rsz  = ImGui::GetItemRectSize();
                viewportRectMin  = glm::vec2(rmin.x, rmin.y); // for the play crosshair
                viewportRectSize = glm::vec2(rsz.x, rsz.y);
                const ImVec2 mp   = ImGui::GetIO().MousePos;
                viewportMouseNdc = glm::vec2(
                    (rsz.x > 0.0f ? (mp.x - rmin.x) / rsz.x : 0.5f) * 2.0f - 1.0f,
                    1.0f - (rsz.y > 0.0f ? (mp.y - rmin.y) / rsz.y : 0.5f) * 2.0f);
                // Keep the multi-selection consistent with the active object before
                // any panel/viewport consumes it this frame.
                sel.normalize();
                viewportClicked = viewportHovered &&
                                  ImGui::IsMouseClicked(ImGuiMouseButton_Left);

                // A camera preview owns the viewport: say which one, and give it
                // a way out that is right where it took the view from. Without
                // this the free camera has simply gone, and getting it back means
                // knowing that some camera in the hierarchy has it -- a trap, and
                // exactly the kind this editor is meant not to set.
                if (!playMode && activeCam >= 0) {
                    const Entity* pcam = document.find(activeCam);
                    char lbl[160];
                    std::snprintf(lbl, sizeof lbl, "Exit camera: %s",
                                  pcam ? pcam->name.c_str() : "(gone)");
                    ImGui::SetCursorScreenPos(ImVec2(rmin.x + 12.0f, rmin.y + 30.0f));
                    if (ImGui::Button(lbl)) activeCam = -1;
                    // The pick test above already latched this click. Clicking a
                    // button that sits over the scene must not also select
                    // whatever happens to be behind it.
                    if (ImGui::IsItemHovered()) viewportClicked = false;
                }

                // Which way we are looking, when it is a standard view. Blender
                // puts this in the same corner, and for the same reason: front
                // and back look identical until something moves, so a view you
                // cannot name is one you have to test by nudging the camera --
                // which is exactly what the standard views are for avoiding.
                if (!playMode)
                    if (const char* vl = viewnav::label(viewNav.current())) {
                        ImDrawList* vdl = ImGui::GetWindowDrawList();
                        const ImVec2 at(rmin.x + 12.0f, rmin.y + 10.0f);
                        vdl->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f),
                                     IM_COL32(0, 0, 0, 160), vl);
                        vdl->AddText(at, IM_COL32(235, 240, 250, 225), vl);
                    }

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
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* pl =
                            ImGui::AcceptDragDropPayload("ASSET_GUID")) {
                        const AssetId gid = AssetId::fromString(std::string(
                            static_cast<const char*>(pl->Data), pl->DataSize));
                        const AssetType at = assetDb.typeForId(gid);
                        // Is it one of the scene's materials? Asked of the library
                        // rather than of the asset database, because a material
                        // dragged out of the Materials panel has a GUID and may
                        // have no .fmat on disk yet -- and it is still the thing
                        // the drop is about.
                        int dropMat = -1;
                        for (int k = 0; k < static_cast<int>(materials.size()); ++k)
                            if (materials[k].assetId == gid) { dropMat = k; break; }
                        const float asp = static_cast<float>(viewW) /
                                          static_cast<float>(viewH);
                        const glm::mat4 vp =
                            camera.projectionMatrix(asp) * camera.viewMatrix();
                        if (at == AssetType::Model) {
                            glm::vec3 hit;
                            if (roadPickTerrain(viewportMouseNdc, vp, hit)) {
                                const std::string mp = assetDb.pathForId(gid).string();
                                if (isStructuredModel(mp)) addModelHierarchy(hit, mp);
                                else {
                                    const int id = models.import(mp, assetDb, materials);
                                    if (id >= 0) addModelEntity(hit, id);
                                }
                            }
                        } else if (at == AssetType::Material || dropMat >= 0) {
                            // A material dropped ON A FACE dresses that face
                            // alone; dropped anywhere else on an object it
                            // becomes the object's. This is the drag half of the
                            // Modeling panel's picker, and it exists BESIDE it
                            // rather than instead of it: aiming at a face is a
                            // gesture some days do not have, and the panel's
                            // combo is the same operation without one.
                            const int mi = dropMat;
                            const glm::mat4 inv = glm::inverse(vp);
                            glm::vec4 pn = inv * glm::vec4(viewportMouseNdc, -1.0f, 1.0f); pn /= pn.w;
                            glm::vec4 pf = inv * glm::vec4(viewportMouseNdc,  1.0f, 1.0f); pf /= pf.w;
                            const glm::vec3 ro = glm::vec3(pn);
                            const glm::vec3 rd = glm::normalize(glm::vec3(pf) - glm::vec3(pn));
                            if (mi < 0) {
                                exportStatus = "That material isn't in this scene's "
                                               "library -- open the project it "
                                               "belongs to first.";
                            } else {
                            // The face under the cursor, over every modelled mesh
                            // in the scene: dressing a face should not first
                            // require selecting the object it belongs to.
                            int   faceEnt = -1, faceHit = -1;
                            float faceT   = 1e30f;
                            for (int i = 0; i < static_cast<int>(entities.size()); ++i) {
                                const MeshComponent* emc =
                                    entities[i].components.get<MeshComponent>();
                                if (!emc) continue;
                                for (int f = 0;
                                     f < static_cast<int>(emc->mesh.faces.size()); ++f) {
                                    const std::vector<glm::vec3> w =
                                        meshFaceWorld(entities[i], *emc, f);
                                    // The same fan the GPU mesh is built from, so
                                    // what is dropped on is exactly what is drawn.
                                    for (std::size_t k = 1; k + 1 < w.size(); ++k) {
                                        const float t =
                                            rayTriangle(ro, rd, w[0], w[k], w[k + 1]);
                                        if (t >= 0.0f && t < faceT) {
                                            faceT = t; faceHit = f; faceEnt = i;
                                        }
                                    }
                                }
                            }
                            int hit = faceEnt;
                            if (hit < 0) {
                                // No face: the nearest solid takes it whole.
                                float bestT = 1e30f;
                                for (int i = 0; i < static_cast<int>(entities.size()); ++i) {
                                    if (!isSolidPrimitive(entities[i].type)) continue;
                                    const float d = rayAABB(ro, rd, entities[i].center - entities[i].half,
                                                                    entities[i].center + entities[i].half);
                                    if (d >= 0.0f && d < bestT) { bestT = d; hit = i; }
                                }
                            }
                            if (hit >= 0) {
                                const std::vector<int> ids{entities[hit].id};
                                auto before = snapshotEntities(ids);
                                Entity& e = entities[hit];
                                if (faceEnt == hit && faceHit >= 0) {
                                    MeshComponent* emc = e.components.get<MeshComponent>();
                                    emc->mesh.setFaceMaterial(faceHit, gid);
                                    emc->touch();   // the GPU copy is split by material
                                    meshFaceOwner = e.id;
                                    meshFaceSel   = faceHit;
                                    exportStatus  = "Material on one face.";
                                } else if (auto* emc = e.components.get<MaterialComponent>()) {
                                    emc->material = gid;
                                } else {
                                    auto nc = std::make_unique<MaterialComponent>();
                                    nc->material = gid;
                                    e.components.items.push_back(std::move(nc));
                                }
                                sel.selectIndex(hit);
                                matSel    = mi;
                                auto cmd = std::make_unique<ModifyEntitiesCmd>(
                                    before, snapshotEntities(ids));
                                if (!cmd->trivial()) history.pushApplied(std::move(cmd));
                            }
                            }
                        } else if (at == AssetType::Texture) {
                            // Pick the solid under the drop point.
                            const glm::mat4 inv = glm::inverse(vp);
                            glm::vec4 pn = inv * glm::vec4(viewportMouseNdc, -1.0f, 1.0f); pn /= pn.w;
                            glm::vec4 pf = inv * glm::vec4(viewportMouseNdc,  1.0f, 1.0f); pf /= pf.w;
                            const glm::vec3 ro = glm::vec3(pn);
                            const glm::vec3 rd = glm::normalize(glm::vec3(pf) - glm::vec3(pn));
                            int hit = -1; float bestT = 1e30f;
                            for (int i = 0; i < static_cast<int>(entities.size()); ++i) {
                                if (!isSolidPrimitive(entities[i].type)) continue;
                                const float d = rayAABB(ro, rd, entities[i].center - entities[i].half,
                                                                entities[i].center + entities[i].half);
                                if (d >= 0.0f && d < bestT) { bestT = d; hit = i; }
                            }
                            if (hit >= 0) {
                                // A new material that samples the dropped texture.
                                MaterialDef nm;
                                nm.assetId = AssetId::generate();
                                const AssetDatabase::Entry* te = assetDb.entry(gid);
                                nm.name  = te ? std::filesystem::path(te->relPath).stem().string()
                                              : "Textured";
                                nm.texId = gid;
                                nm.tex   = assetDb.loadTexture(gid);
                                materials.push_back(nm);
                                matSel = static_cast<int>(materials.size()) - 1;
                                // Assign it to the object's MaterialComponent (undoable).
                                const std::vector<int> ids{entities[hit].id};
                                auto before = snapshotEntities(ids);
                                Entity& e = entities[hit];
                                if (auto* mc = e.components.get<MaterialComponent>())
                                    mc->material = nm.assetId;
                                else {
                                    auto c = std::make_unique<MaterialComponent>();
                                    c->material = nm.assetId;
                                    e.components.items.push_back(std::move(c));
                                }
                                sel.selectIndex(hit);
                                auto cmd = std::make_unique<ModifyEntitiesCmd>(
                                    before, snapshotEntities(ids));
                                if (!cmd->trivial()) history.pushApplied(std::move(cmd));
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                // --- Road edit handles: draggable control-point markers -----
                if (roadEditMode) {
                    // The handles belong to the road the Roads panel has selected;
                    // the others are drawn by the renderer and are not grabbable
                    // (there is one set of point indices, and a bridge names its
                    // ends by index). Bound here rather than per use: the selection
                    // cannot change in the middle of a viewport gesture.
                    RoadSystem& road = roads.active();
                    const float asp = static_cast<float>(viewW) / static_cast<float>(viewH);
                    const glm::mat4 vp = camera.projectionMatrix(asp) * camera.viewMatrix();
                    const ImVec2 org = rmin; // image top-left in screen space
                    // Handles sit on the road, not on the ground: a lifted point
                    // has to be grabbable where its road actually runs.
                    auto handleWorld = [&](int i) {
                        return glm::vec3(road.roadPts[i].x,
                                         streamer.heightAt(road.roadPts[i].x, road.roadPts[i].y)
                                             + 0.10f + road.liftOf(i),
                                         road.roadPts[i].y);
                    };
                    auto toScreen = [&](const glm::vec3& wp, ImVec2& out) {
                        const glm::vec4 c = vp * glm::vec4(wp, 1.0f);
                        if (c.w <= 1e-4f) return false;
                        const glm::vec3 n = glm::vec3(c) / c.w;
                        if (n.z > 1.0f) return false;
                        out = ImVec2(org.x + (n.x * 0.5f + 0.5f) * viewW,
                                     org.y + (1.0f - (n.y * 0.5f + 0.5f)) * viewH);
                        return true;
                    };

                    // Handle under the cursor, or -1. Computed every frame (not
                    // only on click) so the drawing below can show what a click
                    // would grab -- and so picking and highlighting can never
                    // disagree about which point that is.
                    int roadHover = -1;
                    if (viewportHovered && !roadDragging) {
                        float bestD = 12.0f; // pixel grab radius
                        for (int i = 0; i < static_cast<int>(road.roadPts.size()); ++i) {
                            ImVec2 sp;
                            if (!toScreen(handleWorld(i), sp)) continue;
                            const float d = std::hypot(sp.x - mp.x, sp.y - mp.y);
                            if (d < bestD) { bestD = d; roadHover = i; }
                        }
                    }

                    // A loop needs a grip of its own. It is NAMED by two control
                    // points, but it stands twenty metres over them -- so the thing
                    // to point at is the crown of the turn, where the loop actually
                    // is, and picking it selects the pair whose row the panel edits.
                    // Control points win the contest: they are smaller targets and
                    // there are far more of them.
                    const std::vector<roadloop::Loop>& builtLoops = road.loopGeometry();
                    auto loopCrown = [](const roadloop::Loop& lp) {
                        glm::vec3 top = lp.frames.empty() ? glm::vec3(0.0f)
                                                          : lp.frames.front().pos;
                        for (const roadloop::Frame& f : lp.frames)
                            if (f.pos.y > top.y) top = f.pos;
                        return top;
                    };
                    int loopHover = -1;
                    if (viewportHovered && !roadDragging && roadHover < 0) {
                        float bestD = 15.0f; // pixel grab radius, a touch larger
                        for (int i = 0; i < static_cast<int>(builtLoops.size()); ++i) {
                            if (builtLoops[i].frames.empty()) continue;
                            ImVec2 sp;
                            if (!toScreen(loopCrown(builtLoops[i]), sp)) continue;
                            const float d = std::hypot(sp.x - mp.x, sp.y - mp.y);
                            if (d < bestD) { bestD = d; loopHover = i; }
                        }
                    }

                    // Pick / add on click.
                    if (viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        const int best = roadHover;
                        if (best >= 0) {
                            // Shift-click marks the far end of a bridge instead of
                            // re-selecting; plain click picks (and starts a drag);
                            // Ctrl+drag raises/lowers the point instead of moving
                            // it across the ground.
                            if (ImGui::GetIO().KeyShift && roadSel >= 0 && best != roadSel)
                                roadSel2 = best;
                            else {
                                roadSel = best; roadSel2 = -1; roadDragging = true;
                                roadDragHeight = ImGui::GetIO().KeyCtrl;
                                // Opened here, pushed on mouse release: the whole
                                // drag is one undo step.
                                beginRoadEdit();
                            }
                        } else if (loopHover >= 0) {
                            // Selecting the turn selects the pair that names it, so
                            // the panel's row for THIS loop is the live one and its
                            // radius is one field away -- rather than a list of
                            // "#2 -> #1" to count out against the viewport.
                            roadSel  = builtLoops[loopHover].pa;
                            roadSel2 = builtLoops[loopHover].pb;
                        } else {
                            glm::vec3 h;
                            if (roadPickTerrain(viewportMouseNdc, vp, h)) {
                                // With an END of the road selected, extend from
                                // THAT end. Otherwise insert at the nearest
                                // segment, so a click on an existing road drops a
                                // waypoint in the middle.
                                //
                                // Nearest-in-plan-view is the wrong question once
                                // roads cross in three dimensions: drawing a road
                                // UNDER a bridge puts every click right beside the
                                // stretch flying overhead, and the new point gets
                                // spliced into that stretch instead of continuing
                                // the one being drawn. Having picked an end, the
                                // author has already said which end they mean --
                                // and since the new point takes the selection, a
                                // run of clicks lays out a road point by point.
                                const int n = static_cast<int>(road.roadPts.size());
                                const bool ends = !road.closed && n >= 2;
                                const int at = (ends && roadSel == n - 1) ? n
                                             : (ends && roadSel == 0)     ? 0
                                             : roadInsertIndex({h.x, h.z});
                                addRoadPoint(at, glm::vec2(h.x, h.z));
                            }
                        }
                    }
                    // Drag the selected handle: across the terrain, or (Ctrl)
                    // straight up and down.
                    if (roadDragging && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                        roadSel >= 0 && roadSel < static_cast<int>(road.roadPts.size())) {
                        if (roadDragHeight) {
                            // Metres per pixel at the handle's own depth, so the
                            // point tracks the cursor instead of drifting away
                            // from it as you zoom in or out.
                            const glm::vec3 hw = handleWorld(roadSel);
                            const float dist = glm::length(hw - camera.position());
                            const float mpp =
                                2.0f * dist *
                                std::tan(glm::radians(camera.fov() * 0.5f)) /
                                std::max(1.0f, static_cast<float>(viewH));
                            const float dy = ImGui::GetIO().MouseDelta.y;
                            if (dy != 0.0f)
                                road.setLift(roadSel,
                                             road.liftOf(roadSel) - dy * mpp);
                        } else {
                            glm::vec3 h;
                            if (roadPickTerrain(viewportMouseNdc, vp, h)) {
                                road.roadPts[roadSel] = glm::vec2(h.x, h.z);
                                road.needsBuild = true;
                            }
                        }
                    }
                    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && roadDragging) {
                        roadDragging = false;
                        commitRoadEdit(roadDragHeight ? "Raise point" : "Move point");
                    }
                    // Delete the selected point.
                    if (roadSel >= 0 && roadSel < static_cast<int>(road.roadPts.size()) &&
                        ImGui::IsKeyPressed(ImGuiKey_Delete))
                        deleteRoadPoint(roadSel);

                    // Keyboard nudge for the selected point: the arrows move it
                    // across the ground, PageUp/Down raise and lower it. Camera
                    // relative, because "left" means what you see, not where the
                    // world's X axis happens to point. Held keys repeat, and the
                    // whole burst is bracketed into one undo step (released ->
                    // committed below).
                    if (roadSel >= 0 && roadSel < static_cast<int>(road.roadPts.size()) &&
                        !ImGui::GetIO().WantTextInput && !roadDragging) {
                        const float step = (ImGui::GetIO().KeyShift ? 2.5f : 0.25f);
                        glm::vec3 f = camera.front();
                        f.y = 0.0f;
                        if (glm::length(f) < 1e-4f) f = glm::vec3(0, 0, -1);
                        f = glm::normalize(f);
                        const glm::vec3 r(-f.z, 0.0f, f.x); // right-hand perp in XZ
                        glm::vec2 d(0.0f);
                        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow,    true)) d += glm::vec2(f.x, f.z);
                        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow,  true)) d -= glm::vec2(f.x, f.z);
                        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) d += glm::vec2(r.x, r.z);
                        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  true)) d -= glm::vec2(r.x, r.z);
                        float dh = 0.0f;
                        if (ImGui::IsKeyPressed(ImGuiKey_PageUp,   true)) dh += step;
                        if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true)) dh -= step;
                        if (d != glm::vec2(0.0f) || dh != 0.0f) {
                            beginRoadEdit();
                            if (d != glm::vec2(0.0f)) {
                                road.roadPts[roadSel] += d * step;
                                road.needsBuild = true;
                            }
                            if (dh != 0.0f)
                                road.setLift(roadSel, road.liftOf(roadSel) + dh);
                        }
                        // Burst over (no nudge key still down) -> close the step.
                        const bool held =
                            ImGui::IsKeyDown(ImGuiKey_UpArrow)   || ImGui::IsKeyDown(ImGuiKey_DownArrow) ||
                            ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsKeyDown(ImGuiKey_RightArrow) ||
                            ImGui::IsKeyDown(ImGuiKey_PageUp)    || ImGui::IsKeyDown(ImGuiKey_PageDown);
                        if (!held && roadUndoOpen) commitRoadEdit("Nudge point");
                    }

                    // Live preview: the smoothed spline as it will be built -- the
                    // curved centreline plus its left/right edges at the road width.
                    // Yellow = not yet built, cyan = matches the committed road.
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const RoadSystem::Preview pv = road.previewGeometry();
                    const ImU32 edgeCol = road.needsBuild ? IM_COL32(255, 210, 70, 200)
                                                          : IM_COL32(90, 210, 190, 190);
                    const ImU32 midCol  = road.needsBuild ? IM_COL32(255, 235, 140, 150)
                                                          : IM_COL32(150, 235, 220, 130);
                    auto drawPolyline = [&](const std::vector<glm::vec3>& line,
                                            ImU32 col, float th) {
                        ImVec2 prev; bool have = false;
                        for (const glm::vec3& wp : line) {
                            ImVec2 sp;
                            if (!toScreen(wp, sp)) { have = false; continue; }
                            if (have) dl->AddLine(prev, sp, col, th);
                            prev = sp; have = true;
                        }
                    };
                    drawPolyline(pv.left,  edgeCol, 2.0f);
                    drawPolyline(pv.right, edgeCol, 2.0f);
                    drawPolyline(pv.center, midCol, 1.5f);

                    // Which stretch of centreline belongs to which pair of control
                    // points, so a bridge can be shown where it actually runs
                    // instead of only as "#3 -> #7" in the panel. The candidate
                    // pair (selected + shift-clicked) is drawn the same way before
                    // it exists, which is what makes picking the two ends
                    // something you can see rather than count out.
                    auto drawSpan = [&](int pa, int pb, ImU32 col, float th) {
                        const int n = static_cast<int>(pv.ptSample.size());
                        const int a = std::min(pa, pb), b = std::max(pa, pb);
                        if (a < 0 || b >= n) return;
                        ImVec2 prev; bool have = false;
                        for (int i = pv.ptSample[a];
                             i <= pv.ptSample[b] && i < static_cast<int>(pv.center.size()); ++i) {
                            ImVec2 sp;
                            if (!toScreen(pv.center[i], sp)) { have = false; continue; }
                            if (have) dl->AddLine(prev, sp, col, th);
                            prev = sp; have = true;
                        }
                    };
                    // The same, addressed by centreline SAMPLE rather than by
                    // control point -- a loop's footprint starts at a point but ends
                    // wherever its own geometry comes back down.
                    auto drawFootprint = [&](int i0, int i1, ImU32 col, float th) {
                        ImVec2 prev; bool have = false;
                        for (int i = std::max(0, i0);
                             i <= i1 && i < static_cast<int>(pv.center.size()); ++i) {
                            ImVec2 sp;
                            if (!toScreen(pv.center[i], sp)) { have = false; continue; }
                            if (have) dl->AddLine(prev, sp, col, th);
                            prev = sp; have = true;
                        }
                    };
                    for (const RoadSystem::BridgeSpec& b : road.bridges)
                        drawSpan(b.a, b.b, IM_COL32(255, 140, 60, 220), 4.0f);
                    // Bored stretches in a cooler colour: a road can carry both,
                    // and one orange for two opposite structures reads as neither.
                    for (const RoadSystem::BridgeSpec& t : road.tunnels)
                        drawSpan(t.a, t.b, IM_COL32(90, 190, 255, 220), 4.0f);
                    if (roadSel >= 0 && roadSel2 >= 0 && roadSel != roadSel2)
                        drawSpan(roadSel, roadSel2, IM_COL32(255, 255, 255, 200), 3.0f);

                    // --- Loops -----------------------------------------------
                    // A loop gets a colour of its own: it is neither a deck nor a
                    // bore, and borrowing one of theirs would make three structures
                    // read as two. The ring is the turn's own centreline projected,
                    // so it is drawn where the loop stands -- which is what makes a
                    // loop something to look at and click rather than a row in a
                    // list naming two numbers.
                    auto drawRing = [&](const roadloop::Loop& lp, ImU32 col, float th,
                                        bool dashed) {
                        ImVec2 prev; bool have = false;
                        for (std::size_t i = 0; i < lp.frames.size(); ++i) {
                            ImVec2 sp;
                            if (!toScreen(lp.frames[i].pos, sp)) { have = false; continue; }
                            if (have && (!dashed || (i / 4) % 2 == 0))
                                dl->AddLine(prev, sp, col, th);
                            prev = sp; have = true;
                        }
                    };
                    auto shadowText = [&](ImVec2 at, ImU32 col, const char* txt) {
                        dl->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f),
                                    IM_COL32(0, 0, 0, 200), txt);
                        dl->AddText(at, col, txt);
                    };
                    for (int i = 0; i < static_cast<int>(builtLoops.size()); ++i) {
                        const roadloop::Loop& lp = builtLoops[i];
                        if (lp.frames.empty()) continue;
                        const bool lsel = (lp.pa == roadSel && lp.pb == roadSel2) ||
                                          (lp.pa == roadSel2 && lp.pb == roadSel);
                        const bool lhov = (i == loopHover);
                        const ImU32 col = lsel ? IM_COL32(255, 190, 255, 255)
                                        : lhov ? IM_COL32(240, 205, 255, 250)
                                               : IM_COL32(190, 130, 255, 205);
                        // The ground the turn stands in place of -- its FOOTPRINT,
                        // not the pair's whole stretch: a turn advances only half
                        // its radius while it goes round, so the road past its feet
                        // is still road. This is the same span the ribbon leaves
                        // out, so it answers "where did my road go?" exactly.
                        drawFootprint(lp.sa, lp.sb, (col & 0x00FFFFFFu) | 0x55000000u,
                                      4.0f);
                        drawRing(lp, col, (lsel || lhov) ? 3.0f : 2.0f, /*dashed=*/false);
                        ImVec2 sp;
                        if (toScreen(loopCrown(lp), sp)) {
                            const float rad = (lsel || lhov) ? 7.0f : 5.5f;
                            dl->AddCircleFilled(sp, rad, col);
                            dl->AddCircle(sp, rad, IM_COL32(0, 0, 0, 190), 0, 1.5f);
                            char lb[80];
                            std::snprintf(lb, sizeof(lb), "Loop #%d-#%d  %.0f m tall%s",
                                          lp.pa, lp.pb, lp.radius * 2.0f,
                                          lp.inverts ? "" : "  (a hump: too long to turn over)");
                            shadowText(ImVec2(sp.x + rad + 3.0f, sp.y - rad - 2.0f),
                                       IM_COL32(240, 220, 255, 245), lb);
                        }
                    }

                    // What "Create loop" would give you, drawn before it is asked
                    // for: with two points picked and no loop on them yet, the turn
                    // that button would build is ghosted in. Planned by the SAME
                    // routine the build runs, so the ghost cannot promise a shape
                    // the road would not produce -- including refusing to: a pair
                    // too close together plans nothing, and says so where the eye
                    // already is, instead of leaving a button that quietly does
                    // nothing.
                    if (roadSel >= 0 && roadSel2 >= 0 && roadSel != roadSel2 &&
                        pv.center.size() >= 2) {
                        bool already = false;
                        for (const roadloop::Spec& sp : road.loops)
                            already = already ||
                                      (sp.a == roadSel && sp.b == roadSel2) ||
                                      (sp.a == roadSel2 && sp.b == roadSel);
                        if (!already) {
                            std::vector<glm::vec2> gcen(pv.center.size());
                            std::vector<float>     gprof(pv.center.size());
                            for (std::size_t i = 0; i < pv.center.size(); ++i) {
                                gcen[i]  = glm::vec2(pv.center[i].x, pv.center[i].z);
                                gprof[i] = pv.center[i].y;
                            }
                            roadloop::Spec want;
                            want.a = roadSel; want.b = roadSel2;
                            const std::vector<roadloop::Loop> ghost =
                                roadloop::plan(gcen, gprof, pv.ptSample, {want},
                                               road.width);
                            const ImU32 ghostCol = IM_COL32(230, 180, 255, 190);
                            if (!ghost.empty() && !ghost.front().frames.empty()) {
                                const roadloop::Loop& lp = ghost.front();
                                drawRing(lp, ghostCol, 2.0f, /*dashed=*/true);
                                ImVec2 sp;
                                if (toScreen(loopCrown(lp), sp)) {
                                    // How far the turn travels while it goes round
                                    // is the distance between the two points, so
                                    // both numbers a loop is judged by are on screen
                                    // while they are still being chosen.
                                    float run = 0.0f;
                                    for (int k = lp.sa; k < lp.sb &&
                                         k + 1 < static_cast<int>(pv.center.size()); ++k)
                                        run += glm::length(
                                            glm::vec2(pv.center[k + 1].x - pv.center[k].x,
                                                      pv.center[k + 1].z - pv.center[k].z));
                                    char lb[128];
                                    if (!lp.inverts)
                                        std::snprintf(lb, sizeof(lb),
                                                      "%.0f m of road is too far for a %.0f m "
                                                      "radius: a hump, not a loop",
                                                      run, lp.radius);
                                    else if (lp.sway > 0.05f)
                                        std::snprintf(lb, sizeof(lb),
                                                      "Loop here: %.0f m tall, %.0f m of road, "
                                                      "swaying %.0f m to clear itself",
                                                      lp.radius * 2.0f, run, lp.sway);
                                    else
                                        std::snprintf(lb, sizeof(lb),
                                                      "Loop here: %.0f m tall, %.0f m of road",
                                                      lp.radius * 2.0f, run);
                                    shadowText(ImVec2(sp.x + 9.0f, sp.y - 9.0f),
                                               lp.inverts ? ghostCol
                                                          : IM_COL32(255, 190, 120, 235), lb);
                                }
                            } else {
                                const int pa = std::min(roadSel, roadSel2);
                                ImVec2 sp;
                                if (pa < static_cast<int>(pv.ptSample.size()) &&
                                    pv.ptSample[pa] < static_cast<int>(pv.center.size()) &&
                                    toScreen(pv.center[pv.ptSample[pa]] +
                                                 glm::vec3(0.0f, 1.5f, 0.0f), sp))
                                    shadowText(ImVec2(sp.x + 9.0f, sp.y - 9.0f),
                                               IM_COL32(255, 150, 130, 235),
                                               "Too close together for a loop");
                            }
                        }
                    }

                    // Handles. A point the terrain hides is drawn faint rather
                    // than dropped: it still has to be findable behind a ridge,
                    // just not compete with the ones you can actually see.
                    auto occluded = [&](const glm::vec3& wp) {
                        const glm::vec3 eye = camera.position();
                        const glm::vec3 d   = wp - eye;
                        for (int s = 1; s < 24; ++s) { // skip the endpoints
                            const glm::vec3 p = eye + d * (static_cast<float>(s) / 24.0f);
                            if (streamer.heightAt(p.x, p.z) > p.y + 0.25f) return true;
                        }
                        return false;
                    };
                    const int ptCount = static_cast<int>(road.roadPts.size());
                    for (int i = 0; i < ptCount; ++i) {
                        ImVec2 sp;
                        const glm::vec3 hw = handleWorld(i);
                        if (!toScreen(hw, sp)) continue;
                        const bool sel   = (i == roadSel);
                        const bool mate  = (i == roadSel2);
                        const bool hover = (i == roadHover);
                        const float rad  = sel ? 7.0f : (hover ? 6.5f : 5.0f);
                        ImU32 col = sel   ? IM_COL32(255, 210,  60, 255)
                                  : mate  ? IM_COL32(255, 140,  60, 245)
                                  : hover ? IM_COL32(210, 235, 255, 255)
                                          : IM_COL32( 90, 180, 255, 235);
                        if (occluded(hw) && !sel && !hover)
                            col = (col & 0x00FFFFFF) | 0x50000000; // keep hue, drop alpha
                        // A raised or sunken point gets a stalk down to the ground
                        // it left: without it a lifted handle just looks like a
                        // point somewhere else on the terrain.
                        if (road.liftOf(i) != 0.0f) {
                            ImVec2 gp;
                            const glm::vec3 g(hw.x, hw.y - road.liftOf(i), hw.z);
                            if (toScreen(g, gp)) {
                                dl->AddLine(gp, sp, IM_COL32(255, 210, 60, 140), 1.5f);
                                dl->AddCircle(gp, 2.5f, IM_COL32(255, 210, 60, 160), 0, 1.5f);
                            }
                        }
                        dl->AddCircleFilled(sp, rad, col);
                        dl->AddCircle(sp, rad, IM_COL32(0, 0, 0, 190), 0, 1.5f);
                        // Index labels: all of them while the road is short enough
                        // to read, otherwise only the ones a bridge or the cursor
                        // is about to involve -- a hundred numbers along a long
                        // road is noise, not information.
                        if (ptCount <= 30 || sel || mate || hover) {
                            char lbl[16];
                            std::snprintf(lbl, sizeof(lbl), "%d", i);
                            const ImVec2 at(sp.x + rad + 2.0f, sp.y - rad - 2.0f);
                            dl->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f),
                                        IM_COL32(0, 0, 0, 200), lbl);
                            dl->AddText(at, IM_COL32(235, 240, 250, 235), lbl);
                        }
                        // The selected point also states its height, so a stretch
                        // can be set to a round number without hunting in the panel.
                        if (sel && road.liftOf(i) != 0.0f) {
                            char hb[32];
                            std::snprintf(hb, sizeof(hb), "%+.2f m", road.liftOf(i));
                            const ImVec2 at(sp.x + rad + 2.0f, sp.y + 2.0f);
                            dl->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f),
                                        IM_COL32(0, 0, 0, 200), hb);
                            dl->AddText(at, IM_COL32(255, 225, 140, 245), hb);
                        }
                    }
                }

                // --- Spline handles: the same gesture the road editor uses, for
                //     fences, walls and track. The tool itself is in SplineEdit.cpp
                //     -- main only hands it the viewport and the undo bracket.
                if (splineEditMode) {
                    // Only one tool may own the left button. The sibling panels
                    // each switch their rivals off from their own list; rather
                    // than thread this flag through three more PanelStates, the
                    // newcomer yields whenever one of them is on.
                    if (grassPaintMode || treePaintMode || flowerPaintMode ||
                        sculptMode || paintMode || scatterMode || roadEditMode ||
                        meshPaintMode || riverEditMode) {
                        splineEditMode = false;
                    } else {
                        const float asp = static_cast<float>(viewW) / static_cast<float>(viewH);
                        splineedit::Context sc{splines, splineSel, splinePtSel,
                                               splineDragging, splineDragHeight};
                        sc.viewProj    = camera.projectionMatrix(asp) * camera.viewMatrix();
                        sc.origin      = rmin;
                        sc.viewW       = static_cast<float>(viewW);
                        sc.viewH       = static_cast<float>(viewH);
                        sc.hovered     = viewportHovered;
                        sc.mouseNdc    = viewportMouseNdc;
                        sc.mousePos    = mp;
                        sc.cameraPos   = camera.position();
                        sc.cameraFront = camera.front();
                        sc.cameraFov   = camera.fov();
                        sc.pickTerrain = roadPickTerrain;
                        sc.groundAt    = [&streamer](float x, float z) {
                            return streamer.heightAt(x, z);
                        };
                        sc.beginEdit = beginSplineEdit;
                        sc.endEdit   = commitSplineEdit;
                        sc.editOpen  = [&splineUndoOpen] { return splineUndoOpen; };
                        splineedit::handle(sc);
                    }
                }

                // --- Water handles: the same gesture again, for brooks, rivers
                //     and canals. The tool is in RiverEdit.cpp -- main only hands
                //     it the viewport and the undo bracket, and the bracket is
                //     what cuts the bed when the gesture ends.
                if (riverEditMode) {
                    if (grassPaintMode || treePaintMode || flowerPaintMode ||
                        sculptMode || paintMode || scatterMode || roadEditMode ||
                        meshPaintMode || splineEditMode) {
                        riverEditMode = false;
                    } else {
                        const float asp = static_cast<float>(viewW) / static_cast<float>(viewH);
                        riveredit::Context rc{rivers, riverSel, riverPtSel,
                                              riverDragging, riverDragHeight};
                        rc.viewProj    = camera.projectionMatrix(asp) * camera.viewMatrix();
                        rc.origin      = rmin;
                        rc.viewW       = static_cast<float>(viewW);
                        rc.viewH       = static_cast<float>(viewH);
                        rc.hovered     = viewportHovered;
                        rc.mouseNdc    = viewportMouseNdc;
                        rc.mousePos    = mp;
                        rc.cameraPos   = camera.position();
                        rc.cameraFront = camera.front();
                        rc.cameraFov   = camera.fov();
                        rc.pickTerrain = roadPickTerrain;
                        rc.groundAt    = [&streamer](float x, float z) {
                            return streamer.heightAt(x, z);
                        };
                        rc.beginEdit = beginRiverEdit;
                        rc.endEdit   = commitRiverEdit;
                        rc.editOpen  = [&riverUndoOpen] { return riverUndoOpen; };
                        riveredit::handle(rc);
                    }
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
                        const float asp = static_cast<float>(viewW) / static_cast<float>(viewH);
                        vehiclegizmo::Context gc{*gvc, worldOf(ve),
                                                 vehGizmoSel, vehGizmoDrag};
                        gc.editable  = vehGizmoEdit;
                        gc.origin    = rmin;
                        gc.viewW     = static_cast<float>(viewW);
                        gc.viewH     = static_cast<float>(viewH);
                        gc.viewProj  = camera.projectionMatrix(asp) * camera.viewMatrix();
                        gc.hovered   = viewportHovered;
                        gc.mouseNdc  = viewportMouseNdc;
                        gc.mousePos  = mp;
                        gc.cameraPos = camera.position();
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
                if (grassPaintMode) {
                    const float asp = static_cast<float>(viewW) / static_cast<float>(viewH);
                    const glm::mat4 vp = camera.projectionMatrix(asp) * camera.viewMatrix();
                    const ImVec2 org = rmin;
                    glm::vec3 center;
                    const bool onGround = viewportHovered &&
                                          roadPickTerrain(viewportMouseNdc, vp, center);
                    const bool erasing  = brushErase || ImGui::GetIO().KeyAlt;

                    // A fresh press starts a stroke; forget the last stamp point.
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                        lastStampPos = glm::vec2(1e9f);

                    if (onGround && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        const glm::vec2 cxz(center.x, center.z);
                        if (erasing) {
                            veg.eraseGrass(cxz, brushRadius);
                        } else if (glm::length(cxz - lastStampPos) > brushRadius * 0.4f) {
                            // Throttle so a slow drag doesn't pile blades up: step
                            // ~0.4 radius between stamps for an even trail.
                            veg.stampGrass(cxz, brushRadius, brushRng, brushDensity,
                                           waterLevel, look.snowLevel);
                            lastStampPos = cxz;
                        }
                    }

                    // Brush cursor: a ground-hugging ring drawn in the overlay.
                    if (onGround) {
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        const ImU32 col = erasing ? IM_COL32(255, 90, 70, 220)
                                                  : IM_COL32(120, 235, 120, 220);
                        const int SEG = 48;
                        ImVec2 prev; bool have = false;
                        for (int i = 0; i <= SEG; ++i) {
                            const float a  = static_cast<float>(i) / SEG * 6.2831853f;
                            const float wx = center.x + std::cos(a) * brushRadius;
                            const float wz = center.z + std::sin(a) * brushRadius;
                            const glm::vec4 c = vp * glm::vec4(
                                wx, streamer.heightAt(wx, wz) + 0.05f, wz, 1.0f);
                            if (c.w <= 1e-4f) { have = false; continue; }
                            const glm::vec3 n = glm::vec3(c) / c.w;
                            const ImVec2 sp(org.x + (n.x * 0.5f + 0.5f) * viewW,
                                            org.y + (1.0f - (n.y * 0.5f + 0.5f)) * viewH);
                            if (have) dl->AddLine(prev, sp, col, 2.0f);
                            prev = sp; have = true;
                        }
                    }
                }

                // --- Tree brush: scatter/erase hand-placed trees under a circular
                //     3D brush. Drag LMB to plant; hold Alt (or Erase) to remove.
                if (treePaintMode) {
                    const float asp = static_cast<float>(viewW) / static_cast<float>(viewH);
                    const glm::mat4 vp = camera.projectionMatrix(asp) * camera.viewMatrix();
                    const ImVec2 org = rmin;
                    glm::vec3 center;
                    const bool onGround = viewportHovered &&
                                          roadPickTerrain(viewportMouseNdc, vp, center);
                    const bool erasing  = brushErase || ImGui::GetIO().KeyAlt;

                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                        lastStampPos = glm::vec2(1e9f);

                    if (onGround && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        const glm::vec2 cxz(center.x, center.z);
                        if (erasing) {
                            veg.eraseTree(cxz, veg.treeBrushRadius);
                        } else if (glm::length(cxz - lastStampPos) > veg.treeBrushRadius * 0.5f) {
                            veg.stampTree(cxz, veg.treeBrushRadius, brushRng, waterLevel,
                                          look.snowLevel);
                            lastStampPos = cxz;
                        }
                    }

                    if (onGround) {
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        const ImU32 col = erasing ? IM_COL32(255, 90, 70, 220)
                                                  : IM_COL32(90, 200, 120, 220);
                        const int SEG = 48;
                        ImVec2 prev; bool have = false;
                        for (int i = 0; i <= SEG; ++i) {
                            const float a  = static_cast<float>(i) / SEG * 6.2831853f;
                            const float wx = center.x + std::cos(a) * veg.treeBrushRadius;
                            const float wz = center.z + std::sin(a) * veg.treeBrushRadius;
                            const glm::vec4 c = vp * glm::vec4(
                                wx, streamer.heightAt(wx, wz) + 0.05f, wz, 1.0f);
                            if (c.w <= 1e-4f) { have = false; continue; }
                            const glm::vec3 n = glm::vec3(c) / c.w;
                            const ImVec2 sp(org.x + (n.x * 0.5f + 0.5f) * viewW,
                                            org.y + (1.0f - (n.y * 0.5f + 0.5f)) * viewH);
                            if (have) dl->AddLine(prev, sp, col, 2.0f);
                            prev = sp; have = true;
                        }
                    }
                }

                // --- Flower brush: scatter/erase hand-placed blooms under a
                //     circular 3D brush. Drag LMB to plant; Alt (or Erase) removes.
                if (flowerPaintMode) {
                    const float asp = static_cast<float>(viewW) / static_cast<float>(viewH);
                    const glm::mat4 vp = camera.projectionMatrix(asp) * camera.viewMatrix();
                    const ImVec2 org = rmin;
                    glm::vec3 center;
                    const bool onGround = viewportHovered &&
                                          roadPickTerrain(viewportMouseNdc, vp, center);
                    const bool erasing  = brushErase || ImGui::GetIO().KeyAlt;

                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                        lastStampPos = glm::vec2(1e9f);

                    if (onGround && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        const glm::vec2 cxz(center.x, center.z);
                        if (erasing) {
                            veg.eraseFlower(cxz, veg.flowerBrushRadius);
                        } else if (glm::length(cxz - lastStampPos) > veg.flowerBrushRadius * 0.4f) {
                            veg.stampFlower(cxz, veg.flowerBrushRadius, brushRng, waterLevel,
                                            look.snowLevel);
                            lastStampPos = cxz;
                        }
                    }

                    if (onGround) {
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        const ImU32 col = erasing ? IM_COL32(255, 90, 70, 220)
                                                  : IM_COL32(240, 150, 210, 220);
                        const int SEG = 48;
                        ImVec2 prev; bool have = false;
                        for (int i = 0; i <= SEG; ++i) {
                            const float a  = static_cast<float>(i) / SEG * 6.2831853f;
                            const float wx = center.x + std::cos(a) * veg.flowerBrushRadius;
                            const float wz = center.z + std::sin(a) * veg.flowerBrushRadius;
                            const glm::vec4 c = vp * glm::vec4(
                                wx, streamer.heightAt(wx, wz) + 0.05f, wz, 1.0f);
                            if (c.w <= 1e-4f) { have = false; continue; }
                            const glm::vec3 n = glm::vec3(c) / c.w;
                            const ImVec2 sp(org.x + (n.x * 0.5f + 0.5f) * viewW,
                                            org.y + (1.0f - (n.y * 0.5f + 0.5f)) * viewH);
                            if (have) dl->AddLine(prev, sp, col, 2.0f);
                            prev = sp; have = true;
                        }
                    }
                }

                // --- Object scatter brush: sprinkle weighted random models under
                //     a circular 3D brush (one stamp = one undo step). Drag LMB
                //     to scatter; hold Alt (or Erase) to remove scattered objects.
                if (scatterMode) {
                    const float asp = static_cast<float>(viewW) / static_cast<float>(viewH);
                    const glm::mat4 vp = camera.projectionMatrix(asp) * camera.viewMatrix();
                    const ImVec2 org = rmin;
                    glm::vec3 center;
                    const bool onGround = viewportHovered &&
                                          roadPickTerrain(viewportMouseNdc, vp, center);
                    const bool erasing  = brushErase || ImGui::GetIO().KeyAlt;

                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                        lastStampPos = glm::vec2(1e9f);

                    if (onGround && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        const glm::vec2 cxz(center.x, center.z);
                        if (erasing) {
                            scatterErase(cxz);
                        } else if (glm::length(cxz - lastStampPos) > scatterCfg.radius * 0.6f) {
                            // Throttle so a slow drag doesn't pile objects up: step
                            // ~0.6 radius between stamps for an even trail.
                            scatterStamp(cxz);
                            lastStampPos = cxz;
                        }
                    }

                    // Brush cursor: a ground-hugging ring drawn in the overlay.
                    if (onGround) {
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        const ImU32 col = erasing ? IM_COL32(255, 90, 70, 220)
                                                  : IM_COL32(255, 190, 90, 220);
                        const int SEG = 48;
                        ImVec2 prev; bool have = false;
                        for (int i = 0; i <= SEG; ++i) {
                            const float a  = static_cast<float>(i) / SEG * 6.2831853f;
                            const float wx = center.x + std::cos(a) * scatterCfg.radius;
                            const float wz = center.z + std::sin(a) * scatterCfg.radius;
                            const glm::vec4 c = vp * glm::vec4(
                                wx, streamer.heightAt(wx, wz) + 0.05f, wz, 1.0f);
                            if (c.w <= 1e-4f) { have = false; continue; }
                            const glm::vec3 n = glm::vec3(c) / c.w;
                            const ImVec2 sp(org.x + (n.x * 0.5f + 0.5f) * viewW,
                                            org.y + (1.0f - (n.y * 0.5f + 0.5f)) * viewH);
                            if (have) dl->AddLine(prev, sp, col, 2.0f);
                            prev = sp; have = true;
                        }
                    }
                }

                // --- Terrain sculpt brush: raise/lower/smooth/flatten the ground
                //     under a 3D disc that hugs the surface. Hold LMB to apply;
                //     Alt inverts raise/lower. -------------------------------
                if (sculptMode) {
                    const float asp = static_cast<float>(viewW) / static_cast<float>(viewH);
                    const glm::mat4 vp = camera.projectionMatrix(asp) * camera.viewMatrix();
                    const ImVec2 org = rmin;
                    glm::vec3 center;
                    const bool onGround = viewportHovered &&
                                          roadPickTerrain(viewportMouseNdc, vp, center);

                    // Grab the flatten target from the surface on press.
                    if (onGround && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                        sculptFlattenH = center.y;

                    // --- Pull: one gesture, absolute height ------------------
                    // Anchored on the press and driven by the mouse from there on
                    // -- deliberately NOT by where the cursor lands on the ground,
                    // because the ground it would be asking is the ground this
                    // gesture is busy moving, and a tool that reads its own output
                    // runs away from you.
                    if (sculptTool == 8) {
                        if (onGround && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            pullActive  = true;
                            pullCenter  = glm::vec2(center.x, center.z);
                            pullRadius  = sculptRadius;
                            pullShape   = pullFalloff;
                            pullApplied = 0.0f;
                            pullStartY  = ImGui::GetIO().MousePos.y;
                            // How many metres of world one pixel of drag is worth,
                            // measured AT THE ANCHOR: project a metre of height
                            // there and see how tall it comes out on screen. So
                            // the peak keeps up with the cursor whether the anchor
                            // is at your feet or across the valley, which is the
                            // difference between a tool that feels like pulling
                            // and one that feels like a slider in disguise.
                            const glm::vec4 a = vp * glm::vec4(center, 1.0f);
                            const glm::vec4 b = vp * glm::vec4(center + glm::vec3(0.0f, 1.0f, 0.0f), 1.0f);
                            const float pxPerM = (a.w > 1e-4f && b.w > 1e-4f)
                                ? std::fabs((b.y / b.w - a.y / a.w)) * 0.5f * viewH
                                : 0.0f;
                            // Clamped, because the measurement degenerates: from
                            // straight overhead a metre of height is worth almost
                            // no pixels at all, and an unclamped scale there turns
                            // a twitch into a mountain.
                            pullScale = glm::clamp(
                                (pxPerM > 0.5f) ? 1.0f / pxPerM : 0.5f, 0.01f, 0.5f);
                        }
                        if (pullActive && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                            // A drag ends by becoming the new click height, so
                            // the next hill matches the one just made without
                            // anybody having to read a number off the screen.
                            if (std::fabs(pullApplied) > 1e-3f) pullHeight = pullApplied;
                            pullActive = false;
                        }
                        if (pullActive) {
                            const ImGuiIO& io = ImGui::GetIO();
                            // The click is worth `pullHeight` on its own; moving
                            // up or down from there adjusts it. Up the screen is
                            // up the world, and Shift is the fine gear -- the same
                            // gesture over four times the travel.
                            float travel = (pullStartY - io.MousePos.y) * pullScale;
                            if (io.KeyShift) travel *= 0.25f;
                            const float want = pullHeight + travel;
                            const float step = want - pullApplied;
                            if (std::fabs(step) > 1e-4f) {
                                sculptWork.pull(pullCenter, pullRadius, step, pullShape);
                                pullApplied = want;
                                publishSculpt();
                                const float m = pullRadius + 3.0f * sculptWork.cell;
                                streamer.editsChanged(
                                    glm::vec2(pullCenter.x - m, pullCenter.y - m),
                                    glm::vec2(pullCenter.x + m, pullCenter.y + m));
                                veg.grassDirty = true;
                            }
                        }
                    }

                    // Stamp drops a landform once per click; the other tools apply
                    // continuously while the button is held.
                    const bool stampTool = (sculptTool == 5);
                    const bool apply = onGround && sculptTool != 8 &&
                        (stampTool ? ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                                   : ImGui::IsMouseDown(ImGuiMouseButton_Left));
                    if (apply) {
                        const glm::vec2 c(center.x, center.z);
                        const bool invert = ImGui::GetIO().KeyAlt;
                        switch (sculptTool) {
                            case 0: case 1: {                 // raise / lower
                                float dir = (sculptTool == 1) ? -1.0f : 1.0f;
                                if (invert) dir = -dir;
                                sculptWork.raise(c, sculptRadius,
                                                 dir * sculptStrength * 14.0f * dt);
                                break;
                            }
                            case 2:                           // smooth
                                sculptWork.smooth(streamer.settings(), c, sculptRadius,
                                    glm::clamp(sculptStrength * 5.0f * dt, 0.0f, 1.0f));
                                break;
                            case 3:                           // flatten to grabbed height
                                sculptWork.flatten(streamer.settings(), c, sculptRadius,
                                    glm::clamp(sculptStrength * 5.0f * dt, 0.0f, 1.0f),
                                    sculptFlattenH);
                                break;
                            case 4:                           // erode (weathering)
                                sculptWork.erode(streamer.settings(), c, sculptRadius,
                                    glm::clamp(sculptStrength * 6.0f * dt, 0.0f, 1.0f));
                                break;
                            case 5:                           // stamp a landform
                                sculptWork.stamp(c, sculptRadius,
                                    invert ? -stampHeight : stampHeight,
                                    stampShape, stampRot);
                                break;
                            case 6:                           // noise / roughen
                                sculptWork.roughen(c, sculptRadius,
                                    sculptStrength * 3.0f * dt, noiseFreq, noiseSeed);
                                noiseSeed += 1.7f;            // decorrelate next dab
                                break;
                            case 7:                           // carve valley (Alt: ridge)
                                sculptWork.carve(streamer.settings(), c, sculptRadius,
                                    glm::clamp(sculptStrength * 4.0f * dt, 0.0f, 1.0f),
                                    invert ? -carveDepth : carveDepth);
                                break;
                        }
                        // Publish the new shape, then rebuild the touched chunks.
                        // Erosion/stamp reach a little past the disc, so pad the
                        // rebuilt rectangle beyond the radius.
                        publishSculpt();
                        const float m = sculptRadius + 3.0f * sculptWork.cell;
                        streamer.editsChanged(glm::vec2(c.x - m, c.y - m),
                                              glm::vec2(c.x + m, c.y + m));
                        veg.grassDirty = true; // vegetation re-drapes on the new ground
                    }

                    // Brush cursor: a ground-hugging ring, coloured per tool.
                    // Once a pull is under way the ring stays on its ANCHOR --
                    // that is where the edit is, and a ring that followed the
                    // cursor would be pointing at ground the tool is not touching.
                    const bool ringHere = onGround || pullActive;
                    const glm::vec2 ringAt = pullActive ? pullCenter
                                                        : glm::vec2(center.x, center.z);
                    const float ringR = pullActive ? pullRadius : sculptRadius;
                    if (ringHere) {
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        const ImU32 col = sculptTool == 8 ? IM_COL32(150, 255, 210, 235)
                                        : sculptTool == 2 ? IM_COL32(120, 200, 255, 225)
                                        : sculptTool == 3 ? IM_COL32(255, 210, 90, 225)
                                        : sculptTool == 4 ? IM_COL32(200, 150, 110, 225)
                                        : sculptTool == 5 ? IM_COL32(200, 140, 255, 225)
                                        : sculptTool == 6 ? IM_COL32(180, 180, 190, 225)
                                        : sculptTool == 7 ? IM_COL32(90, 170, 255, 225)
                                        : sculptTool == 1 ? IM_COL32(255, 130, 90, 225)
                                                          : IM_COL32(140, 235, 140, 225);
                        const int SEG = 56;
                        ImVec2 prev; bool have = false;
                        auto toScreen = [&](float wx, float wy, float wz, ImVec2& out) {
                            const glm::vec4 cc = vp * glm::vec4(wx, wy, wz, 1.0f);
                            if (cc.w <= 1e-4f) return false;
                            const glm::vec3 n = glm::vec3(cc) / cc.w;
                            out = ImVec2(org.x + (n.x * 0.5f + 0.5f) * viewW,
                                         org.y + (1.0f - (n.y * 0.5f + 0.5f)) * viewH);
                            return true;
                        };
                        for (int i = 0; i <= SEG; ++i) {
                            const float a  = static_cast<float>(i) / SEG * 6.2831853f;
                            const float wx = ringAt.x + std::cos(a) * ringR;
                            const float wz = ringAt.y + std::sin(a) * ringR;
                            ImVec2 sp;
                            if (!toScreen(wx, streamer.heightAt(wx, wz) + 0.05f, wz, sp)) {
                                have = false; continue;
                            }
                            if (have) dl->AddLine(prev, sp, col, 2.0f);
                            prev = sp; have = true;
                        }
                        // A pull also draws its own stem and says how far it has
                        // come. Reading the height off the silhouette of a hill
                        // you are in the middle of making is guesswork, and this
                        // tool is here so that a height can be aimed at.
                        if (pullActive) {
                            const float ground = streamer.heightAt(ringAt.x, ringAt.y);
                            ImVec2 foot, tip;
                            if (toScreen(ringAt.x, ground - pullApplied, ringAt.y, foot) &&
                                toScreen(ringAt.x, ground, ringAt.y, tip)) {
                                dl->AddLine(foot, tip, col, 2.0f);
                                dl->AddCircleFilled(tip, 4.0f, col);
                                char lbl[32];
                                std::snprintf(lbl, sizeof lbl, "%+.2f m", pullApplied);
                                dl->AddText(ImVec2(tip.x + 8.0f, tip.y - 8.0f), col, lbl);
                            }
                        }
                    }
                }

                // --- Terrain texture paint brush: paint the chosen layer onto the
                //     ground under a 3D disc. Hold LMB to paint; Alt (or Erase)
                //     reverts toward the automatic height/slope blend. ----------
                if (paintMode) {
                    const float asp = static_cast<float>(viewW) / static_cast<float>(viewH);
                    const glm::mat4 vp = camera.projectionMatrix(asp) * camera.viewMatrix();
                    const ImVec2 org = rmin;
                    glm::vec3 center;
                    const bool onGround = viewportHovered &&
                                          roadPickTerrain(viewportMouseNdc, vp, center);
                    if (onGround && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        const glm::vec2 c(center.x, center.z);
                        const bool  erasing = paintErase || ImGui::GetIO().KeyAlt;
                        const float rate = glm::clamp(paintStrength * 4.0f * dt, 0.0f, 1.0f);
                        if (erasing) paintWork.erase(c, paintRadius, rate);
                        else         paintWork.paint(c, paintRadius, paintLayer, rate);
                        // Republish + rebuild the touched chunks (paint is baked into
                        // the mesh, so it rides the same edit-rebuild path as sculpt).
                        publishPaint();
                        const float m = paintRadius + 2.0f * paintWork.cell;
                        streamer.editsChanged(glm::vec2(c.x - m, c.y - m),
                                              glm::vec2(c.x + m, c.y + m));
                    }
                    // Brush cursor: a ground-hugging ring (teal paint / grey erase).
                    if (onGround) {
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        const bool erasing = paintErase || ImGui::GetIO().KeyAlt;
                        const ImU32 col = erasing ? IM_COL32(205, 205, 215, 225)
                                                  : IM_COL32(90, 230, 210, 225);
                        const int SEG = 56;
                        ImVec2 prev; bool have = false;
                        for (int i = 0; i <= SEG; ++i) {
                            const float a  = static_cast<float>(i) / SEG * 6.2831853f;
                            const float wx = center.x + std::cos(a) * paintRadius;
                            const float wz = center.z + std::sin(a) * paintRadius;
                            const glm::vec4 cc = vp * glm::vec4(
                                wx, streamer.heightAt(wx, wz) + 0.05f, wz, 1.0f);
                            if (cc.w <= 1e-4f) { have = false; continue; }
                            const glm::vec3 n = glm::vec3(cc) / cc.w;
                            const ImVec2 sp(org.x + (n.x * 0.5f + 0.5f) * viewW,
                                            org.y + (1.0f - (n.y * 0.5f + 0.5f)) * viewH);
                            if (have) dl->AddLine(prev, sp, col, 2.0f);
                            prev = sp; have = true;
                        }
                    }
                }

                // --- Mesh texture paint brush: the terrain's layers, brushed
                //     onto the selected modelled object. Hold LMB over the mesh;
                //     Alt (or Erase) takes the paint back off. --------------
                if (meshPaintMode) {
                    MeshComponent* mc = selectedMesh();
                    // Bank a stroke that is in progress, whichever way this block
                    // is left. Without it, dropping the brush mid-stroke keeps the
                    // snapshot around and the NEXT stroke undoes back past it --
                    // one Ctrl+Z throwing away work the user never joined up.
                    // Looks the entity up by id: the push replaces components, so
                    // no pointer taken before it survives, `mc` included.
                    auto bankStroke = [&]{
                        if (!meshPaintStroking) return;
                        meshPaintStroking = false;
                        Entity* e = document.find(meshPaintBefore.id);
                        if (!e) return;
                        auto cmd = std::make_unique<ModifyEntityCmd>(meshPaintBefore, *e);
                        if (!cmd->trivial()) history.pushApplied(std::move(cmd));
                    };
                    // Only one tool may own the left button. The older panels each
                    // switch their rivals off from their own list; rather than add
                    // this one to six of them, the newcomer yields -- the same deal
                    // the spline editor takes above.
                    if (grassPaintMode || treePaintMode || flowerPaintMode ||
                        sculptMode || paintMode || scatterMode || roadEditMode ||
                        splineEditMode || riverEditMode) {
                        bankStroke();
                        meshPaintMode = false;
                    } else if (!mc) {
                        bankStroke();
                        // The selection moved off the mesh -- there is nothing to
                        // paint on, so let go of the left button rather than sit
                        // on it invisibly.
                        meshPaintMode = false;
                    } else {
                        Entity& me = entities[sel.index()];
                        // The matrix the mesh is DRAWN through, so the brush
                        // measures metres where the user sees them even on an
                        // object somebody scaled.
                        glm::vec3 mn, mx;
                        mc->mesh.bounds(mn, mx);
                        const glm::vec3 sz = glm::max(mx - mn, glm::vec3(1e-4f));
                        const glm::mat4 mm =
                            composeModel(me.center, me.rotation, (me.half * 2.0f) / sz);

                        const float asp = static_cast<float>(viewW) / static_cast<float>(viewH);
                        const glm::mat4 vp = camera.projectionMatrix(asp) * camera.viewMatrix();
                        const glm::mat4 inv = glm::inverse(vp);
                        glm::vec4 pn = inv * glm::vec4(viewportMouseNdc, -1.0f, 1.0f); pn /= pn.w;
                        glm::vec4 pf = inv * glm::vec4(viewportMouseNdc,  1.0f, 1.0f); pf /= pf.w;
                        const glm::vec3 ro = glm::vec3(pn);
                        const glm::vec3 rd = glm::normalize(glm::vec3(pf) - glm::vec3(pn));

                        meshpaint::Hit hit;
                        const bool onMesh = viewportHovered &&
                                            meshpaint::pick(mc->mesh, mm, ro, rd, hit);
                        const bool erasing = meshPaintErase || ImGui::GetIO().KeyAlt;

                        // An empty slot has nothing to lay down, so the brush does
                        // not lay it down: weights in a slot the shader will skip
                        // are invisible work, and the panel says so where the slot
                        // is chosen. Erasing stays available -- taking paint off
                        // needs no slot at all.
                        const bool slotReady =
                            meshPaintSlot >= 0 &&
                            meshPaintSlot < static_cast<int>(mc->paintSlots.size()) &&
                            mc->paintSlots[meshPaintSlot].material.valid();
                        if (onMesh && (slotReady || erasing) &&
                            ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                            if (!meshPaintStroking) {
                                meshPaintStroking = true;
                                meshPaintBefore   = me; // the whole stroke undoes as one
                            }
                            const float rate =
                                glm::clamp(meshPaintStrength * 4.0f * dt, 0.0f, 1.0f);
                            // Split first, then paint: the dab lands on the corners
                            // the split just made rather than on the four the face
                            // started with. Erasing never splits -- taking paint off
                            // needs no more corners than putting it on did.
                            const int split =
                                erasing ? 0
                                        : meshpaint::refine(mc->mesh, mm, hit.world,
                                                            meshPaintRadius, meshPaintDetail,
                                                            kMeshPaintMaxFaces);
                            const bool dabbed =
                                meshpaint::dab(mc->mesh, mm, hit.world, meshPaintRadius,
                                               meshPaintSlot, rate, erasing);
                            if (split > 0 || dabbed) mc->touch();
                            // A split leaves the selected index pointing at a
                            // QUARTER of the face it was picked on. Rather than
                            // hand the modelling panel a face nobody chose, drop
                            // the selection.
                            if (split > 0) meshFaceSel = -1;
                        }
                        // The stroke ended -- but the undo push is deferred to the
                        // BOTTOM of this block on purpose. Pushing runs the
                        // command's redo, which assigns the "after" snapshot over
                        // the entity and so replaces its components with fresh
                        // clones: every MeshComponent* taken above is dead the
                        // instant it happens, `mc` included, and the brush cursor
                        // below still wants it. Bank the stroke once nothing needs
                        // the pointer any more.
                        const bool endStroke =
                            meshPaintStroking && !ImGui::IsMouseDown(ImGuiMouseButton_Left);

                        // Brush cursor: a ring lying in the surface it would paint,
                        // lifted a hair off it so it is not swallowed by the face.
                        if (onMesh && mc->mesh.validFace(hit.face)) {
                            std::vector<glm::vec3> w;
                            for (int i : mc->mesh.faces[hit.face])
                                w.push_back(glm::vec3(mm * glm::vec4(mc->mesh.verts[i], 1.0f)));
                            glm::vec3 fn(0.0f, 1.0f, 0.0f);
                            if (w.size() >= 3) {
                                const glm::vec3 c = glm::cross(w[1] - w[0], w[2] - w[0]);
                                if (glm::dot(c, c) > 1e-12f) fn = glm::normalize(c);
                            }
                            // Any two axes in the face's plane will do for a circle.
                            const glm::vec3 ref = (std::abs(fn.y) > 0.9f)
                                                ? glm::vec3(1.0f, 0.0f, 0.0f)
                                                : glm::vec3(0.0f, 1.0f, 0.0f);
                            const glm::vec3 t1 = glm::normalize(glm::cross(ref, fn));
                            const glm::vec3 t2 = glm::cross(fn, t1);
                            ImDrawList* dl = ImGui::GetWindowDrawList();
                            const ImU32 col = erasing ? IM_COL32(205, 205, 215, 225)
                                                      : IM_COL32(90, 230, 210, 225);
                            const int SEG = 48;
                            ImVec2 prev; bool have = false;
                            for (int i = 0; i <= SEG; ++i) {
                                const float a = static_cast<float>(i) / SEG * 6.2831853f;
                                const glm::vec3 p = hit.world + fn * 0.01f +
                                                    (t1 * std::cos(a) + t2 * std::sin(a)) *
                                                        meshPaintRadius;
                                const glm::vec4 cc = vp * glm::vec4(p, 1.0f);
                                if (cc.w <= 1e-4f) { have = false; continue; }
                                const glm::vec3 n = glm::vec3(cc) / cc.w;
                                const ImVec2 sp(rmin.x + (n.x * 0.5f + 0.5f) * viewW,
                                                rmin.y + (1.0f - (n.y * 0.5f + 0.5f)) * viewH);
                                if (have) dl->AddLine(prev, sp, col, 2.0f);
                                prev = sp; have = true;
                            }
                        }

                        // Last thing in the block: see the comment at `endStroke`.
                        // `mc` must be treated as dangling from here on.
                        if (endStroke) bankStroke();
                    }
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
                    const float asp = static_cast<float>(viewW) / static_cast<float>(viewH);
                    const glm::mat4 vp = camera.projectionMatrix(asp) * camera.viewMatrix();
                    const ImVec2 org = rmin;
                    glm::vec3 lo, hi;
                    VolumetricFog::worldBox(volFogSet, camera.position(), lo, hi);

                    ImVec2 sp[8];
                    bool   ok[8];
                    for (int c = 0; c < 8; ++c) {
                        const glm::vec3 w((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y,
                                          (c & 4) ? hi.z : lo.z);
                        const glm::vec4 cc = vp * glm::vec4(w, 1.0f);
                        ok[c] = cc.w > 1e-4f;
                        if (ok[c]) {
                            const glm::vec3 n = glm::vec3(cc) / cc.w;
                            ok[c] = n.z <= 1.0f;
                            sp[c] = ImVec2(org.x + (n.x * 0.5f + 0.5f) * viewW,
                                           org.y + (1.0f - (n.y * 0.5f + 0.5f)) * viewH);
                        }
                    }
                    static const int kEdges[12][2] = {
                        {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7},
                        {0,4},{1,5},{2,6},{3,7}};
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const ImU32 col = volFogSet.enabled ? IM_COL32(150, 200, 255, 190)
                                                        : IM_COL32(150, 200, 255, 80);
                    for (const auto& e : kEdges)
                        if (ok[e[0]] && ok[e[1]]) dl->AddLine(sp[e[0]], sp[e[1]], col, 1.5f);
                }

                // --- Solid blocks: click to select an existing box or place a
                //     new one on the terrain; Del removes the selected block. ----
                {   // Viewport interaction: selecting works in both modes; the
                    // transform gizmo and click-to-place are Edit-mode only.
                    const float asp = static_cast<float>(viewW) / static_cast<float>(viewH);
                    const glm::mat4 view = camera.viewMatrix();
                    const glm::mat4 proj = camera.projectionMatrix(asp);
                    const glm::mat4 vp = proj * view;

                    // --- Blender-style 3D cursor -----------------------------
                    // Shift+Right-click drops the cursor onto the terrain (the look
                    // control ignores right-drag while Shift is held, see above).
                    if (!playMode && viewportHovered && ImGui::GetIO().KeyShift &&
                        ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                        glm::vec3 h;
                        if (roadPickTerrain(viewportMouseNdc, vp, h)) cursor3D = h;
                    }
                    // Draw it: a red/white split ring with crosshair ticks, always
                    // on top (2D overlay), so it reads like Blender's cursor.
                    if (cursorVisible && !playMode) {
                        const glm::vec4 cc = vp * glm::vec4(cursor3D, 1.0f);
                        if (cc.w > 1e-4f) {
                            const glm::vec3 n = glm::vec3(cc) / cc.w;
                            if (n.z <= 1.0f) {
                                const ImVec2 c(rmin.x + (n.x * 0.5f + 0.5f) * viewW,
                                               rmin.y + (1.0f - (n.y * 0.5f + 0.5f)) * viewH);
                                ImDrawList* cdl = ImGui::GetWindowDrawList();
                                const float R = 10.0f;
                                const ImU32 red = IM_COL32(232, 66, 66, 255);
                                const ImU32 wht = IM_COL32(245, 245, 245, 255);
                                for (int s = 0; s < 8; ++s) {
                                    const float a0 = s * 0.7853982f, a1 = (s + 1) * 0.7853982f;
                                    cdl->PathArcTo(c, R, a0, a1, 8);
                                    cdl->PathStroke((s & 1) ? wht : red, 0, 2.2f);
                                }
                                const ImU32 k = IM_COL32(20, 20, 20, 220);
                                cdl->AddLine({c.x - R - 5, c.y}, {c.x - R + 1, c.y}, k, 1.4f);
                                cdl->AddLine({c.x + R - 1, c.y}, {c.x + R + 5, c.y}, k, 1.4f);
                                cdl->AddLine({c.x, c.y - R - 5}, {c.x, c.y - R + 1}, k, 1.4f);
                                cdl->AddLine({c.x, c.y + R - 1}, {c.x, c.y + R + 5}, k, 1.4f);
                                cdl->AddCircleFilled(c, 1.6f, k);
                            }
                        }
                    }
                    // The face the modelling operations act on, outlined and
                    // faintly filled. Drawn as a 2D overlay like the cursor: it is
                    // an authoring mark, not something in the scene.
                    if (showModeling && !playMode) {
                        if (const MeshComponent* mc = selectedMesh()) {
                            const std::vector<glm::vec3> fw =
                                meshFaceWorld(entities[sel.index()], *mc, meshFaceSel);
                            std::vector<ImVec2> pts;
                            bool onScreen = !fw.empty();
                            for (const glm::vec3& p : fw) {
                                const glm::vec4 cc = vp * glm::vec4(p, 1.0f);
                                if (cc.w <= 1e-4f) { onScreen = false; break; }
                                const glm::vec3 n = glm::vec3(cc) / cc.w;
                                pts.push_back(ImVec2(rmin.x + (n.x * 0.5f + 0.5f) * viewW,
                                                     rmin.y + (1.0f - (n.y * 0.5f + 0.5f)) * viewH));
                            }
                            if (onScreen && pts.size() >= 3) {
                                ImDrawList* fdl = ImGui::GetWindowDrawList();
                                fdl->AddConvexPolyFilled(pts.data(),
                                                         static_cast<int>(pts.size()),
                                                         IM_COL32(255, 170, 40, 55));
                                fdl->AddPolyline(pts.data(), static_cast<int>(pts.size()),
                                                 IM_COL32(255, 195, 70, 235),
                                                 ImDrawFlags_Closed, 2.0f);
                            }
                        }
                    }

                    // Shift+S opens the Blender-style snap menu (Ctrl+S stays Save).
                    if (!playMode && viewportHovered && !ImGui::GetIO().WantTextInput &&
                        ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl &&
                        ImGui::IsKeyPressed(ImGuiKey_S))
                        ImGui::OpenPopup("##snapMenu");
                    // Moderate outer padding; the menu labels get an explicit left
                    // (and matching right) inset via Indent, since MenuItem renders
                    // its label flush to the window's inner edge otherwise.
                    const ImVec2 basePad = ImGui::GetStyle().WindowPadding;
                    const float  inset   = basePad.x * 0.9f;
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                        ImVec2(basePad.x, basePad.y * 1.7f));
                    if (ImGui::BeginPopup("##snapMenu")) {
                        const bool haveSel = cursorHaveSel();
                        ImGui::Indent(inset);
                        ImGui::TextDisabled("Snap");
                        ImGui::Unindent(inset);
                        ImGui::Separator();
                        ImGui::Indent(inset);
                        // Trailing spaces reserve right-edge room so the label isn't
                        // flush against the popup's right border either.
                        if (ImGui::MenuItem("Cursor to World Origin      ")) snapCursorToOrigin();
                        if (ImGui::MenuItem("Cursor to Grid      "))         snapCursorToGrid();
                        if (ImGui::MenuItem("Cursor to Terrain      "))      snapCursorToTerrain();
                        if (ImGui::MenuItem("Cursor to Selection      ", nullptr, false, haveSel))
                            snapCursorToSelection();
                        ImGui::Unindent(inset);
                        ImGui::Separator();
                        ImGui::Indent(inset);
                        if (ImGui::MenuItem("Selection to Cursor      ", nullptr, false, haveSel))
                            snapSelectionToCursor();
                        if (ImGui::MenuItem("Selection to Grid      ", nullptr, false, haveSel))
                            snapSelectionToGrid();
                        ImGui::Unindent(inset);
                        ImGui::EndPopup();
                    }
                    ImGui::PopStyleVar(); // WindowPadding

                    // Transform gizmo for the selected block (move / scale).
                    if (entityEditMode) {
                        ImGuizmo::SetOrthographic(false);
                        ImGuizmo::SetDrawlist();
                        ImGuizmo::SetRect(rmin.x, rmin.y, static_cast<float>(viewW),
                                                          static_cast<float>(viewH));
                        // A finished gizmo drag becomes one undoable Transform step.
                        if (gizmoActive && !ImGuizmo::IsUsing()) {
                            gizmoActive = false;
                            auto cmd = std::make_unique<ModifyEntitiesCmd>(
                                gizmoBefore, snapshotEntities(gizmoIds));
                            if (!cmd->trivial()) history.pushApplied(std::move(cmd));
                        }
                    }
                    if (sel.valid() &&
                        entities[sel.index()].type != EntityType::Sun) {
                        Entity& b = entities[sel.index()];
                        const int selId = b.id;
                        float t[3] = {b.center.x, b.center.y, b.center.z};
                        float r[3] = {b.rotation.x, b.rotation.y, b.rotation.z};
                        float s[3] = {b.half.x * 2.0f, b.half.y * 2.0f, b.half.z * 2.0f};

                        // --- Face gizmo -------------------------------------
                        // With a face selected in Modeling, the gizmo drives THAT
                        // face rather than the object: the same Move/Rotate/Scale
                        // handles (Q/W/E), the same drag, applied to four corners
                        // instead of a transform. The panel's numbered buttons
                        // stay -- typing 0.4 m and dragging to about 0.4 m are
                        // different tools, and which one is right depends on the
                        // day and on the hand.
                        MeshComponent* faceMc =
                            (showModeling && entityEditMode) ? b.components.get<MeshComponent>()
                                                             : nullptr;
                        if (faceMc && !faceMc->mesh.validFace(meshFaceSel)) faceMc = nullptr;
                        if (faceMc) {
                            glm::vec3 mn, mx;
                            faceMc->mesh.bounds(mn, mx);
                            const glm::vec3 sz = glm::max(mx - mn, glm::vec3(1e-4f));
                            const glm::mat4 M  =
                                composeModel(b.center, b.rotation, (b.half * 2.0f) / sz);
                            // The gizmo sits at the face's centre, oriented like
                            // the object. Handed over fresh each frame; what comes
                            // back is a DELTA, which is the only form that can be
                            // baked into geometry -- an absolute matrix would be
                            // re-applied on top of itself every frame and a scale
                            // drag would run away exponentially.
                            const glm::mat4 F =
                                glm::translate(glm::mat4(1.0f),
                                               faceMc->mesh.faceCenter(meshFaceSel));
                            glm::mat4 world = M * F;
                            float delta[16];
                            ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj),
                                                 gizmoOp, gizmoMode,
                                                 glm::value_ptr(world), delta);
                            const bool using3d = ImGuizmo::IsUsing();
                            if (using3d && !faceGizmoActive) {
                                faceGizmoActive   = true;
                                faceGizmoBefore   = b;   // one undo step per drag
                                faceGizmoScale    = meshScaleOf(b, *faceMc);
                                faceGizmoAccScale = glm::vec3(1.0f);
                            }
                            if (using3d) {
                                const glm::mat4 D = glm::make_mat4(delta);
                                glm::mat4 L(1.0f);
                                if (gizmoOp == ImGuizmo::SCALE) {
                                    // ImGuizmo reports the scale delta on different
                                    // terms from the other two: measured from the
                                    // START of the drag, and in the gizmo's own
                                    // frame -- while move and rotate report the step
                                    // since the last frame, in world space. Applying
                                    // it as if it were a step multiplies the face by
                                    // the whole drag again every frame, which runs
                                    // away exponentially. Divide out what has
                                    // already been applied to get the actual step.
                                    const glm::vec3 acc(D[0][0], D[1][1], D[2][2]);
                                    const glm::vec3 step =
                                        acc / glm::max(faceGizmoAccScale, glm::vec3(1e-6f));
                                    faceGizmoAccScale = acc;
                                    L = F * glm::scale(glm::mat4(1.0f), step) *
                                        glm::inverse(F);
                                } else {
                                    // World-space step, conjugated into the mesh's
                                    // own space: p' = M^-1 * D * M * p.
                                    L = glm::inverse(M) * D * M;
                                }
                                editmesh::transformFace(faceMc->mesh, meshFaceSel, L);
                                // Square the object's bounds with the new shape NOW,
                                // not when the drag ends. The mesh is drawn at
                                // half/bounds, so leaving `half` behind while the
                                // geometry grows shrinks that factor by exactly as
                                // much as the mesh grew: the shape would sit there
                                // apparently unmoved while its local size ran off,
                                // and let go of it at the end to reveal a body
                                // stretched to the horizon.
                                normalizeMeshEntity(b, *faceMc, faceGizmoScale);
                            } else if (faceGizmoActive) {
                                // Drag finished: bank the whole of it as one
                                // undoable step. The bounds are already square with
                                // the shape -- that happens on every frame above.
                                faceGizmoActive = false;
                                auto cmd = std::make_unique<ModifyEntityCmd>(faceGizmoBefore, b);
                                if (!cmd->trivial()) history.pushApplied(std::move(cmd));
                            }
                        }
                        else if (entityEditMode && !vehGizmoOwnsMouse) {
                            float model[16];
                            ImGuizmo::RecomposeMatrixFromComponents(t, r, s, model);
                            ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj),
                                                 gizmoOp, gizmoMode, model);
                            const bool gizmoUsing = ImGuizmo::IsUsing();
                            if (gizmoUsing && !gizmoActive) { // drag start: snapshot subtrees
                                gizmoActive = true;
                                gizmoRoots  = sel.ids();
                                gizmoIds.clear();
                                for (int rid : gizmoRoots)
                                    for (int id : collectSubtreeIds(rid))
                                        if (std::find(gizmoIds.begin(), gizmoIds.end(), id)
                                                == gizmoIds.end())
                                            gizmoIds.push_back(id);
                                gizmoBefore = snapshotEntities(gizmoIds);
                                gizmoPrevT = glm::vec3(t[0], t[1], t[2]);
                                gizmoPrevR = glm::vec3(r[0], r[1], r[2]);
                                gizmoPrevS = glm::vec3(s[0], s[1], s[2]);
                            }
                            if (gizmoUsing) {
                                ImGuizmo::DecomposeMatrixToComponents(model, t, r, s);
                                const glm::vec3 newT(t[0], t[1], t[2]);
                                const glm::vec3 newR(r[0], r[1], r[2]);
                                const glm::vec3 newS(s[0], s[1], s[2]);
                                b.half = glm::max(newS * 0.5f, glm::vec3(0.05f));
                                // World-space edit -> local (children then follow via
                                // resolveHierarchy).
                                const glm::mat4 pw = parentWorldMat(b);
                                setWorld(b, newT, newR, b.parent >= 0 ? &pw : nullptr);
                                // Multi-select: apply the active object's incremental
                                // delta to every other selected root (each scales /
                                // rotates about its own centre; children follow via
                                // resolveHierarchy).
                                if (gizmoRoots.size() > 1) {
                                    const glm::vec3 dT = newT - gizmoPrevT;
                                    const glm::vec3 dR = newR - gizmoPrevR;
                                    const glm::vec3 ratio =
                                        newS / glm::max(gizmoPrevS, glm::vec3(1e-4f));
                                    for (int rid : gizmoRoots) {
                                        if (rid == selId) continue;
                                        Entity* re = document.find(rid);
                                        if (!re || re->type == EntityType::Sun) continue;
                                        re->half = glm::max(re->half * ratio, glm::vec3(0.05f));
                                        const glm::mat4 rpw = parentWorldMat(*re);
                                        setWorld(*re, re->center + dT, re->rotation + dR,
                                                 re->parent >= 0 ? &rpw : nullptr);
                                    }
                                }
                                gizmoPrevT = newT; gizmoPrevR = newR; gizmoPrevS = newS;
                            }
                        }

                        // Oriented wireframe highlight. One projector, reused for the
                        // active object (bright) and any other selected objects (dim),
                        // so a multi-selection shows every picked box.
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        auto wireBox = [&](const Entity& e, ImU32 col, float thick) {
                            const glm::mat4 boxX =
                                composeModel(e.center, e.rotation, glm::vec3(1.0f));
                            ImVec2 sp[8]; bool ok[8];
                            for (int c = 0; c < 8; ++c) {
                                const glm::vec3 lh((c & 1) ? e.half.x : -e.half.x,
                                                   (c & 2) ? e.half.y : -e.half.y,
                                                   (c & 4) ? e.half.z : -e.half.z);
                                const glm::vec4 cc = vp * (boxX * glm::vec4(lh, 1.0f));
                                ok[c] = cc.w > 1e-4f;
                                if (ok[c]) {
                                    const glm::vec3 n = glm::vec3(cc) / cc.w;
                                    ok[c] = n.z <= 1.0f;
                                    sp[c] = ImVec2(rmin.x + (n.x * 0.5f + 0.5f) * viewW,
                                                   rmin.y + (1.0f - (n.y * 0.5f + 0.5f)) * viewH);
                                }
                            }
                            static const int kBoxEdges[12][2] = {
                                {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7},
                                {0,4},{1,5},{2,6},{3,7}};
                            for (const auto& ed : kBoxEdges)
                                if (ok[ed[0]] && ok[ed[1]])
                                    dl->AddLine(sp[ed[0]], sp[ed[1]], col, thick);
                        };
                        // Other selected objects first (dim) so the active box (bright)
                        // draws on top.
                        for (int sid : sel.multi()) {
                            if (sid == selId) continue;
                            if (const Entity* se = document.find(sid))
                                wireBox(*se, IM_COL32(255, 170, 40, 150), 1.4f);
                        }
                        wireBox(b, IM_COL32(255, 140, 0, 230), 1.8f);

                        // Component gizmos: each component of the selected entity
                        // draws its own world-space overlay (a radius, a path).
                        // Generic -- the viewport only supplies the projection, so
                        // a new component brings its gizmo with no change here.
                        struct VpGizmo : GizmoDraw {
                            ImDrawList* dl; glm::mat4 vp; ImVec2 org; float vw, vh;
                            bool project(const glm::vec3& w, ImVec2& out) const {
                                const glm::vec4 c = vp * glm::vec4(w, 1.0f);
                                if (c.w <= 1e-4f) return false;
                                const glm::vec3 n = glm::vec3(c) / c.w;
                                if (n.z > 1.0f) return false;
                                out = ImVec2(org.x + (n.x * 0.5f + 0.5f) * vw,
                                             org.y + (1.0f - (n.y * 0.5f + 0.5f)) * vh);
                                return true;
                            }
                            static ImU32 toCol(const glm::vec4& c) {
                                return IM_COL32(int(c.r * 255.0f), int(c.g * 255.0f),
                                                int(c.b * 255.0f), int(c.a * 255.0f));
                            }
                            void line(const glm::vec3& a, const glm::vec3& b,
                                      const glm::vec4& c) override {
                                ImVec2 pa, pb;
                                if (project(a, pa) && project(b, pb))
                                    dl->AddLine(pa, pb, toCol(c), 2.0f);
                            }
                            void circle(const glm::vec3& ctr, float rad,
                                        const glm::vec3& axis, const glm::vec4& c) override {
                                const glm::vec3 n = glm::normalize(axis);
                                const glm::vec3 up = (std::abs(n.y) < 0.99f)
                                    ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
                                const glm::vec3 u = glm::normalize(glm::cross(n, up));
                                const glm::vec3 v = glm::cross(n, u);
                                const int SEG = 40;
                                ImVec2 prev; bool have = false;
                                for (int i = 0; i <= SEG; ++i) {
                                    const float a = 6.2831853f * i / SEG;
                                    ImVec2 s2;
                                    if (!project(ctr + (u * std::cos(a) + v * std::sin(a)) * rad, s2)) {
                                        have = false; continue;
                                    }
                                    if (have) dl->AddLine(prev, s2, toCol(c), 1.5f);
                                    prev = s2; have = true;
                                }
                            }
                        };
                        VpGizmo gz;
                        gz.dl = dl; gz.vp = vp; gz.org = rmin;
                        gz.vw = static_cast<float>(viewW); gz.vh = static_cast<float>(viewH);
                        if (b.parent >= 0)
                            if (const Entity* pe = document.find(b.parent)) {
                                gz.parentCenter = pe->center;
                                gz.parentHalf   = pe->half;
                                gz.hasParent    = true;
                            }
                        // A multishot camera's "parent" for gizmo purposes is
                        // what it SHOOTS, which is deliberately not what it hangs
                        // from (see CameraComponent::shotTarget). Same question --
                        // which object is this camera about -- so it goes down the
                        // same channel rather than growing a second one.
                        if (const auto* mcam = b.components.get<CameraComponent>();
                            mcam && mcam->mode == CameraComponent::Multishot &&
                            mcam->shotTarget >= 0)
                            if (const Entity* se = document.find(mcam->shotTarget)) {
                                gz.parentCenter = se->center;
                                gz.parentHalf   = se->half;
                                gz.hasParent    = true;
                            }
                        for (const auto& comp : b.components.items)
                            comp->onGizmo(gz, b.center, glm::quat(glm::radians(b.rotation)));
                    }

                    // Empties have no mesh, so draw a constant-size screen icon at
                    // each one (editor only) -- otherwise they'd be invisible and
                    // only reachable from the hierarchy. Their AABB pick box still
                    // makes them clickable in the viewport.
                    if (!playMode) {
                        ImDrawList* odl = ImGui::GetWindowDrawList();
                        for (const Entity& e : entities) {
                            if (e.type != EntityType::Empty) continue;
                            if (!e.activeInHierarchy) continue;   // hidden group node
                            const glm::vec4 cc = vp * glm::vec4(e.center, 1.0f);
                            if (cc.w <= 1e-4f) continue;
                            const glm::vec3 n = glm::vec3(cc) / cc.w;
                            if (n.z > 1.0f) continue;
                            const ImVec2 sc(rmin.x + (n.x * 0.5f + 0.5f) * viewW,
                                            rmin.y + (1.0f - (n.y * 0.5f + 0.5f)) * viewH);
                            const float r = 7.0f;
                            const ImU32 col = IM_COL32(170, 175, 185, 220);
                            odl->AddLine({sc.x - r, sc.y}, {sc.x + r, sc.y}, col, 1.5f);
                            odl->AddLine({sc.x, sc.y - r}, {sc.x, sc.y + r}, col, 1.5f);
                            odl->AddCircle(sc, r * 0.45f, col, 0, 1.5f);
                            if (!e.name.empty())
                                odl->AddText({sc.x + r + 3.0f, sc.y - 7.0f}, col,
                                             e.name.c_str());
                        }
                    }

                    // Click to select/place, but not while grabbing the gizmo or
                    // running a viewport tool -- the active tool owns the left
                    // button then. One predicate rather than a negation list at
                    // the `if`: a tool missing from that list silently gets both
                    // actions on one click (road points used to drop a primitive
                    // under every waypoint placed in Create mode).
                    const bool toolOwnsClick =
                        grassPaintMode || treePaintMode || flowerPaintMode ||
                        roadEditMode || sculptMode || paintMode || scatterMode ||
                        splineEditMode || meshPaintMode || riverEditMode ||
                        vehGizmoOwnsMouse;
                    const ImGuiIO& io = ImGui::GetIO();
                    const bool selMod  = io.KeyCtrl; // Ctrl = modify-selection gesture
                    const bool canPick = !ImGuizmo::IsOver() && !ImGuizmo::IsUsing() &&
                                         !toolOwnsClick && viewportHovered;
                    // The entity ids the mouse ray passes through, nearest first
                    // (shared by plain click and Ctrl+click).
                    auto rayPickIds = [&]() -> std::vector<int> {
                        const glm::mat4 inv = glm::inverse(vp);
                        glm::vec4 pn = inv * glm::vec4(viewportMouseNdc, -1.0f, 1.0f); pn /= pn.w;
                        glm::vec4 pf = inv * glm::vec4(viewportMouseNdc,  1.0f, 1.0f); pf /= pf.w;
                        const glm::vec3 ro = glm::vec3(pn);
                        const glm::vec3 rd = glm::normalize(glm::vec3(pf) - glm::vec3(pn));
                        std::vector<std::pair<float, int>> hits;
                        for (int i = 0; i < static_cast<int>(entities.size()); ++i) {
                            if (!entities[i].activeInHierarchy) continue; // not shown, not pickable
                            const float t = rayAABB(ro, rd, entities[i].center - entities[i].half,
                                                            entities[i].center + entities[i].half);
                            if (t >= 0.0f) hits.emplace_back(t, entities[i].id);
                        }
                        std::sort(hits.begin(), hits.end());
                        std::vector<int> ids; ids.reserve(hits.size());
                        for (const auto& h : hits) ids.push_back(h.second);
                        return ids;
                    };

                    // Ctrl+left: start a selection gesture (a click toggles one; a
                    // drag draws an additive box).
                    if (canPick && selMod && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        boxSelecting = true;
                        boxStart = io.MousePos;
                    }
                    if (boxSelecting) {
                        const ImVec2 cur = io.MousePos;
                        const ImVec2 a(std::min(boxStart.x, cur.x), std::min(boxStart.y, cur.y));
                        const ImVec2 b2(std::max(boxStart.x, cur.x), std::max(boxStart.y, cur.y));
                        ImDrawList* bdl = ImGui::GetWindowDrawList();
                        bdl->AddRectFilled(a, b2, IM_COL32(255, 160, 0, 40));
                        bdl->AddRect(a, b2, IM_COL32(255, 160, 0, 180));
                        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                            boxSelecting = false;
                            const float dragPx = std::max(std::abs(cur.x - boxStart.x),
                                                          std::abs(cur.y - boxStart.y));
                            if (dragPx < 4.0f) { // no drag -> toggle the object clicked
                                const std::vector<int> ids = rayPickIds();
                                if (!ids.empty()) sel.toggle(ids[0]);
                            } else {             // box -> add every centre inside the rect
                                std::vector<int> inBox;
                                for (const Entity& e : entities) {
                                    if (!e.activeInHierarchy || e.type == EntityType::Sun) continue;
                                    const glm::vec4 cc = vp * glm::vec4(e.center, 1.0f);
                                    if (cc.w <= 1e-4f) continue;
                                    const glm::vec3 n = glm::vec3(cc) / cc.w;
                                    if (n.z > 1.0f) continue;
                                    const ImVec2 sc(rmin.x + (n.x * 0.5f + 0.5f) * viewW,
                                                    rmin.y + (1.0f - (n.y * 0.5f + 0.5f)) * viewH);
                                    if (sc.x >= a.x && sc.x <= b2.x &&
                                        sc.y >= a.y && sc.y <= b2.y)
                                        inBox.push_back(e.id);
                                }
                                sel.addMany(inBox);
                            }
                        }
                    }
                    // Plain left-click (no Ctrl): select/place exactly as before.
                    else if (canPick && !selMod &&
                             ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        // ...except while modelling, where a click that lands on
                        // the selected mesh picks one of its FACES. It only takes
                        // the click when it actually hits that mesh, so clicking
                        // anything else still selects objects as usual -- and
                        // clicking the object you already have selected was a
                        // no-op anyway, which is the click this borrows.
                        int   faceHit = -1;
                        float faceT   = 1e30f;
                        if (showModeling) {
                            if (const MeshComponent* mc = selectedMesh()) {
                                const Entity& me = entities[sel.index()];
                                const glm::mat4 inv = glm::inverse(vp);
                                glm::vec4 pn = inv * glm::vec4(viewportMouseNdc, -1.0f, 1.0f); pn /= pn.w;
                                glm::vec4 pf = inv * glm::vec4(viewportMouseNdc,  1.0f, 1.0f); pf /= pf.w;
                                const glm::vec3 ro = glm::vec3(pn);
                                const glm::vec3 rd = glm::normalize(glm::vec3(pf) - glm::vec3(pn));
                                for (int f = 0; f < static_cast<int>(mc->mesh.faces.size()); ++f) {
                                    const std::vector<glm::vec3> w = meshFaceWorld(me, *mc, f);
                                    // Same fan the GPU mesh is built from, so what
                                    // is picked is exactly what is drawn.
                                    for (std::size_t i = 1; i + 1 < w.size(); ++i) {
                                        const float t = rayTriangle(ro, rd, w[0], w[i], w[i + 1]);
                                        if (t >= 0.0f && t < faceT) { faceT = t; faceHit = f; }
                                    }
                                }
                            }
                        }
                        if (faceHit >= 0) {
                            meshFaceSel = faceHit;   // the click went to the face
                        } else {
                        // A click that missed every face lets go of the one that
                        // was selected -- which is also how the gizmo is handed
                        // back to the whole object.
                        meshFaceSel = -1;
                        const std::vector<int> ids = rayPickIds();
                        if (!ids.empty()) {
                            // Same overlapping stack as last click -> advance to the
                            // next candidate; a new stack -> start at the nearest.
                            if (ids == pickStack)
                                pickIdx = (pickIdx + 1) % static_cast<int>(ids.size());
                            else { pickStack = ids; pickIdx = 0; }
                            sel.select(ids[pickIdx]);
                        } else if (placeMode) {
                            glm::vec3 h; // Create mode: empty ground -> drop a new block
                            if (roadPickTerrain(viewportMouseNdc, vp, h)) addEntity(h, entityNewType);
                        } else {
                            sel.clear(); // empty click clears it
                            pickStack.clear(); pickIdx = -1;
                        }
                        }
                    }
                    if (sel.valid() &&
                        ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                        deleteSelection();
                    }
                }
            } else {
                viewportHovered = false;
                viewportClicked = false;
            }
            ImGui::End();
            ImGui::PopStyleVar();

            if (showAbout) {
                ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_Appearing);
                if (ImGui::Begin("About Fitzel", &showAbout,
                                 ImGuiWindowFlags_NoDocking |
                                 ImGuiWindowFlags_NoSavedSettings)) {
                    ImGui::Text("Fitzel %d.%d.%d",
                                fitzel::kVersionMajor, fitzel::kVersionMinor,
                                fitzel::kVersionPatch);
                    ImGui::TextDisabled("3D vegetation & road engine");
                    ImGui::Separator();
                    // The four-part version alone can't tell two builds of one
                    // commit apart, so show what identifies this binary exactly.
                    ImGui::Text("Build %d", fitzel::kVersionBuild);
                    if (fitzel::kGitHash[0])
                        ImGui::Text("Commit %s%s", fitzel::kGitHash,
                                    fitzel::kGitDirty ? " (uncommitted changes)" : "");
                    ImGui::Spacing();
                    if (ImGui::Button("Copy version"))
                        ImGui::SetClipboardText(fitzel::kVersionFull);
                }
                ImGui::End();
            }

            if (showStats) { if (ImGui::Begin("Stats", &showStats)) {
                const char* sceneNames[] = {"Nature", "Empty (build)"};
                if (ImGui::Combo("Scene", &scene, sceneNames, 2)) applyScene(scene);
                ImGui::Separator();
                ImGui::Text("%.1f FPS (%.2f ms)", ImGui::GetIO().Framerate,
                            1000.0f / ImGui::GetIO().Framerate);
                ImGui::Text("Camera: %.0f, %.0f, %.0f",
                            camera.position().x, camera.position().y, camera.position().z);
                ImGui::Text("Chunks: %d loaded, %d pending",
                            streamer.loadedChunkCount(), streamer.pendingChunkCount());
                ImGui::Text("Entities: %d (%d selected)",
                            static_cast<int>(entities.size()),
                            static_cast<int>(sel.count()));
                ImGui::Text("Draws: %d visible, %d culled",
                            renderer.lastDrawn(), renderer.lastCulled());
                ImGui::Separator();
                ImGui::SliderFloat("Move speed", &camera.moveSpeed, 2.0f, 80.0f);
                ImGui::SliderInt("View distance", &viewRadius, 2, 9, "%d chunks");
                ImGui::SameLine();
                ImGui::Text("(%.0f m)", viewRadius * streamer.settings().chunkSize);
                // The culling limit. Deliberately next to View distance and the
                // draw counters above: those three are one dial each on the same
                // trade, and the counters are the readout you tune against.
                ImGui::Checkbox("Auto far plane", &farPlaneAuto);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Tie the culling limit to the streamed terrain\n"
                                      "(1.7 chunks past the ring, so its corners\n"
                                      "stay inside the frustum) -- and to the\n"
                                      "roadside city's range, whichever reaches\n"
                                      "further, so a skyline is never sliced off\n"
                                      "before its own range runs out. Turn off to\n"
                                      "set it by hand.");
                ImGui::BeginDisabled(farPlaneAuto);
                ImGui::SliderFloat("Far plane", &farPlaneManual, 100.0f, 5000.0f,
                                   "%.0f m");
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Where the camera stops drawing. Past the\n"
                                      "streamed terrain you see the ring end;\n"
                                      "past ~2000 m the depth buffer starts\n"
                                      "fighting itself in the distance (the near\n"
                                      "plane is 0.1 m). Set by hand this does NOT\n"
                                      "follow the city's range, so a city set to\n"
                                      "reach further than this gets cut off here.\n"
                                      "Shadows stop at the streamed terrain either\n"
                                      "way -- pushing this out costs draws, not\n"
                                      "shadow resolution.");
                ImGui::SameLine();
                ImGui::TextDisabled("now %.0f m", camera.farPlane());
                ImGui::Separator();
                if (ImGui::Button("Reset layout")) requestDockRebuild = true;
            }
            ImGui::End(); }

            if (showCamera) { if (ImGui::Begin("Camera", &showCamera)) {
                if (ImGui::Checkbox("First-person (Shift+F)", &fpsMode)) {
                    input.setCursorLocked(fpsMode);
                    fpsVelY = 0.0f;
                    if (fpsMode) {
                        const glm::vec3 p = camera.position();
                        camera.setPosition({p.x, streamer.heightAt(p.x, p.z) + eyeHeight, p.z});
                    }
                }
                ImGui::SameLine();
                ImGui::TextDisabled(fpsMode ? "(walk + jump, Esc to exit)"
                                            : "(hold right mouse: look + WASD/QE fly)");
                // Sync from the camera (mouse-look may have changed it), then
                // apply only when a slider is actually edited.
                camFov = camera.fov(); camYaw = camera.yaw(); camPitch = camera.pitch();
                if (ImGui::SliderFloat("FOV",   &camFov, 25.0f, 100.0f, "%.0f deg"))
                    camera.setFov(camFov);
                if (ImGui::SliderFloat("Yaw",   &camYaw, -180.0f, 180.0f, "%.0f"))
                    camera.setYaw(camYaw);
                if (ImGui::SliderFloat("Pitch", &camPitch, -89.0f, 89.0f, "%.0f"))
                    camera.setPitch(camPitch);
            }
            ImGui::End(); }

            // The audio mixer, as a strip of vertical faders.
            if (showMixer) { if (ImGui::Begin("Mixer", &showMixer)) {
                ImGui::TextDisabled("Master scales the device; Ambient the weather "
                                    "loops, SFX the one-shots");
                ImGui::Separator();
                auto fader = [&](const char* name, float* level, bool* mute) {
                    ImGui::PushID(name);
                    ImGui::BeginGroup();
                    ImGui::TextUnformatted(name);
                    if (*mute) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(185, 65, 60, 255));
                    if (ImGui::Button("Mute", ImVec2(46.0f, 0.0f))) *mute = !*mute;
                    if (*mute) ImGui::PopStyleColor();
                    ImGui::VSliderFloat("##v", ImVec2(46.0f, 150.0f), level, 0.0f, 1.0f, "");
                    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                        ImGui::SetTooltip("%.0f%%", *level * 100.0f);
                    ImGui::Text("%3.0f%%", *level * 100.0f);
                    ImGui::EndGroup();
                    ImGui::PopID();
                    ImGui::SameLine();
                };
                fader("Master",  &masterVolume,     &muted);
                fader("Ambient", &mixAmbient.level, &mixAmbient.mute);
                fader("SFX",     &mixSfx.level,     &mixSfx.mute);
                ImGui::NewLine();
                ImGui::TextDisabled("Master feeds the device; Ambient the weather/\n"
                                    "zone loops; SFX the one-shot bus.");
            }
            ImGui::End(); }

            if (showWeather) { if (ImGui::Begin("Weather & audio", &showWeather)) {
                ImGui::Checkbox("Auto weather", &autoWeather);
                ImGui::SliderFloat("Storm", &weather, 0.0f, 1.0f);
                ImGui::Text("Rain %.0f%%   Wet %.0f%%   Lightning %s",
                            rainIntensity * 100.0f, roadWetness * 100.0f,
                            weather > 0.5f ? "armed" : "off");
                ImGui::Separator();
                ImGui::Checkbox("Mute", &muted);
                ImGui::SameLine();
                ImGui::SliderFloat("Volume", &masterVolume, 0.0f, 1.0f);
                if (!audio.ok()) ImGui::TextDisabled("(audio device unavailable)");
            }
            ImGui::End(); }

            if (showSky) { if (ImGui::Begin("Sky & atmosphere", &showSky)) {
                ImGui::SliderFloat("Time of day", &timeOfDay, 0.0f, 24.0f, "%.1f h");
                ImGui::SameLine();
                ImGui::Checkbox("Pause", &timePaused);
                ImGui::SliderFloat("Day length",  &dayLength, 0.0f, 600.0f, "%.0f s");
                ImGui::SliderFloat("Coverage",    &cloudCoverage, 0.0f, 1.0f);
                ImGui::SliderFloat("Density",     &cloudDensity, 0.0f, 3.0f);
                ImGui::SliderFloat("Cloud scale", &cloudScale, 0.0003f, 0.005f, "%.4f");
                ImGui::SliderFloat("Wind",        &cloudSpeed, 0.0f, 20.0f);
                ImGui::SliderFloat("Cloud base",  &cloudBottom, 100.0f, 3000.0f, "%.0f m");
                ImGui::SliderFloat("Cloud top",   &cloudTop, 300.0f, 7000.0f, "%.0f m");
                ui::hint("Base, top and scale decide whether the sky reads as\n"
                         "weather or as a ceiling. A cumulus is at least as\n"
                         "TALL as it is wide, so a thin slab under wide\n"
                         "features can only ever be a textured lid -- scale\n"
                         "sets that width, and LOWER means bigger clouds.\n"
                         "Coverage does two jobs: how much sky is taken, and\n"
                         "how far the tops build into it.");

                // --- The high layer -----------------------------------------
                // Its own section because it is its own weather. Cirrus sits
                // above everything the storm slider touches, and a sky with no
                // cumulus in it at all can still have this.
                if (ui::header("Cirrus & contrails")) {
                    ImGui::SliderFloat("Cirrus",   &cirrusAmount, 0.0f, 1.0f);
                    ImGui::SliderFloat("Height",   &cirrusHeight, 400.0f, 6000.0f, "%.0f m");
                    ImGui::SliderFloat("Jet wind", &cirrusSpeed, 0.0f, 12.0f);
                    ImGui::SliderFloat("Contrails", &contrailAmount, 0.0f, 1.0f);
                    ui::hint("Ice, drawn into streaks by a wind that has nothing\n"
                             "to do with the one below. Contrails come in one at\n"
                             "a time as the slider rises, each older than the\n"
                             "last -- wider, softer and more broken up.\n"
                             "Everything up here scales with Height, so raising\n"
                             "the layer does not turn aircraft into motorways.");
                }
                ImGui::SliderFloat("Fog density", &fogDensity, 0.0f, 0.02f, "%.4f");
                ImGui::SliderFloat("Fog falloff", &fogFalloff, 0.005f, 0.1f, "%.3f");

                // --- Volumetric fog: the world-wide volume ----------------
                // Folded away by default, and deliberately sitting right under
                // the two sliders it is not: those are the height haze, which is
                // everywhere and has no shape.
                //
                // This one box is the WORLD's air. Mist that belongs somewhere in
                // particular is not authored here at all -- it is a Volumetric Fog
                // component on an entity, so it can be placed, scaled and rotated
                // like anything else in the scene, and there can be many. Both end
                // up in the same march; the hint says so, because a panel that
                // does not mention the other way is a panel that hides it.
                if (ui::header("Volumetric fog (world)")) {
                    ImGui::Checkbox("Enabled##volfog", &volFogSet.enabled);
                    ImGui::SameLine();
                    ImGui::Checkbox("Show volume", &volFogSet.showVolume);
                    ui::hint("The haze above does distance. This does shape:\n"
                             "banks that drift, holes that pass, sun shafts.\n"
                             "For mist in ONE place, add a Volumetric Fog\n"
                             "component to an Empty and scale it instead.");

                    ui::sectionText("Volume");
                    ImGui::DragFloat3("Centre", &volFogSet.center.x, 0.5f,
                                      -20000.0f, 20000.0f, "%.0f m");
                    ImGui::DragFloat3("Size", &volFogSet.size.x, 0.5f,
                                      1.0f, 20000.0f, "%.0f m");
                    // Placing a volume you cannot grab is the awkward part, so
                    // the two placements anyone actually wants are buttons: put
                    // it where I am standing, and sit it on the ground under it.
                    if (ImGui::Button("Centre on camera"))
                        volFogSet.center = camera.position();
                    ImGui::SameLine();
                    if (ImGui::Button("Sit on ground"))
                        volFogSet.center.y =
                            streamer.heightAt(volFogSet.center.x, volFogSet.center.z) +
                            volFogSet.size.y * 0.5f;
                    ImGui::Checkbox("Follow camera (X/Z)", &volFogSet.followCamera);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Ground mist over a whole track without a\n"
                                          "box big enough to cover it: the same steps\n"
                                          "spread over kilometres lose all structure.");
                    ImGui::SliderFloat("Edge fade", &volFogSet.medium.edge, 0.02f, 1.0f);
                    ImGui::SliderFloat("Height falloff##volfog",
                                       &volFogSet.medium.heightFalloff, 0.0f, 3.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("How much harder it is for fog to exist near\n"
                                          "the top of the box. Carves the lid out of the\n"
                                          "noise, so the layer has a ragged top rather\n"
                                          "than a smooth fade.");

                    ui::sectionText("Medium");
                    ImGui::SliderFloat("Thickness", &volFogSet.medium.density, 0.0f, 0.5f,
                                       "%.3f /m");
                    ImGui::ColorEdit3("Tint##volfog", &volFogSet.medium.color.x);
                    ImGui::SliderFloat("Coverage##volfog", &volFogSet.medium.coverage,
                                       0.0f, 0.95f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("How much of the volume has fog in it at all.\n"
                                          "Low = a solid body, high = separate banks\n"
                                          "with clear air between them.");

                    ui::sectionText("Noise");
                    ImGui::SliderFloat("Scale##volfog", &volFogSet.medium.noiseScale,
                                       0.001f, 0.06f, "%.4f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Smaller = bigger banks.");
                    ImGui::SliderFloat("Vertical detail",
                                       &volFogSet.medium.verticalDetail, 0.25f, 8.0f,
                                       "%.2fx");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("How much finer the field is going up than\n"
                                          "sideways. At 1 a shallow layer sits inside a\n"
                                          "single feature and the fog looks like a flat\n"
                                          "pattern pulled upward.");
                    ImGui::SliderFloat("Detail", &volFogSet.medium.detail, 0.0f, 0.95f);
                    ImGui::SliderFloat("Swirl", &volFogSet.medium.warp, 0.0f, 1.5f);
                    ImGui::DragFloat3("Wind##volfog", &volFogSet.medium.wind.x, 0.05f,
                                      -30.0f, 30.0f, "%.2f m/s");

                    ui::sectionText("Light");
                    ImGui::SliderFloat("Forward scatter", &volFogSet.medium.anisotropy,
                                       -0.9f, 0.9f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("How much light keeps going the way it came.\n"
                                          "High values put the glow around the sun.");
                    ImGui::SliderFloat("Sun##volfog", &volFogSet.medium.sunIntensity, 0.0f, 4.0f);
                    ImGui::SliderFloat("Ambient##volfog", &volFogSet.medium.ambientIntensity,
                                       0.0f, 4.0f);
                    ImGui::Checkbox("Sun shafts", &volFogSet.medium.shafts);
                    ImGui::SameLine();
                    ImGui::Checkbox("Self-shadow", &volFogSet.medium.selfShadow);

                    ui::sectionText("Cost");
                    ImGui::SliderInt("Steps", &volFogSet.medium.steps, 8, 128);
                    ImGui::SliderInt("Resolution", &volFogSet.resScale, 1, 4,
                                     "1/%d of the pane");
                    ui::hint("Steps buy structure along the ray, resolution buys it\n"
                             "across the screen. Fog is soft, so 1/2 is free money.\n"
                             "Resolution is the whole PASS -- every placed volume\n"
                             "is marched into the same buffer.");
                }
                ImGui::SliderFloat("Exposure",   &exposure, 0.2f, 3.0f);
                ImGui::SliderFloat("Bloom",      &bloomIntensity, 0.0f, 1.5f);
                ImGui::SliderFloat("Bloom threshold", &bloomThreshold, 0.2f, 4.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Luminance where the glow starts. Lower it to make\n"
                                      "emissive materials bloom sooner; the knee below\n"
                                      "keeps the onset soft instead of popping.");
                ImGui::SliderFloat("Bloom knee", &bloomKnee, 0.0f, 1.5f, "%.2f");
                ImGui::SliderFloat("Sun rays",   &rayIntensity, 0.0f, 1.5f);
                ImGui::SliderFloat("SSAO",       &ssaoStrength, 0.0f, 1.0f);
                ImGui::SliderFloat("SSAO radius",&ssaoRadius, 0.2f, 4.0f);
                ImGui::SliderFloat("SSAO angle bias", &ssaoBias, 0.0f, 0.6f, "%.2f rad");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Horizons below this elevation don't occlude.\n"
                                      "Raise it if flat surfaces look dirty, lower it\n"
                                      "for more contact shading in creases.");
                ImGui::SliderFloat("Cascade split", &renderer.shadows().splitLambda, 0.0f, 1.0f);
                // Reflection probe: the cubemap a wet road (and any reflective
                // material) mirrors. Applied on pick rather than per frame --
                // changing it reallocates both cubes.
                {
                    ui::sectionText("Reflections");
                    const int sizes[] = {128, 256, 512, 1024};
                    char cur[16];
                    std::snprintf(cur, sizeof(cur), "%d", envProbeRes);
                    if (ImGui::BeginCombo("Probe resolution", cur)) {
                        for (int s : sizes) {
                            char lbl[16];
                            std::snprintf(lbl, sizeof(lbl), "%d", s);
                            if (ImGui::Selectable(lbl, s == envProbeRes)) {
                                envProbeRes = s;
                                renderer.setEnvProbeResolution(s);
                            }
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Cube-face size of the environment probe: how\n"
                                          "sharp reflections are on a wet road or a\n"
                                          "reflective material. Six scene passes either\n"
                                          "way -- raising it costs fill, not draw calls --\n"
                                          "but 1024 is 64x the pixels of 128.");
                    // How fresh that cube is kept. This is a LATENCY control,
                    // not a quality one: the probe is filled one face at a
                    // time, so a cube filled at one face per frame is six to
                    // twelve frames old when it is sampled -- twenty metres of
                    // it at racing speed, which reads as the reflection
                    // dragging behind the car. The rate is only spent when the
                    // viewpoint actually moves, so raising this costs nothing
                    // in a parked editor.
                    const char* faceLbl[] = {"1 face (cheapest)", "2 faces",
                                             "3 faces", "4 faces", "5 faces",
                                             "6 faces (no lag)"};
                    const int fi = glm::clamp(envProbeFaces, 1, 6) - 1;
                    if (ImGui::BeginCombo("Probe refresh", faceLbl[fi])) {
                        for (int k = 0; k < 6; ++k)
                            if (ImGui::Selectable(faceLbl[k], k == fi)) {
                                envProbeFaces = k + 1;
                                renderer.setEnvProbeMaxFaces(envProbeFaces);
                            }
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Cube faces the probe may refresh per frame,\n"
                                          "at most. The actual rate follows how fast the\n"
                                          "camera moves, so a still scene pays one face\n"
                                          "whatever this says. Raise it if reflections\n"
                                          "lag behind at speed; lower it if the probe\n"
                                          "costs too much (it is six scene passes).");
                }
                ui::sectionText("Depth of field");
                ImGui::SliderFloat("DOF blur", &dofMax, 0.0f, 12.0f, "%.1f px");
                ImGui::SliderFloat("Focus near", &dofNear, 2.0f, 120.0f, "%.0f m");
                ImGui::SliderFloat("Focus far",  &dofFar, 20.0f, 400.0f, "%.0f m");
                ui::sectionText("Motion blur");
                ImGui::SliderFloat("Speed blur", &motionBlurStrength, 0.0f, 2.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Radial speed streak while driving/flying: the\n"
                                      "world smears outward past the craft, growing\n"
                                      "with speed. 0 = off. (No effect on the free camera.)");
                ui::sectionText("Anti-aliasing");
                ImGui::Checkbox("FXAA", &fxaaEnabled);
                ui::sectionText("Split screen");
                ImGui::Checkbox("Two panes", &splitScreen);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Draw the world twice, side by side, one pane\n"
                                      "per player. The whole frame costs roughly\n"
                                      "double -- watch the profiler before counting\n"
                                      "on it.");
            }
            ImGui::End(); }

            if (showColorGrade) { if (ImGui::Begin("Colour grade", &showColorGrade)) {
                ImGui::SliderFloat("Hue",        &hueShift, -180.0f, 180.0f, "%.0f");
                ImGui::SliderFloat("Saturation", &saturation, 0.0f, 2.0f);
                ImGui::SliderFloat("Brightness", &valueGain, 0.3f, 2.0f);
                ImGui::SliderFloat("Warmth",     &warmth, -0.5f, 0.5f);
                ImGui::SliderFloat("Contrast",   &contrast, 0.0f, 0.6f);
            }
            ImGui::End(); }

            if (showWater) { if (ImGui::Begin("Water", &showWater)) {
                ImGui::SliderFloat("Level",       &waterLevel, -15.0f, 15.0f);
                ImGui::SliderFloat("Swell height",&waveHeight, 0.0f, 2.5f);
                ImGui::SliderFloat("Choppiness",  &waveChoppy, 0.0f, 1.0f);
                ImGui::SliderFloat("Ripples",     &waveStrength, 0.0f, 0.05f, "%.3f");
                ImGui::SliderFloat("Ripple size", &waveScale, 0.01f, 0.2f, "%.3f");
                ImGui::SliderFloat("Shore foam",  &foamWidth, 0.0f, 8.0f);
                ImGui::SliderFloat("Reflectivity",&waterReflectivity, 0.0f, 1.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Max mirror strength. Lower = less glassy,\n"
                                      "more of the water body shows through.");
                ImGui::SliderFloat("Clarity",     &waterClarity, 0.2f, 3.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How clear the water is. Higher = see the bed\n"
                                      "deeper; lower = murkier, tints sooner.");
                ImGui::SliderFloat("IOR",         &waterIor, 1.0f, 2.0f, "%.3f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Index of refraction. Water = 1.33 (~2%% edge-on\n"
                                      "reflection); higher = more reflective + more bend.");
                ImGui::ColorEdit3("Tint",         &waterColor.x);
            }
            ImGui::End(); }

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

            sculptui::drawPanel({
                showSculpt, sculptMode,
                grassPaintMode, roadEditMode, treePaintMode, flowerPaintMode, paintMode,
                scatterMode,
                sculptTool, sculptRadius, sculptStrength, pullFalloff, pullHeight,
                sculptFlattenH,
                stampShape, stampHeight, stampRot, noiseFreq, carveDepth,
                sculptWork, streamer, veg.grassDirty, publishSculpt,
            });

            paintui::drawPanel({
                showPaint, paintMode,
                grassPaintMode, roadEditMode, treePaintMode, flowerPaintMode, sculptMode,
                scatterMode,
                look, paintLayer, paintRadius, paintStrength, paintErase,
                paintWork, streamer, publishPaint,
            });

            {
                // Children of the "Scattered" group, for the panel's counter.
                int scatteredCount = 0;
                const int sg = findScatterGroup();
                if (sg >= 0)
                    for (const Entity& e : entities)
                        if (e.parent == sg) ++scatteredCount;
                scatterui::drawPanel({
                    showScatter, scatterMode,
                    grassPaintMode, roadEditMode, treePaintMode, flowerPaintMode,
                    sculptMode, paintMode,
                    brushErase, scatterCfg, models, scatteredCount,
                    roads.active().roadPts.size() >= 2,
                    scatterRoadside, scatterClearAll,
                });
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

            if (showVegetation) { if (ImGui::Begin("Vegetation", &showVegetation)) {
                ui::sectionText("Grass");
                ImGui::Checkbox("Grass", &veg.grassEnabled);
                bool regrow = false;
                regrow |= ImGui::SliderFloat("Density", &veg.grassDensity, 0.1f, 3.0f);
                regrow |= ImGui::SliderFloat("Grass range", &veg.grassRadius, 20.0f, 90.0f);
                regrow |= ImGui::SliderFloat("Blade height", &veg.grassHeight, 0.2f, 1.2f);
                regrow |= ImGui::SliderFloat("Chaos", &veg.grassChaos, 0.0f, 2.0f);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Irregularity of height, density and gaps\n"
                                      "0 = even lawn, 1 = wild meadow");
                if (regrow) veg.grassDirty = true; // baked per blade -> regrow
                ImGui::ColorEdit3("Tint", &veg.grassTint.x);
                ImGui::Text("Blades: %d", veg.grassCount);

                ui::sectionText("Paint grass (3D brush)");
                if (ImGui::Checkbox("Paint mode", &grassPaintMode) && grassPaintMode)
                    roadEditMode = sculptMode = treePaintMode = flowerPaintMode = paintMode = scatterMode = false; // brush owns the left button
                if (grassPaintMode) {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                        "Drag = paint | hold Alt = erase");
                } else {
                    ImGui::TextDisabled("Enable to paint blades onto the terrain");
                }
                ImGui::Checkbox("Erase", &brushErase);
                ImGui::SliderFloat("Brush size", &brushRadius, 0.5f, 40.0f, "%.1f m");
                ImGui::SliderFloat("Brush density", &brushDensity, 0.1f, 4.0f);
                ImGui::Text("Painted blades: %d",
                            static_cast<int>(veg.paintedBlades.size() / 7));
                if (ImGui::Button("Clear painted")) {
                    veg.paintedBlades.clear();
                    veg.paintedDirty = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Save##grass")) {
                    std::ofstream f("grass.txt");
                    for (std::size_t i = 0; i < veg.paintedBlades.size(); ++i)
                        f << veg.paintedBlades[i] << ((i % 7 == 6) ? '\n' : ' ');
                }
                ImGui::SameLine();
                if (ImGui::Button("Load##grass")) {
                    std::ifstream f("grass.txt");
                    if (f) {
                        veg.paintedBlades.clear();
                        float v;
                        while (f >> v) veg.paintedBlades.push_back(v);
                        veg.paintedBlades.resize(veg.paintedBlades.size() / 7 * 7); // whole blades
                        veg.paintedDirty = true;
                    }
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(grass.txt)");

                veg.panelTrees(treePaintMode, brushErase, [&]{
                    grassPaintMode = roadEditMode = sculptMode =
                        flowerPaintMode = paintMode = scatterMode = false; // own the LMB
                });

                ui::sectionText("Flowers");
                ImGui::Checkbox("Flowers", &veg.flowerEnabled);
                if (ImGui::SliderFloat("Flower density", &veg.flowerDensity, 0.0f, 2.0f))
                    veg.grassDirty = true; // flowers regenerate with the grass pass
                ImGui::SameLine();
                if (ImGui::SmallButton("Regrow")) veg.grassDirty = true;
                ImGui::Text("Flowers: %d", veg.flowerCount);

                ui::sectionText("Paint flowers (3D brush)");
                if (ImGui::Checkbox("Paint mode##flower", &flowerPaintMode) && flowerPaintMode)
                    grassPaintMode = roadEditMode = sculptMode = treePaintMode = paintMode = scatterMode = false;
                if (flowerPaintMode)
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.85f, 1.0f),
                        "Drag = plant | hold Alt = erase");
                else
                    ImGui::TextDisabled("Enable to plant flowers onto the terrain");
                ImGui::Checkbox("Erase##flower", &brushErase);
                ImGui::SliderFloat("Brush size##flower", &veg.flowerBrushRadius, 1.0f, 30.0f, "%.1f m");
                ImGui::SliderFloat("Density##flower", &veg.flowerBrushDensity, 0.1f, 4.0f);
                ImGui::Text("Painted flowers: %d",
                            static_cast<int>(veg.paintedFlowers.size() / 8));
                ImGui::BeginDisabled(veg.paintedFlowers.empty());
                if (ImGui::Button("Clear painted##flower")) {
                    veg.clearPaintedFlowers();
                }
                ImGui::EndDisabled();

                veg.panelBirdsFireflies();
            }
            ImGui::End(); }

            if (showCamPath) { if (ImGui::Begin("Camera path", &showCamPath)) {
                camPathRec.panel(camera);
            }
            ImGui::End(); }

            // Roads + bridges: the whole panel lives in RoadPanel.cpp; main only
            // hands it the state it may touch (see roadui::PanelState).
            roadui::drawPanel({showRoads, roads, roadEditMode, roadSel, roadSel2, assetDb,
                [&]{ grassPaintMode = sculptMode = treePaintMode = flowerPaintMode =
                         paintMode = scatterMode = splineEditMode =
                         riverEditMode = false; }, // don't fight over LMB
                buildRoad, deleteRoadPoint,
                addRoad, deleteRoad, selectRoad,
                roadPrefabCfg,
                [&]{ const std::string d = prefabDir();
                     return d.empty()
                         ? std::vector<std::pair<std::string, std::string>>()
                         : prefab::list(d); },
                placeRoadPrefabs,
                beginRoadEdit, commitRoadEdit});

            // Fences, walls and railway track: the paths live in SplineSystem
            // (saved + undoable on their own timeline), the panel only edits them.
            // See SplinePanel.cpp.
            splineui::drawPanel({showSplines, splines, splineEditMode, splineSel,
                splinePtSel, materials,
                [&]{ grassPaintMode = sculptMode = treePaintMode = flowerPaintMode =
                         paintMode = scatterMode = roadEditMode =
                         riverEditMode = false; },
                [&](fitzel::AssetId id) {
                    // Jump to the material the author just pointed an element at,
                    // so giving it a texture is one click from the picker.
                    const int mi = document.materialIndex(id);
                    if (mi >= 0) matSel = mi;
                    showMaterials = true;
                },
                beginSplineEdit, commitSplineEdit});

            // Brooks, rivers and canals: the courses live in RiverSystem (saved +
            // undoable on their own timeline), the panel only edits them. See
            // RiverPanel.cpp.
            riverui::drawPanel({showRivers, rivers, riverEditMode, riverSel,
                riverPtSel,
                [&]{ grassPaintMode = sculptMode = treePaintMode = flowerPaintMode =
                         paintMode = scatterMode = roadEditMode =
                         splineEditMode = false; },
                beginRiverEdit, commitRiverEdit});

            // Roadside city: the biome rules live on the road (saved + undoable
            // with it), the panel only edits them. See CityPanel.cpp.
            if (showCity)
                cityui::drawPanel({showCity, roads.active(), roads.active().built(),
                                   [&]{ roads.active().rebuildCity(); },
                                   bakeNearestBuilding,
                                   beginRoadEdit, commitRoadEdit,
                                   exportStatus});

            if (showModeling) {
                MeshComponent* mc = selectedMesh();
                const bool haveSel = cursorHaveSel();
                // A face index belongs to one object's mesh and to one version of
                // it: drop it when the selection moves, or when an undo left the
                // mesh with fewer faces than the index.
                const int selId = haveSel ? entities[sel.index()].id : -1;
                if (selId != meshFaceOwner) { meshFaceOwner = selId; meshFaceSel = -1; }
                if (!mc || meshFaceSel >= static_cast<int>(mc->mesh.faces.size()))
                    meshFaceSel = -1;
                modelui::drawPanel({
                    showModeling, mc, meshFaceSel, materials, haveSel,
                    haveSel && !mc && entities[sel.index()].type == EntityType::Box,
                    [&]{ convertToMesh(); }, applyMeshEdit,
                    // "Edit this material" on a face: the surface itself is a
                    // material, and the place to change one is the Materials
                    // panel. Reads only, so it is safe from inside the panel.
                    [&](AssetId id) {
                        if (!id.valid()) return;
                        matSel        = document.materialIndex(id);
                        showMaterials = true;
                    },
                    mc ? static_cast<int>(mc->mesh.faces.size()) : 0,
                    mc ? static_cast<int>(mc->mesh.verts.size()) : 0,
                });
            }

            // Where the selected face's texture sits. Shares the Modeling
            // panel's face selection and its one-undo-step edit callback: this
            // is the same mesh being shaped, looked at from the texture's side.
            if (showUv) {
                MeshComponent* mc = selectedMesh();
                const bool haveSel = cursorHaveSel();
                const int  selId   = haveSel ? entities[sel.index()].id : -1;
                if (selId != meshFaceOwner) { meshFaceOwner = selId; meshFaceSel = -1; }
                if (!mc || meshFaceSel >= static_cast<int>(mc->mesh.faces.size()))
                    meshFaceSel = -1;
                // The material the OBJECT wears: the panel draws the texture the
                // face is actually seen through, and a face wearing none of its
                // own is seen through this one.
                AssetId objMat;
                if (haveSel)
                    if (const auto* mcp = entities[sel.index()].components.get<MaterialComponent>())
                        objMat = mcp->material;
                uvui::drawPanel({
                    showUv, mc, meshFaceSel, materials, objMat, haveSel,
                    haveSel && !mc && entities[sel.index()].type == EntityType::Box,
                    [&]{ convertToMesh(); }, applyMeshEdit,
                    mc ? static_cast<int>(mc->mesh.faces.size()) : 0,
                });
            }

            if (showMeshPaint) {
                MeshComponent* mc  = selectedMesh();
                const bool haveSel = cursorHaveSel();
                int painted = 0;
                if (mc)
                    for (const glm::vec4& w : mc->mesh.paint)
                        if (w.x > 0.0f || w.y > 0.0f || w.z > 0.0f || w.w > 0.0f) ++painted;
                meshPaintSlot = glm::clamp(meshPaintSlot, 0, 3);
                // The panel does not touch the document: it says what it wants
                // and the host does it below, once the panel has stopped reading
                // from the component. An undo push assigns the entity's snapshot
                // over it and replaces its components, so a panel that edited in
                // place would spend the rest of its frame drawing from freed
                // memory -- which is exactly what it used to do.
                meshpaintui::SlotEdit slotEdit;
                bool                  wantClearPaint = false;
                meshpaintui::drawPanel({
                    showMeshPaint, meshPaintMode,
                    paintMode, grassPaintMode, roadEditMode, treePaintMode,
                    flowerPaintMode, sculptMode, scatterMode,
                    materials, meshPaintSlot, meshPaintRadius, meshPaintStrength,
                    meshPaintDetail, meshPaintErase,
                    mc, haveSel,
                    haveSel && !mc && entities[sel.index()].type == EntityType::Box,
                    mc ? static_cast<int>(mc->mesh.faces.size()) : 0, painted,
                    slotEdit,
                    [&]{ convertToMesh(); },
                    [&]{ wantClearPaint = true; },
                    // "Edit" on a slot: the texture itself is a material, and the
                    // place to change a material is the Materials panel. Reads
                    // only, so it is safe from inside the panel.
                    [&](int k) {
                        MeshComponent* m = selectedMesh();
                        if (!m || k < 0 || k >= static_cast<int>(m->paintSlots.size()))
                            return;
                        const AssetId id = m->paintSlots[k].material;
                        if (!id.valid()) return;
                        matSel        = document.materialIndex(id);
                        showMaterials = true;
                    },
                });

                // What the panel asked for, applied as one undo step each. Filling
                // a slot needs no touch(): the geometry did not move, only what its
                // weights are drawn with.
                if (MeshComponent* m = selectedMesh()) {
                    Entity&      e      = entities[sel.index()];
                    const Entity before = e;
                    bool         did    = false;
                    if (slotEdit.slot >= 0 &&
                        slotEdit.slot < static_cast<int>(m->paintSlots.size())) {
                        MeshPaintSlot& sl = m->paintSlots[slotEdit.slot];
                        if (slotEdit.setMaterial) { sl.material = slotEdit.material; did = true; }
                        if (slotEdit.setScale)    { sl.scale    = slotEdit.scale;    did = true; }
                    }
                    if (wantClearPaint && meshpaint::clear(m->mesh)) { m->touch(); did = true; }
                    if (did) {
                        auto cmd = std::make_unique<ModifyEntityCmd>(before, e);
                        if (!cmd->trivial()) history.pushApplied(std::move(cmd));
                    }
                }
            }

            if (showCursor) { if (ImGui::Begin("3D Cursor", &showCursor)) {
                ImGui::Checkbox("Show cursor", &cursorVisible);
                ImGui::TextDisabled("Shift+Right-click in the viewport to place it.");
                ImGui::DragFloat3("Position", &cursor3D.x, 0.05f, 0.0f, 0.0f, "%.2f");
                ImGui::SetNextItemWidth(140.0f);
                ImGui::DragFloat("Grid step", &cursorGrid, 0.05f, 0.01f, 100.0f, "%.2f m");
                ImGui::TextDisabled("Shift+S in the viewport opens the snap menu.");

                // The drawn grid IS this step, on this cursor's plane -- so these
                // controls belong next to it rather than in a panel of their own.
                ui::sectionText("Grid");
                ImGui::Checkbox("Show grid", &showGrid);
                ImGui::BeginDisabled(!showGrid);
                ImGui::SetNextItemWidth(140.0f);
                ImGui::DragFloat("Fade out", &gridFade, 2.0f, 20.0f, 1000.0f, "%.0f m");
                ImGui::EndDisabled();
                ui::hint("One cell = the grid step above, a heavier line every ten.\n"
                         "It lies on the cursor's height, so moving the cursor up\n"
                         "moves the plane you are building on with it. The fade is\n"
                         "capped by the view distance -- it cannot reach past it.");

                const bool haveSel = cursorHaveSel();

                ui::sectionText("Snap cursor");
                if (ImGui::Button("To world origin")) snapCursorToOrigin();
                ImGui::SameLine();
                if (ImGui::Button("To grid"))         snapCursorToGrid();
                if (ImGui::Button("To terrain"))      snapCursorToTerrain();
                ImGui::SameLine();
                ImGui::BeginDisabled(!haveSel);
                if (ImGui::Button("To selection"))    snapCursorToSelection();
                ImGui::EndDisabled();

                ui::sectionText("Snap selection");
                ImGui::BeginDisabled(!haveSel);
                if (ImGui::Button("Selection to cursor")) snapSelectionToCursor();
                ImGui::SameLine();
                if (ImGui::Button("Selection to grid"))   snapSelectionToGrid();
                ImGui::EndDisabled();

                ui::sectionText("Create");
                if (ImGui::Button("Add object at cursor"))
                    addEntity(cursor3D, entityNewType);
                ImGui::SameLine();
                ImGui::TextDisabled("(base rests on the cursor)");
            }
            ImGui::End(); }

            // The scene tree: selection, inline rename, drag-to-reparent and the
            // create/duplicate/delete menu (see HierarchyPanel.cpp).
            hierarchyui::drawPanel({entities, document, sel,
                                    renameId, renameBuf, sizeof(renameBuf), renameFocus,
                                    duplicateEntity, deleteEntity,
                                    duplicateSelection, deleteSelection,
                                    addEmptyParent, addEmptyChild, addPrimitiveChild,
                                    addShotCamera, addVehicleLights, setMainCamera,
                                    isUnderId, worldOf, rebaseLocal,
                                    prefabNameBuf, sizeof(prefabNameBuf), showPrefabs});

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
                                    entityNewHalf});

            // Material library: create/edit reusable surface materials. Solids are
            // assigned one via the Inspector; edits here update every mesh using it.
            materialsui::drawPanel({showMaterials, materials, document, entities,
                                    matSel, matFilter, sizeof(matFilter),
                                    assetDb, videos, texSwatch});

            // Model import: list glTF/GLB files under models/ and drop one into
            // the scene in front of the camera as a Model entity.
            modelsui::drawPanel({showModels, modelDir, modelFile,
                                 models, assetDb, materials,
                                 camera, streamer,
                                 isStructuredModel, addModelHierarchy,
                                 addModelEntity});

            // Prefabs: reusable object templates saved in the project's prefabs/
            // folder. Make one from the current selection, or click a saved prefab
            // to drop an instance into the scene (on the ground in front of the
            // camera). See PrefabSystem.hpp.
            prefabsui::drawPanel({showPrefabs, prefabDir, entities, sel,
                                  prefabNameBuf, sizeof(prefabNameBuf),
                                  createPrefabFromSelection,
                                  instantiatePrefabFile});

            // Import Unity asset: browse an asset folder, preview which textures
            // map by Unity naming convention, then import the FBX as a hierarchy
            // with those maps auto-assigned (the matching also runs on reload).
            if (showUnityImport) {
                ImGui::SetNextWindowSize(ImVec2(560.0f, 470.0f), ImGuiCond_FirstUseEver);
                if (ImGui::Begin("Import Unity asset", &showUnityImport)) {
                    if (unityDir.empty()) unityDir = modelDir;
                    ImGui::TextWrapped(
                        "Unity FBX files don't reference their textures directly, so a plain "
                        "import leaves them unmapped. Point this at an asset's folder: maps "
                        "kept in a Textures/ folder and named like the material or model "
                        "(e.g. Rock_Albedo, Rock_Normal) are matched automatically.");
                    ImGui::Separator();

                    ImGui::TextWrapped("Folder: %s",
                                       unityDir.empty() ? "(none)" : unityDir.c_str());
                    if (ImGui::Button("Browse...")) {
                        std::string picked;
                        if (ed::pickFolder(picked, unityDir)) {
                            unityDir = picked; unityFbx.clear(); unityFbxScanDir.clear();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Use models/ folder")) {
                        unityDir = modelDir; unityFbx.clear(); unityFbxScanDir.clear();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Rescan")) unityFbxScanDir.clear();

                    // (Re)scan only when the folder changes -- a manual directory
                    // stack so one unreadable or over-long subfolder can't abort the
                    // whole listing (recursive_directory_iterator aborts on the first
                    // error), and so we don't hit the disk every frame.
                    if (unityDir != unityFbxScanDir) {
                        unityFbxList.clear();
                        unityFbxScanDir = unityDir;
                        std::vector<std::filesystem::path> stack;
                        if (!unityDir.empty()) stack.push_back(std::filesystem::path(unityDir));
                        int scanned = 0;
                        while (!stack.empty() && unityFbxList.size() < 2000 && scanned < 40000) {
                            const std::filesystem::path dir = stack.back();
                            stack.pop_back();
                            std::error_code lec;
                            std::filesystem::directory_iterator
                                dit(dir, std::filesystem::directory_options::skip_permission_denied, lec),
                                dend;
                            for (; !lec && dit != dend; dit.increment(lec)) {
                                ++scanned;
                                std::error_code tec;
                                if (dit->is_directory(tec)) { stack.push_back(dit->path()); continue; }
                                std::string ext = dit->path().extension().string();
                                for (char& c : ext) c = static_cast<char>(std::tolower(
                                    static_cast<unsigned char>(c)));
                                if (ext != ".fbx") continue;
                                std::error_code rec;
                                std::string rel = std::filesystem::relative(
                                    dit->path(), unityDir, rec).generic_string();
                                if (rel.empty()) rel = dit->path().filename().string();
                                unityFbxList.push_back({ rel, dit->path().generic_string() });
                            }
                        }
                        std::sort(unityFbxList.begin(), unityFbxList.end());
                    }

                    ImGui::Spacing();
                    ImGui::Text("FBX files (%d):", static_cast<int>(unityFbxList.size()));
                    ImGui::BeginChild("##fbxlist", ImVec2(0.0f, 130.0f), true);
                    for (const auto& h : unityFbxList)
                        if (ImGui::Selectable(h.first.c_str(), unityFbx == h.second))
                            unityFbx = h.second;
                    if (unityFbxList.empty())
                        ImGui::TextDisabled("(no .fbx found under this folder)");
                    ImGui::EndChild();

                    // Recompute the texture-match preview when the selection changes.
                    if (unityFbx != unityPreviewFor) {
                        unityPreview = unityFbx.empty()
                            ? std::vector<fitzel::UnityTexMatch>{}
                            : fitzel::previewUnityTextures(unityFbx);
                        unityNearby = unityFbx.empty()
                            ? std::vector<std::string>{}
                            : fitzel::nearbyTextureFiles(unityFbx);
                        unityPreviewFor = unityFbx;
                    }

                    if (!unityFbx.empty()) {
                        ImGui::Text("Materials & matched maps:");
                        if (ImGui::BeginTable("##unitytex", 4,
                                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_SizingStretchProp |
                                ImGuiTableFlags_ScrollY,
                                ImVec2(0.0f, 150.0f))) {
                            ImGui::TableSetupColumn("Material");
                            ImGui::TableSetupColumn("Albedo");
                            ImGui::TableSetupColumn("Normal");
                            ImGui::TableSetupColumn("Emission");
                            ImGui::TableHeadersRow();
                            const ImVec4 ok(0.55f, 0.85f, 0.55f, 1.0f);
                            const ImVec4 no(0.6f, 0.6f, 0.6f, 1.0f);
                            auto cell = [&](const std::string& p){
                                if (p.empty()) ImGui::TextColored(no, "- none");
                                else ImGui::TextColored(ok, "%s",
                                    std::filesystem::path(p).filename().string().c_str());
                            };
                            for (const auto& m : unityPreview) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TextUnformatted(m.material.c_str());
                                ImGui::TableSetColumnIndex(1); cell(m.albedo);
                                ImGui::TableSetColumnIndex(2); cell(m.normal);
                                ImGui::TableSetColumnIndex(3); cell(m.emission);
                            }
                            ImGui::EndTable();
                        }
                        if (unityPreview.empty())
                            ImGui::TextDisabled("(no materials found in this FBX)");

                        // Diagnostic: the actual image files the matcher looked at.
                        // If maps show "- none" above but files are listed here, the
                        // naming is unusual -- tell me these names and I'll tune it.
                        if (ImGui::TreeNode("Texture files found nearby "
                                            "(diagnostic)")) {
                            if (unityNearby.empty())
                                ImGui::TextDisabled("(no image files found in the "
                                                    "usual Textures/ folders)");
                            for (const std::string& n : unityNearby)
                                ImGui::BulletText("%s", n.c_str());
                            ImGui::TreePop();
                        }
                    }

                    ImGui::Separator();
                    ImGui::BeginDisabled(unityFbx.empty());
                    if (ImGui::Button("Import to scene", ImVec2(160.0f, 0.0f))) {
                        std::error_code cec;
                        std::string src = unityFbx;
                        // A model imported from OUTSIDE the project's asset tree has
                        // no persistent GUID, so it would vanish on reload and never
                        // show in Assets. Copy it (plus the maps the matcher resolved)
                        // into the project's models/ folder, register it, and import
                        // the copy -- now it round-trips through save/load by GUID.
                        if (!assetDb.idForPath(unityFbx).valid()) {
                            const std::filesystem::path fp(unityFbx);
                            std::string parent = fp.parent_path().filename().string();
                            for (char& c : parent) c = static_cast<char>(std::tolower(
                                static_cast<unsigned char>(c)));
                            const bool inMeshDir = parent == "meshes" || parent == "models" ||
                                                   parent == "mesh"   || parent == "fbx";
                            const std::string pack = (inMeshDir
                                ? fp.parent_path().parent_path().filename()
                                : fp.parent_path().filename()).string();
                            const std::string destPack = modelDir + "/" +
                                (pack.empty() ? fp.stem().string() : pack);
                            const std::string destMesh = destPack + "/Meshes";
                            const std::string destTex  = destPack + "/Textures";
                            std::filesystem::create_directories(destMesh, cec);
                            std::filesystem::create_directories(destTex, cec);
                            const std::string destFbx = destMesh + "/" + fp.filename().string();
                            std::filesystem::copy_file(unityFbx, destFbx,
                                std::filesystem::copy_options::overwrite_existing, cec);
                            int nTex = 0;
                            std::unordered_set<std::string> done;
                            for (const auto& m : fitzel::previewUnityTextures(unityFbx))
                                for (const std::string& t : {m.albedo, m.normal, m.emission})
                                    if (!t.empty() && done.insert(t).second) {
                                        std::error_code fc;
                                        std::filesystem::copy_file(t, destTex + "/" +
                                            std::filesystem::path(t).filename().string(),
                                            std::filesystem::copy_options::skip_existing, fc);
                                        if (!fc) ++nTex;
                                    }
                            assetDb.refresh(); // register the copied FBX + maps (GUIDs)
                            src = destFbx;
                            char buf[256];
                            std::snprintf(buf, sizeof(buf),
                                "Copied into project (%d map(s)); it now persists and "
                                "appears in Assets.", nTex);
                            unityStatus = buf;
                        } else {
                            unityStatus = "Imported (already in the project).";
                        }
                        const glm::vec3 p = camera.position() + camera.front() * 8.0f;
                        const glm::vec3 g(p.x, streamer.heightAt(p.x, p.z), p.z);
                        addModelHierarchy(g, src, unityFlipV);
                    }
                    ImGui::EndDisabled();
                    if (!unityStatus.empty()) ImGui::TextDisabled("%s", unityStatus.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("One entity per part.");
                    ImGui::Checkbox("Flip texture V", &unityFlipV);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("If the texture looks misplaced on an atlas, toggle "
                                          "this and re-import.\nFBX/DAE usually need it on; some "
                                          "packs need it off.");
                    ImGui::TextDisabled("Tip: keep the asset inside your project so it "
                                        "reloads with the scene.");
                }
                ImGui::End();
            }

            // Asset browser: every asset in the database, grouped by source
            // (Engine vs Project) and labelled by type. Drag a Model onto the
            // viewport to place it, or a Texture onto a material's Base texture
            // slot. Double-click a Model to drop it ahead of the camera.
            if (showAssets) {
                if (ImGui::Begin("Assets", &showAssets)) {
                    // Toolbar: preview size, name filter, texture-only toggle.
                    ImGui::SetNextItemWidth(120.0f);
                    ImGui::SliderFloat("Size", &assetThumbSize, 48.0f, 160.0f, "%.0f");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(150.0f);
                    ImGui::InputTextWithHint("##assetFilter", "filter...",
                                             assetFilter, sizeof(assetFilter));
                    ImGui::SameLine();
                    ImGui::Checkbox("Textures only", &assetTexturesOnly);
                    ImGui::TextDisabled("Drag a tile onto a material slot / the "
                                        "viewport; double-click a model to place it.");
                    ImGui::TextDisabled("Drop files here from Explorer to copy them "
                                        "into the project.");

                    // Take an OS file drop that landed on this window. The hit test
                    // uses the cursor position captured in the drop callback, not
                    // the live one: the pointer may have moved on since, and a file
                    // dropped on Assets belongs in Assets either way.
                    if (!g_fileDrop.paths.empty()) {
                        const ImVec2 wp = ImGui::GetWindowPos();
                        const ImVec2 ws = ImGui::GetWindowSize();
                        if (g_fileDrop.x >= wp.x && g_fileDrop.x < wp.x + ws.x &&
                            g_fileDrop.y >= wp.y && g_fileDrop.y < wp.y + ws.y) {
                            const std::string proj =
                                currentProject.empty()
                                    ? std::string()
                                    : std::filesystem::path(currentProject)
                                          .parent_path().generic_string();
                            assetDropStatus =
                                assetdrop::importInto(proj, g_fileDrop.paths, assetDb)
                                    .message;
                            g_fileDrop.paths.clear();
                        }
                    }
                    if (!assetDropStatus.empty())
                        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s",
                                           assetDropStatus.c_str());
                    ImGui::Separator();

                    // (Thumbnails finished off-thread are uploaded once per frame by
                    // pumpThumbnails(), before the panels are drawn.)

                    // Case-insensitive substring match for the filter box.
                    std::string flt = assetFilter;
                    std::transform(flt.begin(), flt.end(), flt.begin(),
                        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                    auto matches = [&](const std::string& s){
                        if (flt.empty()) return true;
                        std::string l = s;
                        std::transform(l.begin(), l.end(), l.begin(),
                            [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                        return l.find(flt) != std::string::npos;
                    };

                    const float pad  = ImGui::GetStyle().ItemSpacing.x;
                    const auto& srcs = assetDb.sources();
                    for (int si = 0; si < static_cast<int>(srcs.size()); ++si) {
                        const char* kind = srcs[si].kind == AssetSourceKind::Engine
                                               ? "Engine" : "Project";
                        const std::string hdr =
                            srcs[si].name + " (" + kind + ")###src" + std::to_string(si);
                        if (!ui::header(hdr.c_str(),
                                                     ImGuiTreeNodeFlags_DefaultOpen))
                            continue;
                        ImGui::PushID(si);
                        const float avail = ImGui::GetContentRegionAvail().x;
                        const int   cols  = std::max(1,
                            static_cast<int>(avail / (assetThumbSize + pad)));
                        int shown = 0, col = 0;
                        for (AssetId id : assetDb.allAssets()) {
                            const AssetDatabase::Entry* e = assetDb.entry(id);
                            if (!e || e->sourceIndex != si) continue;
                            const bool isTex = (e->type == AssetType::Texture);
                            if (assetTexturesOnly && !isTex) continue;
                            if (!matches(e->relPath)) continue;
                            ++shown;
                            if (col != 0) ImGui::SameLine();

                            ImGui::PushID(id.toString().c_str());
                            ImGui::BeginGroup();

                            // Resolve a small preview thumbnail via the shared cache.
                            // Only request a decode when the tile is actually on
                            // screen, so scrolling a big browser doesn't queue every
                            // texture at once.
                            unsigned tid = 0;
                            if (isTex) {
                                auto it = assetThumbs.find(id);
                                if (it != assetThumbs.end())
                                    tid = it->second ? it->second->id() : 0;
                                else if (ImGui::IsRectVisible(
                                             ImVec2(assetThumbSize, assetThumbSize)))
                                    tid = thumbFor(id);
                            }

                            const ImVec2 sz(assetThumbSize, assetThumbSize);
                            if (tid) {
                                ImGui::ImageButton("##thumb",
                                    (ImTextureID)(intptr_t)tid, sz);
                            } else {
                                const char* tag = isTex ? "TEX"
                                    : e->type == AssetType::Model ? "MDL"
                                    : e->type == AssetType::Sound ? "SND"
                                    : e->type == AssetType::Video ? "VID" : "?";
                                ImGui::Button(tag, sz);
                            }

                            // Drag source (same GUID payload the drop targets expect).
                            if (ImGui::BeginDragDropSource(
                                    ImGuiDragDropFlags_SourceAllowNullID)) {
                                const std::string g = id.toString();
                                ImGui::SetDragDropPayload("ASSET_GUID", g.data(), 32);
                                ImGui::Text("%s  %s", assetTypeName(e->type),
                                            e->relPath.c_str());
                                ImGui::EndDragDropSource();
                            }
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s\n%s", assetTypeName(e->type),
                                                  e->relPath.c_str());
                            if (e->type == AssetType::Model &&
                                ImGui::IsItemHovered() &&
                                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                const std::string mp = e->absPath.string();
                                const glm::vec3 p = camera.position() + camera.front() * 8.0f;
                                const glm::vec3 g(p.x, streamer.heightAt(p.x, p.z), p.z);
                                if (isStructuredModel(mp)) addModelHierarchy(g, mp);
                                else {
                                    const int id2 = models.import(mp, assetDb, materials);
                                    if (id2 >= 0) addModelEntity(g, id2);
                                }
                            }

                            // Caption: file name, clipped to the tile width.
                            std::string stem =
                                std::filesystem::path(e->relPath).filename().string();
                            const int maxCh = std::max(4,
                                static_cast<int>(assetThumbSize / 7.0f));
                            if (static_cast<int>(stem.size()) > maxCh)
                                stem = stem.substr(0, maxCh - 1) + "\xE2\x80\xA6"; // ellipsis
                            ImGui::PushTextWrapPos(
                                ImGui::GetCursorPosX() + assetThumbSize);
                            ImGui::TextUnformatted(stem.c_str());
                            ImGui::PopTextWrapPos();

                            ImGui::EndGroup();
                            ImGui::PopID();
                            col = (col + 1) % cols;
                        }
                        if (shown == 0) ImGui::TextDisabled("  (empty)");
                        ImGui::PopID();
                    }
                }
                ImGui::End();
            }

            // Lua script editor (syntax-highlighted). Open/create/save the .lua
            // files under scripts/; saving reloads the script VM so the next Play
            // uses the edited code. Assign a script to an entity in the Inspector.
            if (showScriptEditor) {
                bool openNewScript = false;
                if (ImGui::Begin("Scripts", &showScriptEditor,
                                 ImGuiWindowFlags_MenuBar)) {
                    bool doSave = false;
                    if (ImGui::BeginMenuBar()) {
                        if (ImGui::BeginMenu("File")) {
                            if (ImGui::MenuItem("New...")) openNewScript = true;
                            if (ImGui::BeginMenu("Open")) {
                                const auto files = listScripts();
                                if (files.empty()) ImGui::TextDisabled("(none)");
                                for (const std::string& f : files)
                                    if (ImGui::MenuItem(f.c_str())) openScript(f);
                                ImGui::EndMenu();
                            }
                            if (ImGui::MenuItem("Save", "Ctrl+S", false,
                                                !editorPath.empty()))
                                doSave = true;
                            ImGui::EndMenu();
                        }
                        ImGui::EndMenuBar();
                    }

                    ImGui::Text("%s%s", editorPath.empty() ? "(no file)"
                                                           : editorPath.c_str(),
                                editorDirty ? " *" : "");
                    if (!scripts.lastError().empty()) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f),
                                           "  %s", scripts.lastError().c_str());
                    }

                    // Ctrl+S saves while the editor window is focused.
                    const bool winFocused =
                        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
                    if (winFocused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))
                        doSave = true;

                    // Code completion: intercept navigate/accept/dismiss keys BEFORE
                    // the editor consumes them. We disable the editor's keyboard only
                    // on the exact frame we act on a key, so typing is unaffected.
                    ImFont* mono = gui.monoFont();
                    bool acceptComp = false, suppressKb = false;
                    if (comp.open && winFocused && !comp.items.empty()) {
                        const int n = static_cast<int>(comp.items.size());
                        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
                            comp.sel = (comp.sel + 1) % n; suppressKb = true;
                        } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
                            comp.sel = (comp.sel - 1 + n) % n; suppressKb = true;
                        } else if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
                            acceptComp = true; suppressKb = true;
                        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                            comp.open = false; suppressKb = true;
                            comp.manualClose = true; comp.closedPrefix = comp.prefix;
                        }
                    }

                    if (mono) ImGui::PushFont(mono);
                    if (suppressKb) luaEditor.SetHandleKeyboardInputs(false);
                    luaEditor.Render("LuaText");
                    if (suppressKb) luaEditor.SetHandleKeyboardInputs(true);
                    const ImVec2 edMin = ImGui::GetItemRectMin();
                    const ImVec2 edMax = ImGui::GetItemRectMax();
                    const float  charW = mono ? ImGui::CalcTextSize("A").x : 8.0f;
                    const float  lineH = ImGui::GetTextLineHeightWithSpacing();
                    if (mono) ImGui::PopFont();

                    if (luaEditor.IsTextChanged()) editorDirty = true;

                    // Accept the highlighted match: insert the identifier's tail
                    // after the already-typed prefix.
                    if (acceptComp && comp.sel >= 0 && comp.sel < static_cast<int>(comp.items.size())) {
                        const std::string full = comp.items[comp.sel].text;
                        if (full.size() > comp.prefix.size())
                            luaEditor.InsertText(full.substr(comp.prefix.size()));
                        comp.open = false; editorDirty = true;
                    }

                    // Recompute candidates from the new cursor/text (skip on the
                    // frame we suppressed the editor, so navigation/dismiss stick).
                    if (!winFocused) comp.open = false;
                    else if (!suppressKb) refreshCompletion(luaEditor, comp);

                    // Completion popup, best-effort anchored under the caret and
                    // clamped inside the editor rect.
                    if (comp.open && !comp.items.empty()) {
                        const auto cur = luaEditor.GetCursorPosition();
                        ImVec2 at(edMin.x + charW * (6.0f + cur.mColumn),
                                  edMin.y + lineH * (cur.mLine + 1));
                        at.x = std::min(at.x, edMax.x - 300.0f);
                        at.y = std::min(at.y, edMax.y - lineH);
                        at.x = std::max(at.x, edMin.x);
                        at.y = std::max(at.y, edMin.y);
                        ImGui::SetNextWindowPos(at);
                        ImGui::SetNextWindowSizeConstraints(
                            ImVec2(240.0f, 0.0f), ImVec2(520.0f, lineH * 10.0f + 12.0f));
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
                        if (ImGui::Begin("##luacomplete", nullptr,
                                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing |
                                ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoSavedSettings)) {
                            for (int i = 0; i < static_cast<int>(comp.items.size()); ++i) {
                                const bool sel = (i == comp.sel);
                                if (mono) ImGui::PushFont(mono);
                                if (ImGui::Selectable(comp.items[i].text, sel)) {
                                    const std::string full = comp.items[i].text;
                                    if (full.size() > comp.prefix.size())
                                        luaEditor.InsertText(full.substr(comp.prefix.size()));
                                    comp.open = false; editorDirty = true;
                                }
                                if (mono) ImGui::PopFont();
                                if (comp.items[i].hint && comp.items[i].hint[0]) {
                                    ImGui::SameLine();
                                    ImGui::TextDisabled("%s", comp.items[i].hint);
                                }
                                if (sel) ImGui::SetScrollHereY();
                            }
                        }
                        ImGui::End();
                        ImGui::PopStyleVar();
                    }

                    if (doSave) saveEditor();
                }
                ImGui::End();

                // New-script modal: create scripts/<name>.lua from a template.
                if (openNewScript) ImGui::OpenPopup("New Script");
                if (ImGui::BeginPopupModal("New Script", nullptr,
                                           ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::SetNextItemWidth(260.0f);
                    ImGui::InputText("Name", newScriptName, sizeof(newScriptName));
                    const char* templates[] = { "Empty component",
                                                "Component (documented)" };
                    ImGui::SetNextItemWidth(260.0f);
                    ImGui::Combo("Template", &newScriptTemplate, templates, 2);
                    const std::string safe = safeName(newScriptName);
                    const std::string file = safe + ".lua";
                    std::error_code sec;
                    const bool exists = newScriptName[0] &&
                        std::filesystem::exists(scriptPath(file), sec);
                    if (exists)
                        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f),
                                           "scripts/%s already exists.", file.c_str());
                    ImGui::BeginDisabled(newScriptName[0] == '\0' || exists);
                    if (ImGui::Button("Create", ImVec2(110.0f, 0.0f))) {
                        std::error_code ec;
                        std::filesystem::create_directories(scriptsDir(), ec);
                        std::ofstream out(scriptPath(file));
                        if (out) {
                            char body[2048];
                            std::snprintf(body, sizeof(body),
                                newScriptTemplate == 1 ? kTemplateDocumented
                                                       : kTemplateEmpty,
                                file.c_str());
                            out << body;
                        }
                        newScriptName[0] = '\0';
                        openScript(file);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f)))
                        ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
            }

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
            if (showEnv) {
                if (ImGui::Begin("Environment", &showEnv)) {
                    ImGui::TextDisabled("Equirectangular .hdr / .exr panorama.");
                    // Gather HDRI panoramas from the asset library: .hdr/.exr
                    // textures, excluding PBR material maps (normal/rough/etc).
                    auto isMaterialMap = [](const std::string& n){
                        std::string s = n;
                        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){
                            return static_cast<char>(std::tolower(c)); });
                        for (const char* t : {"_nor", "_normal", "_rough", "_disp",
                                "_diff", "_albedo", "_ao", "_spec", "_metal",
                                "_height", "_bump", "_opacity", "_mask", "_gloss",
                                "_translucent", "_color"})
                            if (s.find(t) != std::string::npos) return true;
                        return false;
                    };
                    std::vector<std::pair<std::string, std::string>> hdris; // (label, path)
                    for (const AssetId id : assetDb.allAssets()) {
                        const AssetDatabase::Entry* e = assetDb.entry(id);
                        if (!e || e->type != AssetType::Texture) continue;
                        std::string ext = e->absPath.extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(),
                            [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                        if ((ext != ".exr" && ext != ".hdr") || isMaterialMap(e->relPath))
                            continue;
                        hdris.push_back({e->relPath, e->absPath.string()});
                    }
                    std::sort(hdris.begin(), hdris.end());

                    ImGui::SetNextItemWidth(260.0f);
                    const char* curLabel = hdriLoaded.empty() ? "(select HDRI)"
                                                              : hdriLoaded.c_str();
                    if (ImGui::BeginCombo("HDRI", curLabel)) {
                        if (hdris.empty())
                            ImGui::TextDisabled("(no .hdr/.exr panoramas found)");
                        for (const auto& [label, path] : hdris)
                            if (ImGui::Selectable(label.c_str(), label == hdriLoaded)) {
                                if (environment.load(path)) {
                                    hdriLoaded  = label;
                                    hdriAbsPath = path;
                                    iblEnabled  = true;
                                }
                            }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled(environment.valid() ? "loaded" : "not loaded");

                    ImGui::BeginDisabled(!environment.valid());
                    ImGui::Checkbox("Enable IBL lighting", &iblEnabled);
                    ImGui::Checkbox("Show HDRI as background", &iblSkybox);
                    ImGui::SliderFloat("Intensity", &iblIntensity, 0.0f, 4.0f);
                    if (environment.valid())
                        ImGui::TextDisabled("auto-normalised x%.3g (panoramas differ\n"
                                            "in absolute brightness by decades)",
                                            environment.exposureScale());
                    ImGui::EndDisabled();
                    ImGui::TextDisabled("Lights surfaces from the panorama\n"
                                        "(diffuse irradiance + specular).");
                }
                ImGui::End();
            }

            if (showVehiclePanel) { if (ImGui::Begin("Vehicle", &showVehiclePanel)) {
                if (ImGui::Checkbox("Drive mode (V)", &vehicleMode)) {
                    if (vehicleMode) enterVehicleMode();
                    else             endEditorDrive();
                }
                if (vehicleMode)
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                                       "W/S drive, A/D steer, Space brake, Esc exit");
                else
                    ImGui::TextDisabled("Press V or tick above to drive");

                // Per-scene Play options (saved with the scene / exported game).
                ui::sectionText("Play start");
                // WHAT Play starts as lives in File > Game Settings now ("Start
                // as"). It is a statement about the game rather than about this
                // panel, there are five answers rather than one checkbox here and
                // another in the Glider panel, and two checkboxes could disagree.
                // The pointer stays because this is where people look for it.
                ImGui::TextDisabled("Start mode: File > Game Settings");
                ImGui::Checkbox("Show crosshair", &showCrosshair);

                ui::sectionText("Skid marks");
                ImGui::Checkbox("Enable skid marks", &skids.enabled);
                ImGui::BeginDisabled(!skids.enabled);
                ImGui::SliderFloat("Slip threshold", &skids.slipThresh, 0.1f, 1.5f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How much a wheel must slip (lock/spin/drift)\n"
                                      "before it leaves a mark (lower = more marks).");
                ImGui::SliderFloat("Mark width", &skids.markHalfW, 0.05f, 0.6f, "%.2f m");
                ImGui::SliderFloat("Darkness", &skids.opacity, 0.1f, 1.0f, "%.2f");
                ImGui::EndDisabled();

                ui::sectionText("Contrails");
                ImGui::Checkbox("Enable contrails", &trails.enabled);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Vapour trails streaming behind the racers\n"
                                      "(the driven craft and every opponent) in Play.");
                ImGui::BeginDisabled(!trails.enabled);
                ImGui::SliderFloat("Trail length", &trails.life, 0.3f, 5.0f, "%.1f s");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How long each puff lingers before it fades\n"
                                      "out -- longer = a longer streak.");
                ImGui::SliderFloat("Trail width", &trails.width, 0.05f, 1.5f, "%.2f m");
                ImGui::SliderFloat("Trail opacity", &trails.opacity, 0.05f, 1.0f, "%.2f");
                ImGui::SliderFloat("Trail glow", &trails.glow, 0.0f, 6.0f, "%.1f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Self-illumination: the streak glows on its\n"
                                      "own instead of being lit (and dimmed) by the sun.");
                ImGui::SliderFloat("Trail spacing", &trails.minStep, 0.2f, 3.0f, "%.1f m");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Distance between recorded points. Smaller = a\n"
                                      "smoother ribbon (more geometry).");
                ImGui::ColorEdit3("Trail colour", &trails.color.x);
                ImGui::EndDisabled();

                ui::sectionText("Missiles");
                weapons.settingsPanel(soundPickerCombo);

                // Scene vehicles: hook a model into the vehicle system with one
                // click. The auto-setup edit goes through the undo history.
                auto makeDrivable = [&](int rootId) -> std::string {
                    Entity* e = document.find(rootId);
                    if (!e) return std::string();
                    const Entity before = *e;
                    std::string rep = vehicleui::autoSetup(document, rootId);
                    if (Entity* after = document.find(rootId)) {
                        auto cmd = std::make_unique<ModifyEntityCmd>(before, *after);
                        if (!cmd->trivial()) history.pushApplied(std::move(cmd));
                    }
                    return rep;
                };
                const int selId =
                    (sel.valid())
                        ? entities[sel.index()].id : -1;
                const int pick = vehicleui::panelSection(document, selId, makeDrivable);
                if (pick >= 0) sel.select(pick);

                ui::sectionText("Setup gizmo");
                ImGui::Checkbox("Edit setup in viewport", &vehGizmoEdit);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Drag the axles, track, wheels, collision box and centre\n"
                        "of mass directly in the viewport.\n\n"
                        "The shape is ALWAYS drawn for the selected vehicle -- this\n"
                        "hands the handles the left mouse button, so the transform\n"
                        "gizmo pauses while it is on.\n\n"
                        "Arrow keys nudge the selected handle (Shift = bigger steps).");
                ImGui::TextDisabled("Select a vehicle to see its setup drawn.");

                ui::sectionText("Test car");
                ImGui::Checkbox("Show vehicle", &showVehicle);
                if (ImGui::Button("Place at camera")) placeCar();
                if (carPlaced) ImGui::Text("Speed: %.0f km/h", std::abs(carSpeed) * 3.6f);
                else           ImGui::TextDisabled("Vehicle not placed yet");
            }
            ImGui::End(); }

            if (showGliderPanel) { if (ImGui::Begin("Glider", &showGliderPanel)) {
                if (ImGui::Checkbox("Fly mode (G)", &gliderMode)) {
                    if (gliderMode) {
                        if (vehicleMode) { vehicleMode = false; endEditorDrive(); }
                        enterGliderMode();
                    } else {
                        endGliderDrive();
                    }
                }
                if (gliderMode && driveGliderId >= 0)
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                                       "W/S thrust, A/D steer, Space air-brake, Esc exit");
                else
                    ImGui::TextDisabled("Add a Glider component, then press G to fly");

                // Starting Play already flying is File > Game Settings ("Start
                // as") now -- see the note in the Vehicle panel.
                ui::sectionText("Play start");
                ImGui::TextDisabled("Start mode: File > Game Settings");

                // Turn a selected model into a glider with one click (undoable).
                auto makeGlider = [&](int rootId) -> std::string {
                    Entity* e = document.find(rootId);
                    if (!e) return std::string();
                    const Entity before = *e;
                    std::string rep = gliderui::autoSetup(document, rootId);
                    if (Entity* after = document.find(rootId)) {
                        auto cmd = std::make_unique<ModifyEntityCmd>(before, *after);
                        if (!cmd->trivial()) history.pushApplied(std::move(cmd));
                    }
                    return rep;
                };
                const int selId =
                    (sel.valid())
                        ? entities[sel.index()].id : -1;
                const int pick = gliderui::panelSection(document, selId, makeGlider);
                if (pick >= 0) sel.select(pick);

                if (gliderMode && driveGliderId >= 0)
                    ImGui::Text("Speed: %.0f km/h",
                                glm::length(glm::vec3(gliderVel.x, 0.0f, gliderVel.z)) * 3.6f);
            }
            ImGui::End(); }

            // Scene UI overlay editor: author the per-scene 2D HUD (text, buttons,
            // images). The list edit is bracketed into one undo step -- opened when
            // a field is first touched, committed when nothing is active -- exactly
            // like the Inspector and the road edits.
            if (showUiOverlay) {
                const std::vector<UiElement> uiFrameStart = uiOverlay.elements();

                // Scene names for the LoadScene action picker (stems of the sibling
                // .fitzel files), and the sound list for PlaySound.
                std::vector<std::string> sceneNames;
                if (!currentProject.empty()) {
                    const std::string projFolder =
                        std::filesystem::path(currentProject).parent_path().generic_string();
                    for (const auto& sc : listScenesIn(projFolder))
                        sceneNames.push_back(sc.first);
                }
                const std::vector<std::string> soundNames = listSounds();

                // "Copy to scene": write this overlay into a sibling scene file
                // without opening it. Only the overlay keys of the target's
                // settings are touched -- its entities and everything else stay.
                auto copyOverlayToScene = [&](const std::string& stem) -> std::string {
                    if (currentProject.empty()) return "No project open.";
                    const std::filesystem::path cur(currentProject);
                    if (cur.stem().string() == stem)
                        return "That's the scene you're editing.";
                    const std::filesystem::path target =
                        cur.parent_path() / (stem + ".fitzel");
                    std::error_code cec;
                    if (!std::filesystem::exists(target, cec))
                        return "Scene not found: " + stem;
                    nlohmann::json keys = nlohmann::json::object();
                    uiOverlay.save(keys); // "uiOverlay" + "uiOverlayMenu"
                    if (keys.empty()) return "Nothing to copy.";
                    if (!projectio::mergeSceneSettings(target.generic_string(), keys))
                        return "Could not write " + stem + ".fitzel";
                    return "Copied " + std::to_string(uiOverlay.elements().size()) +
                           " element(s) into " + stem +
                           " (its previous overlay was replaced).";
                };

                uiOverlay.drawEditorPanel(&showUiOverlay, uiSel, assetDb,
                                          sceneNames, soundNames, copyOverlayToScene);

                const bool uiActive  = ImGui::IsAnyItemActive();
                const bool uiChanged = uiOverlay.elements() != uiFrameStart;
                if (uiChanged && !uiEditOpen) { uiEditOpen = true; uiEditBefore = uiFrameStart; }
                if (uiEditOpen && !uiActive) {
                    uiEditOpen = false;
                    auto cmd = std::make_unique<UiOverlayCmd>(
                        uiOverlay, uiEditBefore, uiOverlay.elements());
                    if (!cmd->trivial()) history.push(std::move(cmd), document);
                }
            }

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
                      .set("uTexScale", texScale)
                      .set("uNormalStrength", normalStrength)
                      .set("uWaterLevel", waterLevel)
                      .set("uWetness", roadWetness)
                      .set("uMeshPaint", 0)             // object paint is not the terrain's
                      .set("uAlbedo", glm::vec3(0.5f)); // neutral grey where no layer covers
            {
                int bound = 0;
                for (const TerrainLayer& L : look.layers) {
                    if (!L.tex || bound >= kMaxTerrainLayers) continue;
                    const std::string ix = std::to_string(bound);
                    terrainMat.setTexture("uLayerTex[" + ix + "]", *L.tex,
                                          terrainLayerUnit(bound))
                              .set("uLayerBand[" + ix + "]",
                                   glm::vec4(L.heightStart, L.heightEnd,
                                             L.slopeStart, L.slopeEnd))
                              .set("uLayerScale[" + ix + "]", L.scale);
                    // Optional normal map, kept high so it clears the
                    // shadow/env/IBL samplers the renderer binds lower down.
                    if (L.norm) {
                        terrainMat.setTexture("uLayerNorm[" + ix + "]", *L.norm,
                                              terrainLayerNormUnit(bound))
                                  .set("uLayerHasNorm[" + ix + "]", 1);
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

            renderer.setViewport(paneW, fbH);
            // Shadow cascades are fitted to player one's frustum and shared by
            // both panes: they are built once per frame, before either pane is
            // drawn. Good enough while the two are racing the same stretch of
            // track; a player who drives far away from the other gets cascades
            // sized for someone else's view.
            renderer.begin(camera, aspect, light);

            for (const TerrainChunk* chunk : streamer.visibleChunks()) {
                renderer.submit(chunk->mesh(), terrainMat, glm::mat4(1.0f), false);
            }

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
            //     Animation component on an animated model, advance its clock and
            //     re-skin the model's meshes so the shared static render path shows
            //     the deformed pose. (Meshes are shared per model: instances of the
            //     same model animate together.)
            {
                std::vector<Vertex> skinScratch;
                for (Entity& e : entities) {
                    if (!e.activeInHierarchy) continue;   // deactivated: don't skin
                    auto* ac = e.components.get<AnimationComponent>();
                    const auto* mc = e.components.get<ModelComponent>();
                    if (!ac || !mc) continue;
                    LoadedModel* lm = models.byId(mc->modelId);
                    if (!lm || !lm->animated || !lm->animData) continue;
                    const auto& clips = lm->animData->animations;
                    if (clips.empty()) continue;
                    const int ci = glm::clamp(ac->clip, 0,
                                              static_cast<int>(clips.size()) - 1);
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
                    const auto palette = sampleSkeleton(*lm->animData, ci, ac->time);
                    if (palette.empty()) continue;
                    const auto& prims = lm->animData->primitives;
                    for (std::size_t p = 0;
                         p < lm->meshes.size() && p < prims.size(); ++p) {
                        skinPrimitive(prims[p], palette, skinScratch);
                        lm->meshes[p].update(skinScratch);
                    }
                }
            }

            // --- Scene entities through the renderer (shadows, lighting, water).
            // Fences, walls and track: regenerate whatever an edit dirtied, HERE
            // rather than at the edit site, so a path is rebuilt at most once per
            // frame however many sliders moved -- and before gpuMats is built,
            // because the generator find-or-creates its palette materials in the
            // library and a batch drawn with a material that isn't in gpuMats yet
            // would be skipped for a frame.
            splines.update(materials);

            // Water: re-solve whatever an edit dirtied, here for the same reason
            // -- once per frame however many sliders moved. Only the SURFACE is
            // rebuilt; the bed waits for the gesture to end (see carveRivers),
            // which is why a drag shows the water moving live and the ground
            // catching up when the mouse comes back up.
            rivers.update();   // ...and before gpuMats, for the same reason

            // Handing them over is one function, in SceneSubmit.cpp, so that the
            // editor is a CALLER of it rather than the only place it exists --
            // see the header there for why that matters the moment anything else
            // has to draw a scene. The scratch lives here because the render
            // queue points into it and is replayed several times per frame.
            scenesubmit::Scratch submitScratch;
            scenesubmit::submit({entities, materials, document, models, meshCache,
                                 lit, renderer,
                                 carCube, rampMesh, cylMesh, sphereMesh, planeMesh,
                                 composeModel, roadWetness, playMode},
                                submitScratch);
            // One GPU material per library asset -- and the batched geometry
            // below (the roadside city, the side objects, the splines) wears
            // the same library materials the entities do, so it draws from that
            // same table instead of building a second one that could disagree.
            const std::vector<Material>& gpuMats = submitScratch.gpuMats;

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
            // Knockable posts follow their rigid body: read the box's world
            // transform (its centre == the model's AABB centre, as placed) and draw
            // the model there, so a clipped post tumbles and flies off.
            if (playMode && physics)
                for (const SidePost& p : sidePosts) {
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

            // Missile motors and detonations are lights too -- a blast that does
            // not light the corner it goes off in reads as a decal pasted over
            // the scene. Appended last so authored scene lights keep priority
            // when the renderer's budget runs out.
            weapons.collectLights(pointLights);
            weapons2.collectLights(pointLights);

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
#ifndef FITZEL_PLAYER
            // The offline renderer harvests HERE and nowhere else: every
            // system has submitted, the lights are set, and begin() has not
            // yet cleared the queue. Costs a bool test unless somebody has
            // actually pressed Render.
            pathpanel::SceneLook ptLook;
            ptLook.hdriPath      = hdriAbsPath;
            ptLook.hdriIntensity = iblEnabled ? iblIntensity : 0.0f;
            // The grade the post chain will put on this very frame. Without it a
            // render comes out flat and cool beside the viewport, because the
            // viewport never shows an ungraded image -- not even in a project
            // nobody has touched the Colour grade panel in.
            ptLook.grade.hueShift   = hueShift;
            ptLook.grade.saturation = saturation;
            ptLook.grade.value      = valueGain;
            ptLook.grade.warmth     = warmth;
            ptLook.grade.contrast   = contrast;
            pathpanel::service(pathRender, lightGrid, renderer, camera, ptLook,
                               currentProject.empty()
                                   ? std::filesystem::path()
                                   : std::filesystem::path(currentProject));
            // The path-traced viewport harvests from the same window and for
            // the same reason. Off, this is one early return.
            viewtrace::service(viewTrace, shade == kShadePathTraced, renderer,
                               camera, ptLook, viewW, viewH, now);
#endif
            renderer.preparePointShadows(); // omni shadow cubemaps (opt-in lights)

            // --- Multi-pass render with sky and planar water ------------
            // Trees cast shadows: drawn into every cascade via this callback.
            // Which eye the cascades are being fitted to. Split screen runs the
            // pass twice with two different cameras, and the tree cull measures
            // distance from the eye -- fed player one's position both times, the
            // second pane loses the shadows around itself.
            glm::vec2 shadowEyeXZ(camera.position().x, camera.position().z);
            auto treeShadowCaster = [&](const glm::mat4& lightSpace, int,
                                        float cascadeFar) {
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
                const float reach =
                    cascadeFar + std::min(25.0f * sunXZ / sunY, 60.0f);
                // The author's own limit still wins: it is the one that decides
                // whether the far cascades get a forest at all.
                const float limit = veg.treeShadowDistance > 0.0f
                                        ? std::min(veg.treeShadowDistance, reach)
                                        : reach;
                veg.drawTreeShadow(lightSpace, now, weather, shadowEyeXZ, limit);
            };
            // Cascades for player one. With two panes up the second pane fits
            // its own set inside the loop below -- shadows are cut to a view
            // frustum, so they cannot be shared between two people looking at
            // different places.
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
                sky.setFloat("uCloudScale", cloudScale);
                sky.setFloat("uCloudSpeed", effWind);
                sky.setFloat("uCloudBottom", effCloudBot);
                sky.setFloat("uCloudTop", cloudTop);
                sky.setFloat("uCirrus", cirrusAmount);
                sky.setFloat("uCirrusHeight", cirrusHeight);
                sky.setFloat("uCirrusSpeed", cirrusSpeed);
                sky.setFloat("uContrails", contrailAmount);
                sky.setFloat("uExposure", exposure);
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
                skybox.setFloat("uExposure", exposure);
                skybox.setInt("uTonemap", tonemap ? 1 : 0);
                fsQuad.draw();
                glDepthMask(GL_TRUE);
                glEnable(GL_DEPTH_TEST);
                glEnable(GL_CULL_FACE);
            };

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
                glm::vec3 probePos = camera.position();
                for (const Entity& b : entities) {
                    const auto* mc = b.components.get<MaterialComponent>();
                    if (b.type != EntityType::Light && b.type != EntityType::Sun && mc &&
                        isMirror(materials[document.materialIndex(mc->material)])) {
                        probePos = b.center;
                        break;
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
            const glm::mat4  proj   = vcam.projectionMatrix(aspect);
            const glm::mat4  mainVP = proj * view;

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
            // Terrain occlusion is deliberately ignored -- that would want an
            // occlusion query, and erring that way only costs a pass that could
            // have been skipped, never a missing reflection.
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
                    if (above && below) return true;        // frustum straddles it
                }
                return false;
            }();

            if (waterVisible) {
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
                glCullFace(GL_FRONT); // mirroring flips winding
                renderer.renderScene(reflView, proj, reflEye,
                                     glm::vec4(0, 1, 0, -waterLevel + 0.1f), false);
                glCullFace(GL_BACK);
                {   // trees mirror in the water (reflected view/eye)
                    veg.drawTrees(makeFrameContext(proj * reflView, reflEye, now, weather,
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
            if (hdrRT.width() != paneW || hdrRT.height() != fbH)
                hdrRT = RenderTarget(paneW, fbH, RenderTarget::Format::RGBA16F, true);
            post.resize(paneW, fbH);   // no-op unless the pane actually changed
            // The one target that stays FULL width: it is the finished image
            // both panes are blitted into (and what the editor shows in its
            // viewport panel).
            if (viewportRT.width() != fbW || viewportRT.height() != fbH)
                viewportRT = RenderTarget(fbW, fbH, RenderTarget::Format::RGBA8);
            hdrRT.bind();
            // A plain mode gets a flat ground to stand on rather than a sky: the
            // sky is a material too, and a wireframe read against a sunset is
            // exactly the reading these modes exist to avoid. Linear, because
            // the composite tonemaps afterwards.
            if (!shadeFull) glClearColor(0.055f, 0.060f, 0.070f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            if (shadeFull) drawBackground(glm::inverse(mainVP), camPos, false);
            if (shade == kShadeWireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            {
                FZ_GPU_ZONE("GPU terrain + objects");
                renderer.renderScene(view, proj, camPos, Renderer::kNoClip, false);
            }
            if (shade == kShadeWireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

            // Shared draw context for the lit vegetation (grass, trees, billboards)
            // in this HDR pass.
            const FrameContext gctx =
                makeFrameContext(mainVP, camPos, now, weather, light, fog);

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
                veg.drawBirds(mainVP, now, camPos);
            }

            // 4) The water surface: a large quad following the camera, sampling
            //    the reflection/refraction targets with Fresnel + ripples. Drawn
            //    only when those targets were filled this frame -- otherwise it
            //    would mirror whatever the camera was looking at last time it
            //    saw water.
            if (waterVisible) {
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
                water.setFloat("uExposure", exposure);
                water.setInt("uTonemap", 0); // linear into HDR; composite tonemaps
                water.setInt("uReflection", 0);
                water.setInt("uRefraction", 1);
                water.setInt("uRefractionDepth", 2);
                water.setFloat("uNear", camera.nearPlane());
                water.setFloat("uFar", camera.farPlane());
                water.setFloat("uFoamWidth", foamWidth);
                reflectRT.bindColorTexture(0);
                refractRT.bindColorTexture(1);
                refractRT.bindDepthTexture(2);
                waterMesh.draw();
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
                river.setFloat("uExposure", exposure);
                river.setInt("uTonemap", 0); // linear into HDR; composite tonemaps
                // The scene probe, for the reflection. Unit 2 is the renderer's
                // own probe unit and it rebinds it every lit pass, so borrowing
                // it here costs nothing and cannot alias a 2D sampler: this
                // shader reads the cube and nothing else from it.
                renderer.bindEnvProbe(Renderer::kEnvProbeUnit);
                river.setInt("uEnvProbe", Renderer::kEnvProbeUnit);
                river.setFloat("uEnvMaxLod", renderer.envProbeMaxLod());
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
            // Authored emitters. Stepped with the frame clock rather than the
            // sim's fixed one: these are decoration, and a puff of smoke that
            // resolves one frame late is worth less than the code to avoid it.
            // Updated HERE, next to the draw, so a paused editor still shows the
            // effect it is being tuned against.
            particles.update(entities, dt, assetDb);
            particles.draw(gctx);

            // --- Fireflies: night-only glowing wanderers, additive into HDR ---
            veg.drawFireflies(mainVP, now, 1.0f - dayF, camPos);

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
                    gfxSet, ssaoStrength, bloomIntensity, rayIntensity, dofMax,
                    motionBlurStrength * blurSt.blurSpeed01 * 0.35f);
                pp.ssaoRadius = ssaoRadius; pp.ssaoBias = ssaoBias;
                pp.ssaoPower  = ssaoPower;  pp.ssaoStrength = gate.ssaoStrength;
                pp.bloomThreshold = bloomThreshold; pp.bloomKnee = bloomKnee;
                pp.bloomIntensity = gate.bloomIntensity;
                pp.rayIntensity   = gate.rayIntensity;
                pp.dofNear = dofNear; pp.dofFar = dofFar; pp.dofMax = gate.dofMax;
                pp.exposure = exposure;
                pp.hueShift = hueShift; pp.saturation = saturation;
                pp.valueGain = valueGain; pp.warmth = warmth; pp.contrast = contrast;
                pp.blurStrength     = gate.blurStrength;
                pp.blurAnchor       = blurSt.blurAnchorWorld;
                pp.blurAnchorValid  = blurSt.blurAnchorValid;
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
              post.present(fsQuad, fxaaEnabled); }
            } // per-pane loop

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
                grid.cell   = cursorGrid;   // what you see is what you snap to
                grid.plane  = cursor3D.y;   // ...on the plane the cursor is on
                grid.cursor = cursor3D;
                // Never fade beyond what the camera can see: the grid's quad ends
                // at its fade distance, so a fade further out than the far plane
                // would be sliced off mid-strength by the clip instead of easing
                // away. View distance is the knob for seeing further.
                grid.fade   = std::min(gridFade, camera.farPlane() * 0.7f);
                grid.highlightCursorCell = cursorVisible;
                grid.viewportPx    = glm::vec2(fbW, fbH);
                grid.sceneDepthUnit = 0;
                hdrRT.bindDepthTexture(0);
                grid.draw(makeFrameContext(mainVP, camPos, now, weather, light, fog));
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
                if (showCrosshair && !vehicleMode && !gliderMode && !showroomUi.active()) {
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
                        input.setCursorLocked(fpsMode);
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
                    input.setCursorLocked(fpsMode);
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
        }

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
        return 1;
    }

    return 0;
}
