#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
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

#include <fitzel/Fitzel.hpp>
#include <fitzel/graphics/EnvironmentIBL.hpp>
#include <fitzel/physics/Physics.hpp>

#include "SceneTypes.hpp"
#include "Document.hpp"
#include "Command.hpp"
#include "PropertyMeta.hpp"
#include "Primitives.hpp"
#include "ModelLibrary.hpp"
#include "SandboxMath.hpp"
#include "CameraPath.hpp"
#include "ScriptSystem.hpp"
#include "ProjectIO.hpp"
#include "PaintPanel.hpp"
#include "SculptPanel.hpp"
#include "AssetDrop.hpp"
#include "FrameRender.hpp"
#include "RainRenderer.hpp"
#include "SpraySystem.hpp"
#include "TerrainPanel.hpp"
#include "FolderDialog.hpp"
#include "VegetationSystem.hpp"
#include "RoadSystem.hpp"
#include "RoadPanel.hpp"
#include "SkidSystem.hpp"
#include "ScatterTool.hpp"
#include "VehicleTool.hpp"
#include "CarAudio.hpp"

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

int main(int argc, char** argv) {
    try {
        // Resolve all relative paths (assets/, content/, scripts/, game.json,
        // project/) against the executable's own directory, so the app behaves
        // the same whether launched from a shell, a shortcut, or a double-click.
        {
            std::error_code ec;
            if (argc > 0) {
                auto exePath = std::filesystem::absolute(argv[0], ec);
                if (!ec && exePath.has_parent_path())
                    std::filesystem::current_path(exePath.parent_path(), ec);
            }
        }
        // Exported/player build: a game.json next to the exe boots straight into
        // the game with the editor hidden. `--play <projectFolder>` does the same.
        std::string bootProject;
        bool        bootFullscreen = true;
        {
            std::error_code ec;
            if (std::filesystem::exists("game.json", ec)) {
                std::ifstream gin("game.json");
                try {
                    nlohmann::json gj; gin >> gj;
                    bootProject    = gj.value("project", std::string{});
                    bootFullscreen = gj.value("fullscreen", true);
                } catch (...) {}
            }
        }
        for (int i = 1; i + 1 < argc; ++i)
            if (std::string(argv[i]) == "--play") bootProject = argv[i + 1];
        const bool playerMode = !bootProject.empty();

        Window window(WindowConfig{
            .width     = 1280,
            .height    = 720,
            .title     = "Fitzel",
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

        // Content roots: prefer a `content/` next to the exe (a portable/exported
        // build ships its assets there), else the compile-time dev tree.
        const bool localContent = std::filesystem::exists("content") &&
                                  std::filesystem::is_directory("content");
        const std::string contentRoot = localContent
            ? std::filesystem::absolute("content").generic_string()
            : std::string(FITZEL_CONTENT_DIR);
        const std::string modelDir = localContent ? contentRoot + "/models"
                                                   : std::string(FITZEL_MODEL_DIR);

        // Startup loading screen: render one frame with a progress bar. Called
        // between the (synchronous, GL-bound) asset loads so the window shows what
        // it is doing instead of staying black while everything loads.
        auto showProgress = [&](float frac, const char* label) {
            window.pollEvents();
            int w = 0, h = 0;
            window.framebufferSize(w, h);
            glViewport(0, 0, w, h);
            glClearColor(0.08f, 0.09f, 0.11f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            gui.beginFrame();
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(
                ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                       vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(480.0f, 0.0f));
            ImGui::Begin("##loading", nullptr,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse);
            ImGui::Text("Fitzel");
            ImGui::Spacing();
            ImGui::TextUnformatted(label);
            ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f));
            ImGui::End();
            gui.endFrame();
            window.swapBuffers();
        };
        showProgress(0.02f, "Starting up...");

        // Central asset registry: scans the project's content/ tree, giving each
        // texture/model/sound a stable GUID (persisted in a `<file>.meta` sidecar)
        // and caching decoded assets so repeated loads are deduplicated. Model
        // imports and material textures below resolve through this database.
        showProgress(0.05f, "Scanning content library...");
        AssetDatabase assetDb(contentRoot);
        assetDb.refresh();

        Shader lit = Shader::fromFiles("assets/shaders/lit.vert",
                                       "assets/shaders/lit.frag");
        if (!lit.isValid()) {
            std::fprintf(stderr, "Failed to load lit shader\n");
            return 1;
        }

        // Slope/height-driven terrain palette (TerrainLook, defined in
        // TerrainPanel.hpp), exposed as material parameters and edited in the
        // Terrain panel.
        TerrainLook look;

        // Terrain PBR-ish albedo textures (triplanar), loaded from the repo's
        // textures/ folder (path injected by CMake).
        const std::string texDir = localContent ? contentRoot + "/textures"
                                                 : std::string(FITZEL_TEXTURE_DIR);
        // Default terrain surface: one moon macro texture drives all four
        // material layers (sand/ground/cliff/snow), so the whole terrain reads
        // as a single moon surface; the height/slope blend then only varies the
        // tiling scale between the layers.
        showProgress(0.12f, "Loading terrain texture (moon)...");
        Texture texMoon  = Texture::fromFile(texDir + "/moon_flat_macro_02_diff_4k.jpg");
        // Matching normal map. PolyHaven ships these as DWAA-compressed EXR (which
        // tinyexr can't decode), converted once to PNG (see README); no v-flip.
        showProgress(0.32f, "Loading terrain normal map (moon)...");
        Texture texMoonN = Texture::fromFile(texDir + "/moon_flat_macro_02_nor_gl_4k.png", false);
        if (!texMoon.isValid() || !texMoonN.isValid()) {
            std::fprintf(stderr, "Warning: moon terrain textures failed to load from %s\n",
                         texDir.c_str());
        }
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
        { // lift the camera to stand ~9 units above the terrain at its position
            const glm::vec3 cp = camera.position();
            camera.setPosition({cp.x, streamer.heightAt(cp.x, cp.z) + 9.0f, cp.z});
        }
        Renderer        renderer(2048, 4);
        DirectionalLight light;

        // Image-based lighting from an HDRI (chosen from the asset library).
        EnvironmentIBL environment;
        bool  iblEnabled   = false;
        bool  iblSkybox    = false;   // draw the HDRI as the sky background
        float iblIntensity = 1.0f;
        std::string hdriLoaded;       // relPath of the loaded HDRI ("" = none)

        // Water: planar reflection/refraction targets + a surface quad.
        Shader water = Shader::fromFiles("assets/shaders/water.vert",
                                         "assets/shaders/water.frag");
        if (!water.isValid()) {
            std::fprintf(stderr, "Failed to load water shader\n");
            return 1;
        }
        // A tessellated water grid so Gerstner waves can displace its vertices.
        const int gridN = 400;
        std::vector<Vertex>        waterVerts;
        std::vector<std::uint32_t> waterIdx;
        waterVerts.reserve(static_cast<std::size_t>(gridN) * gridN);
        for (int z = 0; z < gridN; ++z) {
            for (int x = 0; x < gridN; ++x) {
                const float fx = static_cast<float>(x) / (gridN - 1) - 0.5f;
                const float fz = static_cast<float>(z) / (gridN - 1) - 0.5f;
                waterVerts.push_back({{fx, 0.0f, fz}, {0, 1, 0},
                                      {static_cast<float>(x) / (gridN - 1),
                                       static_cast<float>(z) / (gridN - 1)}});
            }
        }
        for (int z = 0; z < gridN - 1; ++z) {
            for (int x = 0; x < gridN - 1; ++x) {
                const std::uint32_t i0 = static_cast<std::uint32_t>(z * gridN + x);
                const std::uint32_t i1 = i0 + 1;
                const std::uint32_t i2 = i0 + gridN;
                const std::uint32_t i3 = i2 + 1;
                waterIdx.insert(waterIdx.end(), {i0, i2, i1, i1, i2, i3});
            }
        }
        Mesh waterMesh = Mesh::create(waterVerts, waterIdx);
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

        // Sky + volumetric clouds (fullscreen raymarch pass).
        Shader sky = Shader::fromFiles("assets/shaders/sky.vert",
                                       "assets/shaders/sky.frag");
        if (!sky.isValid()) {
            std::fprintf(stderr, "Failed to load sky shader\n");
            return 1;
        }
        // HDRI skybox (reuses the fullscreen sky vertex shader).
        Shader skybox = Shader::fromFiles("assets/shaders/sky.vert",
                                          "assets/shaders/skybox.frag");
        const std::vector<Vertex> fsVerts = {
            {{-1.0f, -1.0f, 0.0f}, {0, 0, 1}, {0, 0}},
            {{ 1.0f, -1.0f, 0.0f}, {0, 0, 1}, {1, 0}},
            {{ 1.0f,  1.0f, 0.0f}, {0, 0, 1}, {1, 1}},
            {{-1.0f,  1.0f, 0.0f}, {0, 0, 1}, {0, 1}},
        };
        Mesh fsQuad = Mesh::create(fsVerts, {0, 1, 2, 0, 2, 3});

        // HDR scene buffer + post-processing (bloom, god rays, lens flare, tonemap).
        Shader composite = Shader::fromFiles("assets/shaders/sky.vert",
                                             "assets/shaders/composite.frag");
        if (!composite.isValid()) {
            std::fprintf(stderr, "Failed to load composite shader\n");
            return 1;
        }
        int hdrW = 0, hdrH = 0;
        window.framebufferSize(hdrW, hdrH);
        RenderTarget hdrRT(hdrW, hdrH, RenderTarget::Format::RGBA16F, /*depthTex=*/true);
        float bloomIntensity = 0.35f;
        float rayIntensity   = 0.5f;

        // SSAO (half-res), reconstructing position/normal from the HDR depth.
        Shader ssao = Shader::fromFiles("assets/shaders/sky.vert",
                                        "assets/shaders/ssao.frag");
        if (!ssao.isValid()) { std::fprintf(stderr, "Failed to load ssao shader\n"); return 1; }
        RenderTarget ssaoRT(hdrW / 2, hdrH / 2);

        // The final composited image lives in this target and is shown as the
        // central "Viewport" dock panel (IDE/editor style). Its size tracks the
        // panel's content region, so the scene renders at the viewport's pixels.
        RenderTarget viewportRT(hdrW, hdrH, RenderTarget::Format::RGBA8);
        // FXAA reads this LDR composite result and writes the anti-aliased image
        // to the viewport texture (or screen). Toggle via fxaaEnabled.
        Shader fxaa = Shader::fromFiles("assets/shaders/sky.vert",
                                        "assets/shaders/fxaa.frag");
        if (!fxaa.isValid()) { std::fprintf(stderr, "Failed to load fxaa shader\n"); return 1; }
        RenderTarget postRT(hdrW, hdrH, RenderTarget::Format::RGBA8);
        bool fxaaEnabled = true;
        int  viewW = hdrW, viewH = hdrH;
        bool viewportHovered = false;
        glm::vec2 viewportMouseNdc(0.0f); // cursor within the viewport, NDC [-1,1]
        bool viewportClicked = false;     // left-click landed on the viewport image
        glm::vec2 viewportRectMin(0.0f);  // viewport image top-left in screen px
        glm::vec2 viewportRectSize(0.0f); // viewport image size in screen px
        {
            std::mt19937 rng(1337u);
            std::uniform_real_distribution<float> d(0.0f, 1.0f);
            ssao.bind();
            const int kn = 24;
            for (int i = 0; i < kn; ++i) {
                glm::vec3 s(d(rng) * 2.0f - 1.0f, d(rng) * 2.0f - 1.0f, d(rng));
                s = glm::normalize(s) * d(rng);
                const float t = static_cast<float>(i) / kn;
                s *= glm::mix(0.1f, 1.0f, t * t); // cluster samples near the origin
                ssao.setVec3("uKernel[" + std::to_string(i) + "]", s);
            }
            ssao.setInt("uKernelSize", kn);
        }
        float ssaoStrength = 0.7f;
        float ssaoRadius   = 1.5f;
        float ssaoBias     = 0.03f;
        float ssaoPower    = 1.6f;

        // Day/night cycle.
        float timeOfDay = 7.3f;    // hours [0,24)
        float dayLength = 240.0f;  // real seconds per full 24h (0 = frozen)
        bool  timePaused = true;   // freeze the time of day where it is

        // Cloud controls.
        float cloudCoverage = 0.5f;
        float cloudDensity  = 1.0f;
        float cloudScale    = 0.0025f;
        float cloudSpeed    = 5.0f;
        float cloudBottom   = 140.0f;
        float cloudTop      = 320.0f;

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

        // --- Object scatter -------------------------------------------------
        // A 3D brush that sprinkles imported models over the terrain as regular
        // Model entities, grouped under a root "Scattered" Empty; one stamp =
        // one undo step. Settings/placement/panel live in ScatterTool.
        bool               scatterMode = false;
        scatterui::Settings scatterCfg;


        // --- Trees: instanced model + billboard LOD (owned by VegetationSystem)
        if (!veg.initTrees(modelDir, texDir)) return 1;

        // --- Roads / paths (owned by RoadSystem) -----------------------------
        // Ribbon mesh along a Catmull-Rom spline, draped on the terrain. RoadSystem
        // owns the mesh/material/collider/centreline; main keeps the control-point
        // editor state (shares the LMB) and the roadPickTerrain helper below (used
        // by every viewport brush, not just roads).
        RoadSystem road(lit, assetDb, streamer, texDir);
        SkidSystem skids(lit);       // tyre skid marks laid while wheels slip in Play
        bool roadEditMode = false;   // edit-mode flag (mutually exclusive brushes)
        int  roadSel      = -1;       // selected control point (-1 = none)
        int  roadSel2     = -1;       // shift-clicked second point (bridge far end)
        bool roadDragging = false;    // dragging the selected handle
        // Erase a control point, keeping the bridges that name points by index
        // honest: any bridge ending on it goes with it, and later points shift down.
        auto removeRoadPoint = [&](int k) {
            if (k < 0 || k >= static_cast<int>(road.roadPts.size())) return;
            road.roadPts.erase(road.roadPts.begin() + k);
            std::vector<RoadSystem::BridgeSpec> keep;
            for (RoadSystem::BridgeSpec b : road.bridges) {
                if (b.a == k || b.b == k) continue;
                if (b.a > k) --b.a;
                if (b.b > k) --b.b;
                keep.push_back(b);
            }
            road.bridges.swap(keep);
            roadSel = roadSel2 = -1;
            road.needsBuild = true;
        };
        // Insert a control point at index `at`, mirroring removeRoadPoint's bookkeeping:
        // any bridge endpoint at or after `at` shifts up by one so it keeps naming the
        // same point. Selects the new point.
        auto insertRoadPoint = [&](int at, glm::vec2 p) {
            at = glm::clamp(at, 0, static_cast<int>(road.roadPts.size()));
            road.roadPts.insert(road.roadPts.begin() + at, p);
            for (RoadSystem::BridgeSpec& b : road.bridges) {
                if (b.a >= at) ++b.a;
                if (b.b >= at) ++b.b;
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
            if (road.build(sculptWork, mn, mx)) {
                publishSculpt();
                streamer.editsChanged(mn, mx);
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
        // Play-start options (per-scene, serialized): drop straight into the car
        // when Play begins, and whether the aiming crosshair is drawn in Play.
        bool  startInVehicleMode = false;
        bool  showCrosshair      = true;
        // Scene vehicle (a model with a VehicleComponent) being driven: its
        // entity id, and -- for the editor test-drive -- the transform snapshot
        // restored when drive mode ends (a test-drive must not edit the scene).
        int                 driveVehicleId    = -1;
        bool                editorDriveActive = false;
        std::vector<Entity> driveBackup;
        glm::vec3 carPos(0.0f);
        float carYaw     = 0.0f;   // heading (radians)
        float carSpeed   = 0.0f;   // m/s (negative = reverse)
        float wheelSpin  = 0.0f;   // rolling angle (radians)
        float steerAngle = 0.0f;   // front-wheel steer (radians, arcade sim)
        float physSteer  = 0.0f;   // smoothed steer input -1..1 (Jolt car)
        // Engine-sound feed, refreshed each frame by whichever drive block runs
        // (physics or arcade); consumed in the audio mix block.
        bool  engineDriving  = false;
        float engineSpeedMps = 0.0f;
        float engineThrottle = 0.0f;
        float engineWheelR   = 0.42f;
        bool  carInWater     = false;  // chassis was submerged last frame (splash edge)
        float carWaterSub    = 0.0f;   // 0..1 chassis submersion this frame (audio/FX)
        bool  boatMode       = false;  // vehicle floats deep enough -> motorboat controls
        glm::vec3 camChase(0.0f);  // smoothed chase-camera position
        const float wheelR = 0.42f, bodyW = 1.8f, bodyH = 0.7f, bodyL = 4.0f;
        const float cabW = 1.5f, cabH = 0.6f, cabL = 1.8f;
        const float halfTrack = 0.85f, halfBase = 1.35f;
        auto placeCar = [&] {
            const glm::vec3 p = camera.position();
            carPos    = glm::vec3(p.x, streamer.heightAt(p.x, p.z), p.z);
            carYaw    = glm::radians(90.0f - camera.yaw()); // align with view heading
            carSpeed  = 0.0f;
            carPlaced = true;
            camChase  = camera.position();
        };

        // --- Scene entities: placeable objects (box / ramp / cylinder / light) ---
        Mesh rampMesh   = Mesh::create(makeRampVerts());
        Mesh cylMesh    = Mesh::create(makeCylinderYVerts());
        Mesh sphereMesh = Mesh::create(makeSphereVerts());
        // The scene document owns the authored content (entities + materials);
        // `entities`/`materials` below are just aliases so existing code reads
        // unchanged. Every content edit goes through `history` (undo/redo).
        Document     document;
        CommandStack history;
        std::vector<Entity>& entities = document.entities();
        int       entitySel      = -1;
        // Round-robin picking: the entity ids the last click's ray passed through
        // (nearest first) and which one is currently selected, so repeated clicks
        // at the same spot cycle to the next overlapping entity (a parent group's
        // bounding box no longer permanently swallows clicks meant for a child).
        std::vector<int> pickStack;
        int       pickIdx        = -1;
        int       renameId       = -1;   // hierarchy node being inline-renamed (entity id)
        bool      renameFocus    = false; // request keyboard focus on the rename field
        char      renameBuf[128] = "";
        bool      entityEditMode = true; // start ready to edit; Esc -> selection
        glm::vec3 entityNewHalf(1.0f, 1.0f, 1.0f); // default size (half-extents)
        EntityType entityNewType = EntityType::Box; // type placed on click
        int       entityCounter = 0; // for unique default names

        // Blender-style 3D cursor: a world-space reference point placed with
        // Shift+Right-click, used as a snap/placement anchor (see the "3D Cursor"
        // panel). cursorGrid is the step for the grid-snap operations.
        glm::vec3 cursor3D{0.0f};
        bool      cursorVisible = true;
        float     cursorGrid    = 1.0f;

        // Material library: named surface assets solids can be assigned. New
        // objects get the material selected in the Materials panel (matSel).
        std::vector<MaterialDef>& materials = document.materials();
        int  matSel          = 0;    // selected material in the Materials panel
        // Secondary-panel visibility (toggled from the View menu). The default
        // layout is just Hierarchy | Scene | Inspector; everything else is hidden.
        bool showMaterials   = false;
        bool showModels      = false;
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
        bool showStats       = false;
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
        bool showCamPath     = false;
        bool showRoads       = false;
        bool showCursor      = false; // 3D cursor panel
        bool showVehiclePanel = false;
        bool showEnv         = false;
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
        char              projNameBuf[64] = "";
        std::string       prefLocation = defaultProjectsRoot; // wizard default dir
        std::vector<std::string> recentProjects;              // folders, newest first
        const std::string prefsPath = "editor.json";
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
        // Scene look/settings serialization hooks. The tunable registry that
        // backs these is built later (once all the tunables exist), so saveScene/
        // loadScene call through these std::functions instead of the registry.
        std::function<void(nlohmann::json&)>       writeSettingsFn;
        std::function<void(const nlohmann::json&)> readSettingsFn;
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
        // The scene always has exactly one Sun (directional light), non-deletable.
        {
            Entity sun;
            sun.type      = EntityType::Sun;
            sun.name      = "Sun";
            sun.id        = entityCounter++;
            sun.components.items.push_back(std::make_unique<SunComponent>());
            entities.push_back(std::move(sun));
        }
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
            nb.half   = (type == EntityType::Light) ? glm::vec3(0.3f)
                      : (type == EntityType::Empty) ? glm::vec3(0.5f)
                      : entityNewHalf;
            nb.localCenter = nb.center =
                glm::vec3(groundPos.x, groundPos.y + nb.half.y, groundPos.z);
            if (type == EntityType::Light)
                nb.components.items.push_back(std::make_unique<LightComponent>());
            const bool solid = type == EntityType::Box || type == EntityType::Ramp ||
                               type == EntityType::Cylinder || type == EntityType::Sphere;
            if (solid && !materials.empty()) {
                auto mc = std::make_unique<MaterialComponent>();
                mc->material = materials[glm::clamp(matSel, 0,
                                   static_cast<int>(materials.size()) - 1)].assetId;
                nb.components.items.push_back(std::move(mc));
            }
            nb.id     = entityCounter++;
            nb.name   = std::string(entityTypeName(type)) + " " + std::to_string(nb.id);
            history.push(std::make_unique<AddEntityCmd>(nb), document);
            entitySel = document.indexOf(nb.id);
        };
        // World-space half-extents of a placed model (its local AABB * scale).
        auto modelHalf = [&](const LoadedModel& lm, float sc) {
            return 0.5f * lm.size() * sc;
        };
        // Build translate * rotate(euler deg) * scale via ImGuizmo's own compose
        // so the gizmo and the rendered transform share one Euler convention.
        auto composeModel = [](const glm::vec3& t, const glm::vec3& rotDeg,
                               const glm::vec3& s) {
            const float tt[3] = {t.x, t.y, t.z};
            const float rr[3] = {rotDeg.x, rotDeg.y, rotDeg.z};
            const float ss[3] = {s.x, s.y, s.z};
            float m[16];
            ImGuizmo::RecomposeMatrixFromComponents(tt, rr, ss, m);
            return glm::make_mat4(m);
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
            entitySel = document.indexOf(nb.id);
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
            entitySel = document.indexOf(rootId);
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
            entitySel = -1;
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
            entities, materials, matSel, entityCounter, entitySel,
            currentProject, projNameBuf, sizeof(projNameBuf), prefLocation,
            recentProjects, prefsPath, exportStatus,
            assetDb, contentRoot, modelDir,
            [&]{ seedDefaultMaterials(); },
            [&](const std::string& p){ return models.import(p, assetDb, materials); },
            [&](const std::string& p, int n){ return models.importNode(p, n, true, assetDb, materials); },
            [&](int id){ return models.byId(id); },
            [&]{ models.clear(); },
            writeSettingsFn, readSettingsFn,
        };
        projectio::loadPrefs(pio);

        auto safeName             = [&](const std::string& s){ return projectio::safeName(s); };
        auto loadProjectMaterials = [&](const std::string& d){ projectio::loadProjectMaterials(pio, d); };
        auto saveProjectTo        = [&](const std::string& f){ projectio::saveProjectTo(pio, f); };
        auto saveCurrent          = [&](){ projectio::saveCurrent(pio); };
        auto exportGame           = [&](const std::string& o){ projectio::exportGame(pio, o); };
        auto listProjectsIn       = [&](const std::string& r){ return projectio::listProjectsIn(r); };
        // Loading/creating a project replaces the document, so the undo history
        // must not survive the boundary.
        // Rescan road-surface textures to include the project being opened before
        // the scene loads (loadScene restores the saved surface by name, so the
        // project's textures must already be in the list at that point).
        auto openProjectFolder    = [&](const std::string& f){ road.refreshTextures(f); const bool ok = projectio::openProjectFolder(pio, f); history.clear(); return ok; };
        auto newProject           = [&](){ projectio::newProject(pio); history.clear(); road.refreshTextures(std::string()); };
        // Scenes within the open project. Switching/creating replaces the document,
        // so the undo history is cleared at the boundary (like opening a project).
        auto listScenesIn         = [&](const std::string& f){ return projectio::listScenesIn(f); };
        auto saveSceneFile        = [&](const std::string& p){ projectio::saveScene(pio, p); };
        auto loadSceneFile        = [&](const std::string& p){ const bool ok = projectio::loadSceneFile(pio, p); history.clear(); return ok; };
        auto newSceneInProject    = [&](const std::string& f, const std::string& n){ auto p = projectio::newSceneInProject(pio, f, n); history.clear(); return p; };
        auto renameScene          = [&](const std::string& p, const std::string& n){ return projectio::renameScene(pio, p, n); };
        auto deleteSceneFile      = [&](const std::string& p){ return projectio::deleteSceneFile(p); };

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
            return entitySel >= 0 && entitySel < static_cast<int>(entities.size());
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
            Entity& b = entities[entitySel];
            const glm::mat4 pw = parentWorldMat(b);
            setWorld(b, wPos, b.rotation, b.parent >= 0 ? &pw : nullptr);
        };
        auto snapCursorToOrigin    = [&] { cursor3D = glm::vec3(0.0f); };
        auto snapCursorToGrid      = [&] { cursor3D = snapToGrid(cursor3D); };
        auto snapCursorToTerrain   = [&] { cursor3D.y = streamer.heightAt(cursor3D.x, cursor3D.z); };
        auto snapCursorToSelection = [&] { if (cursorHaveSel()) cursor3D = entities[entitySel].center; };
        auto snapSelectionToCursor = [&] { moveSelectionTo(cursor3D); };
        auto snapSelectionToGrid   = [&] { if (cursorHaveSel()) moveSelectionTo(snapToGrid(entities[entitySel].center)); };
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
            entitySel = -1;
        };
        // Duplicate an entity (offset copy, unparented) as one undoable step.
        auto duplicateEntity = [&](int idx) {
            if (idx < 0 || idx >= static_cast<int>(entities.size())) return;
            if (entities[idx].type == EntityType::Sun) return;
            Entity nb = entities[idx];
            nb.localCenter.x += nb.half.x * 2.2f;
            nb.center.x     += nb.half.x * 2.2f;
            nb.id     = entityCounter++;
            nb.parent = -1;
            nb.name  += " copy";
            history.push(std::make_unique<AddEntityCmd>(nb), document);
            entitySel = document.indexOf(nb.id);
        };
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
                    : entityNewHalf;
            if (type == EntityType::Light)
                nb.components.items.push_back(std::make_unique<LightComponent>());
            const bool solid = type == EntityType::Box || type == EntityType::Ramp ||
                               type == EntityType::Cylinder || type == EntityType::Sphere;
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
            if (!cmd->trivial()) history.push(std::move(cmd), document);
        };
        // Context-menu helpers (index-based; capture the id first so the entities
        // vector may safely grow underneath).
        auto addEmptyChild = [&](int idx) {
            if (idx < 0 || idx >= static_cast<int>(entities.size())) return;
            const Entity& n = entities[idx];
            const int id = spawnChild(n.id, EntityType::Empty, n.center, n.rotation);
            entitySel = document.indexOf(id);
        };
        auto addPrimitiveChild = [&](int idx, EntityType type) {
            if (idx < 0 || idx >= static_cast<int>(entities.size())) return;
            const Entity& n = entities[idx];
            const int id = spawnChild(n.id, type, n.center, glm::vec3(0.0f));
            entitySel = document.indexOf(id);
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
            entitySel = document.indexOf(emptyId);
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
            entitySel = document.indexOf(vehId);
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
        const std::string soundDir = localContent ? contentRoot + "/sounds"
                                                   : std::string(FITZEL_SOUND_DIR);
        Sound rainSnd    = Sound::fromFile(audio, soundDir + "/rain.wav", true);
        Sound windSnd    = Sound::fromFile(audio, soundDir + "/wind.wav", true);
        Sound breezeSnd  = Sound::fromFile(audio, soundDir + "/breeze.wav", true);
        Sound thunderSnd = Sound::fromFile(audio, soundDir + "/thunder.wav", false);
        // Water: a one-shot splash when the car plunges in, and a loop that stays
        // audible (volume follows submersion) while it wades through.
        Sound splashSnd  = Sound::fromFile(audio, soundDir + "/splash.wav", false);
        Sound waterSnd   = Sound::fromFile(audio, soundDir + "/water.wav", true);
        // Storm bed: a heavy loop that fades in as the weather peaks.
        Sound stormSnd   = Sound::fromFile(audio, soundDir + "/storm.wav", true);
        rainSnd.setVolume(0.0f);   rainSnd.play();   // loops; volume follows weather
        windSnd.setVolume(0.0f);   windSnd.play();
        breezeSnd.setVolume(0.0f); breezeSnd.play();
        waterSnd.setVolume(0.0f);  waterSnd.play();  // loops; volume follows submersion
        stormSnd.setVolume(0.0f);  stormSnd.play();  // loops; volume follows the storm
        // Engine sound: RPM-layered loops + an automatic gearbox. Voiced only
        // while a vehicle is being driven (see the audio mix block below).
        CarAudio carAudio;
        carAudio.load(audio, soundDir);
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
        int  savedWX = 0, savedWY = 0, savedWW = 0, savedWH = 0;

        // First-person (walk on terrain) mode.
        bool        fpsMode  = false;
        bool        prevF    = false;
        bool        prevEsc  = false;
        bool        prevSpace = false;
        bool        prevQkey = false, prevWkey = false, prevEkey = false; // gizmo tools
        bool        prevXkey = false; // X: toggle gizmo local/world space
        bool        camFocusing = false;      // F: smoothly gliding to a focus point
        glm::vec3   camFocusTarget{0.0f};

        // Undo/redo edge state + gizmo-drag snapshot (a drag is one undoable step).
        bool                prevUndo = false, prevRedo = false;
        bool                gizmoActive = false;
        std::vector<int>    gizmoIds;
        std::vector<Entity> gizmoBefore;
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
            road.rebuildMesh(); // re-drape the committed road on the new terrain
        };
        applyScene(scene); // start in the selected scene (Empty by default)

        // Reset the world to the editor's default (used by New Scene): flat "Empty"
        // terrain, no texture layers, no road, no hand-painted vegetation -- so a new
        // scene starts blank instead of inheriting the terrain you were just editing.
        auto resetWorldForNewScene = [&]() {
            look.layers.clear();
            road.roadPts.clear();
            road.bridges.clear();
            road.needsBuild = false;
            roadSel = roadSel2 = -1;
            veg.paintedBlades.clear();
            veg.paintedTrees.clear();
            veg.paintedFlowers.clear();
            veg.paintedDirty = true;
            veg.grassDirty   = true;
            applyScene(1); // flat default terrain + rebuild + re-drape the (now empty) road
        };

        // --- Scene settings registry --------------------------------------
        // Every tunable is bound by name to a getter/setter and serialised as
        // part of the project scene (.fitzel "settings" object). Missing keys are
        // ignored, so scenes keep loading as fields come and go.
        struct Setting {
            std::string                                key;
            std::function<void(nlohmann::json&)>       write;
            std::function<void(const nlohmann::json&)> read;
        };
        std::vector<Setting> tunables;
        auto addF = [&](const char* k, float& r) {
            tunables.push_back({k,
                [k, &r](nlohmann::json& j){ j[k] = r; },
                [k, &r](const nlohmann::json& j){ r = j.value(k, r); }});
        };
        auto addB = [&](const char* k, bool& r) {
            tunables.push_back({k,
                [k, &r](nlohmann::json& j){ j[k] = r; },
                [k, &r](const nlohmann::json& j){ r = j.value(k, r); }});
        };
        auto addI = [&](const char* k, int& r) {
            tunables.push_back({k,
                [k, &r](nlohmann::json& j){ j[k] = r; },
                [k, &r](const nlohmann::json& j){ r = j.value(k, r); }});
        };
        addF("moveSpeed", camera.moveSpeed);   addI("viewRadius", viewRadius);
        addB("autoWeather", autoWeather);      addF("weather", weather);
        addB("muted", muted);                  addF("volume", masterVolume);
        addB("startInVehicleMode", startInVehicleMode);
        addB("showCrosshair", showCrosshair);
        addB("skidMarks", skids.enabled);      addF("skidSlip", skids.slipThresh);
        addF("skidWidth", skids.markHalfW);    addF("skidDark", skids.opacity);
        addF("mixAmbient", mixAmbient.level);   addB("mixAmbientMute", mixAmbient.mute);
        addF("mixSfx", mixSfx.level);           addB("mixSfxMute", mixSfx.mute);
        addF("timeOfDay", timeOfDay);          addF("dayLength", dayLength);
        addF("coverage", cloudCoverage);       addF("cloudDensity", cloudDensity);
        addF("cloudScale", cloudScale);        addF("cloudWind", cloudSpeed);
        addF("fogDensity", fogDensity);        addF("fogFalloff", fogFalloff);
        addF("exposure", exposure);            addF("bloom", bloomIntensity);
        addF("rays", rayIntensity);            addF("ssao", ssaoStrength);
        addF("ssaoRadius", ssaoRadius);        addF("cascadeSplit", renderer.shadows().splitLambda);
        addF("hue", hueShift);                 addF("saturation", saturation);
        addF("value", valueGain);              addF("warmth", warmth);
        addF("contrast", contrast);
        addF("waterLevel", waterLevel);        addF("waveHeight", waveHeight);
        addF("waveChoppy", waveChoppy);        addF("waveStrength", waveStrength);
        addF("waveScale", waveScale);          addF("foamWidth", foamWidth);
        addF("waterColorR", waterColor.x);     addF("waterColorG", waterColor.y);
        addF("waterColorB", waterColor.z);
        addF("waterReflectivity", waterReflectivity); addF("waterClarity", waterClarity);
        addF("waterIor", waterIor);
        addF("cursorX", cursor3D.x); addF("cursorY", cursor3D.y); addF("cursorZ", cursor3D.z);
        addF("cursorGrid", cursorGrid);
        addF("terrHeight", uiSettings.heightScale);   addF("terrRidge", uiSettings.ridgeScale);
        addF("terrContinent", uiSettings.continentAmp); addF("terrBiome", uiSettings.biomeFreq);
        addF("terrTerrace", uiSettings.terrace);      addF("terrWarp", uiSettings.warpStrength);
        addF("terrFreq", uiSettings.frequency);       addI("terrOctaves", uiSettings.octaves);
        addF("terrSeed", uiSettings.seed);
        addF("terrValley", uiSettings.valleyDepth);   addF("terrPeak", uiSettings.peakSharpness);
        addF("terrRelief", uiSettings.reliefGain);
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
                const int ix = static_cast<int>(k >> 32);
                const int iz = static_cast<int>(
                    static_cast<std::int32_t>(static_cast<std::uint32_t>(k)));
                es << ix << ' ' << iz << ' ' << d << ' ';
            }
            j["terrainEdits"] = es.str();

            // Terrain texture paint: grid spacing + an "ix iz r g b a ..." blob of
            // every painted cell's four layer weights (same compact-string scheme).
            j["terrainPaintCell"] = paintWork.cell;
            std::ostringstream ps;
            ps.precision(5);
            for (const auto& [k, w] : paintWork.weights) {
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
                        {"reflectivity", md.reflectivity},
                        {"roughness", md.roughness},
                        {"opacity", md.opacity},
                        {"glass", md.glass},
                        {"alphaMode", static_cast<int>(md.alphaMode)},
                        {"alphaCutoff", md.alphaCutoff},
                        {"emission", {md.emission.x, md.emission.y, md.emission.z}},
                        {"emissionStrength", md.emissionStrength},
                    };
                }
            }
            j["modelMaterialOverrides"] = std::move(ov);

            // The road owns its own scene state (the graded terrain corridor rides
            // along in "terrainEdits" above; the mesh is re-lofted on load).
            road.save(j["road"]);

            // Editor fly-camera pose, so reopening a project returns to the exact
            // view it was saved from (position + look direction).
            const glm::vec3 camP = camera.position();
            j["editorCamera"] = {
                {"x", camP.x}, {"y", camP.y}, {"z", camP.z},
                {"yaw", camera.yaw()}, {"pitch", camera.pitch()},
            };
        };
        readSettingsFn = [&](const nlohmann::json& j){
            for (const Setting& s : tunables) s.read(j);
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

            // Road: empty for scenes saved before roads were persisted (the old
            // road.txt Save/Load in the panel still works for those).
            if (j.contains("road") && j["road"].is_object()) {
                road.load(j["road"]);
                roadSel = roadSel2 = -1;
            }
            // The graded corridor is already baked into the restored terrain
            // edits above, so just re-loft the committed mesh on that ground.
            road.rebuildMesh();

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
                        md.reflectivity  = e.value("reflectivity", md.reflectivity);
                        md.roughness     = e.value("roughness", md.roughness);
                        md.opacity       = e.value("opacity", md.opacity);
                        md.glass         = e.value("glass", md.glass);
                        md.alphaMode     = static_cast<AlphaMode>(
                            e.value("alphaMode", static_cast<int>(md.alphaMode)));
                        md.alphaCutoff   = e.value("alphaCutoff", md.alphaCutoff);
                        md.emission      = rd3(e.value("emission", nlohmann::json{}), md.emission);
                        md.emissionStrength = e.value("emissionStrength", md.emissionStrength);
                    }
                }
            }
        };

        // --- Play mode: run the scene as a game -------------------------------
        // Play snapshots the editable scene state and drops the player into
        // first-person walk mode; Stop restores the snapshot and the edit camera
        // exactly, so play-time changes never leak into the edited scene.
        bool playMode = false;
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
        // Code-completion popup state for the Lua editor. `compItems` are the
        // matches for the identifier currently under the cursor; the popup shows
        // while non-empty and the editor is focused (Tab/Enter accepts, arrows
        // navigate, Esc dismisses -- see the editor window below).
        std::vector<Completion> compItems;
        std::string             compPrefix;   // the partial word being completed
        int                     compSel  = 0; // highlighted match
        bool                    compOpen = false;
        bool                    compGameMember = false; // completing after "game."
        bool                    compManualClose = false; // Esc: stay closed until
        std::string             compClosedPrefix;        // the prefix changes
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
        // Write the editor buffer back and reload the VM so Play picks it up.
        auto saveEditor = [&](){
            if (editorPath.empty()) return;
            std::ofstream out(editorPath);
            if (out) { out << luaEditor.GetText(); scripts.reset(); editorDirty = false; }
        };
        // Refresh the code-completion candidates from the identifier under the
        // cursor. Fills compPrefix/compItems/compGameMember and toggles compOpen.
        // Called each frame after the editor renders (so it sees the latest edit).
        auto recomputeCompletion = [&](){
            compItems.clear();
            const auto cur = luaEditor.GetCursorPosition();
            const std::string line = luaEditor.GetCurrentLineText();
            const int tab = luaEditor.GetTabSize();
            // Map the tab-expanded cursor column back to a byte index in the line.
            int idx = 0, col = 0;
            while (idx < static_cast<int>(line.size()) && col < cur.mColumn) {
                col += (line[idx] == '\t') ? (tab - (col % tab)) : 1;
                ++idx;
            }
            auto isIdent = [](char c){
                return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
            int start = idx;
            while (start > 0 && isIdent(line[start - 1])) --start;
            compPrefix = line.substr(start, idx - start);
            // "game." member context: a '.' right before the word, and the token
            // before the dot is exactly "game".
            compGameMember = false;
            if (start > 0 && line[start - 1] == '.') {
                int ws = start - 1;
                while (ws > 0 && isIdent(line[ws - 1])) --ws;
                compGameMember = (line.substr(ws, (start - 1) - ws) == "game");
            }
            if (compPrefix.empty() && !compGameMember) {
                compOpen = false; compManualClose = false; return;
            }
            // Esc keeps the popup closed until the prefix actually changes.
            if (compManualClose) {
                if (compPrefix == compClosedPrefix) { compOpen = false; return; }
                compManualClose = false;
            }
            auto lower = [](std::string s){
                for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return s; };
            const std::string pfx = lower(compPrefix);
            auto consider = [&](const Completion* arr, std::size_t count){
                for (std::size_t i = 0; i < count; ++i)
                    if (lower(arr[i].text).rfind(pfx, 0) == 0) compItems.push_back(arr[i]);
            };
            if (compGameMember)
                consider(kGameMembers, sizeof(kGameMembers) / sizeof(kGameMembers[0]));
            else
                consider(kTopLevel, sizeof(kTopLevel) / sizeof(kTopLevel[0]));
            // Nothing useful to offer (no match, or the sole match is already typed).
            if (compItems.empty() ||
                (compItems.size() == 1 && lower(compItems[0].text) == pfx)) {
                compOpen = false; return;
            }
            if (compSel >= static_cast<int>(compItems.size())) compSel = 0;
            compOpen = true;
        };
#endif // !FITZEL_PLAYER
        // Sound assets known to the asset database (engine + project), by bare
        // filename -- what game.playSound / CollectibleComponent resolve against.
        // Backs the Collectible sound picker so it's chosen, not typed.
        auto listSounds = [&](){
            std::vector<std::string> out;
            for (const AssetId& id : assetDb.allAssets())
                if (assetDb.typeForId(id) == AssetType::Sound)
                    if (const auto* e = assetDb.entry(id))
                        out.push_back(e->absPath.filename().string());
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
            return out;
        };
        // Inspector combo that assigns a Sound asset (by filename) to a string
        // field. Shared by the Collectible and Trigger sound pickers.
        auto soundPickerCombo = [&](const char* label, std::string& field) {
            const std::vector<std::string> sounds = listSounds();
            const std::string cur = field.empty() ? "(none)" : field;
            if (ImGui::BeginCombo(label, cur.c_str())) {
                if (ImGui::Selectable("(none)", field.empty())) field.clear();
                for (const std::string& s : sounds)
                    if (ImGui::Selectable(s.c_str(), field == s)) field = s;
                ImGui::EndCombo();
            }
            if (!field.empty() &&
                std::find(sounds.begin(), sounds.end(), field) == sounds.end())
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f),
                                   "Missing sound: %s", field.c_str());
        };
        std::vector<Entity>      playEntities;
        std::vector<MaterialDef> playMaterials;
        std::unique_ptr<PhysicsWorld> physics;      // rigid-body world during Play
        std::map<int, PhysicsBodyId>  physicsBody;  // entity id -> body handle

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
        // rides higher than the model so the wheels (which hang 0.3-0.5 m of
        // suspension below the box bottom) land where they were modelled.
        auto vehicleVisualY = [](const VehicleComponent& vc) {
            return -vc.chassisHalf.y - 0.4f - vc.wheelY;
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
            camChase = camera.position();
        };

        // --- Lua `game` API bridge -------------------------------------------
        // Scripts mutate the entity list only through deferred queues (the tick
        // loop iterates entities), applied once per frame after scripts run.
        ScriptHost                       host;
        std::vector<Entity>              pendingSpawns;
        std::unordered_map<int, glm::vec3> pendingSpawnVel; // spawn id -> velocity
        std::vector<int>                 pendingDestroy;
        std::unordered_map<int, unsigned char> keyPrev, mousePrev; // edge state
        std::vector<int>                 keyQ, mouseQ;              // queried this frame
        host.keyDown      = [&](int kc){ return input.isKeyDown(kc); };
        host.keyPressed   = [&](int kc){ keyQ.push_back(kc);
                                         return input.isKeyDown(kc) && !keyPrev[kc]; };
        host.mouseDown    = [&](int b){ return input.isMouseButtonDown(b); };
        host.mousePressed = [&](int b){ mouseQ.push_back(b);
                                        return input.isMouseButtonDown(b) && !mousePrev[b]; };
        host.spawn = [&](const ScriptSpawn& s) -> int {
            Entity e;
            e.type     = static_cast<EntityType>(s.type);
            e.localCenter   = e.center   = s.pos;
            e.half     = glm::max(s.half, glm::vec3(0.02f));
            e.localRotation = e.rotation = s.rot;
            e.name     = s.name.empty() ? "spawned" : s.name;
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
                std::error_code ec;
                if (std::filesystem::exists(projSnd, ec)) return projSnd;
            }
            return soundDir + "/" + n;
        };
        host.playSound = [&](const std::string& n){ audio.playOneShot(resolveSoundPath(n)); };
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
        scripts.setHost(&host);

        glm::vec3 playCamPos{0.0f};
        float     playCamYaw = 0.0f, playCamPitch = 0.0f, playMoveSpeed = 20.0f;
        float     playCamFov = 60.0f;
        bool      playPrevEdit = false;
        int       activeCam = -1; // entity id of the active Camera in Play (-1 = player)

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
            entitySel      = -1;
            vehicleMode    = false;
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
            resolveHierarchy(); // world transforms fresh before bodies are created

            // Physics: fresh world with the terrain as a static heightfield
            // ground, plus a rigid body per physics-tagged entity.
            physics = std::make_unique<PhysicsWorld>();
            physics->setGravity(glm::vec3(0.0f, -9.81f, 0.0f));
            // Fresh world: the previous collider id is void. Build the terrain
            // heightfield around the start position; it follows the focus below.
            terrainCollId = 0;
            refitTerrainCollision(glm::vec2(camera.position().x, camera.position().z));
            // Roads: a static triangle-mesh collider (from the last Build, graded
            // into the terrain), so the player and objects can walk/drive on them.
            if (road.enabled && road.collIndices().size() >= 3)
                physics->addMesh(road.collVerts().data(),
                                 static_cast<int>(road.collVerts().size()),
                                 road.collIndices().data(),
                                 static_cast<int>(road.collIndices().size()));
            skids.clear(); // no skid marks carry over from a previous Play session
            physicsBody.clear();
            for (Entity& e : entities) {
                const auto* pc = e.components.get<PhysicsComponent>();
                if (!pc || !e.activeInHierarchy ||
                    e.type == EntityType::Light || e.type == EntityType::Sun)
                    continue;
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

            // Optionally start behind the wheel instead of the walking player.
            // enterVehicleMode spawns/drives the nearest scene vehicle (or a
            // fallback car) and takes over from the first-person setup above.
            if (startInVehicleMode) {
                vehicleMode = true;
                enterVehicleMode();
            }
        };
        auto stopPlay = [&] {
            if (!playMode) return;
            playMode  = false;
            vehicleMode = false;    // the physics car is gone with the world
            driveVehicleId = -1;    // the scene restore below un-drives the model
            skids.clear();          // drop skid marks so they don't linger in the editor
            terrainCollId = 0;      // the collider dies with the world below
            physics.reset();
            physicsBody.clear();
            zoneSounds.clear(); // stop + free any looping TriggerSound voices
            audioVoices.clear(); // stop + free any AudioSource voices
            entities  = std::move(playEntities);
            materials = std::move(playMaterials);
            fpsMode   = false;
            input.setCursorLocked(false);
            camera.setPosition(playCamPos);
            camera.setYaw(playCamYaw);
            camera.setPitch(playCamPitch);
            camera.moveSpeed = playMoveSpeed;
            camera.setFov(playCamFov);
            entityEditMode = playPrevEdit;
            entitySel = -1;
        };

        showProgress(0.95f, "Generating world...");
        streamer.update(camera.position()); // kick off the initial terrain ring
        showProgress(1.0f, "Ready");

        // Player build: load the game project, hide the editor, go fullscreen and
        // start playing immediately. Esc quits (handled in the input loop).
        if (playerMode) {
            if (openProjectFolder(bootProject)) {
                if (bootFullscreen) {
                    GLFWwindow* w = window.nativeHandle();
                    glfwGetWindowPos(w, &savedWX, &savedWY);
                    glfwGetWindowSize(w, &savedWW, &savedWH);
                    GLFWmonitor* mon = glfwGetPrimaryMonitor();
                    const GLFWvidmode* vm = glfwGetVideoMode(mon);
                    glfwSetWindowMonitor(w, mon, 0, 0, vm->width, vm->height,
                                         vm->refreshRate);
                }
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
        double       lastActive  = window.time();
        double       frameStart  = window.time();
        const double kIdleGrace  = 0.4;        // s of full-rate after last input
        const double kIdleFrame  = 1.0 / 10.0; // idle cap period
        const double kActiveFrame = 1.0 / 25.0; // active (editing) cap period

        while (window.isOpen()) {
            const bool uncapped = playMode || playerMode;
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
            input.update();

            const double now = window.time();
            const float  dt  = static_cast<float>(now - lastTime);
            lastTime = now;

            // Hot reload: pick up on-disk asset edits ~twice a second. Textures
            // and models reload in place (existing handles update automatically);
            // edited/added/removed materials refresh the project's library.
            if (now >= nextAssetPoll) {
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

            // --- Input ---------------------------------------------------
            // F frames the selected object; Shift+F toggles first-person walk mode.
            const bool fDown  = input.isKeyDown(GLFW_KEY_F);
            const bool shiftF = input.isKeyDown(GLFW_KEY_LEFT_SHIFT) ||
                                input.isKeyDown(GLFW_KEY_RIGHT_SHIFT);
            if (fDown && !prevF && !vehicleMode && !ImGui::GetIO().WantTextInput) {
                if (shiftF) { // Shift+F: toggle first-person (cursor locks, mouse-look)
                    fpsMode = !fpsMode;
                    input.setCursorLocked(fpsMode);
                    fpsVelY = 0.0f;
                    if (fpsMode) { // drop to standing height immediately
                        const glm::vec3 p = camera.position();
                        camera.setPosition({p.x, streamer.heightAt(p.x, p.z) + eyeHeight, p.z});
                    }
                } else if (!fpsMode && entitySel >= 0 &&
                           entitySel < static_cast<int>(entities.size())) {
                    // Focus: keep the view direction, back off to fit the object,
                    // and glide there smoothly (applied each frame below).
                    const Entity& e = entities[entitySel];
                    const float radius = glm::max(glm::length(e.half), 0.25f);
                    const float fov    = glm::radians(glm::max(camera.fov(), 1.0f));
                    const float dist   = radius / std::max(std::tan(fov * 0.5f), 0.05f) * 1.3f;
                    camFocusTarget = e.center - camera.front() * dist;
                    camFocusing    = true;
                }
            }
            prevF = fDown;

            // V toggles the drive-a-vehicle mode. A scene vehicle (a model with
            // a Vehicle component, nearest to the camera) takes precedence: in
            // Play it spawns the Jolt car from the component at the model, in
            // the editor the arcade sim test-drives the model itself. With no
            // scene vehicle, the primitive test car behaves as before.
            const bool vDown = input.isKeyDown(GLFW_KEY_V);
            if (vDown && !prevV) {
                vehicleMode = !vehicleMode;
                if (vehicleMode) enterVehicleMode();
                else             endEditorDrive();
            }
            prevV = vDown;

            // F11 toggles borderless-fullscreen presentation (UI hidden).
            const bool f11 = input.isKeyDown(GLFW_KEY_F11);
            if (f11 && !prevF11) {
                presentMode = !presentMode;
                GLFWwindow* w = window.nativeHandle();
                if (presentMode) {
                    glfwGetWindowPos(w, &savedWX, &savedWY);
                    glfwGetWindowSize(w, &savedWW, &savedWH);
                    GLFWmonitor* m = glfwGetPrimaryMonitor();
                    const GLFWvidmode* vm = glfwGetVideoMode(m);
                    glfwSetWindowMonitor(w, m, 0, 0, vm->width, vm->height, vm->refreshRate);
                } else {
                    glfwSetWindowMonitor(w, nullptr, savedWX, savedWY, savedWW, savedWH, 0);
                }
            }
            prevF11 = f11;

            const bool escDown = input.isKeyDown(GLFW_KEY_ESCAPE);
            if (escDown && !prevEsc) {
                if (playerMode)          { window.requestClose(); }
                else if (presentMode) {
                    presentMode = false;
                    glfwSetWindowMonitor(window.nativeHandle(), nullptr,
                                         savedWX, savedWY, savedWW, savedWH, 0);
                } else if (playMode)     { stopPlay(); }
                else if (vehicleMode)    { vehicleMode = false; endEditorDrive(); }
                else if (fpsMode) { fpsMode = false; input.setCursorLocked(false); }
                // Plain editor: Esc steps back to selection (drop the transform
                // tool), then a second Esc clears the selection. Never quits.
                else if (entityEditMode) { entityEditMode = false; }
                else if (entitySel >= 0) { entitySel = -1; }
            }
            prevEsc = escDown;

            // Transform-tool shortcuts (Blender/Unity-style): Q/W/E pick the gizmo
            // and (re)enter Edit mode. Only in the plain editor, never while a
            // camera-fly drag (right mouse) or a text field owns the keys.
            if (!playMode && !fpsMode && !vehicleMode && !presentMode &&
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
                if (wantUndo && !prevUndo) { history.undo(document); entitySel = -1; }
                if (wantRedo && !prevRedo) { history.redo(document); entitySel = -1; }
                prevUndo = wantUndo;
                prevRedo = wantRedo;
            } else {
                prevUndo = prevRedo = false;
            }

            engineDriving = false; // re-armed by whichever drive block runs below
            carWaterSub   = 0.0f;  // re-armed by the buoyancy block when submerged

            if (vehicleMode && playMode && physics && physics->hasVehicle()) {
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
                glm::vec3 vel(0.0f);
                physics->getLinearVelocity(physCarId, vel);
                const float halfH = svc ? glm::max(svc->chassisHalf.y, 0.1f) : 0.5f;
                const float mass  = svc ? glm::max(svc->mass, 1.0f)         : 1200.0f;
                const float depth = waterLevel - (cp.y - halfH);
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
                            p.pos = cp + off; p.pos.y = waterLevel + 0.03f;
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
                            p.pos.y = waterLevel + 0.05f;
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

                // Follow-cam tuning from the driven vehicle's component (built-in
                // default car has none -> the fallback values).
                {
                    Entity* dv = (driveVehicleId >= 0) ? document.find(driveVehicleId)
                                                       : nullptr;
                    auto*   dvc = dv ? dv->components.get<VehicleComponent>() : nullptr;
                    const float camDist  = dvc ? dvc->camDistance   : 7.0f;
                    const float camH     = dvc ? dvc->camHeight     : 3.2f;
                    const float camSide  = dvc ? dvc->camSide       : 0.0f;
                    const float camLook  = dvc ? dvc->camLookHeight : 1.2f;
                    const float camStiff = dvc ? dvc->camStiffness  : 4.0f;
                    const glm::vec3 target = cp + glm::vec3(0.0f, camLook, 0.0f);
                    const glm::vec3 wanted = cp - fwd * camDist + right * camSide +
                                             glm::vec3(0.0f, camH, 0.0f);
                    camChase += (wanted - camChase) * std::min(1.0f, dt * camStiff);
                    camera.setPosition(camChase);
                    const glm::vec3 dcam = glm::normalize(target - camChase);
                    camera.setYaw(glm::degrees(std::atan2(dcam.z, dcam.x)));
                    camera.setPitch(glm::degrees(std::asin(glm::clamp(dcam.y, -1.0f, 1.0f))));
                }
            } else if (vehicleMode) {
                // Arcade car: throttle + steering, drag, bicycle-model heading.
                // When a scene vehicle is being test-driven, its component
                // supplies the geometry and the sim glues the model along.
                Entity* dv  = (driveVehicleId >= 0) ? document.find(driveVehicleId)
                                                    : nullptr;
                auto*   dvc = dv ? dv->components.get<VehicleComponent>() : nullptr;
                const float wb = dvc ? glm::max(dvc->frontZ - dvc->rearZ, 0.5f) : 2.7f;
                const float wr = dvc ? glm::max(dvc->wheelRadius, 0.05f) : wheelR;

                const bool kW = input.isKeyDown(GLFW_KEY_W);
                const bool kS = input.isKeyDown(GLFW_KEY_S);
                const bool kA = input.isKeyDown(GLFW_KEY_A);
                const bool kD = input.isKeyDown(GLFW_KEY_D);
                bool  kBrake   = input.isKeyDown(GLFW_KEY_SPACE);
                float throttle = (kW ? 1.0f : 0.0f) - (kS ? 1.0f : 0.0f);
                float steerIn  = (kA ? 1.0f : 0.0f) - (kD ? 1.0f : 0.0f);
                // Gamepad: RT accelerate / LT reverse; left stick steers (note this
                // model's steerIn is left-positive, so subtract the stick); B brakes.
                if (input.hasGamepad()) {
                    throttle = glm::clamp(throttle
                        + input.gamepadTrigger(GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER)
                        - input.gamepadTrigger(GLFW_GAMEPAD_AXIS_LEFT_TRIGGER), -1.0f, 1.0f);
                    steerIn = glm::clamp(
                        steerIn - input.gamepadStick(GLFW_GAMEPAD_AXIS_LEFT_X), -1.0f, 1.0f);
                    if (input.gamepadButton(GLFW_GAMEPAD_BUTTON_B)) kBrake = true;
                }

                const float maxSteer =
                    glm::radians(dvc ? dvc->maxSteerDeg : 32.0f);
                const float steerSpd = dvc ? dvc->steerSpeed : 7.0f;
                steerAngle += (steerIn * maxSteer - steerAngle) * std::min(1.0f, dt * steerSpd);

                carSpeed += throttle * 14.0f * dt;                    // accelerate
                if (kBrake) carSpeed -= glm::sign(carSpeed) * 26.0f * dt;
                carSpeed *= (1.0f - 0.6f * dt);                       // drag
                if (throttle == 0.0f && !kBrake) carSpeed *= (1.0f - 1.2f * dt);
                carSpeed = glm::clamp(carSpeed, -8.0f, 26.0f);
                if (std::abs(carSpeed) < 0.02f) carSpeed = 0.0f;

                // Feed the engine sound from the arcade sim's speed/throttle.
                engineDriving  = true;
                engineSpeedMps = std::abs(carSpeed);
                engineThrottle = std::abs(throttle);
                engineWheelR   = wr;

                carYaw += (carSpeed / wb) * std::tan(steerAngle) * dt;
                const glm::vec3 fwd(std::sin(carYaw), 0.0f, std::cos(carYaw));
                carPos   += fwd * carSpeed * dt;
                carPos.y  = streamer.heightAt(carPos.x, carPos.z);
                wheelSpin += (carSpeed / wr) * dt;

                // Glue the driven model onto the sim: the root follows the
                // heading at its rest ride height, wheel children spin/steer
                // (restored from the snapshot when drive mode ends).
                if (dv && dvc) {
                    const float restY  = wr - dvc->wheelY; // ground -> body centre
                    const float yawDeg = glm::degrees(carYaw) -
                                         (dvc->forward == 1 ? 180.0f : 0.0f);
                    const glm::mat4 pw = parentWorldMat(*dv);
                    setWorld(*dv, carPos + glm::vec3(0.0f, restY, 0.0f),
                             glm::vec3(0.0f, yawDeg, 0.0f),
                             dv->parent >= 0 ? &pw : nullptr);
                    const float spinSign = (dvc->forward == 1) ? -1.0f : 1.0f;
                    auto restOf = [&](int id) -> const Entity* {
                        for (const Entity& b : driveBackup)
                            if (b.id == id) return &b;
                        return nullptr;
                    };
                    for (int i = 0; i < 4; ++i) {
                        Entity*       w    = document.find(dvc->wheelId[i]);
                        const Entity* rest = restOf(dvc->wheelId[i]);
                        if (!w || !rest) continue;
                        glm::vec3 rot = rest->localRotation;
                        rot.x += glm::degrees(wheelSpin) * spinSign;
                        if (i < 2) rot.y += glm::degrees(steerAngle); // fronts steer
                        w->localRotation = rot;
                    }
                }

                // Chase camera: behind and above, smoothly following, looking
                // ahead; distance/height/stiffness come from the vehicle component.
                const float camDist  = dvc ? dvc->camDistance   : 7.0f;
                const float camH     = dvc ? dvc->camHeight     : 3.2f;
                const float camSide  = dvc ? dvc->camSide       : 0.0f;
                const float camLook  = dvc ? dvc->camLookHeight : 1.2f;
                const float camStiff = dvc ? dvc->camStiffness  : 4.0f;
                const glm::vec3 right = glm::normalize(glm::cross(glm::vec3(0, 1, 0), fwd));
                const glm::vec3 target = carPos + glm::vec3(0.0f, camLook, 0.0f);
                const glm::vec3 wanted = carPos - fwd * camDist + right * camSide +
                                         glm::vec3(0.0f, camH, 0.0f);
                camChase += (wanted - camChase) * std::min(1.0f, dt * camStiff);
                camera.setPosition(camChase);
                const glm::vec3 d = glm::normalize(target - camChase);
                camera.setYaw(glm::degrees(std::atan2(d.z, d.x)));
                camera.setPitch(glm::degrees(std::asin(glm::clamp(d.y, -1.0f, 1.0f))));
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
            }

            // Focus (F): glide the camera to the target, cancelled by any manual
            // camera input (right-mouse fly) or leaving the free camera.
            if (camFocusing) {
                if (fpsMode || vehicleMode || playMode ||
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

            // --- Camera path: record samples or drive playback ----------
            camPathRec.update(camera, dt, !vehicleMode);

            // View distance: drive the streaming radius and the camera far plane.
            streamer.setRadius(viewRadius);
            camera.setFarPlane(
                std::max(250.0f, viewRadius * streamer.settings().chunkSize * 1.7f));

            // Stream terrain chunks around the camera.
            streamer.update(camera.position());

            // When the road settles (not mid-drag), regrow vegetation so it
            // clears off the new road; debounced to avoid thrashing while editing.
            if (road.vegDirty && !roadDragging) {
                road.vegDirty = false;
                veg.grassDirty   = true;
                veg.treeCenter   = glm::vec2(1e9f);
            }

            // Regrow grass (async) / trees when the camera has moved far enough.
            {
                const glm::vec2 camXZ(camera.position().x, camera.position().z);
                if (veg.updateGrass(camXZ, road.centerline(), road.width * 0.5f + 1.5f,
                                    waterLevel, look.snowLevel) && veg.flowerEnabled)
                    veg.regenFlowers(veg.grassCenter(), road.centerline(), road.width,
                                     waterLevel, look.snowLevel);
                veg.updateFlowers(); // finish + upload a pending async flower regen
                veg.updateTrees(camXZ, road.centerline(), road.width, waterLevel, look.snowLevel);
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
            const float ringAmount =
                rainIntensity * glm::min(roadWetness * 2.0f, 1.0f) * road.rainRings;

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

                // Player-proximity behaviours (Collectible, Trigger). A mid-body
                // reference point keeps low objects reachable.
                {
                    glm::vec3 playerC = camera.position();
                    playerC.y -= eyeHeight * 0.5f;
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
                        // TriggerSound: a looping ambient zone (volume fades with
                        // distance) or a one-shot on entry. The looping voice lives
                        // in zoneSounds, created lazily and stopped when out of range.
                        if (auto* ts = e.components.get<TriggerSoundComponent>()) {
                            const float dist   = glm::distance(playerC, e.center);
                            const bool  inside = dist <= ts->radius;
                            if (ts->loop) {
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

                // Spawner: emit a dynamic solid above itself every `interval`, up
                // to `maxCount`, through the same deferred spawn queue as scripts.
                for (Entity& e : entities) {
                    auto* sw = e.components.get<SpawnerComponent>();
                    if (!sw || sw->spawned >= static_cast<int>(sw->maxCount)) continue;
                    sw->timer += dt;
                    if (sw->timer < glm::max(sw->interval, 0.05f)) continue;
                    sw->timer = 0.0f;
                    ScriptSpawn s;
                    s.type    = sw->spawnType;
                    s.pos     = e.center + glm::vec3(0.0f, e.half.y + 0.4f, 0.0f);
                    s.half    = glm::vec3(0.3f);
                    s.physics = 2; // dynamic
                    // Launch direction: random within a cone of half-angle `spread`
                    // (deg) around +Y. Sampling cos(theta) uniformly over the cap
                    // gives an even spread; spread 0 -> straight up, 180 -> any dir.
                    {
                        const float spreadRad =
                            glm::radians(glm::clamp(sw->spread, 0.0f, 180.0f));
                        const float ct = glm::mix(std::cos(spreadRad), 1.0f, spawnU(spawnRng));
                        const float st = std::sqrt(glm::max(0.0f, 1.0f - ct * ct));
                        const float ph = 6.2831853f * spawnU(spawnRng);
                        const glm::vec3 dir(st * std::cos(ph), ct, st * std::sin(ph));
                        s.vel = dir * sw->speed;
                    }
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

                // Active camera: render the view from the chosen Camera entity
                // (overriding the player camera). Done after control so this frame
                // renders from it; player motion becomes relative to this view.
                const Entity* ce = activeCam >= 0 ? document.find(activeCam) : nullptr;
                const CameraComponent* cc = ce ? ce->components.get<CameraComponent>()
                                               : nullptr;
                if (cc) {
                    const glm::vec3 fwd = glm::normalize(
                        glm::quat(glm::radians(ce->rotation)) * glm::vec3(0, 0, -1));
                    camera.setPosition(ce->center);
                    camera.setYaw(glm::degrees(std::atan2(fwd.z, fwd.x)));
                    camera.setPitch(glm::degrees(std::asin(glm::clamp(fwd.y, -1.0f, 1.0f))));
                    camera.setFov(cc->fov);
                } else {
                    activeCam = -1;                 // target vanished -> player view
                    camera.setFov(playCamFov);      // restore the player FOV
                }
            }

            // --- UI ------------------------------------------------------
            gui.beginFrame();
            ImGuizmo::BeginFrame();
            if (presentMode) {
                // Presentation: hide the editor UI, render the scene full-window.
                window.framebufferSize(viewW, viewH);
                viewportHovered = true;
            }
#ifndef FITZEL_PLAYER
            else {
            // --- Main menu bar (File / Edit / View) ----------------------
            if (ImGui::BeginMainMenuBar()) {
                if (ImGui::BeginMenu("File")) {
                    if (ImGui::MenuItem("New Project...")) {
                        wizardIsNew = true;
                        wizName[0] = '\0';
                        std::snprintf(wizLocation, sizeof(wizLocation), "%s",
                                      prefLocation.c_str());
                        wizardOpen = true;
                    }
                    if (ImGui::MenuItem("Save Project", nullptr, false,
                                        !currentProject.empty()))
                        saveCurrent();
                    if (ImGui::MenuItem("Save Project As...")) {
                        wizardIsNew = false;
                        std::snprintf(wizName, sizeof(wizName), "%s", projNameBuf);
                        std::snprintf(wizLocation, sizeof(wizLocation), "%s",
                                      prefLocation.c_str());
                        wizardOpen = true;
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Export Game...", nullptr, false,
                                        !currentProject.empty())) {
                        std::string picked;
                        if (ed::pickFolder(picked, prefLocation)) exportGame(picked);
                    }
                    if (!exportStatus.empty())
                        ImGui::TextDisabled("%s", exportStatus.c_str());
                    ImGui::Separator();
                    if (ImGui::BeginMenu("Open Project")) {
                        if (ImGui::MenuItem("Browse folder...")) {
                            std::string picked;
                            if (ed::pickFolder(picked, prefLocation) &&
                                !openProjectFolder(picked))
                                std::fprintf(stderr,
                                    "No project (.fitzel) in %s\n", picked.c_str());
                        }
                        if (!recentProjects.empty()) {
                            ImGui::SeparatorText("Recent");
                            for (const std::string& folder : recentProjects) {
                                const std::string lbl =
                                    std::filesystem::path(folder).filename().string() +
                                    "##r" + folder;
                                if (ImGui::MenuItem(lbl.c_str()))
                                    openProjectFolder(folder);
                            }
                        }
                        ImGui::SeparatorText("In default location");
                        const auto projs = listProjectsIn(prefLocation);
                        if (projs.empty()) ImGui::TextDisabled("(none)");
                        for (const auto& [n, folder] : projs)
                            if (ImGui::MenuItem((n + "##d" + folder).c_str()))
                                openProjectFolder(folder);
                        ImGui::EndMenu();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Exit")) window.requestClose();
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Scene")) {
                    if (currentProject.empty()) {
                        ImGui::TextDisabled("Open or create a project first.");
                    } else {
                        const std::string projFolder =
                            std::filesystem::path(currentProject).parent_path().generic_string();
                        if (ImGui::MenuItem("New Scene...")) {
                            sceneNameBuf[0] = '\0';
                            sceneNewOpen = true;
                        }
                        if (ImGui::MenuItem("Save Scene"))
                            saveSceneFile(currentProject);
                        if (ImGui::MenuItem("Rename Scene...")) {
                            std::snprintf(sceneNameBuf, sizeof(sceneNameBuf), "%s",
                                std::filesystem::path(currentProject).stem().string().c_str());
                            sceneRenameOpen = true;
                        }
                        const auto scenes = listScenesIn(projFolder);
                        ImGui::BeginDisabled(scenes.size() < 2); // keep at least one scene
                        if (ImGui::MenuItem("Delete Scene..."))
                            sceneDeleteOpen = true;
                        ImGui::EndDisabled();
                        ImGui::SeparatorText("Switch to");
                        for (const auto& [n, path] : scenes) {
                            const bool active = (path == currentProject);
                            if (ImGui::MenuItem((n + "##sc" + path).c_str(), nullptr, active) &&
                                !active) {
                                saveSceneFile(currentProject); // don't lose current edits
                                loadSceneFile(path);
                            }
                        }
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Edit")) {
                    const std::string undoLbl = history.canUndo()
                        ? std::string("Undo ") + history.undoName() : "Undo";
                    const std::string redoLbl = history.canRedo()
                        ? std::string("Redo ") + history.redoName() : "Redo";
                    if (ImGui::MenuItem(undoLbl.c_str(), "Ctrl+Z", false, history.canUndo())) {
                        history.undo(document); entitySel = -1;
                    }
                    if (ImGui::MenuItem(redoLbl.c_str(), "Ctrl+Y", false, history.canRedo())) {
                        history.redo(document); entitySel = -1;
                    }
                    ImGui::Separator();
                    const bool hasSel = entitySel >= 0 &&
                        entitySel < static_cast<int>(entities.size()) &&
                        entities[entitySel].type != EntityType::Sun;
                    if (ImGui::MenuItem("Duplicate", nullptr, false, hasSel))
                        duplicateEntity(entitySel);
                    if (ImGui::MenuItem("Delete", nullptr, false, hasSel))
                        deleteEntity(entitySel);
                    ImGui::Separator();
                    if (ImGui::MenuItem("Clear objects")) {
                        entities.erase(std::remove_if(entities.begin(), entities.end(),
                            [](const Entity& e){ return e.type != EntityType::Sun; }),
                            entities.end());
                        entitySel = -1;
                        history.clear(); // bulk reset -> drop history
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("View")) {
                    ImGui::MenuItem("Stats",           nullptr, &showStats);
                    ImGui::MenuItem("Camera",          nullptr, &showCamera);
                    ImGui::MenuItem("Mixer",           nullptr, &showMixer);
                    ImGui::MenuItem("Weather & audio", nullptr, &showWeather);
                    ImGui::MenuItem("Sky & atmosphere",nullptr, &showSky);
                    ImGui::MenuItem("Colour grade",    nullptr, &showColorGrade);
                    ImGui::MenuItem("Water",           nullptr, &showWater);
                    ImGui::MenuItem("Terrain",         nullptr, &showTerrain);
                    ImGui::MenuItem("Terrain sculpt",  nullptr, &showSculpt);
                    ImGui::MenuItem("Terrain paint",   nullptr, &showPaint);
                    ImGui::MenuItem("Vegetation",      nullptr, &showVegetation);
                    ImGui::MenuItem("Scatter",         nullptr, &showScatter);
                    ImGui::MenuItem("Camera path",     nullptr, &showCamPath);
                    ImGui::MenuItem("Roads",           nullptr, &showRoads);
                    ImGui::MenuItem("3D cursor",       nullptr, &showCursor);
                    ImGui::MenuItem("Vehicle",         nullptr, &showVehiclePanel);
                    ImGui::Separator();
                    ImGui::MenuItem("Materials",       nullptr, &showMaterials);
                    ImGui::MenuItem("Models",          nullptr, &showModels);
                    ImGui::MenuItem("Import Unity asset", nullptr, &showUnityImport);
                    ImGui::MenuItem("Assets",          nullptr, &showAssets);
                    ImGui::MenuItem("Scripts",         nullptr, &showScriptEditor);
                    ImGui::MenuItem("Environment",     nullptr, &showEnv);
                    ImGui::Separator();
                    if (ImGui::MenuItem("Reset layout")) requestDockRebuild = true;
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
            //     so the dockspace below shifts down automatically. Each button
            //     draws its shape (no icon font); clicking it makes that type the
            //     active one and drops one in front of the camera.
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
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const ImVec2 bs(bh, bh);
                    auto shapeBtn = [&](EntityType t, const char* tip) {
                        ImGui::PushID(static_cast<int>(t) + 1);
                        const ImVec2 p0 = ImGui::GetCursorScreenPos();
                        const bool clicked = ImGui::Button("##s", bs);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
                        const ImVec2 c(p0.x + bs.x * 0.5f, p0.y + bs.y * 0.5f);
                        const float r = 8.0f;
                        const ImU32 col = (entityNewType == t)
                            ? IM_COL32(255, 205, 70, 255) : IM_COL32(215, 215, 220, 255);
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
                            case EntityType::Light:
                                dl->AddCircleFilled(c, r * 0.45f, col);
                                for (int a = 0; a < 8; ++a) {
                                    const float ang = a * 0.7853982f;
                                    const ImVec2 d(std::cos(ang), std::sin(ang));
                                    dl->AddLine({c.x + d.x * r * 0.7f, c.y + d.y * r * 0.7f},
                                                {c.x + d.x * r, c.y + d.y * r}, col, 1.5f);
                                }
                                break;
                            case EntityType::Empty: // small dashed cross = transform node
                                dl->AddLine({c.x - r, c.y}, {c.x + r, c.y}, col, 1.5f);
                                dl->AddLine({c.x, c.y - r}, {c.x, c.y + r}, col, 1.5f);
                                dl->AddCircle(c, r * 0.4f, col, 0, 1.5f);
                                break;
                            default: break;
                        }
                        ImGui::PopID();
                        ImGui::SameLine();
                        if (clicked) {
                            entityNewType = t;
                            const glm::vec3 pp = camera.position() + camera.front() * 6.0f;
                            addEntity(glm::vec3(pp.x, streamer.heightAt(pp.x, pp.z), pp.z), t);
                        }
                    };
                    shapeBtn(EntityType::Box, "Box");
                    shapeBtn(EntityType::Ramp, "Ramp");
                    shapeBtn(EntityType::Cylinder, "Cylinder");
                    shapeBtn(EntityType::Sphere, "Sphere");
                    shapeBtn(EntityType::Light, "Light");
                    shapeBtn(EntityType::Empty, "Empty (transform-only grouping node)");

                    // Gap, then the transform-gizmo modes (Q/W/E).
                    ImGui::Dummy(ImVec2(10.0f, 1.0f));
                    ImGui::SameLine();
                    auto modeBtn = [&](ImGuizmo::OPERATION op, const char* tip) {
                        ImGui::PushID(100 + static_cast<int>(op));
                        const ImVec2 p0 = ImGui::GetCursorScreenPos();
                        const bool clicked = ImGui::Button("##m", bs);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
                        const ImVec2 c(p0.x + bs.x * 0.5f, p0.y + bs.y * 0.5f);
                        const float r = 8.0f, a = 3.5f;
                        const ImU32 col = (gizmoOp == op)
                            ? IM_COL32(255, 205, 70, 255) : IM_COL32(215, 215, 220, 255);
                        if (op == ImGuizmo::TRANSLATE) {          // 4-way arrow
                            dl->AddLine({c.x - r, c.y}, {c.x + r, c.y}, col, 1.6f);
                            dl->AddLine({c.x, c.y - r}, {c.x, c.y + r}, col, 1.6f);
                            dl->AddTriangleFilled({c.x + r, c.y}, {c.x + r - a, c.y - a}, {c.x + r - a, c.y + a}, col);
                            dl->AddTriangleFilled({c.x - r, c.y}, {c.x - r + a, c.y - a}, {c.x - r + a, c.y + a}, col);
                            dl->AddTriangleFilled({c.x, c.y - r}, {c.x - a, c.y - r + a}, {c.x + a, c.y - r + a}, col);
                            dl->AddTriangleFilled({c.x, c.y + r}, {c.x - a, c.y + r - a}, {c.x + a, c.y + r - a}, col);
                        } else if (op == ImGuizmo::ROTATE) {      // circular arrow
                            dl->PathArcTo(c, r, 0.6f, 5.4f, 20);
                            dl->PathStroke(col, 0, 1.8f);
                            const ImVec2 e(c.x + std::cos(5.4f) * r, c.y + std::sin(5.4f) * r);
                            const ImVec2 tg(-std::sin(5.4f), std::cos(5.4f));
                            const ImVec2 no(std::cos(5.4f), std::sin(5.4f));
                            dl->AddTriangleFilled({e.x + tg.x * a, e.y + tg.y * a},
                                                  {e.x - no.x * a * 0.7f, e.y - no.y * a * 0.7f},
                                                  {e.x + no.x * a * 0.7f, e.y + no.y * a * 0.7f}, col);
                        } else {                                  // scale: diagonal + boxes
                            dl->AddLine({c.x - r * 0.7f, c.y + r * 0.7f}, {c.x + r * 0.7f, c.y - r * 0.7f}, col, 1.8f);
                            dl->AddRectFilled({c.x + r * 0.7f - 3, c.y - r * 0.7f - 3},
                                              {c.x + r * 0.7f + 3, c.y - r * 0.7f + 3}, col);
                            dl->AddRect({c.x - r * 0.7f - 3, c.y + r * 0.7f - 3},
                                        {c.x - r * 0.7f + 3, c.y + r * 0.7f + 3}, col, 0.0f, 0, 1.5f);
                        }
                        ImGui::PopID();
                        ImGui::SameLine();
                        if (clicked) gizmoOp = op;
                    };
                    modeBtn(ImGuizmo::TRANSLATE, "Move (Q)");
                    modeBtn(ImGuizmo::ROTATE, "Rotate (W)");
                    modeBtn(ImGuizmo::SCALE, "Scale (E)");

                    // Gap, then the gizmo reference-frame toggle (local vs world).
                    ImGui::Dummy(ImVec2(10.0f, 1.0f));
                    ImGui::SameLine();
                    {
                        const bool isLocal = (gizmoMode == ImGuizmo::LOCAL);
                        ImGui::PushID("gizmoSpace");
                        const ImVec2 p0 = ImGui::GetCursorScreenPos();
                        const bool clicked = ImGui::Button("##sp", bs);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Gizmo space: %s  (X to toggle)",
                                              isLocal ? "Local" : "World");
                        const ImVec2 c(p0.x + bs.x * 0.5f, p0.y + bs.y * 0.5f);
                        const float r = 8.0f;
                        const ImU32 col = IM_COL32(255, 205, 70, 255);
                        if (isLocal) {
                            // Object box with its own tilted axis = local frame.
                            dl->AddRect({c.x - r * 0.7f, c.y - r * 0.55f},
                                        {c.x + r * 0.35f, c.y + r * 0.7f}, col, 0.0f, 0, 1.6f);
                            dl->AddLine({c.x + r * 0.35f, c.y - r * 0.55f},
                                        {c.x + r, c.y - r}, col, 1.6f);
                        } else {
                            // Globe with meridian/equator = world (global) frame.
                            dl->AddCircle(c, r, col, 0, 1.6f);
                            dl->AddLine({c.x - r, c.y}, {c.x + r, c.y}, col, 1.2f);
                            dl->AddLine({c.x, c.y - r}, {c.x, c.y + r}, col, 1.2f);
                            dl->AddBezierQuadratic({c.x, c.y - r}, {c.x - r * 0.9f, c.y},
                                                   {c.x, c.y + r}, col, 1.1f);
                            dl->AddBezierQuadratic({c.x, c.y - r}, {c.x + r * 0.9f, c.y},
                                                   {c.x, c.y + r}, col, 1.1f);
                        }
                        ImGui::PopID();
                        ImGui::SameLine();
                        if (clicked)
                            gizmoMode = isLocal ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
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

            const ImGuiID dockId = gui.dockspace();

            // First run (or after "Reset layout"): lay the panels out into a tidy
            // right-hand column, split top/bottom, so they don't start as a heap
            // of floating windows. Once arranged, ImGui persists it in imgui.ini.
            if (requestDockRebuild || ImGui::DockBuilderGetNode(dockId) == nullptr) {
                requestDockRebuild = false;
                ImGui::DockBuilderRemoveNode(dockId);
                ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_PassthruCentralNode
                                                | ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->WorkSize);

                // Default layout: Hierarchy (left) | Scene (centre) | Inspector
                // (right). Every other panel is toggled from the View menu and
                // floats until the user docks it where they like.
                ImGuiID central = 0;
                ImGuiID left  = ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Left, 0.20f,
                                                            nullptr, &central);
                ImGuiID right = ImGui::DockBuilderSplitNode(central, ImGuiDir_Right, 0.28f,
                                                            nullptr, &central);

                ImGui::DockBuilderDockWindow("Scene", central);
                ImGui::DockBuilderDockWindow("Hierarchy", left);
                ImGui::DockBuilderDockWindow("Inspector", right);
                ImGui::DockBuilderFinish(dockId);
            }

            // Central scene viewport: shows the composited render texture. Its
            // content size drives the render resolution (set below for next pass).
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            if (ImGui::Begin("Scene", nullptr, ImGuiWindowFlags_NoScrollbar
                                             | ImGuiWindowFlags_NoScrollWithMouse)) {
                const ImVec2 avail = ImGui::GetContentRegionAvail();
                viewW = std::max(1, static_cast<int>(avail.x));
                viewH = std::max(1, static_cast<int>(avail.y));
                // GL textures are bottom-up: flip V (uv0.y=1, uv1.y=0).
                ImGui::Image((ImTextureID)(intptr_t)viewportRT.colorTexture(),
                             ImVec2(static_cast<float>(viewW), static_cast<float>(viewH)),
                             ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
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
                viewportClicked = viewportHovered &&
                                  ImGui::IsMouseClicked(ImGuiMouseButton_Left);

                // Drag an asset from the Assets browser into the viewport: a Model
                // drops onto the terrain; a Texture drops onto the object under the
                // cursor, making a fresh material that uses it and assigning it.
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* pl =
                            ImGui::AcceptDragDropPayload("ASSET_GUID")) {
                        const AssetId gid = AssetId::fromString(std::string(
                            static_cast<const char*>(pl->Data), pl->DataSize));
                        const AssetType at = assetDb.typeForId(gid);
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
                        } else if (at == AssetType::Texture) {
                            // Pick the solid under the drop point.
                            const glm::mat4 inv = glm::inverse(vp);
                            glm::vec4 pn = inv * glm::vec4(viewportMouseNdc, -1.0f, 1.0f); pn /= pn.w;
                            glm::vec4 pf = inv * glm::vec4(viewportMouseNdc,  1.0f, 1.0f); pf /= pf.w;
                            const glm::vec3 ro = glm::vec3(pn);
                            const glm::vec3 rd = glm::normalize(glm::vec3(pf) - glm::vec3(pn));
                            int hit = -1; float bestT = 1e30f;
                            for (int i = 0; i < static_cast<int>(entities.size()); ++i) {
                                const EntityType t = entities[i].type;
                                const bool solid = t == EntityType::Box || t == EntityType::Ramp ||
                                                   t == EntityType::Cylinder || t == EntityType::Sphere;
                                if (!solid) continue;
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
                                entitySel = hit;
                                auto cmd = std::make_unique<ModifyEntitiesCmd>(
                                    before, snapshotEntities(ids));
                                if (!cmd->trivial()) history.push(std::move(cmd), document);
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                // --- Road edit handles: draggable control-point markers -----
                if (roadEditMode) {
                    const float asp = static_cast<float>(viewW) / static_cast<float>(viewH);
                    const glm::mat4 vp = camera.projectionMatrix(asp) * camera.viewMatrix();
                    const ImVec2 org = rmin; // image top-left in screen space
                    auto handleWorld = [&](int i) {
                        return glm::vec3(road.roadPts[i].x,
                                         streamer.heightAt(road.roadPts[i].x, road.roadPts[i].y) + 0.10f,
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

                    // Pick / add on click.
                    if (viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        int  best = -1;
                        float bestD = 12.0f; // pixel grab radius
                        for (int i = 0; i < static_cast<int>(road.roadPts.size()); ++i) {
                            ImVec2 sp;
                            if (!toScreen(handleWorld(i), sp)) continue;
                            const float d = std::hypot(sp.x - mp.x, sp.y - mp.y);
                            if (d < bestD) { bestD = d; best = i; }
                        }
                        if (best >= 0) {
                            // Shift-click marks the far end of a bridge instead of
                            // re-selecting; plain click picks (and starts a drag).
                            if (ImGui::GetIO().KeyShift && roadSel >= 0 && best != roadSel)
                                roadSel2 = best;
                            else { roadSel = best; roadSel2 = -1; roadDragging = true; }
                        } else {
                            glm::vec3 h;
                            if (roadPickTerrain(viewportMouseNdc, vp, h)) {
                                // Insert at the nearest segment so a click on an
                                // existing road drops a waypoint in the middle; a
                                // click past an open end still extends the road.
                                insertRoadPoint(roadInsertIndex({h.x, h.z}),
                                                glm::vec2(h.x, h.z));
                            }
                        }
                    }
                    // Drag the selected handle across the terrain.
                    if (roadDragging && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                        roadSel >= 0 && roadSel < static_cast<int>(road.roadPts.size())) {
                        glm::vec3 h;
                        if (roadPickTerrain(viewportMouseNdc, vp, h)) {
                            road.roadPts[roadSel] = glm::vec2(h.x, h.z);
                            road.needsBuild = true;
                        }
                    }
                    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) roadDragging = false;
                    // Delete the selected point.
                    if (roadSel >= 0 && roadSel < static_cast<int>(road.roadPts.size()) &&
                        ImGui::IsKeyPressed(ImGuiKey_Delete))
                        removeRoadPoint(roadSel);

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
                    for (int i = 0; i < static_cast<int>(road.roadPts.size()); ++i) {
                        ImVec2 sp;
                        if (!toScreen(handleWorld(i), sp)) continue;
                        const bool s = (i == roadSel);
                        dl->AddCircleFilled(sp, s ? 7.0f : 5.0f,
                                            s ? IM_COL32(255, 210, 60, 255)
                                              : IM_COL32(90, 180, 255, 235));
                        dl->AddCircle(sp, s ? 7.0f : 5.0f, IM_COL32(0, 0, 0, 190), 0, 1.5f);
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

                    // Stamp drops a landform once per click; the other tools apply
                    // continuously while the button is held.
                    const bool stampTool = (sculptTool == 5);
                    const bool apply = onGround &&
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
                    if (onGround) {
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        const ImU32 col = sculptTool == 2 ? IM_COL32(120, 200, 255, 225)
                                        : sculptTool == 3 ? IM_COL32(255, 210, 90, 225)
                                        : sculptTool == 4 ? IM_COL32(200, 150, 110, 225)
                                        : sculptTool == 5 ? IM_COL32(200, 140, 255, 225)
                                        : sculptTool == 6 ? IM_COL32(180, 180, 190, 225)
                                        : sculptTool == 7 ? IM_COL32(90, 170, 255, 225)
                                        : sculptTool == 1 ? IM_COL32(255, 130, 90, 225)
                                                          : IM_COL32(140, 235, 140, 225);
                        const int SEG = 56;
                        ImVec2 prev; bool have = false;
                        for (int i = 0; i <= SEG; ++i) {
                            const float a  = static_cast<float>(i) / SEG * 6.2831853f;
                            const float wx = center.x + std::cos(a) * sculptRadius;
                            const float wz = center.z + std::sin(a) * sculptRadius;
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
                            if (!cmd->trivial()) history.push(std::move(cmd), document);
                        }
                    }
                    if (entitySel >= 0 && entitySel < static_cast<int>(entities.size()) &&
                        entities[entitySel].type != EntityType::Sun) {
                        Entity& b = entities[entitySel];
                        const int selId = b.id;
                        float t[3] = {b.center.x, b.center.y, b.center.z};
                        float r[3] = {b.rotation.x, b.rotation.y, b.rotation.z};
                        float s[3] = {b.half.x * 2.0f, b.half.y * 2.0f, b.half.z * 2.0f};
                        if (entityEditMode) {
                            float model[16];
                            ImGuizmo::RecomposeMatrixFromComponents(t, r, s, model);
                            ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj),
                                                 gizmoOp, gizmoMode, model);
                            const bool gizmoUsing = ImGuizmo::IsUsing();
                            if (gizmoUsing && !gizmoActive) { // drag start: snapshot subtree
                                gizmoActive = true;
                                gizmoIds    = collectSubtreeIds(selId);
                                gizmoBefore = snapshotEntities(gizmoIds);
                            }
                            if (gizmoUsing) {
                                ImGuizmo::DecomposeMatrixToComponents(model, t, r, s);
                                b.half = glm::max(glm::vec3(s[0], s[1], s[2]) * 0.5f, glm::vec3(0.05f));
                                // World-space edit -> local (children then follow via
                                // resolveHierarchy).
                                const glm::mat4 pw = parentWorldMat(b);
                                setWorld(b, glm::vec3(t[0], t[1], t[2]),
                                         glm::vec3(r[0], r[1], r[2]),
                                         b.parent >= 0 ? &pw : nullptr);
                            }
                        }

                        // Orange oriented wireframe around the selected object.
                        const glm::mat4 boxX =
                            composeModel(b.center, b.rotation, glm::vec3(1.0f));
                        ImVec2 sp[8]; bool ok[8];
                        for (int c = 0; c < 8; ++c) {
                            const glm::vec3 lh((c & 1) ? b.half.x : -b.half.x,
                                               (c & 2) ? b.half.y : -b.half.y,
                                               (c & 4) ? b.half.z : -b.half.z);
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
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        for (const auto& e : kBoxEdges)
                            if (ok[e[0]] && ok[e[1]])
                                dl->AddLine(sp[e[0]], sp[e[1]],
                                            IM_COL32(255, 140, 0, 230), 1.6f);

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
                    // painting grass (the brush owns the left button then).
                    if (!ImGuizmo::IsOver() && !ImGuizmo::IsUsing() && !grassPaintMode &&
                        !sculptMode && !paintMode && !scatterMode &&
                        viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        const glm::mat4 inv = glm::inverse(vp);
                        glm::vec4 pn = inv * glm::vec4(viewportMouseNdc, -1.0f, 1.0f); pn /= pn.w;
                        glm::vec4 pf = inv * glm::vec4(viewportMouseNdc,  1.0f, 1.0f); pf /= pf.w;
                        const glm::vec3 ro = glm::vec3(pn);
                        const glm::vec3 rd = glm::normalize(glm::vec3(pf) - glm::vec3(pn));
                        // Every AABB the ray passes through, nearest first.
                        std::vector<std::pair<float, int>> hits;
                        for (int i = 0; i < static_cast<int>(entities.size()); ++i) {
                            if (!entities[i].activeInHierarchy) continue; // not shown, not pickable
                            const float t = rayAABB(ro, rd, entities[i].center - entities[i].half,
                                                            entities[i].center + entities[i].half);
                            if (t >= 0.0f) hits.emplace_back(t, entities[i].id);
                        }
                        std::sort(hits.begin(), hits.end());
                        if (!hits.empty()) {
                            std::vector<int> ids;
                            ids.reserve(hits.size());
                            for (const auto& h : hits) ids.push_back(h.second);
                            // Same overlapping stack as last click -> advance to the
                            // next candidate; a new stack -> start at the nearest.
                            if (ids == pickStack)
                                pickIdx = (pickIdx + 1) % static_cast<int>(ids.size());
                            else { pickStack = ids; pickIdx = 0; }
                            entitySel = document.indexOf(ids[pickIdx]);
                        } else if (entityEditMode) {
                            glm::vec3 h; // Edit mode: empty ground -> drop a new block
                            if (roadPickTerrain(viewportMouseNdc, vp, h)) addEntity(h, entityNewType);
                        } else {
                            entitySel = -1; // Selection mode: empty click clears it
                            pickStack.clear(); pickIdx = -1;
                        }
                    }
                    if (entitySel >= 0 && entitySel < static_cast<int>(entities.size()) &&
                        ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                        deleteEntity(entitySel);
                    }
                }
            } else {
                viewportHovered = false;
                viewportClicked = false;
            }
            ImGui::End();
            ImGui::PopStyleVar();

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
                ImGui::Text("Draws: %d visible, %d culled",
                            renderer.lastDrawn(), renderer.lastCulled());
                ImGui::Separator();
                ImGui::SliderFloat("Move speed", &camera.moveSpeed, 2.0f, 80.0f);
                ImGui::SliderInt("View distance", &viewRadius, 2, 9, "%d chunks");
                ImGui::SameLine();
                ImGui::Text("(%.0f m)", viewRadius * streamer.settings().chunkSize);
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
                ImGui::SliderFloat("Cloud scale", &cloudScale, 0.001f, 0.006f, "%.4f");
                ImGui::SliderFloat("Wind",        &cloudSpeed, 0.0f, 20.0f);
                ImGui::SliderFloat("Fog density", &fogDensity, 0.0f, 0.02f, "%.4f");
                ImGui::SliderFloat("Fog falloff", &fogFalloff, 0.005f, 0.1f, "%.3f");
                ImGui::SliderFloat("Exposure",   &exposure, 0.2f, 3.0f);
                ImGui::SliderFloat("Bloom",      &bloomIntensity, 0.0f, 1.5f);
                ImGui::SliderFloat("Sun rays",   &rayIntensity, 0.0f, 1.5f);
                ImGui::SliderFloat("SSAO",       &ssaoStrength, 0.0f, 1.0f);
                ImGui::SliderFloat("SSAO radius",&ssaoRadius, 0.2f, 4.0f);
                ImGui::SliderFloat("Cascade split", &renderer.shadows().splitLambda, 0.0f, 1.0f);
                ImGui::SeparatorText("Depth of field");
                ImGui::SliderFloat("DOF blur", &dofMax, 0.0f, 12.0f, "%.1f px");
                ImGui::SliderFloat("Focus near", &dofNear, 2.0f, 120.0f, "%.0f m");
                ImGui::SliderFloat("Focus far",  &dofFar, 20.0f, 400.0f, "%.0f m");
                ImGui::SeparatorText("Anti-aliasing");
                ImGui::Checkbox("FXAA", &fxaaEnabled);
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
                texScale, normalStrength, veg.grassDirty, veg.treeCenter, road.needsBuild,
                assetDb, thumbFor,
            });

            sculptui::drawPanel({
                showSculpt, sculptMode,
                grassPaintMode, roadEditMode, treePaintMode, flowerPaintMode, paintMode,
                scatterMode,
                sculptTool, sculptRadius, sculptStrength, sculptFlattenH,
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
                    road.roadPts.size() >= 2,
                    scatterRoadside, scatterClearAll,
                });
            }

            if (showVegetation) { if (ImGui::Begin("Vegetation", &showVegetation)) {
                ImGui::SeparatorText("Grass");
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

                ImGui::SeparatorText("Paint grass (3D brush)");
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

                ImGui::SeparatorText("Flowers");
                ImGui::Checkbox("Flowers", &veg.flowerEnabled);
                if (ImGui::SliderFloat("Flower density", &veg.flowerDensity, 0.0f, 2.0f))
                    veg.grassDirty = true; // flowers regenerate with the grass pass
                ImGui::SameLine();
                if (ImGui::SmallButton("Regrow")) veg.grassDirty = true;
                ImGui::Text("Flowers: %d", veg.flowerCount);

                ImGui::SeparatorText("Paint flowers (3D brush)");
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
            roadui::drawPanel({showRoads, road, roadEditMode, roadSel, roadSel2,
                [&]{ grassPaintMode = sculptMode = treePaintMode = flowerPaintMode =
                         paintMode = scatterMode = false; }, // don't fight over LMB
                buildRoad, removeRoadPoint});

            if (showCursor) { if (ImGui::Begin("3D Cursor", &showCursor)) {
                ImGui::Checkbox("Show cursor", &cursorVisible);
                ImGui::TextDisabled("Shift+Right-click in the viewport to place it.");
                ImGui::DragFloat3("Position", &cursor3D.x, 0.05f, 0.0f, 0.0f, "%.2f");
                ImGui::SetNextItemWidth(140.0f);
                ImGui::DragFloat("Grid step", &cursorGrid, 0.05f, 0.01f, 100.0f, "%.2f m");
                ImGui::TextDisabled("Shift+S in the viewport opens the snap menu.");

                const bool haveSel = cursorHaveSel();

                ImGui::SeparatorText("Snap cursor");
                if (ImGui::Button("To world origin")) snapCursorToOrigin();
                ImGui::SameLine();
                if (ImGui::Button("To grid"))         snapCursorToGrid();
                if (ImGui::Button("To terrain"))      snapCursorToTerrain();
                ImGui::SameLine();
                ImGui::BeginDisabled(!haveSel);
                if (ImGui::Button("To selection"))    snapCursorToSelection();
                ImGui::EndDisabled();

                ImGui::SeparatorText("Snap selection");
                ImGui::BeginDisabled(!haveSel);
                if (ImGui::Button("Selection to cursor")) snapSelectionToCursor();
                ImGui::SameLine();
                if (ImGui::Button("Selection to grid"))   snapSelectionToGrid();
                ImGui::EndDisabled();

                ImGui::SeparatorText("Create");
                if (ImGui::Button("Add object at cursor"))
                    addEntity(cursor3D, entityNewType);
                ImGui::SameLine();
                ImGui::TextDisabled("(base rests on the cursor)");
            }
            ImGui::End(); }

            if (ImGui::Begin("Hierarchy")) {
                ImGui::BeginDisabled(entitySel < 0 || entitySel >= static_cast<int>(entities.size()));
                if (ImGui::Button("Duplicate")) duplicateEntity(entitySel);
                ImGui::SameLine();
                if (ImGui::Button("Delete")) deleteEntity(entitySel);
                ImGui::EndDisabled();

                ImGui::SeparatorText("Scene");
                // A proper tree control: roots first, children nested; the tree
                // fills the panel and scrolls. Single click selects, arrow/double-
                // click expands; drag a node onto another to reparent (onto empty
                // space to unparent); right-click for Duplicate/Delete.
                int reparentSrc = -1, reparentTo = -2; // -2 = none, -1 = root
                int dupReq = -1, delReq = -1;
                // Deferred context-menu creation requests (applied after the tree
                // is drawn, so entities isn't mutated mid-iteration).
                int emptyParentReq = -1, emptyChildReq = -1, primChildReq = -1;
                int vehicleLightsReq = -1;
                EntityType primChildType = EntityType::Box;
                auto typeColor = [](EntityType t) -> ImU32 {
                    switch (t) {
                        case EntityType::Light:    return IM_COL32(255, 224, 130, 255);
                        case EntityType::Sun:      return IM_COL32(255, 200,  90, 255);
                        case EntityType::Model:    return IM_COL32(150, 200, 255, 255);
                        case EntityType::Sphere:   return IM_COL32(190, 230, 200, 255);
                        case EntityType::Cylinder: return IM_COL32(200, 210, 235, 255);
                        case EntityType::Empty:    return IM_COL32(170, 175, 185, 255);
                        default:                   return IM_COL32(220, 220, 225, 255);
                    }
                };
                std::function<void(int)> drawNode = [&](int i) {
                    ImGui::PushID(entities[i].id);           // stable id
                    bool hasChildren = false;
                    for (const Entity& c : entities)
                        if (c.parent == entities[i].id) { hasChildren = true; break; }
                    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                                             | ImGuiTreeNodeFlags_OpenOnDoubleClick
                                             | ImGuiTreeNodeFlags_SpanFullWidth
                                             | ImGuiTreeNodeFlags_DefaultOpen;
                    if (i == entitySel)  flags |= ImGuiTreeNodeFlags_Selected;
                    if (!hasChildren)   flags |= ImGuiTreeNodeFlags_Leaf;
                    const char* nm = entities[i].name.empty() ? "(unnamed)"
                                                              : entities[i].name.c_str();
                    // Start renaming this node: seed the buffer and grab focus.
                    auto beginRename = [&] {
                        renameId = entities[i].id;
                        std::snprintf(renameBuf, sizeof(renameBuf), "%s",
                                      entities[i].name.c_str());
                        renameFocus = true;
                    };
                    const bool renaming = (entities[i].id == renameId);

                    // Dim the label when the object is effectively off (itself or an
                    // ancestor deactivated), so a hidden subtree reads at a glance.
                    // The Active toggle itself lives in the Inspector.
                    ImU32 col = typeColor(entities[i].type);
                    if (!entities[i].activeInHierarchy)
                        col = (col & 0x00FFFFFF) | 0x66000000; // ~40% alpha
                    ImGui::PushStyleColor(ImGuiCol_Text, col);
                    // While renaming, draw the row with a blank label and overlay an
                    // edit field, keeping the tree's arrow + indentation intact.
                    const bool open = ImGui::TreeNodeEx("##n", flags, "%s",
                                                        renaming ? "" : nm);
                    ImGui::PopStyleColor();

                    if (renaming) {
                        ImGui::SameLine();
                        if (renameFocus) { ImGui::SetKeyboardFocusHere(); renameFocus = false; }
                        ImGui::SetNextItemWidth(-1.0f);
                        const bool enter = ImGui::InputText("##rename", renameBuf,
                            sizeof(renameBuf), ImGuiInputTextFlags_EnterReturnsTrue |
                                               ImGuiInputTextFlags_AutoSelectAll);
                        const bool esc = ImGui::IsKeyPressed(ImGuiKey_Escape);
                        // Commit on Enter or when the field loses focus; Escape cancels.
                        if (enter || (ImGui::IsItemDeactivated() && !esc)) {
                            entities[i].name = renameBuf;
                            renameId = -1;
                        } else if (esc) {
                            renameId = -1;
                        }
                    } else {
                        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) entitySel = i;
                        // F2 on the selected node (or a double-click on its label)
                        // starts an inline rename, Unity-style.
                        if (i == entitySel && ImGui::IsWindowFocused() &&
                            ImGui::IsKeyPressed(ImGuiKey_F2))
                            beginRename();
                        if (ImGui::IsItemHovered() && !ImGui::IsItemToggledOpen() &&
                            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                            beginRename();
                        if (ImGui::BeginPopupContextItem()) {
                            entitySel = i;
                            if (ImGui::MenuItem("Rename", "F2")) beginRename();
                            if (ImGui::MenuItem("Duplicate")) dupReq = i;
                            ImGui::Separator();
                            if (ImGui::MenuItem("Create Empty Parent")) emptyParentReq = i;
                            if (ImGui::MenuItem("Create Empty Child"))  emptyChildReq = i;
                            if (ImGui::BeginMenu("Add Primitive")) {
                                if (ImGui::MenuItem("Box"))
                                    { primChildReq = i; primChildType = EntityType::Box; }
                                if (ImGui::MenuItem("Ramp"))
                                    { primChildReq = i; primChildType = EntityType::Ramp; }
                                if (ImGui::MenuItem("Cylinder"))
                                    { primChildReq = i; primChildType = EntityType::Cylinder; }
                                if (ImGui::MenuItem("Sphere"))
                                    { primChildReq = i; primChildType = EntityType::Sphere; }
                                ImGui::EndMenu();
                            }
                            if (const auto* cc = entities[i].components.get<CameraComponent>()) {
                                ImGui::Separator();
                                if (ImGui::MenuItem("Set as Main Camera", nullptr,
                                                    cc->activeOnStart))
                                    setMainCamera(entities[i].id);
                            }
                            if (entities[i].components.get<VehicleComponent>()) {
                                ImGui::Separator();
                                if (ImGui::MenuItem("Add headlights")) vehicleLightsReq = i;
                            }
                            ImGui::Separator();
                            ImGui::BeginDisabled(entities[i].type == EntityType::Sun);
                            if (ImGui::MenuItem("Delete")) delReq = i;
                            ImGui::EndDisabled();
                            ImGui::EndPopup();
                        }
                        if (ImGui::BeginDragDropSource()) {
                            const int sid = entities[i].id;
                            ImGui::SetDragDropPayload("SOLID_ID", &sid, sizeof(int));
                            ImGui::Text("%s", nm);
                            ImGui::EndDragDropSource();
                        }
                        if (ImGui::BeginDragDropTarget()) {
                            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("SOLID_ID")) {
                                reparentSrc = *static_cast<const int*>(pl->Data);
                                reparentTo  = entities[i].id;
                            }
                            ImGui::EndDragDropTarget();
                        }
                    }
                    if (open) {
                        if (hasChildren)
                            for (int c = 0; c < static_cast<int>(entities.size()); ++c)
                                if (entities[c].parent == entities[i].id) drawNode(c);
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                };
                ImGui::BeginChild("##tree", ImVec2(0.0f, 0.0f), true);
                for (int i = 0; i < static_cast<int>(entities.size()); ++i)
                    if (entities[i].parent < 0) drawNode(i);
                // Empty space in the tree unparents a node dropped onto it.
                ImGui::Dummy(ImVec2(-1.0f, ImGui::GetContentRegionAvail().y));
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("SOLID_ID")) {
                        reparentSrc = *static_cast<const int*>(pl->Data);
                        reparentTo  = -1;
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::EndChild();
                if (dupReq >= 0)             duplicateEntity(dupReq);
                else if (delReq >= 0)        deleteEntity(delReq);
                else if (emptyParentReq >= 0) addEmptyParent(emptyParentReq);
                else if (emptyChildReq >= 0)  addEmptyChild(emptyChildReq);
                else if (primChildReq >= 0)   addPrimitiveChild(primChildReq, primChildType);
                else if (vehicleLightsReq >= 0) addVehicleLights(vehicleLightsReq);
                // Apply a requested reparent (rejecting cycles).
                if (reparentSrc >= 0 && reparentTo != -2) {
                    int si = -1;
                    for (int k = 0; k < static_cast<int>(entities.size()); ++k)
                        if (entities[k].id == reparentSrc) { si = k; break; }
                    if (si >= 0 && reparentSrc != reparentTo &&
                        (reparentTo < 0 || !isUnderId(reparentTo, reparentSrc))) {
                        entities[si].parent = reparentTo;
                        // Keep the child put: rebase its local onto the new parent.
                        Entity* np = (reparentTo >= 0) ? document.find(reparentTo) : nullptr;
                        const glm::mat4 pw = np ? worldOf(*np) : glm::mat4(1.0f);
                        rebaseLocal(entities[si], np ? &pw : nullptr);
                    }
                }

            }
            ImGui::End();

            if (ImGui::Begin("Inspector")) {
                if (entitySel >= 0 && entitySel < static_cast<int>(entities.size())) {
                    Entity& b = entities[entitySel];
                    // Undo transaction: snapshot this entity's subtree before any
                    // widget below mutates it (committed at the block's end).
                    const std::vector<int> inspFrameIds   = collectSubtreeIds(b.id);
                    std::vector<Entity>    inspFrameStart = snapshotEntities(inspFrameIds);
                    // Set true when the Camera branch below sets the Main Camera via
                    // setMainCamera(): that pushes its own multi-camera undo step, so
                    // the per-entity edit wrapper must not also log this frame.
                    bool                   mainCamJustSet = false;
                    ImGui::SeparatorText(entityTypeName(b.type));

                    // Auto-generated fields: the property table (PropertyMeta.hpp)
                    // declares each field once -> the right widget, range and
                    // visibility fall out here. Adding a field is a table entry.
                    for (const Property& pr : entityProperties()) {
                        if (!(pr.typeMask & typeBit(b.type))) continue;
                        if (pr.visible && !pr.visible(&b)) continue;
                        // Children follow center/rotation edits automatically
                        // (resolveHierarchy); no per-field side effects left.
                        drawProperty(pr, &b);
                    }

                    if (b.type == EntityType::Sun) {
                        ImGui::SliderFloat("Time of day", &timeOfDay, 0.0f, 24.0f, "%.1f h");
                        ImGui::SameLine();
                        ImGui::Checkbox("Pause", &timePaused);
                        ImGui::TextDisabled("The sun drives the sky and casts shadows.");
                    } else {
                        // --- Bespoke fields (enumerate project state) ------------
                        if (auto* mdl = b.components.get<ModelComponent>()) {
                            LoadedModel* lm = models.byId(mdl->modelId);
                            ImGui::Text("Model: %s", lm ? lm->name.c_str() : "(missing)");
                        }
                        ImGui::Text("Parent: %s",
                                    b.parent < 0 ? "(root)" : ("id " + std::to_string(b.parent)).c_str());
                        if (ImGui::Button("Drop to ground"))
                        {
                            const glm::mat4 pw = parentWorldMat(b);
                            setWorld(b, glm::vec3(b.center.x,
                                streamer.heightAt(b.center.x, b.center.z) + b.half.y,
                                b.center.z), b.rotation, b.parent >= 0 ? &pw : nullptr);
                        }
                        ImGui::SameLine();
                        ImGui::BeginDisabled(b.parent < 0);
                        if (ImGui::Button("Unparent")) { b.parent = -1; rebaseLocal(b, nullptr); }
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        if (ImGui::Button("Delete##insp")) deleteEntity(entitySel);
                    }
                    // Components: optional attached capabilities. Each renders from
                    // its own metadata; add/remove is open via the type registry.
                    // (Re-fetch: Delete##insp above may have cleared the selection.)
                    if (entitySel >= 0 && entitySel < static_cast<int>(entities.size())) {
                        Entity& be = entities[entitySel];
                        ImGui::SeparatorText("Components");
                        for (std::size_t ci = 0; ci < be.components.items.size(); ++ci) {
                            ComponentBase* c = be.components.items[ci].get();
                            ImGui::PushID(static_cast<int>(ci));
                            ImGui::TextUnformatted(c->displayName());
                            ImGui::SameLine();
                            // Engine-managed components (Sun) aren't removable.
                            bool addable = true;
                            for (const auto& t : components::registry())
                                if (t.typeId == c->typeId()) { addable = t.addable; break; }
                            ImGui::BeginDisabled(!addable);
                            const bool remove = ImGui::SmallButton("Remove");
                            ImGui::EndDisabled();
                            if (auto* sc = dynamic_cast<ScriptComponent*>(c)) {
                                // Bespoke picker: enumerate the project's .lua files.
                                std::vector<std::string> luaFiles = listScripts();
                                const std::string cur = sc->file.empty() ? "(none)" : sc->file;
                                ImGui::SetNextItemWidth(-60.0f);
                                if (ImGui::BeginCombo("##scriptfile", cur.c_str())) {
                                    if (ImGui::Selectable("(none)", sc->file.empty())) sc->file.clear();
                                    for (const std::string& f : luaFiles)
                                        if (ImGui::Selectable(f.c_str(), sc->file == f)) sc->file = f;
                                    ImGui::EndCombo();
                                }
                                ImGui::SameLine();
                                ImGui::BeginDisabled(sc->file.empty());
                                if (ImGui::Button("Edit##scr")) openScript(sc->file);
                                ImGui::EndDisabled();
                                if (!sc->file.empty() &&
                                    std::find(luaFiles.begin(), luaFiles.end(), sc->file) == luaFiles.end())
                                    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f),
                                        "Missing: scripts/%s", sc->file.c_str());
                                else if (!scripts.lastError().empty())
                                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f),
                                                       "Script error: %s", scripts.lastError().c_str());
                            } else if (auto* mc = dynamic_cast<MaterialComponent*>(c)) {
                                // Bespoke picker: pick from the material library.
                                const int mi = document.materialIndex(mc->material);
                                if (ImGui::BeginCombo("Material", materials[mi].name.c_str())) {
                                    for (int i = 0; i < static_cast<int>(materials.size()); ++i) {
                                        const bool sel = (i == mi);
                                        if (ImGui::Selectable(materials[i].name.c_str(), sel)) {
                                            mc->material = materials[i].assetId;
                                            matSel = i;
                                        }
                                        if (sel) ImGui::SetItemDefaultFocus();
                                    }
                                    ImGui::EndCombo();
                                }
                                ImGui::SameLine();
                                if (ImGui::SmallButton("Edit##mat")) { matSel = mi; showMaterials = true; }
                            } else if (auto* mdl = dynamic_cast<ModelComponent*>(c)) {
                                // Scale drives the pick box (half) for the model.
                                if (ImGui::SliderFloat("Scale", &mdl->scale, 0.05f, 20.0f, "%.2f"))
                                    if (LoadedModel* lm = models.byId(mdl->modelId))
                                        be.half = modelHalf(*lm, mdl->scale);
                            } else if (auto* col = dynamic_cast<CollectibleComponent*>(c)) {
                                // Points + radius from metadata; Sound is a picker
                                // over the Sound assets (chosen, not typed).
                                for (const Property& pr : col->props())
                                    if (pr.key != "sound") drawProperty(pr, col);
                                soundPickerCombo("Sound", col->sound);
                            } else if (auto* tr = dynamic_cast<TriggerComponent*>(c)) {
                                // Radius/once/message from metadata; Sound is a picker.
                                for (const Property& pr : tr->props())
                                    if (pr.key != "sound") drawProperty(pr, tr);
                                soundPickerCombo("Sound", tr->sound);
                            } else if (auto* ts = dynamic_cast<TriggerSoundComponent*>(c)) {
                                // Radius/volume/loop/once from metadata; Sound picker.
                                for (const Property& pr : ts->props()) drawProperty(pr, ts);
                                soundPickerCombo("Sound", ts->sound);
                            } else if (auto* as = dynamic_cast<AudioSourceComponent*>(c)) {
                                // Volume/loop/play-on-start/spatial from metadata;
                                // Sound is a picker over the project's Sound assets.
                                for (const Property& pr : as->props())
                                    if (pr.key != "sound") drawProperty(pr, as);
                                soundPickerCombo("Sound", as->sound);
                                // Editor preview: audition the sound right here
                                // without entering Play (uses the same voice path).
                                ImGui::BeginDisabled(as->sound.empty());
                                if (ImGui::Button("Preview")) startAudioSource(be.id);
                                ImGui::SameLine();
                                if (ImGui::Button("Stop##audiosrc")) stopAudioSource(be.id);
                                ImGui::EndDisabled();
                            } else if (auto* cam = dynamic_cast<CameraComponent*>(c)) {
                                // FOV from metadata; the Main Camera button marks this
                                // the view Play and the exported game start from,
                                // clearing the flag on every other camera. (The raw
                                // activeOnStart bool is hidden: set it here so exactly
                                // one camera can ever be the main one.)
                                for (const Property& pr : cam->props())
                                    if (pr.key != "activeOnStart") drawProperty(pr, cam);
                                if (cam->activeOnStart) {
                                    ImGui::TextColored(ImVec4(0.5f, 0.85f, 1.0f, 1.0f),
                                                       "* Main Camera");
                                    ImGui::SameLine();
                                    if (ImGui::SmallButton("Clear")) {
                                        setMainCamera(-1); mainCamJustSet = true;
                                    }
                                } else if (ImGui::Button("Set as Main Camera")) {
                                    setMainCamera(be.id); mainCamJustSet = true;
                                }
                            } else if (auto* cs = dynamic_cast<CameraSwitcherComponent*>(c)) {
                                // Radius from metadata; Target is a picker over the
                                // scene's Camera entities (plus the player view).
                                for (const Property& pr : cs->props()) drawProperty(pr, cs);
                                const Entity* cur = document.find(cs->target);
                                const std::string label = cur ? cur->name : "(Player view)";
                                if (ImGui::BeginCombo("Target", label.c_str())) {
                                    if (ImGui::Selectable("(Player view)", cs->target < 0))
                                        cs->target = -1;
                                    for (const Entity& ce : entities)
                                        if (ce.components.get<CameraComponent>())
                                            if (ImGui::Selectable(ce.name.c_str(), cs->target == ce.id))
                                                cs->target = ce.id;
                                    ImGui::EndCombo();
                                }
                            } else if (auto* an = dynamic_cast<AnimationComponent*>(c)) {
                                // Speed/playing/loop from metadata; Clip is a picker
                                // over the model's animation clip names.
                                for (const Property& pr : an->props()) drawProperty(pr, an);
                                const auto* mc = be.components.get<ModelComponent>();
                                LoadedModel* lm = mc ? models.byId(mc->modelId) : nullptr;
                                if (lm && lm->animData && !lm->animData->animations.empty()) {
                                    const auto& clips = lm->animData->animations;
                                    an->clip = glm::clamp(an->clip, 0,
                                                          static_cast<int>(clips.size()) - 1);
                                    if (ImGui::BeginCombo("Clip", clips[an->clip].name.c_str())) {
                                        for (int i = 0; i < static_cast<int>(clips.size()); ++i)
                                            if (ImGui::Selectable(clips[i].name.c_str(), an->clip == i))
                                                { an->clip = i; an->time = 0.0f; }
                                        ImGui::EndCombo();
                                    }
                                } else {
                                    ImGui::TextDisabled("No animated model on this entity.");
                                }
                            } else if (auto* at = dynamic_cast<AnimationTriggerComponent*>(c)) {
                                // Radius/once from metadata; Target = a picker over
                                // the scene's entities that have an Animation.
                                for (const Property& pr : at->props()) drawProperty(pr, at);
                                const Entity* cur = document.find(at->target);
                                const std::string label = cur ? cur->name : "(none)";
                                if (ImGui::BeginCombo("Target", label.c_str())) {
                                    if (ImGui::Selectable("(none)", at->target < 0))
                                        at->target = -1;
                                    for (const Entity& te : entities)
                                        if (te.components.get<AnimationComponent>())
                                            if (ImGui::Selectable(te.name.c_str(), at->target == te.id))
                                                at->target = te.id;
                                    ImGui::EndCombo();
                                }
                            } else if (auto* dop = dynamic_cast<DoorOpenerComponent*>(c)) {
                                // Radius/stayOpen from metadata; Target = a Door
                                // entity ((self) for the door this is attached to).
                                for (const Property& pr : dop->props()) drawProperty(pr, dop);
                                const Entity* cur = document.find(dop->target);
                                const std::string label = cur ? cur->name : "(self)";
                                if (ImGui::BeginCombo("Target door", label.c_str())) {
                                    if (ImGui::Selectable("(self)", dop->target < 0))
                                        dop->target = -1;
                                    for (const Entity& te : entities)
                                        if (te.components.get<DoorComponent>())
                                            if (ImGui::Selectable(te.name.c_str(), dop->target == te.id))
                                                dop->target = te.id;
                                    ImGui::EndCombo();
                                }
                            } else if (auto* vh = dynamic_cast<VehicleComponent*>(c)) {
                                // Props + wheel-slot pickers + re-detect
                                // (see VehicleTool).
                                vehicleui::inspector(*vh, be, document);
                            } else {
                                for (const Property& pr : c->props()) drawProperty(pr, c);
                            }
                            ImGui::PopID();
                            if (remove) {
                                be.components.items.erase(be.components.items.begin() + ci);
                                break;
                            }
                        }
                        if (ImGui::Button("Add Component")) ImGui::OpenPopup("addcomp");
                        if (ImGui::BeginPopup("addcomp")) {
                            for (const components::TypeInfo& t : components::registry())
                                if (t.addable && ImGui::Selectable(t.displayName.c_str()))
                                    be.components.items.push_back(t.make());
                            ImGui::EndPopup();
                        }
                    }
                    // Commit the inspector interaction as one undoable step. Begin
                    // when a field is first touched, commit when nothing is active.
                    // (Re-check selection: Delete##insp above may have cleared it.)
                    if (entitySel >= 0 && entitySel < static_cast<int>(entities.size())) {
                        const int  selId      = entities[entitySel].id;
                        const bool inspActive = ImGui::IsAnyItemActive();
                        if (mainCamJustSet) {
                            // setMainCamera() already pushed its own undo step (over
                            // all cameras); don't let this wrapper log a duplicate.
                            inspEditId = -1;
                        } else if (inspActive && inspEditId != selId) {
                            inspEditId     = selId;
                            inspEditIds    = inspFrameIds;
                            inspEditBefore = inspFrameStart;
                        } else if (!inspActive && inspEditId == selId) {
                            inspEditId = -1;
                            auto cmd = std::make_unique<ModifyEntitiesCmd>(
                                inspEditBefore, snapshotEntities(inspEditIds));
                            if (!cmd->trivial()) history.push(std::move(cmd), document);
                        }
                    }
                } else {
                    ImGui::TextDisabled("Select an object in the Hierarchy or viewport.");
                }
                ImGui::SeparatorText("New block defaults");
                ImGui::SliderFloat3("Size", &entityNewHalf.x, 0.25f, 12.0f, "%.2f m");
                if (ImGui::Button("Materials...")) showMaterials = true;
                ImGui::SameLine();
                if (ImGui::Button("Models...")) showModels = true;
                ImGui::TextDisabled("Walk into blocks in FPS mode (F).");
            }
            ImGui::End();

            // Material library: create/edit reusable surface materials. Solids are
            // assigned one via the Inspector; edits here update every mesh using it.
            if (showMaterials) {
                if (ImGui::Begin("Materials", &showMaterials)) {
                    if (ImGui::Button("New")) {
                        matSel = static_cast<int>(materials.size());
                        document.addMaterial("Material " + std::to_string(materials.size()),
                                             glm::vec3(0.7f), 0.0f, 0.2f);
                    }
                    ImGui::SameLine();
                    const bool selFromModel = matSel >= 0 &&
                        matSel < static_cast<int>(materials.size()) &&
                        materials[matSel].fromModel;
                    // Model materials are owned by their model -> not deletable here.
                    ImGui::BeginDisabled(materials.size() <= 1 || selFromModel);
                    if (ImGui::Button("Delete") && materials.size() > 1 && !selFromModel) {
                        const AssetId removedId = materials[matSel].assetId;
                        materials.erase(materials.begin() + matSel);
                        // Meshes that used it fall back to the first material.
                        for (Entity& e : entities)
                            if (auto* mc = e.components.get<MaterialComponent>();
                                mc && mc->material == removedId)
                                mc->material = materials[0].assetId;
                        matSel = glm::clamp(matSel, 0,
                                            static_cast<int>(materials.size()) - 1);
                    }
                    ImGui::EndDisabled();

                    ImGui::Separator();
                    for (int i = 0; i < static_cast<int>(materials.size()); ++i) {
                        const std::string lbl = materials[i].name + "##m" + std::to_string(i);
                        if (ImGui::Selectable(lbl.c_str(), i == matSel)) matSel = i;
                    }
                    ImGui::Separator();

                    if (matSel >= 0 && matSel < static_cast<int>(materials.size())) {
                        MaterialDef& md = materials[matSel];
                        char mbuf[64];
                        std::snprintf(mbuf, sizeof(mbuf), "%s", md.name.c_str());
                        if (ImGui::InputText("Name", mbuf, sizeof(mbuf))) md.name = mbuf;
                        if (md.fromModel)
                            ImGui::TextDisabled(md.tex ? "From model (textured)"
                                                       : "From model");
                        // A textured material samples its base-colour map, so the
                        // flat albedo is unused; only show it for untextured ones.
                        if (!md.tex)
                            ImGui::ColorEdit3("Albedo", &md.albedo.x);
                        ImGui::SliderFloat("Reflectivity", &md.reflectivity, 0.0f, 1.0f);
                        ImGui::SliderFloat("Roughness", &md.roughness, 0.0f, 1.0f);
                        ImGui::SliderFloat("Opacity", &md.opacity, 0.0f, 1.0f);
                        // Texture-alpha handling ("transparency map"). Cutout
                        // masks (hard edges); Blend alpha-blends (soft/glassy).
                        const char* alphaModes[] = { "Opaque", "Cutout", "Blend" };
                        int am = static_cast<int>(md.alphaMode);
                        if (ImGui::Combo("Alpha mode", &am, alphaModes, 3))
                            md.alphaMode = static_cast<AlphaMode>(am);
                        if (md.alphaMode == AlphaMode::Cutout)
                            ImGui::SliderFloat("Cutoff", &md.alphaCutoff, 0.0f, 1.0f);
                        if (md.alphaMode != AlphaMode::Opaque && !md.tex)
                            ImGui::TextDisabled("(needs a base texture with an alpha channel)");
                        ImGui::Checkbox("Glass", &md.glass);
                        if (md.glass) {
                            ImGui::SameLine();
                            ImGui::TextDisabled("(clear centre, reflective rim)");
                        }
                        // Emission: self-illumination (glow). Colour + strength apply
                        // to all materials; the optional emission-map slot (below,
                        // file-backed materials only) restricts the glow to its texels.
                        ImGui::ColorEdit3("Emission", &md.emission.x);
                        ImGui::SliderFloat("Emission strength", &md.emissionStrength,
                                           0.0f, 8.0f);
                        if (md.emission != glm::vec3(0.0f))
                            ImGui::TextDisabled("Strength >~1.5 makes the glow bloom "
                                                "into the surroundings.");
                        // Base-colour texture slot: drop a Texture asset here from
                        // the Assets browser. File-backed textures persist by GUID
                        // (md.texId) into the .fmat; model-embedded ones don't.
                        if (!md.fromModel) {
                            std::string slot = "(none)";
                            if (md.texId.valid()) {
                                const AssetDatabase::Entry* te = assetDb.entry(md.texId);
                                slot = te ? te->relPath : md.texId.toString();
                            }
                            ImGui::Text("Base texture:");
                            ImGui::SameLine();
                            texSwatch(md.tex, md.texId);
                            ImGui::Button((slot + "##texslot").c_str());
                            if (ImGui::BeginDragDropTarget()) {
                                if (const ImGuiPayload* pl =
                                        ImGui::AcceptDragDropPayload("ASSET_GUID")) {
                                    const AssetId gid = AssetId::fromString(std::string(
                                        static_cast<const char*>(pl->Data), pl->DataSize));
                                    if (assetDb.typeForId(gid) == AssetType::Texture) {
                                        md.texId = gid;
                                        md.tex   = assetDb.loadTexture(gid);
                                    }
                                }
                                ImGui::EndDragDropTarget();
                            }
                            if (md.texId.valid()) {
                                ImGui::SameLine();
                                if (ImGui::SmallButton("Clear##tex")) {
                                    md.texId = {};
                                    md.tex.reset();
                                }
                            }

                            // Normal map slot (tangent-space, OpenGL convention).
                            std::string nslot = "(none)";
                            if (md.normalTexId.valid()) {
                                const AssetDatabase::Entry* ne = assetDb.entry(md.normalTexId);
                                nslot = ne ? ne->relPath : md.normalTexId.toString();
                            }
                            ImGui::Text("Normal map:");
                            ImGui::SameLine();
                            texSwatch(md.normalTex, md.normalTexId);
                            ImGui::Button((nslot + "##nrmslot").c_str());
                            if (ImGui::BeginDragDropTarget()) {
                                if (const ImGuiPayload* pl =
                                        ImGui::AcceptDragDropPayload("ASSET_GUID")) {
                                    const AssetId gid = AssetId::fromString(std::string(
                                        static_cast<const char*>(pl->Data), pl->DataSize));
                                    if (assetDb.typeForId(gid) == AssetType::Texture) {
                                        md.normalTexId = gid;
                                        md.normalTex   = assetDb.loadTexture(gid);
                                    }
                                }
                                ImGui::EndDragDropTarget();
                            }
                            if (md.normalTexId.valid()) {
                                ImGui::SameLine();
                                if (ImGui::SmallButton("Clear##nrm")) {
                                    md.normalTexId = {};
                                    md.normalTex.reset();
                                }
                            }

                            // Emission map slot (Unity _Illum): masks the glow.
                            std::string eslot = "(none)";
                            if (md.emissionTexId.valid()) {
                                const AssetDatabase::Entry* ee = assetDb.entry(md.emissionTexId);
                                eslot = ee ? ee->relPath : md.emissionTexId.toString();
                            }
                            ImGui::Text("Emission map:");
                            ImGui::SameLine();
                            texSwatch(md.emissionTex, md.emissionTexId);
                            ImGui::Button((eslot + "##emslot").c_str());
                            if (ImGui::BeginDragDropTarget()) {
                                if (const ImGuiPayload* pl =
                                        ImGui::AcceptDragDropPayload("ASSET_GUID")) {
                                    const AssetId gid = AssetId::fromString(std::string(
                                        static_cast<const char*>(pl->Data), pl->DataSize));
                                    if (assetDb.typeForId(gid) == AssetType::Texture) {
                                        md.emissionTexId = gid;
                                        md.emissionTex   = assetDb.loadTexture(gid);
                                    }
                                }
                                ImGui::EndDragDropTarget();
                            }
                            if (md.emissionTexId.valid()) {
                                ImGui::SameLine();
                                if (ImGui::SmallButton("Clear##em")) {
                                    md.emissionTexId = {};
                                    md.emissionTex.reset();
                                }
                            }
                        }
                        ImGui::TextDisabled("Reflectivity mirrors the scene (env probe).");
                    }
                }
                ImGui::End();
            }

            // Model import: list glTF/GLB files under models/ and drop one into
            // the scene in front of the camera as a Model entity.
            if (showModels) {
                if (ImGui::Begin("Models", &showModels)) {
                    ImGui::TextDisabled("Import glTF/GLB from the models/ folder.");
                    std::error_code mec;
                    std::vector<std::string> files;
                    for (const auto& e :
                         std::filesystem::directory_iterator(modelDir, mec)) {
                        if (!e.is_regular_file()) continue;
                        std::string ext = e.path().extension().string();
                        for (char& c : ext) c = static_cast<char>(std::tolower(
                            static_cast<unsigned char>(c)));
                        if (ext == ".glb" || ext == ".gltf" || ext == ".dae" || ext == ".fbx")
                            files.push_back(e.path().filename().string());
                    }
                    std::sort(files.begin(), files.end());
                    for (const std::string& f : files)
                        if (ImGui::Selectable(f.c_str(), modelFile == f)) modelFile = f;

                    ImGui::Separator();
                    ImGui::BeginDisabled(modelFile.empty());
                    if (ImGui::Button("Import to scene")) {
                        const std::string path = modelDir + "/" + modelFile;
                        const glm::vec3 p = camera.position() + camera.front() * 8.0f;
                        const glm::vec3 g(p.x, streamer.heightAt(p.x, p.z), p.z);
                        if (isStructuredModel(path)) addModelHierarchy(g, path);
                        else {
                            const int id = models.import(path, assetDb, materials);
                            if (id >= 0) addModelEntity(g, id);
                        }
                    }
                    ImGui::EndDisabled();
                    ImGui::TextDisabled("%d model(s) loaded.",
                                        static_cast<int>(models.size()));
                }
                ImGui::End();
            }

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
                        if (!ImGui::CollapsingHeader(hdr.c_str(),
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
                                    : e->type == AssetType::Sound ? "SND" : "?";
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
                    if (compOpen && winFocused && !compItems.empty()) {
                        const int n = static_cast<int>(compItems.size());
                        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
                            compSel = (compSel + 1) % n; suppressKb = true;
                        } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
                            compSel = (compSel - 1 + n) % n; suppressKb = true;
                        } else if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
                            acceptComp = true; suppressKb = true;
                        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                            compOpen = false; suppressKb = true;
                            compManualClose = true; compClosedPrefix = compPrefix;
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
                    if (acceptComp && compSel >= 0 && compSel < static_cast<int>(compItems.size())) {
                        const std::string full = compItems[compSel].text;
                        if (full.size() > compPrefix.size())
                            luaEditor.InsertText(full.substr(compPrefix.size()));
                        compOpen = false; editorDirty = true;
                    }

                    // Recompute candidates from the new cursor/text (skip on the
                    // frame we suppressed the editor, so navigation/dismiss stick).
                    if (!winFocused) compOpen = false;
                    else if (!suppressKb) recomputeCompletion();

                    // Completion popup, best-effort anchored under the caret and
                    // clamped inside the editor rect.
                    if (compOpen && !compItems.empty()) {
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
                            for (int i = 0; i < static_cast<int>(compItems.size()); ++i) {
                                const bool sel = (i == compSel);
                                if (mono) ImGui::PushFont(mono);
                                if (ImGui::Selectable(compItems[i].text, sel)) {
                                    const std::string full = compItems[i].text;
                                    if (full.size() > compPrefix.size())
                                        luaEditor.InsertText(full.substr(compPrefix.size()));
                                    compOpen = false; editorDirty = true;
                                }
                                if (mono) ImGui::PopFont();
                                if (compItems[i].hint && compItems[i].hint[0]) {
                                    ImGui::SameLine();
                                    ImGui::TextDisabled("%s", compItems[i].hint);
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
                                    hdriLoaded = label;
                                    iblEnabled = true;
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
                ImGui::SeparatorText("Play start");
                ImGui::Checkbox("Start Play in vehicle mode", &startInVehicleMode);
                ImGui::Checkbox("Show crosshair", &showCrosshair);

                ImGui::SeparatorText("Skid marks");
                ImGui::Checkbox("Enable skid marks", &skids.enabled);
                ImGui::BeginDisabled(!skids.enabled);
                ImGui::SliderFloat("Slip threshold", &skids.slipThresh, 0.1f, 1.5f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How much a wheel must slip (lock/spin/drift)\n"
                                      "before it leaves a mark (lower = more marks).");
                ImGui::SliderFloat("Mark width", &skids.markHalfW, 0.05f, 0.6f, "%.2f m");
                ImGui::SliderFloat("Darkness", &skids.opacity, 0.1f, 1.0f, "%.2f");
                ImGui::EndDisabled();

                // Scene vehicles: hook a model into the vehicle system with one
                // click. The auto-setup edit goes through the undo history.
                auto makeDrivable = [&](int rootId) -> std::string {
                    Entity* e = document.find(rootId);
                    if (!e) return std::string();
                    const Entity before = *e;
                    std::string rep = vehicleui::autoSetup(document, rootId);
                    if (Entity* after = document.find(rootId)) {
                        auto cmd = std::make_unique<ModifyEntityCmd>(before, *after);
                        if (!cmd->trivial()) history.push(std::move(cmd), document);
                    }
                    return rep;
                };
                const int selId =
                    (entitySel >= 0 && entitySel < static_cast<int>(entities.size()))
                        ? entities[entitySel].id : -1;
                const int pick = vehicleui::panelSection(document, selId, makeDrivable);
                if (pick >= 0) entitySel = document.indexOf(pick);

                ImGui::SeparatorText("Test car");
                ImGui::Checkbox("Show vehicle", &showVehicle);
                if (ImGui::Button("Place at camera")) placeCar();
                if (carPlaced) ImGui::Text("Speed: %.0f km/h", std::abs(carSpeed) * 3.6f);
                else           ImGui::TextDisabled("Vehicle not placed yet");
            }
            ImGui::End(); }

            } // end editor UI (skipped in presentation mode)
#endif // !FITZEL_PLAYER

            // Push the (possibly edited) terrain params into the material, plus
            // the texture layers: bind each layer with a texture to its own unit
            // (3..3+N-1) and upload its height/slope band + tiling. Layers without
            // a texture are skipped, so uLayerCount is the bound count.
            terrainMat.set("uDetailScale", look.detailScale)
                      .set("uDetailStrength", look.detailStrength)
                      .set("uTerrainSpec", look.gloss)
                      .set("uTexScale", texScale)
                      .set("uNormalStrength", normalStrength)
                      .set("uWaterLevel", waterLevel)
                      .set("uWetness", roadWetness)
                      .set("uAlbedo", glm::vec3(0.5f)); // neutral grey where no layer covers
            {
                int bound = 0;
                for (const TerrainLayer& L : look.layers) {
                    if (!L.tex || bound >= kMaxTerrainLayers) continue;
                    const std::string ix = std::to_string(bound);
                    terrainMat.setTexture("uLayerTex[" + ix + "]", *L.tex, 3 + bound)
                              .set("uLayerBand[" + ix + "]",
                                   glm::vec4(L.heightStart, L.heightEnd,
                                             L.slopeStart, L.slopeEnd))
                              .set("uLayerScale[" + ix + "]", L.scale);
                    // Optional normal map on a high unit (18+) so it clears the
                    // shadow/env/IBL samplers the renderer binds lower down.
                    if (L.norm) {
                        terrainMat.setTexture("uLayerNorm[" + ix + "]", *L.norm,
                                              18 + bound)
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
            const int   fbW = viewW, fbH = viewH;
            const float aspect = static_cast<float>(fbW) / static_cast<float>(fbH);
            const glm::mat4 proj = camera.projectionMatrix(aspect);

            renderer.setViewport(fbW, fbH);
            renderer.begin(camera, aspect, light);

            for (const TerrainChunk* chunk : streamer.visibleChunks()) {
                renderer.submit(chunk->mesh(), terrainMat, glm::mat4(1.0f), false);
            }

            // The committed road mesh only changes on Build (see the Roads panel);
            // editing shows a live preview instead (drawn in the viewport overlay).
            if (road.enabled && road.verts() > 0) {
                road.material().set("uWaterLevel", waterLevel); // wet-darken submerged
                road.material().set("uWetness", roadWetness);   // rain-wet sheen
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
                road.material().set("uRoadWidth", road.width);
                road.material().set("uRoadUMax",  road.texTile > 1e-4f
                                                      ? road.width / road.texTile : 0.0f);
                renderer.submit(road.mesh(), road.material(), glm::mat4(1.0f), false,
                                false, 1.0f, /*forceTransparent=*/roadFades);
            }

            // Bridge decks, built by the same Build as the road they carry. Unlike
            // the ribbon these cast shadows: there is ground under them to fall on.
            if (road.enabled && road.hasBridges()) {
                road.bridgeMaterial().set("uWaterLevel", waterLevel);
                road.bridgeMaterial().set("uWetness", roadWetness);
                // A deck is carriageway too: rain hits it like the rest of the road.
                road.bridgeMaterial().set("uRainRings", ringAmount);
                road.bridgeMaterial().set("uTime", static_cast<float>(now));
                renderer.submit(road.bridgeMesh(), road.bridgeMaterial(),
                                glm::mat4(1.0f));
            }

            // Tyre skid marks accumulated while driving (alpha-blended, on ground).
            skids.render(renderer);

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
            //     One GPU material is built per library asset and shared by every
            //     mesh assigned to it; light markers keep their own emissive one.
            //     All frame-local, reserved so pointers stay valid across passes.
            std::vector<Material> gpuMats;
            gpuMats.reserve(materials.size());
            for (const MaterialDef& md : materials) {
                Material& m = gpuMats.emplace_back(lit);
                m.set("uWaterLevel", -1.0e4f)
                 .set("uWetness", roadWetness)
                 .set("uReflectivity", md.reflectivity)
                 .set("uRoughness", md.roughness)
                 .set("uGlass", md.glass ? 1 : 0);
                if (md.tex)
                    m.set("uColorMode", 2).setTexture("uTexture", *md.tex, 0);
                else
                    m.set("uColorMode", 0).set("uAlbedo", md.albedo);
                if (md.normalTex)
                    m.setTexture("uNormalMap", *md.normalTex, 1).set("uHasNormalMap", 1);
                else
                    m.set("uHasNormalMap", 0);
                // Cutout ("transparency map"): let the shader discard masked
                // texels. Blend routes through the transparent queue at submit.
                if (md.alphaMode == AlphaMode::Cutout)
                    m.set("uAlphaCutout", 1).set("uAlphaCutoff", md.alphaCutoff);
                else
                    m.set("uAlphaCutout", 0);
                // Emission (self-illumination): colour * strength, optionally
                // masked by an _Illum map (unit 3 -- free for object materials).
                m.set("uEmission", md.emission)
                 .set("uEmissionStrength", md.emissionStrength);
                if (md.emissionTex)
                    m.setTexture("uEmissionMap", *md.emissionTex, 3).set("uHasEmissionMap", 1);
                else
                    m.set("uHasEmissionMap", 0);
            }
            std::vector<Material> lightMats;
            lightMats.reserve(entities.size());
            for (const Entity& b : entities) {
                if (!b.activeInHierarchy) continue;         // deactivated: hidden
                if (b.type == EntityType::Sun) continue;   // directional, no geometry
                if (b.type == EntityType::Empty) continue;  // grouping node, no geometry
                // Player-start markers are authoring aids -- hidden while playing.
                if (playMode && b.components.get<PlayerStartComponent>()) continue;
                // Light markers (the glowing cube) are authoring aids too: hide them
                // while playing so headlights etc. don't show a box -- the light
                // itself still shines (collected further below).
                if (playMode && b.type == EntityType::Light) continue;
                if (b.type == EntityType::Model) {
                    // Imported model: draw every primitive with its baked material.
                    // Centre the model's AABB at b.center (so it matches the pick
                    // box), then translate/scale into the world.
                    const auto* mdl = b.components.get<ModelComponent>();
                    LoadedModel* lm = mdl ? models.byId(mdl->modelId) : nullptr;
                    if (!lm) continue;
                    // Derive the scale from the entity's AABB half-extents so the
                    // model fills center +/- half exactly: this makes the Scale
                    // gizmo (which writes half) and the pick box work like a
                    // primitive, in addition to the inspector's Scale slider.
                    const glm::vec3 sz = glm::max(lm->size(), glm::vec3(1e-4f));
                    const glm::mat4 mm =
                        composeModel(b.center, b.rotation, (b.half * 2.0f) / sz) *
                        glm::translate(glm::mat4(1.0f), -lm->center());
                    for (std::size_t i = 0; i < lm->meshes.size(); ++i) {
                        const int mi = document.materialIndex(lm->primMaterialId[i]);
                        renderer.submit(lm->meshes[i], gpuMats[mi], mm, true,
                                        materials[mi].reflectivity > 0.0f,
                                        materials[mi].opacity,
                                        materials[mi].alphaMode == AlphaMode::Blend);
                    }
                    continue;
                }
                const Mesh& mesh = (b.type == EntityType::Ramp)     ? rampMesh
                                 : (b.type == EntityType::Cylinder) ? cylMesh
                                 : (b.type == EntityType::Sphere)   ? sphereMesh
                                                                    : carCube;
                const glm::mat4 m = composeModel(b.center, b.rotation, b.half * 2.0f);
                if (b.type == EntityType::Light) {
                    // Light markers glow (emissive-ish). A marker sits on its own
                    // light position, so it must NOT cast into that light's shadow
                    // cube (it would wrap the light in a caster and go dark).
                    const auto* lc = b.components.get<LightComponent>();
                    const glm::vec3 lcol = lc ? lc->color : glm::vec3(1.0f);
                    Material& mat = lightMats.emplace_back(lit);
                    mat.set("uColorMode", 0).set("uWaterLevel", -1.0e4f)
                       .set("uWetness", 0.0f) // markers glow, never wet
                       .set("uAlbedo", lcol * 1.5f).set("uReflectivity", 0.0f);
                    renderer.submit(mesh, mat, m, /*castsPointShadow=*/false);
                } else {
                    // Assigned library material; reflective solids are excluded
                    // from the env probe so they don't reflect their own interior.
                    const auto* mc = b.components.get<MaterialComponent>();
                    const int mi = document.materialIndex(mc ? mc->material : AssetId{});
                    renderer.submit(mesh, gpuMats[mi], m, true,
                                    materials[mi].reflectivity > 0.0f,
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
            renderer.setPointLights(pointLights);
            renderer.setSpotLights(spotLights);
            renderer.preparePointShadows(); // omni shadow cubemaps (opt-in lights)

            // --- Multi-pass render with sky and planar water ------------
            // Trees cast shadows: drawn into every cascade via this callback.
            auto treeShadowCaster = [&](const glm::mat4& lightSpace) {
                veg.drawTreeShadow(lightSpace, now, weather);
            };
            renderer.prepareShadows(treeShadowCaster); // shadows from the real camera

            const glm::vec3& camPos = camera.position();
            const glm::mat4  view   = camera.viewMatrix();
            const glm::mat4  mainVP = proj * view;

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
                sky.setFloat("uCoverage", glm::mix(0.62f, 0.10f, effCoverage));
                sky.setFloat("uCloudDensity", effDensity);
                sky.setFloat("uCloudScale", cloudScale);
                sky.setFloat("uCloudSpeed", effWind);
                sky.setFloat("uCloudBottom", effCloudBot);
                sky.setFloat("uCloudTop", cloudTop);
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
                if (md.reflectivity > 0.0f) { wantProbe = true; break; }
            if (wantProbe) {
                // Capture at the first reflective object if there is one (best
                // parallax there); otherwise around the camera, so reflective
                // terrain / not-yet-placed materials still get a sensible probe.
                glm::vec3 probePos = camPos;
                for (const Entity& b : entities) {
                    const auto* mc = b.components.get<MaterialComponent>();
                    if (b.type != EntityType::Light && b.type != EntityType::Sun && mc &&
                        materials[document.materialIndex(mc->material)].reflectivity > 0.0f) {
                        probePos = b.center;
                        break;
                    }
                }
                renderer.prepareEnvProbe(probePos,
                    [&](const glm::mat4& ivp, const glm::vec3& eye) {
                        drawBackground(ivp, eye, false);
                    });
            }

            // 1) Reflection: sky + scene mirrored across the water plane,
            //    clipping everything below the surface.
            const glm::mat4 mirror =
                glm::translate(glm::mat4(1.0f), {0.0f, 2.0f * waterLevel, 0.0f}) *
                glm::scale(glm::mat4(1.0f), {1.0f, -1.0f, 1.0f});
            const glm::mat4 reflView = view * mirror;
            const glm::vec3 reflEye{camPos.x, 2.0f * waterLevel - camPos.y, camPos.z};

            // Reflection/refraction render LINEAR (tonemap=false) so the water
            // shader can sample and tonemap them once at the end.
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

            // 3) Main pass: sky + full scene rendered LINEAR into the HDR buffer
            //    (tonemapping happens in the composite pass).
            if (hdrRT.width() != fbW || hdrRT.height() != fbH) {
                hdrRT  = RenderTarget(fbW, fbH, RenderTarget::Format::RGBA16F, true);
                ssaoRT = RenderTarget(std::max(1, fbW / 2), std::max(1, fbH / 2));
                viewportRT = RenderTarget(fbW, fbH, RenderTarget::Format::RGBA8);
                postRT = RenderTarget(fbW, fbH, RenderTarget::Format::RGBA8);
            }
            hdrRT.bind();
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            drawBackground(glm::inverse(mainVP), camPos, false);
            renderer.renderScene(view, proj, camPos, Renderer::kNoClip, false);

            // Shared draw context for the lit vegetation (grass, trees, billboards)
            // in this HDR pass.
            const FrameContext gctx =
                makeFrameContext(mainVP, camPos, now, weather, light, fog);
            veg.drawGrass(gctx); // grass into the HDR buffer, lit + fogged

            // Flowers into the HDR buffer, lit + fogged like grass.
            veg.drawFlowers(gctx);

            // Trees (instanced, per-material) + distant billboards into the HDR.
            veg.drawTrees(gctx);
            veg.drawTreeBillboards(gctx, camera.right());

            // Birds: a flock wheeling above the camera, two-sided into the HDR.
            veg.drawBirds(mainVP, now, camPos);

            // 4) The water surface: a large quad following the camera, sampling
            //    the reflection/refraction targets with Fresnel + ripples.
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

            // --- Rain streaks (storm) + boat foam, into the HDR buffer --------
            rain.draw(gctx);
            spray.update(dt, waterLevel); // age the pool, then draw what survived
            spray.draw(gctx);

            // --- Fireflies: night-only glowing wanderers, additive into HDR ---
            veg.drawFireflies(mainVP, now, 1.0f - dayF, camPos);

            // --- SSAO: occlusion from the HDR depth buffer (half-res) ---
            ssaoRT.bind();
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glDisable(GL_CULL_FACE);
            ssao.bind();
            hdrRT.bindDepthTexture(0);
            ssao.setInt("uDepth", 0);
            ssao.setMat4("uProjection", proj);
            ssao.setMat4("uInvProjection", glm::inverse(proj));
            ssao.setFloat("uRadius", ssaoRadius);
            ssao.setFloat("uBias", ssaoBias);
            ssao.setFloat("uPower", ssaoPower);
            // Fade AO out past the near/mid field so distant, low-precision terrain
            // toward the horizon can't band into horizontal stripes.
            ssao.setFloat("uFadeStart", 50.0f);
            ssao.setFloat("uFadeEnd", 120.0f);
            fsQuad.draw();
            glDepthMask(GL_TRUE);
            glEnable(GL_DEPTH_TEST);
            glEnable(GL_CULL_FACE);

            // --- Composite: bloom + god rays + lens flare + tonemap ------
            // Project the sun to screen space for the rays/flare.
            const glm::vec4 sunClip = mainVP * glm::vec4(camPos + sunDir * 3000.0f, 1.0f);
            glm::vec2 sunUV(0.5f);
            float sunOnScreen = 0.0f;
            if (sunClip.w > 1e-4f) {
                sunUV = glm::vec2(sunClip) / sunClip.w * 0.5f + 0.5f;
                if (sunUV.x > -0.3f && sunUV.x < 1.3f &&
                    sunUV.y > -0.3f && sunUV.y < 1.3f && sunDir.y > -0.05f) {
                    sunOnScreen = 1.0f;
                }
            }

            // Composite into an LDR buffer; FXAA then filters it to the target.
            postRT.bind();
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glDisable(GL_CULL_FACE);
            composite.bind();
            hdrRT.bindColorTexture(0);
            composite.setInt("uHdr", 0);
            hdrRT.bindDepthTexture(1);
            composite.setInt("uDepth", 1);
            composite.setFloat("uNear", camera.nearPlane());
            composite.setFloat("uFar", camera.farPlane());
            composite.setFloat("uFocusNear", dofNear);
            composite.setFloat("uFocusFar", dofFar);
            composite.setFloat("uDofMax", dofMax);
            ssaoRT.bindColorTexture(2);
            composite.setInt("uAO", 2);
            composite.setFloat("uAoStrength", ssaoStrength);
            composite.setVec2("uTexel", {1.0f / fbW, 1.0f / fbH});
            composite.setFloat("uAspect", aspect);
            composite.setFloat("uExposure", exposure);
            composite.setVec2("uSunUV", sunUV);
            composite.setFloat("uSunOnScreen", sunOnScreen);
            composite.setVec3("uSunColor", sunCol);
            composite.setFloat("uBloom", bloomIntensity);
            composite.setFloat("uRays", rayIntensity);
            composite.setFloat("uHueShift", hueShift);
            composite.setFloat("uSaturation", saturation);
            composite.setFloat("uValue", valueGain);
            composite.setFloat("uWarmth", warmth);
            composite.setFloat("uContrast", contrast);
            fsQuad.draw();

            // --- FXAA: filter the composite to the viewport texture (editor) or
            //     straight to the screen (presentation mode) ------------------
            if (presentMode) {
                int winW = 0, winH = 0;
                window.framebufferSize(winW, winH);
                RenderTarget::unbind(winW, winH);
            } else {
                viewportRT.bind();
            }
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            fxaa.bind();
            postRT.bindColorTexture(0);
            fxaa.setInt("uImage", 0);
            fxaa.setVec2("uTexel", {1.0f / fbW, 1.0f / fbH});
            fxaa.setInt("uEnabled", fxaaEnabled ? 1 : 0);
            fsQuad.draw();
            glDepthMask(GL_TRUE);
            glEnable(GL_DEPTH_TEST);
            glEnable(GL_CULL_FACE);

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
                if (showCrosshair && !vehicleMode) {
                    const float ch = std::max(10.0f, vsize.y * 0.018f);
                    const ImU32 white = IM_COL32(255, 255, 255, 220);
                    dl->AddLine(ImVec2(c.x - ch, c.y), ImVec2(c.x + ch, c.y), white, 2.0f);
                    dl->AddLine(ImVec2(c.x, c.y - ch), ImVec2(c.x, c.y + ch), white, 2.0f);
                }

                // Score + HUD line, scaled to the view height and drawn with a
                // dark shadow so it stays legible over any scene.
                ImFont* font = ImGui::GetFont();
                const float fs  = glm::clamp(vsize.y * 0.04f, 22.0f, 48.0f);
                const float pad = fs * 0.6f;
                auto shadowText = [&](float x, float y, ImU32 col, const char* s){
                    dl->AddText(font, fs, ImVec2(x + 2.0f, y + 2.0f),
                                IM_COL32(0, 0, 0, 190), s);
                    dl->AddText(font, fs, ImVec2(x, y), col, s);
                };
                char buf[128];
                std::snprintf(buf, sizeof(buf), "Score: %d", host.score);
                shadowText(vmin.x + pad, vmin.y + pad, IM_COL32(255, 232, 120, 255), buf);
                if (!host.hud.empty())
                    shadowText(vmin.x + pad, vmin.y + pad + fs * 1.2f,
                               IM_COL32(235, 235, 240, 235), host.hud.c_str());
                // Boat-mode banner while afloat: centred near the top of the view.
                if (vehicleMode && boatMode) {
                    const char* bm = "~ BOAT MODE ~";
                    const ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, bm);
                    shadowText(c.x - sz.x * 0.5f, vmin.y + pad,
                               IM_COL32(130, 210, 255, 255), bm);
                }
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
                mouseActive || keyHeld || io.WantTextInput ||
                ImGui::IsAnyItemActive() || ImGuizmo::IsUsing() || vehicleMode;
            if (interacting) lastActive = now;
            activeFrame = (now - lastActive) < kIdleGrace;

#ifndef FITZEL_PLAYER
            // Whatever the Assets panel didn't claim was dropped somewhere else (or
            // while it was closed). Drop it on the floor rather than let it queue up
            // and ride along with the next drop, which would import the wrong files
            // at the wrong moment.
            g_fileDrop.paths.clear();
#endif
            gui.endFrame();
            window.swapBuffers();
        }

    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        return 1;
    }

    return 0;
}
