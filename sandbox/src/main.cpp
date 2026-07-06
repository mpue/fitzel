#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <future>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <imgui.h>
#include <imgui_internal.h> // DockBuilder API for the default panel layout
#include <ImGuizmo.h>       // 3D transform gizmos in the viewport
#include <TextEditor.h>     // ImGuiColorTextEdit: the Lua script editor
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
#include "TerrainPanel.hpp"
#include "FolderDialog.hpp"

using namespace fitzel;

// On laptops with hybrid graphics (NVIDIA Optimus / AMD PowerXpress), ask the
// driver to run us on the discrete high-performance GPU instead of the iGPU.
#if defined(_WIN32)
extern "C" {
    __declspec(dllexport) unsigned long NvOptimusEnablement = 1;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

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
        glm::vec3 waterColor{0.10f, 0.30f, 0.38f};
        float     waveStrength = 0.018f;
        float     waveScale    = 0.06f;
        float     foamWidth    = 2.5f;
        float     waveHeight   = 0.6f; // Gerstner swell amplitude
        float     waveChoppy   = 0.6f;

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

        // Rain: falling line streaks in a box that follows the camera.
        Shader rain = Shader::fromFiles("assets/shaders/rain.vert",
                                        "assets/shaders/rain.frag");
        if (!rain.isValid()) { std::fprintf(stderr, "Failed to load rain shader\n"); return 1; }
        const int   rainDrops   = 14000;
        const float rainBoxHalf = 55.0f;
        const float rainBoxH    = 95.0f;
        GLuint rainVAO = 0, rainVBO = 0;
        {
            std::mt19937 rr(99u);
            std::uniform_real_distribution<float> u(0.0f, 1.0f);
            std::vector<float> data;
            data.reserve(static_cast<std::size_t>(rainDrops) * 2 * 5);
            for (int i = 0; i < rainDrops; ++i) {
                const float bx = (u(rr) - 0.5f) * 2.0f * rainBoxHalf;
                const float bz = (u(rr) - 0.5f) * 2.0f * rainBoxHalf;
                const float ys = u(rr) * rainBoxH;
                const float sp = glm::mix(30.0f, 55.0f, u(rr));
                data.insert(data.end(), {bx, ys, bz, sp, 0.0f});
                data.insert(data.end(), {bx, ys, bz, sp, 1.0f});
            }
            glGenVertexArrays(1, &rainVAO);
            glBindVertexArray(rainVAO);
            glGenBuffers(1, &rainVBO);
            glBindBuffer(GL_ARRAY_BUFFER, rainVBO);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(data.size() * sizeof(float)),
                         data.data(), GL_STATIC_DRAW);
            const GLsizei stride = 5 * sizeof(float);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void*)(4 * sizeof(float)));
            glBindVertexArray(0);
        }

        // --- Grass: GPU-instanced blades placed on suitable terrain ------
        Shader grass = Shader::fromFiles("assets/shaders/grass.vert",
                                         "assets/shaders/grass.frag");
        if (!grass.isValid()) { std::fprintf(stderr, "Failed to load grass shader\n"); return 1; }
        // base: aBlade(x,h01) triangle strip ; instance: iPos3, iRot, iHeight,
        // iPhase, iLush.
        const float blade[] = {
            -0.5f, 0.0f,  0.5f, 0.0f,  -0.45f, 0.33f,  0.45f, 0.33f,
            -0.30f, 0.66f, 0.30f, 0.66f,  0.0f, 1.0f };
        InstancedMesh grassField = InstancedMesh::create(
            blade, sizeof(blade) / sizeof(float), 2 * sizeof(float), {{0, 2, 0}},
            7 * sizeof(float),
            {{1, 3, 0}, {2, 1, 3 * sizeof(float)}, {3, 1, 4 * sizeof(float)},
             {4, 1, 5 * sizeof(float)}, {5, 1, 6 * sizeof(float)}});
        int       grassCount   = 0;
        glm::vec2 grassCenter(1e9f); // forces generation on the first frame
        bool      grassEnabled = true;
        float     grassHeight  = 0.35f;
        float     grassDensity = 1.0f;
        float     grassRadius  = 46.0f;
        glm::vec3 grassTint(1.0f, 1.0f, 1.0f);

        // Grass placement runs on a worker thread (pure noise queries, no GL) so
        // moving never stalls the frame; the result is uploaded on the main thread.
        std::future<std::vector<float>> grassFuture;
        bool          grassPending = false;
        bool          grassDirty   = true; // force the first generation
        glm::vec2     grassPendingCenter(0.0f);
        std::uint32_t grassSeed = 1234u;
        auto computeGrass = [](TerrainSettings s, glm::vec2 c, float waterLvl,
                               float snowLvl, float gHeight, float gDensity,
                               float R, std::uint32_t seed,
                               std::vector<glm::vec2> road, float roadClear)
                               -> std::vector<float> {
            std::vector<float> out;
            std::mt19937 rng(seed);
            std::uniform_real_distribution<float> u(0.0f, 1.0f);
            const float spacing = 0.6f; // sampling grid (one ground query per cell)
            const int   per = std::max(1, static_cast<int>(120.0f * gDensity));
            for (float z = -R; z <= R; z += spacing) {
                for (float x = -R; x <= R; x += spacing) {
                    if (x * x + z * z > R * R) continue;
                    const float wx = c.x + x, wz = c.y + z;
                    if (roadDistanceSq(road, wx, wz) < roadClear * roadClear) continue;
                    const float h = terrainHeight(s, wx, wz);
                    if (h < waterLvl + 0.5f || h > snowLvl - 1.5f) continue;
                    const float e = 1.0f;
                    const glm::vec3 n = glm::normalize(glm::vec3(
                        terrainHeight(s, wx - e, wz) - terrainHeight(s, wx + e, wz),
                        2.0f * e,
                        terrainHeight(s, wx, wz - e) - terrainHeight(s, wx, wz + e)));
                    if (n.y < 0.82f) continue;
                    const float lush = glm::clamp(
                        terrainMoisture(s, wx, wz)
                            - glm::smoothstep(snowLvl - 8.0f, snowLvl, h) * 0.5f,
                        0.0f, 1.0f);
                    if (lush < 0.22f) continue;
                    // Meadow patchiness: clumps of dense/thin grass and bare gaps.
                    const float patch = valNoise2(wx * 0.05f, wz * 0.05f);
                    const float bare  = valNoise2(wx * 0.13f + 19.0f, wz * 0.13f + 7.0f);
                    if (bare < 0.26f) continue; // bare ground -> no grass in this cell
                    const float dens  = glm::mix(0.25f, 1.25f, patch);
                    const float dist  = std::sqrt(x * x + z * z);
                    const float rim   = 1.0f - glm::smoothstep(R * 0.82f, R, dist);
                    // Distance LOD: thin out far cells (most of the area) hard --
                    // near grass stays full, distant grass is a fraction. Big win.
                    const float lod   = glm::mix(1.0f, 0.22f,
                                                 glm::smoothstep(0.28f, 1.0f, dist / R));
                    const int   count = static_cast<int>(per * dens
                                        * glm::mix(0.35f, 1.0f, lush) * rim * lod);
                    for (int b = 0; b < count; ++b) {
                        // Scatter well past the cell so the sampling grid vanishes.
                        // Height follows the patch (taller clumps) plus per-blade jitter.
                        const float bh = gHeight * glm::mix(0.65f, 1.25f, patch)
                                                 * glm::mix(0.8f, 1.1f, u(rng));
                        out.insert(out.end(), {
                            wx + (u(rng) - 0.5f) * spacing * 2.2f, h,
                            wz + (u(rng) - 0.5f) * spacing * 2.2f,
                            u(rng) * 6.2831f,
                            bh,
                            u(rng) * 6.2831f,
                            glm::clamp(lush + (patch - 0.5f) * 0.4f
                                            + (u(rng) - 0.5f) * 0.12f, 0.0f, 1.0f)});
                    }
                }
            }
            return out;
        };

        // --- Trees: instanced low-poly pines (trunk + 3 foliage cones) ---
        Shader tree = Shader::fromFiles("assets/shaders/tree.vert",
                                        "assets/shaders/tree.frag");
        Shader treeDepth = Shader::fromFiles("assets/shaders/treedepth.vert",
                                             "assets/shaders/treedepth.frag");
        if (!tree.isValid() || !treeDepth.isValid()) {
            std::fprintf(stderr, "Failed to load tree shaders\n"); return 1;
        }
        // Load the textured, multi-material tree model and split it into draw
        // groups (one per material/texture). Geometry is normalized to unit
        // height; the instance scale gives the actual world size.
        struct TreePrim { Texture tex; bool hasTex = false; int first = 0; int count = 0; bool cutout = false; };
        std::vector<TreePrim> treePrims;
        std::vector<float>    treeVerts; // combined: pos3 normal3 uv2
        const float treeLocalHeight = 1.0f;
        float       treeSize = 9.0f;     // average tree height (world units)
        showProgress(0.55f, "Loading tree model...");
        {
            ModelData md = loadGltf(modelDir + "/tree1.glb");
            if (md.empty() || md.height() < 0.01f) {
                std::fprintf(stderr, "Tree model failed to load\n");
            } else {
                const float scale = 1.0f / md.height();
                for (ModelPrimitive& p : md.primitives) {
                    TreePrim tp;
                    tp.first  = static_cast<int>(treeVerts.size() / 8);
                    tp.count  = p.vertexCount();
                    tp.cutout = p.alphaCutout;
                    tp.hasTex = !p.texPixels.empty();
                    if (tp.hasTex)
                        tp.tex = Texture::fromPixels(p.texPixels.data(), p.texWidth, p.texHeight, 4);
                    for (std::size_t i = 0; i + 7 < p.vertices.size(); i += 8) {
                        treeVerts.push_back(p.vertices[i + 0] * scale);
                        treeVerts.push_back((p.vertices[i + 1] - md.minY) * scale);
                        treeVerts.push_back(p.vertices[i + 2] * scale);
                        treeVerts.push_back(p.vertices[i + 3]);
                        treeVerts.push_back(p.vertices[i + 4]);
                        treeVerts.push_back(p.vertices[i + 5]);
                        treeVerts.push_back(p.vertices[i + 6]);
                        treeVerts.push_back(p.vertices[i + 7]);
                    }
                    treePrims.push_back(std::move(tp));
                }
                std::printf("[Fitzel] tree model: %d primitives, %d verts\n",
                            static_cast<int>(treePrims.size()),
                            static_cast<int>(treeVerts.size() / 8));
            }
        }
        GLuint treeVAO = 0, treeVBO = 0, treeInstVBO = 0;
        {
            glGenVertexArrays(1, &treeVAO);
            glBindVertexArray(treeVAO);
            glGenBuffers(1, &treeVBO);
            glBindBuffer(GL_ARRAY_BUFFER, treeVBO);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(treeVerts.size() * sizeof(float)),
                         treeVerts.data(), GL_STATIC_DRAW);
            const GLsizei ms = 8 * sizeof(float);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, ms, (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, ms, (void*)(3 * sizeof(float)));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, ms, (void*)(6 * sizeof(float)));
            glGenBuffers(1, &treeInstVBO);
            glBindBuffer(GL_ARRAY_BUFFER, treeInstVBO);
            const GLsizei is = 5 * sizeof(float);
            glEnableVertexAttribArray(3);
            glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, is, (void*)0);
            glVertexAttribDivisor(3, 1);
            glEnableVertexAttribArray(4);
            glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, is, (void*)(3 * sizeof(float)));
            glVertexAttribDivisor(4, 1);
            glEnableVertexAttribArray(5);
            glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, is, (void*)(4 * sizeof(float)));
            glVertexAttribDivisor(5, 1);
            glBindVertexArray(0);
        }

        // Tree billboards (LOD for distant trees): a camera-facing textured quad
        // that reuses the tree instance buffer; the corner comes from gl_VertexID.
        showProgress(0.68f, "Loading tree billboards...");
        Texture billboardTex = Texture::fromFile(texDir + "/billboard_tree_bled.png");
        Shader  billboard = Shader::fromFiles("assets/shaders/billboard.vert",
                                              "assets/shaders/billboard.frag");
        if (!billboard.isValid()) { std::fprintf(stderr, "Failed to load billboard shader\n"); return 1; }
        if (!billboardTex.isValid()) std::fprintf(stderr, "Warning: tree billboard texture missing\n");
        const float bbAspect = (billboardTex.height() > 0)
            ? static_cast<float>(billboardTex.width()) / billboardTex.height() : 0.93f;
        float  lodNear = 45.0f; // 3D trees within this range, billboards beyond
        GLuint bbVAO = 0;
        {
            glGenVertexArrays(1, &bbVAO);
            glBindVertexArray(bbVAO);
            glBindBuffer(GL_ARRAY_BUFFER, treeInstVBO);
            const GLsizei is = 5 * sizeof(float);
            glEnableVertexAttribArray(3);
            glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, is, (void*)0);
            glVertexAttribDivisor(3, 1);
            glEnableVertexAttribArray(4);
            glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, is, (void*)(3 * sizeof(float)));
            glVertexAttribDivisor(4, 1);
            glEnableVertexAttribArray(5);
            glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, is, (void*)(4 * sizeof(float)));
            glVertexAttribDivisor(5, 1);
            glBindVertexArray(0);
        }

        // --- Roads / paths -----------------------------------------------
        // A road is a ribbon mesh lofted along a Catmull-Rom spline through
        // control points, draped onto the terrain and textured with asphalt.
        // Submitted through the Renderer, so it gets lighting/shadows/fog free.
        // Loaded through the asset database (cached/deduplicated): the surface
        // picker below can re-select a texture without re-reading it from disk.
        // Held as a shared handle so the object stays alive while roadMat binds it.
        std::shared_ptr<Texture> roadTex =
            assetDb.loadTexture(texDir + "/asphalt_02_diff_4k.jpg");
        Material roadMat(lit);
        roadMat.set("uColorMode", 2);
        if (roadTex) roadMat.setTexture("uTexture", *roadTex, 0);
        // Selectable surface: gather the diffuse/albedo textures from textures/.
        std::vector<std::string> roadTexFiles;
        int roadTexSel = 0;
        {
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(texDir, ec)) {
                const std::string name = e.path().filename().string();
                const std::string ext  = e.path().extension().string();
                if ((ext == ".jpg" || ext == ".jpeg" || ext == ".png") &&
                    name.find("diff") != std::string::npos)
                    roadTexFiles.push_back(name);
            }
            std::sort(roadTexFiles.begin(), roadTexFiles.end());
            for (int i = 0; i < static_cast<int>(roadTexFiles.size()); ++i)
                if (roadTexFiles[i].find("asphalt") != std::string::npos) roadTexSel = i;
        }
        std::vector<glm::vec2> roadPts;   // control points (world x,z)
        Mesh  roadMesh;
        int   roadVerts    = 0;
        std::vector<glm::vec3>     roadCollVerts;   // road geometry for physics
        std::vector<std::uint32_t> roadCollIndices;
        bool  roadEnabled  = true;
        bool  roadEditMode = false;
        bool  roadDirty    = false;
        int   roadSel      = -1;          // selected control point (-1 = none)
        bool  roadDragging = false;       // dragging the selected handle
        bool  roadVegDirty = false;       // road changed -> clear vegetation on it
        float roadWidth    = 5.0f;
        float roadTexTile  = 8.0f;        // world metres per texture tile
        std::vector<glm::vec2> roadCenterline; // sampled centre (for veg masking)
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
        auto buildRoad = [&] {
            roadDirty = false;
            roadVegDirty = true;  // vegetation must re-evaluate against the new road
            roadCenterline.clear();
            MeshData md;
            const int n = static_cast<int>(roadPts.size());
            if (n < 2) {
                roadMesh = Mesh(); roadVerts = 0;
                roadCollVerts.clear(); roadCollIndices.clear();
                return;
            }

            // Smooth centreline: Catmull-Rom through the points, draped on terrain.
            std::vector<glm::vec3> center;
            const int SUB = 14;
            for (int i = 0; i < n - 1; ++i) {
                const glm::vec2 p0 = roadPts[std::max(0, i - 1)];
                const glm::vec2 p1 = roadPts[i];
                const glm::vec2 p2 = roadPts[i + 1];
                const glm::vec2 p3 = roadPts[std::min(n - 1, i + 2)];
                const int last = (i == n - 2) ? SUB : SUB - 1;
                for (int s = 0; s <= last; ++s) {
                    const glm::vec2 c = catmull(p0, p1, p2, p3, static_cast<float>(s) / SUB);
                    center.push_back({c.x, streamer.heightAt(c.x, c.y), c.y});
                }
            }

            // Keep the flat centreline for vegetation masking.
            roadCenterline.reserve(center.size());
            for (const glm::vec3& p : center) roadCenterline.push_back({p.x, p.z});

            // Loft left/right edges perpendicular to the path (in the XZ plane).
            const float half = roadWidth * 0.5f;
            float vlen = 0.0f;
            for (size_t i = 0; i < center.size(); ++i) {
                glm::vec3 fwd = (i == 0)                 ? center[1] - center[0]
                              : (i + 1 == center.size()) ? center[i] - center[i - 1]
                                                         : center[i + 1] - center[i - 1];
                fwd.y = 0.0f;
                if (glm::length(fwd) < 1e-4f) fwd = glm::vec3(0, 0, 1);
                fwd = glm::normalize(fwd);
                const glm::vec3 side = glm::normalize(glm::cross(glm::vec3(0, 1, 0), fwd));
                if (i > 0) vlen += glm::length(center[i] - center[i - 1]);
                const float v = vlen / roadTexTile;
                glm::vec3 Lp = center[i] - side * half;
                glm::vec3 Rp = center[i] + side * half;
                Lp.y = streamer.heightAt(Lp.x, Lp.z) + 0.10f; // lift off the ground
                Rp.y = streamer.heightAt(Rp.x, Rp.z) + 0.10f;
                const glm::vec3 up(0.0f, 1.0f, 0.0f);
                md.vertices.push_back({Lp, up, {0.0f, v}});
                md.vertices.push_back({Rp, up, {roadWidth / roadTexTile, v}});
            }
            // Two triangles per rung, wound CCW-from-above (front faces up).
            for (std::uint32_t i = 0; i + 1 < center.size(); ++i) {
                const std::uint32_t a = 2 * i;
                md.indices.insert(md.indices.end(), {a, a + 2, a + 1, a + 1, a + 2, a + 3});
            }
            roadMesh  = Mesh::create(md);
            roadVerts = static_cast<int>(md.vertices.size());
            // Keep the CPU geometry for the physics mesh collider (Play mode).
            roadCollVerts.clear();
            roadCollVerts.reserve(md.vertices.size());
            for (const Vertex& vtx : md.vertices) roadCollVerts.push_back(vtx.position);
            roadCollIndices = md.indices;
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
        bool  carPlaced   = false;
        bool  showVehicle = true;
        glm::vec3 carPos(0.0f);
        float carYaw     = 0.0f;   // heading (radians)
        float carSpeed   = 0.0f;   // m/s (negative = reverse)
        float wheelSpin  = 0.0f;   // rolling angle (radians)
        float steerAngle = 0.0f;   // front-wheel steer (radians)
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
        bool      entityEditMode = true; // start ready to edit; Esc -> selection
        glm::vec3 entityNewHalf(1.0f, 1.0f, 1.0f); // default size (half-extents)
        EntityType entityNewType = EntityType::Box; // type placed on click
        int       entityCounter = 0; // for unique default names

        // Material library: named surface assets solids can be assigned. New
        // objects get the material selected in the Materials panel (matSel).
        std::vector<MaterialDef>& materials = document.materials();
        int  matSel          = 0;    // selected material in the Materials panel
        // Secondary-panel visibility (toggled from the View menu). The default
        // layout is just Hierarchy | Scene | Inspector; everything else is hidden.
        bool showMaterials   = false;
        bool showModels      = false;
        bool showAssets      = false;
        bool showScriptEditor = false;
        bool showStats       = false;
        bool showCamera      = false;
        bool showWeather     = false;
        bool showSky         = false;
        bool showColorGrade  = false;
        bool showWater       = false;
        bool showTerrain     = false;
        bool showVegetation  = false;
        bool showCamPath     = false;
        bool showRoads       = false;
        bool showVehiclePanel = false;
        bool showEnv         = false;
        std::string modelFile;       // selected file in the Models panel

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
        // Add an entity of the given type, sitting on the terrain at a world point.
        auto addEntity = [&](glm::vec3 groundPos, EntityType type) {
            Entity nb;
            nb.type   = type;
            nb.half   = (type == EntityType::Light) ? glm::vec3(0.3f) : entityNewHalf;
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
        auto addModelHierarchy = [&](glm::vec3 groundPos, const std::string& path) {
            const auto& ns = models.nodes(path);
            if (ns.empty()) { // no node structure -> fall back to a single model
                const int id = models.import(path, assetDb, materials);
                if (id >= 0) addModelEntity(groundPos, id);
                return;
            }
            std::vector<int>       nodeIds(ns.size(), -1);
            std::vector<glm::vec3> nodeHalf(ns.size(), glm::vec3(0.1f));
            glm::vec3 lo(1e30f), hi(-1e30f);
            for (std::size_t i = 0; i < ns.size(); ++i) {
                nodeIds[i] = models.importNode(path, static_cast<int>(i), assetDb, materials);
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
        auto isStructuredModel = [](const std::string& p) {
            std::string e = std::filesystem::path(p).extension().string();
            for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return e == ".fbx";
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
            [&](const std::string& p, int n){ return models.importNode(p, n, assetDb, materials); },
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
        auto openProjectFolder    = [&](const std::string& f){ const bool ok = projectio::openProjectFolder(pio, f); history.clear(); return ok; };
        auto newProject           = [&](){ projectio::newProject(pio); history.clear(); };

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
            history.push(std::make_unique<DeleteEntityCmd>(document, entities[idx].id),
                         document);
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

        // Tree instances live here (filled by regenTrees below); declared early
        // so flower placement can cluster blooms around the trees.
        std::vector<float> treeInst;
        int                treeCount = 0;

        // --- Flowers: GPU-instanced blooms scattered through lush grass ---
        Shader flower = Shader::fromFiles("assets/shaders/flower.vert",
                                          "assets/shaders/flower.frag");
        if (!flower.isValid()) { std::fprintf(stderr, "Failed to load flower shader\n"); return 1; }
        // base: pos3, normal3, tint ; instance: iPos3, iYaw, iScale, iColor3.
        const std::vector<float> flowerMesh = makeFlowerMesh();
        const int flowerVerts = static_cast<int>(flowerMesh.size() / 7);
        InstancedMesh flowerField = InstancedMesh::create(
            flowerMesh.data(), flowerMesh.size(), 7 * sizeof(float),
            {{0, 3, 0}, {1, 3, 3 * sizeof(float)}, {2, 1, 6 * sizeof(float)}},
            8 * sizeof(float),
            {{3, 3, 0}, {4, 1, 3 * sizeof(float)}, {5, 1, 4 * sizeof(float)},
             {6, 3, 5 * sizeof(float)}});
        int   flowerCount   = 0;
        bool  flowerEnabled = true;
        float flowerDensity = 1.0f;
        auto regenFlowers = [&](glm::vec2 c) {
            std::vector<float> out;
            std::mt19937 rng(4242u);
            std::uniform_real_distribution<float> u(0.0f, 1.0f);
            const float R = grassRadius, spacing = 0.9f;
            const float clear = roadWidth * 0.5f + 1.5f;
            // Natural meadow palette, weighted toward buttercup yellow and white.
            const glm::vec3 palette[5] = {{0.96f, 0.78f, 0.12f},  // buttercup yellow
                                          {0.94f, 0.55f, 0.12f},  // warm orange
                                          {0.95f, 0.95f, 0.88f},  // daisy white
                                          {0.86f, 0.46f, 0.55f},  // soft pink
                                          {0.60f, 0.55f, 0.82f}}; // pale lavender
            for (float z = -R; z <= R; z += spacing) {
                for (float x = -R; x <= R; x += spacing) {
                    if (x * x + z * z > R * R) continue;
                    const float wx = c.x + x, wz = c.y + z;
                    if (roadDistanceSq(roadCenterline, wx, wz) < clear * clear) continue;
                    const float h = streamer.heightAt(wx, wz);
                    if (h < waterLevel + 0.6f || h > look.snowLevel - 2.0f) continue;
                    const float e = 1.0f;
                    const glm::vec3 n = glm::normalize(glm::vec3(
                        streamer.heightAt(wx - e, wz) - streamer.heightAt(wx + e, wz), 2.0f * e,
                        streamer.heightAt(wx, wz - e) - streamer.heightAt(wx, wz + e)));
                    if (n.y < 0.9f) continue;
                    const float moist = terrainMoisture(streamer.settings(), wx, wz);
                    if (moist < 0.3f) continue; // flowers want greener ground

                    // Clumps where a mid-frequency noise peaks; a small background
                    // chance sprinkles lone flowers between the groups.
                    const float clump  = valNoise2(wx * 0.16f + 50.0f, wz * 0.16f + 50.0f);
                    const float groupP = glm::smoothstep(0.66f, 0.9f, clump);

                    // Flowers gather in the shade around tree trunks.
                    float treeP = 0.0f;
                    for (int t = 0; t < treeCount; ++t) {
                        const float dx = wx - treeInst[t * 5 + 0];
                        const float dz = wz - treeInst[t * 5 + 2];
                        const float dd = dx * dx + dz * dz;
                        if (dd < 30.0f) treeP = std::max(treeP, glm::smoothstep(30.0f, 3.0f, dd));
                    }

                    const float prob = (0.02f + groupP * 0.9f + treeP * 0.75f)
                                     * glm::smoothstep(0.3f, 0.7f, moist) * flowerDensity;
                    if (u(rng) > prob) continue;
                    const float fx = wx + (u(rng) - 0.5f) * spacing;
                    const float fz = wz + (u(rng) - 0.5f) * spacing;
                    // Weighted pick: mostly yellow/orange/white, few pink/lavender.
                    const float cr = u(rng);
                    const int ci = cr < 0.42f ? 0 : cr < 0.60f ? 1 : cr < 0.82f ? 2
                                 : cr < 0.92f ? 3 : 4;
                    const glm::vec3 col = palette[ci];
                    // Meadow flowers are small; squared roll keeps most of them tiny.
                    const float sr = u(rng);
                    const float scale = glm::mix(0.32f, 0.75f, sr * sr);
                    out.insert(out.end(), {fx, streamer.heightAt(fx, fz) - 0.02f, fz,
                                           u(rng) * 6.2831f, scale,
                                           col.r, col.g, col.b});
                }
            }
            flowerField.upload(out);
            flowerCount = flowerField.count();
        };

        // --- Birds: a small flock of flapping billboards circling overhead --
        Shader bird = Shader::fromFiles("assets/shaders/bird.vert",
                                        "assets/shaders/bird.frag");
        if (!bird.isValid()) { std::fprintf(stderr, "Failed to load bird shader\n"); return 1; }
        // Gull silhouette: small body + swept, bent wings. pos3, flap (flap
        // rises toward the tips so the wings flex when they beat). +Z forward.
        const float bm[] = {
                // body (diamond: nose, shoulders, tail)
                 0.00f, 0.0f,  0.45f, 0.0f,  -0.12f, 0.0f, 0.05f, 0.0f,   0.12f, 0.0f, 0.05f, 0.0f,
                -0.12f, 0.0f,  0.05f, 0.0f,   0.00f, 0.0f,-0.55f, 0.0f,   0.12f, 0.0f, 0.05f, 0.0f,
                // left wing (inner + outer panel, trailing to the tail)
                -0.12f, 0.0f,  0.05f, 0.0f,  -0.55f, 0.05f,-0.05f, 0.4f,  0.00f, 0.0f,-0.55f, 0.0f,
                -0.55f, 0.05f,-0.05f, 0.4f,  -1.05f, 0.0f,-0.35f, 1.0f,   0.00f, 0.0f,-0.55f, 0.0f,
                // right wing
                 0.12f, 0.0f,  0.05f, 0.0f,   0.00f, 0.0f,-0.55f, 0.0f,   0.55f, 0.05f,-0.05f, 0.4f,
                 0.55f, 0.05f,-0.05f, 0.4f,   0.00f, 0.0f,-0.55f, 0.0f,   1.05f, 0.0f,-0.35f, 1.0f };
        // base: pos3 + flap ; instance: iPos3, iYaw, iPhase.
        InstancedMesh birdField = InstancedMesh::create(
            bm, sizeof(bm) / sizeof(float), 4 * sizeof(float),
            {{0, 3, 0}, {1, 1, 3 * sizeof(float)}},
            5 * sizeof(float),
            {{2, 3, 0}, {3, 1, 3 * sizeof(float)}, {4, 1, 4 * sizeof(float)}});
        bool  birdsEnabled = true;
        int   birdCount    = 18;
        float birdSize     = 2.2f;

        // --- Fireflies: additive glowing points that wander the grass at night --
        Shader firefly = Shader::fromFiles("assets/shaders/firefly.vert",
                                           "assets/shaders/firefly.frag");
        if (!firefly.isValid()) { std::fprintf(stderr, "Failed to load firefly shader\n"); return 1; }
        // Instance-only (quad corner comes from gl_VertexID): iPos3, iPhase.
        InstancedMesh fireflyField = InstancedMesh::create(
            nullptr, 0, 0, {}, 4 * sizeof(float),
            {{0, 3, 0}, {1, 1, 3 * sizeof(float)}});
        bool  fireflyEnabled = true;
        int   fireflyCount   = 70;
        float fireflySize    = 0.09f;
        const float fireflyRadius = 34.0f;
        std::mt19937 flyRng(9001u);
        std::uniform_real_distribution<float> flyU(0.0f, 1.0f);
        // Home xz + blink phase per firefly; homes start "far" so they seed near
        // the camera on the first night frame.
        std::vector<glm::vec3> fireflies(256, glm::vec3(1e9f, 1e9f, 0.0f));
        for (auto& f : fireflies) f.z = flyU(flyRng) * 6.2831f;

        glm::vec2 treeCenter(1e9f);
        bool      treeEnabled  = true;
        float     treeDensity  = 1.0f;
        const float treeRadius = 120.0f;
        std::mt19937 trng(555u);
        auto regenTrees = [&](glm::vec2 cc) {
            treeInst.clear();
            std::uniform_real_distribution<float> u(0.0f, 1.0f);
            const float spacing = 7.0f;
            for (float z = -treeRadius; z <= treeRadius; z += spacing) {
                for (float x = -treeRadius; x <= treeRadius; x += spacing) {
                    if (x * x + z * z > treeRadius * treeRadius) continue;
                    const float wx = cc.x + x, wz = cc.y + z;
                    const float roadClear = roadWidth * 0.5f + 3.0f; // keep trees clear
                    if (roadDistanceSq(roadCenterline, wx, wz) < roadClear * roadClear) continue;
                    const float h = streamer.heightAt(wx, wz);
                    if (h < waterLevel + 0.8f || h > look.snowLevel - 2.0f) continue;
                    const float e = 1.5f;
                    const glm::vec3 n = glm::normalize(glm::vec3(
                        streamer.heightAt(wx - e, wz) - streamer.heightAt(wx + e, wz),
                        2.0f * e,
                        streamer.heightAt(wx, wz - e) - streamer.heightAt(wx, wz + e)));
                    if (n.y < 0.86f) continue;
                    // Forests cluster in moist regions; dry biomes stay open.
                    const float moist  = terrainMoisture(streamer.settings(), wx, wz);
                    const float forest = 0.5f + 0.35f * std::sin(wx * 0.03f)
                                              + 0.35f * std::cos(wz * 0.026f + 1.3f);
                    const float prob = glm::clamp(forest, 0.0f, 1.0f)
                                     * glm::smoothstep(0.35f, 0.75f, moist)
                                     * 0.6f * treeDensity;
                    if (u(trng) > prob) continue;
                    const float tx = wx + (u(trng) - 0.5f) * spacing;
                    const float tz = wz + (u(trng) - 0.5f) * spacing;
                    treeInst.insert(treeInst.end(), {
                        tx, streamer.heightAt(tx, tz) - 0.3f, tz,
                        u(trng) * 6.2831f,
                        glm::mix(treeSize * 0.75f, treeSize * 1.3f, u(trng))});
                }
            }
            treeCount = static_cast<int>(treeInst.size() / 5);
            glBindBuffer(GL_ARRAY_BUFFER, treeInstVBO);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(treeInst.size() * sizeof(float)),
                         treeInst.data(), GL_DYNAMIC_DRAW);
            treeCenter = cc;
        };

        // --- Audio: weather-driven sound layers --------------------------
        showProgress(0.82f, "Loading audio...");
        Audio audio;
        const std::string soundDir = localContent ? contentRoot + "/sounds"
                                                   : std::string(FITZEL_SOUND_DIR);
        Sound rainSnd    = Sound::fromFile(audio, soundDir + "/rain.wav", true);
        Sound windSnd    = Sound::fromFile(audio, soundDir + "/wind.wav", true);
        Sound breezeSnd  = Sound::fromFile(audio, soundDir + "/breeze.wav", true);
        Sound thunderSnd = Sound::fromFile(audio, soundDir + "/thunder.wav", false);
        rainSnd.setVolume(0.0f);   rainSnd.play();   // loops; volume follows weather
        windSnd.setVolume(0.0f);   windSnd.play();
        breezeSnd.setVolume(0.0f); breezeSnd.play();
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

        // Camera path recorder/player state.
        std::vector<CamKey> camPath;
        bool  pathPlaying    = false;
        bool  pathRecording  = false;
        float pathTime       = 0.0f;   // current play/record/scrub time
        float pathSpeed      = 1.0f;   // playback speed multiplier
        bool  pathLoop       = false;
        float keySpacing     = 2.0f;   // seconds granted to a manually added key
        float recordInterval = 0.15f;  // seconds between samples while recording
        float recordAccum    = 0.0f;

        // Snapshot the current camera as a keyframe at time `t`, unwrapping yaw so
        // it stays continuous with the previous key (no 360-degree spin on replay).
        auto appendKey = [&](float t) {
            float y = camera.yaw();
            if (!camPath.empty()) {
                const float prev = camPath.back().yaw;
                while (y - prev >  180.0f) y -= 360.0f;
                while (y - prev < -180.0f) y += 360.0f;
            }
            camPath.push_back({t, camera.position(), y, camera.pitch(), camera.fov()});
        };
        const char* kPathFile = "campath.txt";
        auto savePath = [&] {
            std::ofstream f(kPathFile);
            for (const CamKey& k : camPath)
                f << k.t << ' ' << k.pos.x << ' ' << k.pos.y << ' ' << k.pos.z << ' '
                  << k.yaw << ' ' << k.pitch << ' ' << k.fov << '\n';
        };
        auto loadPath = [&] {
            std::ifstream f(kPathFile);
            if (!f) return;
            std::vector<CamKey> loaded;
            CamKey k;
            while (f >> k.t >> k.pos.x >> k.pos.y >> k.pos.z >> k.yaw >> k.pitch >> k.fov)
                loaded.push_back(k);
            if (!loaded.empty()) { camPath = std::move(loaded); pathPlaying = false; pathTime = 0.0f; }
        };

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
                grassEnabled = treeEnabled = flowerEnabled = false;
                birdsEnabled = fireflyEnabled = false;
                waterLevel = -1000.0f;
            } else {                       // Nature: restore the outdoor world
                uiSettings = natureSettings;
                grassEnabled = treeEnabled = flowerEnabled = true;
                birdsEnabled = fireflyEnabled = true;
                waterLevel = -2.0f;
            }
            streamer.settings() = uiSettings;
            streamer.rebuild();
            streamer.update(camera.position());
            grassDirty = true;
            treeCenter = glm::vec2(1e9f);
            roadDirty  = true;
        };
        applyScene(scene); // start in the selected scene (Empty by default)

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
        addF("terrHeight", uiSettings.heightScale);   addF("terrRidge", uiSettings.ridgeScale);
        addF("terrContinent", uiSettings.continentAmp); addF("terrBiome", uiSettings.biomeFreq);
        addF("terrTerrace", uiSettings.terrace);      addF("terrWarp", uiSettings.warpStrength);
        addF("terrFreq", uiSettings.frequency);       addI("terrOctaves", uiSettings.octaves);
        addF("terrSeed", uiSettings.seed);
        addF("texScale", texScale);            addF("normalStrength", normalStrength);
        addF("rockSlope", look.rockSlope);     addF("slopeSharp", look.slopeSharpness);
        addF("snowLevel", look.snowLevel);     addF("detailStrength", look.detailStrength);
        addB("grassEnabled", grassEnabled);    addF("grassDensity", grassDensity);
        addF("grassRadius", grassRadius);      addF("grassHeight", grassHeight);
        addF("grassTintR", grassTint.x);       addF("grassTintG", grassTint.y);
        addF("grassTintB", grassTint.z);
        addB("treeEnabled", treeEnabled);      addF("treeDensity", treeDensity);
        addF("treeSize", treeSize);            addF("lodNear", lodNear);

        // Wire the serialization hooks now that every tunable and the terrain/
        // vegetation state they drive are in scope. Reading settings applies them
        // and rebuilds the terrain + regrows vegetation (like Regenerate does).
        writeSettingsFn = [&](nlohmann::json& j){
            for (const Setting& s : tunables) s.write(j);
            nlohmann::json larr = nlohmann::json::array();
            for (const TerrainLayer& L : look.layers)
                larr.push_back({{"tex", L.texId.toString()}, {"name", L.name},
                                {"hStart", L.heightStart}, {"hEnd", L.heightEnd},
                                {"sStart", L.slopeStart}, {"sEnd", L.slopeEnd},
                                {"scale", L.scale}});
            j["terrainLayers"] = larr;
        };
        readSettingsFn = [&](const nlohmann::json& j){
            for (const Setting& s : tunables) s.read(j);
            look.layers.clear();
            if (j.contains("terrainLayers") && j["terrainLayers"].is_array())
                for (const auto& lj : j["terrainLayers"]) {
                    TerrainLayer L;
                    L.texId       = AssetId::fromString(lj.value("tex", std::string{}));
                    L.name        = lj.value("name", std::string{});
                    L.heightStart = lj.value("hStart", -1000.0f);
                    L.heightEnd   = lj.value("hEnd",    1000.0f);
                    L.slopeStart  = lj.value("sStart",  0.0f);
                    L.slopeEnd    = lj.value("sEnd",    90.0f);
                    L.scale       = lj.value("scale",   0.08f);
                    if (L.texId.valid()) L.tex = assetDb.loadTexture(L.texId);
                    look.layers.push_back(std::move(L));
                }
            streamer.settings() = uiSettings;
            streamer.rebuild();
            streamer.update(camera.position());
            grassDirty = true;
            treeCenter = glm::vec2(1e9f);
            roadDirty  = true;
        };

        // --- Play mode: run the scene as a game -------------------------------
        // Play snapshots the editable scene state and drops the player into
        // first-person walk mode; Stop restores the snapshot and the edit camera
        // exactly, so play-time changes never leak into the edited scene.
        bool playMode = false;
        ScriptSystem scripts; // Lua entity scripts, ticked while playing

        // --- Lua script editor (ImGuiColorTextEdit) --------------------------
        TextEditor  luaEditor;
        luaEditor.SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
        luaEditor.SetPalette(TextEditor::GetDarkPalette());
        std::string editorPath;          // "scripts/<file>.lua" open ("" = none)
        bool        editorDirty = false;  // unsaved changes
        char        newScriptName[64] = "";
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
        // Resolve a sound filename to a path: prefer the open project's
        // content/sounds/, fall back to the engine's bundled sounds.
        auto resolveSoundPath = [&](const std::string& n) -> std::string {
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
        scripts.setHost(&host);

        glm::vec3 playCamPos{0.0f};
        float     playCamYaw = 0.0f, playCamPitch = 0.0f, playMoveSpeed = 20.0f;
        float     playCamFov = 60.0f;
        bool      playPrevEdit = false;
        int       activeCam = -1; // entity id of the active Camera in Play (-1 = player)
        auto startPlay = [&] {
            if (playMode) return;
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
            {
                const int   N  = 128;   // heightfield resolution (even)
                const float sp = 4.0f;  // metres per sample (~512 m span)
                const glm::vec3 cc = camera.position();
                const float ox = cc.x - (N * 0.5f) * sp;
                const float oz = cc.z - (N * 0.5f) * sp;
                std::vector<float> heights(static_cast<std::size_t>(N) * N);
                for (int z = 0; z < N; ++z)
                    for (int x = 0; x < N; ++x)
                        heights[z * N + x] = streamer.heightAt(ox + x * sp, oz + z * sp);
                physics->addHeightField(heights.data(), N, glm::vec3(ox, 0.0f, oz), sp);
            }
            // Roads: a static triangle-mesh collider (draped on the terrain), so
            // the player and objects can walk/drive on them.
            if (roadDirty) buildRoad();
            if (roadEnabled && roadCollIndices.size() >= 3)
                physics->addMesh(roadCollVerts.data(),
                                 static_cast<int>(roadCollVerts.size()),
                                 roadCollIndices.data(),
                                 static_cast<int>(roadCollIndices.size()));
            physicsBody.clear();
            for (Entity& e : entities) {
                const auto* pc = e.components.get<PhysicsComponent>();
                if (!pc || e.type == EntityType::Light || e.type == EntityType::Sun)
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
        };
        auto stopPlay = [&] {
            if (!playMode) return;
            playMode  = false;
            physics.reset();
            physicsBody.clear();
            zoneSounds.clear(); // stop + free any looping TriggerSound voices
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

        while (window.isOpen()) {
            window.pollEvents();
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
            // F toggles first-person walk mode (cursor locks, mouse-look always on).
            const bool fDown = input.isKeyDown(GLFW_KEY_F);
            if (fDown && !prevF && !vehicleMode) {
                fpsMode = !fpsMode;
                input.setCursorLocked(fpsMode);
                fpsVelY = 0.0f;
                if (fpsMode) { // drop to standing height immediately
                    const glm::vec3 p = camera.position();
                    camera.setPosition({p.x, streamer.heightAt(p.x, p.z) + eyeHeight, p.z});
                }
            }
            prevF = fDown;

            // V toggles the drive-a-vehicle mode.
            const bool vDown = input.isKeyDown(GLFW_KEY_V);
            if (vDown && !prevV) {
                vehicleMode = !vehicleMode;
                if (vehicleMode) {
                    fpsMode = false;
                    input.setCursorLocked(false);
                    if (!carPlaced) placeCar();
                    camChase = camera.position();
                }
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
                else if (vehicleMode)    { vehicleMode = false; }
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
                if (qd && !prevQkey) { gizmoOp = ImGuizmo::TRANSLATE; entityEditMode = true; }
                if (wd && !prevWkey) { gizmoOp = ImGuizmo::ROTATE;    entityEditMode = true; }
                if (ed && !prevEkey) { gizmoOp = ImGuizmo::SCALE;     entityEditMode = true; }
                prevQkey = qd; prevWkey = wd; prevEkey = ed;
            } else { prevQkey = prevWkey = prevEkey = false; }

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

            if (vehicleMode) {
                // Arcade car: throttle + steering, drag, bicycle-model heading.
                const bool kW = input.isKeyDown(GLFW_KEY_W);
                const bool kS = input.isKeyDown(GLFW_KEY_S);
                const bool kA = input.isKeyDown(GLFW_KEY_A);
                const bool kD = input.isKeyDown(GLFW_KEY_D);
                const bool kBrake = input.isKeyDown(GLFW_KEY_SPACE);
                const float throttle = (kW ? 1.0f : 0.0f) - (kS ? 1.0f : 0.0f);
                const float steerIn  = (kA ? 1.0f : 0.0f) - (kD ? 1.0f : 0.0f);

                const float maxSteer = glm::radians(32.0f);
                steerAngle += (steerIn * maxSteer - steerAngle) * std::min(1.0f, dt * 7.0f);

                carSpeed += throttle * 14.0f * dt;                    // accelerate
                if (kBrake) carSpeed -= glm::sign(carSpeed) * 26.0f * dt;
                carSpeed *= (1.0f - 0.6f * dt);                       // drag
                if (throttle == 0.0f && !kBrake) carSpeed *= (1.0f - 1.2f * dt);
                carSpeed = glm::clamp(carSpeed, -8.0f, 26.0f);
                if (std::abs(carSpeed) < 0.02f) carSpeed = 0.0f;

                carYaw += (carSpeed / 2.7f) * std::tan(steerAngle) * dt; // wheelbase 2.7m
                const glm::vec3 fwd(std::sin(carYaw), 0.0f, std::cos(carYaw));
                carPos   += fwd * carSpeed * dt;
                carPos.y  = streamer.heightAt(carPos.x, carPos.z);
                wheelSpin += (carSpeed / wheelR) * dt;

                // Chase camera: behind and above, smoothly following, looking ahead.
                const glm::vec3 target = carPos + glm::vec3(0.0f, 1.2f, 0.0f);
                const glm::vec3 wanted = carPos - fwd * 7.0f + glm::vec3(0.0f, 3.2f, 0.0f);
                camChase += (wanted - camChase) * std::min(1.0f, dt * 4.0f);
                camera.setPosition(camChase);
                const glm::vec3 d = glm::normalize(target - camChase);
                camera.setYaw(glm::degrees(std::atan2(d.z, d.x)));
                camera.setPitch(glm::degrees(std::asin(glm::clamp(d.y, -1.0f, 1.0f))));
            } else if (fpsMode) {
                // Mouse look is always active; movement is on the ground plane.
                const glm::vec2 d = input.mouseDelta();
                camera.processMouse(d.x, d.y);

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
                    if (glm::length(mv) > 1e-4f) mv = glm::normalize(mv);
                    const bool space = input.isKeyDown(GLFW_KEY_SPACE);
                    const bool jump  = space && !prevSpace;
                    prevSpace = space;
                    bool onGround = false;
                    const glm::vec3 foot = physics->moveCharacter(
                        mv * camera.moveSpeed, jump, dt, onGround);
                    grounded = onGround;
                    camera.setPosition(glm::vec3(foot.x, foot.y + eyeHeight, foot.z));
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
                if (glm::length(move) > 1e-4f) move = glm::normalize(move);

                // --- Move + collide against solid blocks -------------------
                const float pr = 0.35f, stepH = 0.55f; // player radius, step height
                glm::vec3 pos = camera.position();
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
                        b.type == EntityType::Model) continue; // models: no AABB stand
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
                const bool space = input.isKeyDown(GLFW_KEY_SPACE);
                if (space && !prevSpace && grounded) fpsVelY = 9.0f;
                prevSpace = space;
                fpsVelY -= 25.0f * dt;
                pos.y += fpsVelY * dt;

                if (pos.y <= groundEye) { pos.y = groundEye; fpsVelY = 0.0f; grounded = true; }
                else                    { grounded = false; }
                camera.setPosition(pos);
                }
            } else {
                // Look only when dragging over the viewport panel (or already
                // locked into a drag); the surrounding dock panels keep the mouse.
                const bool mouseLook = input.isMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)
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
            }

            // --- Camera path: record samples or drive playback ----------
            if (pathRecording) {
                pathTime    += dt;
                recordAccum += dt;
                if (recordAccum >= recordInterval) {
                    recordAccum -= recordInterval;
                    appendKey(pathTime);
                }
            } else if (!vehicleMode && pathPlaying && camPath.size() >= 2) {
                pathTime += dt * pathSpeed;
                const float tmax = camPath.back().t;
                if (pathTime >= tmax) {
                    if (pathLoop) pathTime = std::fmod(pathTime, tmax);
                    else { pathTime = tmax; pathPlaying = false; }
                }
                glm::vec3 p; float y, pi, fv;
                samplePath(camPath, pathTime, p, y, pi, fv);
                camera.setPosition(p);
                camera.setYaw(y);
                camera.setPitch(pi);
                camera.setFov(fv);
            }

            // View distance: drive the streaming radius and the camera far plane.
            streamer.setRadius(viewRadius);
            camera.setFarPlane(
                std::max(250.0f, viewRadius * streamer.settings().chunkSize * 1.7f));

            // Stream terrain chunks around the camera.
            streamer.update(camera.position());

            // When the road settles (not mid-drag), regrow vegetation so it
            // clears off the new road; debounced to avoid thrashing while editing.
            if (roadVegDirty && !roadDragging) {
                roadVegDirty = false;
                grassDirty   = true;
                treeCenter   = glm::vec2(1e9f);
            }

            // Regrow grass (async) / trees when the camera has moved far enough.
            {
                const glm::vec2 camXZ(camera.position().x, camera.position().z);
                if (grassEnabled && !grassPending &&
                    (grassDirty || glm::length(camXZ - grassCenter) > 10.0f)) {
                    grassPending = true;
                    grassDirty   = false;
                    grassPendingCenter = camXZ;
                    grassFuture = std::async(std::launch::async, computeGrass,
                                             streamer.settings(), camXZ, waterLevel,
                                             look.snowLevel, grassHeight, grassDensity,
                                             grassRadius, grassSeed++,
                                             roadCenterline, roadWidth * 0.5f + 1.5f);
                }
                if (grassPending && grassFuture.valid() &&
                    grassFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    const std::vector<float> data = grassFuture.get();
                    grassField.upload(data);
                    grassCount = grassField.count();
                    grassCenter = grassPendingCenter;
                    grassPending = false;
                    // Flowers share the grass area; regenerate them to match.
                    if (flowerEnabled) regenFlowers(grassPendingCenter);
                }
                if (treeEnabled && glm::length(camXZ - treeCenter) > 25.0f) regenTrees(camXZ);
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
            const float rainIntensity = glm::smoothstep(0.45f, 0.85f, weather);
            const float lightDim     = glm::mix(1.0f, 0.30f, weather);

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
            audio.setMasterVolume(muted ? 0.0f : masterVolume);
            rainSnd.setVolume(playMode ? rainIntensity : 0.0f);
            windSnd.setVolume(playMode ? glm::smoothstep(0.15f, 1.0f, weather) * 0.9f : 0.0f);
            breezeSnd.setVolume(playMode ? (1.0f - glm::smoothstep(0.0f, 0.5f, weather)) * 0.5f : 0.0f);
            const bool flashOn = flash > 0.25f;
            if (playMode && flashOn && !prevFlashOn) {
                thunderSnd.setVolume(glm::clamp(weather, 0.3f, 1.0f));
                thunderSnd.play();
            }
            prevFlashOn = flashOn;

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
                    sunTint = sc->color; sunStrength = sc->intensity; break;
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
            }

            // --- Scripts: tick each scripted entity's Lua update while playing --
            if (playMode) {
                host.camPos = camera.position();
                host.camDir = camera.front();
                // Scripts and behaviours just write the entity's world transform;
                // children follow via resolveHierarchy (below), no propagation.
                for (Entity& e : entities)
                    if (e.type != EntityType::Sun)
                        if (auto* sc = e.components.get<ScriptComponent>();
                            sc && !sc->file.empty())
                            scripts.update(e, scriptPath(sc->file), dt,
                                           static_cast<float>(now));

                // Built-in component behaviours (data-authored, no code): Spin.
                // Writes LOCAL rotation; the scene-graph derives world (so a
                // spinning child of a spinning parent orbits AND spins).
                for (Entity& e : entities)
                    for (const auto& c : e.components.items)
                        if (auto* sp = dynamic_cast<SpinComponent*>(c.get()))
                            e.localRotation += sp->axis * sp->speed * dt;

                // Player-proximity behaviours (Collectible, Trigger). A mid-body
                // reference point keeps low objects reachable.
                {
                    glm::vec3 playerC = camera.position();
                    playerC.y -= eyeHeight * 0.5f;
                    for (Entity& e : entities) {
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
                                    voice.setVolume(ts->volume * fall);
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
                    s.vel     = glm::vec3(0.0f, sw->speed, 0.0f);
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
            } else {
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
                    ImGui::MenuItem("Weather & audio", nullptr, &showWeather);
                    ImGui::MenuItem("Sky & atmosphere",nullptr, &showSky);
                    ImGui::MenuItem("Colour grade",    nullptr, &showColorGrade);
                    ImGui::MenuItem("Water",           nullptr, &showWater);
                    ImGui::MenuItem("Terrain",         nullptr, &showTerrain);
                    ImGui::MenuItem("Vegetation",      nullptr, &showVegetation);
                    ImGui::MenuItem("Camera path",     nullptr, &showCamPath);
                    ImGui::MenuItem("Roads",           nullptr, &showRoads);
                    ImGui::MenuItem("Vehicle",         nullptr, &showVehiclePanel);
                    ImGui::Separator();
                    ImGui::MenuItem("Materials",       nullptr, &showMaterials);
                    ImGui::MenuItem("Models",          nullptr, &showModels);
                    ImGui::MenuItem("Assets",          nullptr, &showAssets);
                    ImGui::MenuItem("Script Editor",   nullptr, &showScriptEditor);
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
                        return glm::vec3(roadPts[i].x,
                                         streamer.heightAt(roadPts[i].x, roadPts[i].y) + 0.10f,
                                         roadPts[i].y);
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
                        for (int i = 0; i < static_cast<int>(roadPts.size()); ++i) {
                            ImVec2 sp;
                            if (!toScreen(handleWorld(i), sp)) continue;
                            const float d = std::hypot(sp.x - mp.x, sp.y - mp.y);
                            if (d < bestD) { bestD = d; best = i; }
                        }
                        if (best >= 0) { roadSel = best; roadDragging = true; }
                        else {
                            glm::vec3 h;
                            if (roadPickTerrain(viewportMouseNdc, vp, h)) {
                                roadPts.push_back({h.x, h.z});
                                roadSel = static_cast<int>(roadPts.size()) - 1;
                                roadDirty = true;
                            }
                        }
                    }
                    // Drag the selected handle across the terrain.
                    if (roadDragging && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                        roadSel >= 0 && roadSel < static_cast<int>(roadPts.size())) {
                        glm::vec3 h;
                        if (roadPickTerrain(viewportMouseNdc, vp, h)) {
                            roadPts[roadSel] = glm::vec2(h.x, h.z);
                            roadDirty = true;
                        }
                    }
                    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) roadDragging = false;
                    // Delete the selected point.
                    if (roadSel >= 0 && roadSel < static_cast<int>(roadPts.size()) &&
                        ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                        roadPts.erase(roadPts.begin() + roadSel);
                        roadSel = -1;
                        roadDirty = true;
                    }

                    // Draw the path preview line, then the handles on top.
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    ImVec2 prev; bool havePrev = false;
                    for (int i = 0; i < static_cast<int>(roadPts.size()); ++i) {
                        ImVec2 sp;
                        if (!toScreen(handleWorld(i), sp)) { havePrev = false; continue; }
                        if (havePrev) dl->AddLine(prev, sp, IM_COL32(255, 220, 80, 150), 2.0f);
                        prev = sp; havePrev = true;
                    }
                    for (int i = 0; i < static_cast<int>(roadPts.size()); ++i) {
                        ImVec2 sp;
                        if (!toScreen(handleWorld(i), sp)) continue;
                        const bool s = (i == roadSel);
                        dl->AddCircleFilled(sp, s ? 7.0f : 5.0f,
                                            s ? IM_COL32(255, 210, 60, 255)
                                              : IM_COL32(90, 180, 255, 235));
                        dl->AddCircle(sp, s ? 7.0f : 5.0f, IM_COL32(0, 0, 0, 190), 0, 1.5f);
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
                                                 gizmoOp, ImGuizmo::WORLD, model);
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

                    // Click to select/place, but not while grabbing the gizmo.
                    if (!ImGuizmo::IsOver() && !ImGuizmo::IsUsing() &&
                        viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        const glm::mat4 inv = glm::inverse(vp);
                        glm::vec4 pn = inv * glm::vec4(viewportMouseNdc, -1.0f, 1.0f); pn /= pn.w;
                        glm::vec4 pf = inv * glm::vec4(viewportMouseNdc,  1.0f, 1.0f); pf /= pf.w;
                        const glm::vec3 ro = glm::vec3(pn);
                        const glm::vec3 rd = glm::normalize(glm::vec3(pf) - glm::vec3(pn));
                        int hit = -1; float bestT = 1e30f;
                        for (int i = 0; i < static_cast<int>(entities.size()); ++i) {
                            const float t = rayAABB(ro, rd, entities[i].center - entities[i].half,
                                                            entities[i].center + entities[i].half);
                            if (t >= 0.0f && t < bestT) { bestT = t; hit = i; }
                        }
                        if (hit >= 0) {
                            entitySel = hit; // clicked a block -> select it
                        } else if (entityEditMode) {
                            glm::vec3 h; // Edit mode: empty ground -> drop a new block
                            if (roadPickTerrain(viewportMouseNdc, vp, h)) addEntity(h, entityNewType);
                        } else {
                            entitySel = -1; // Selection mode: empty click clears it
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
                if (ImGui::Checkbox("First-person (F)", &fpsMode)) {
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

            if (showWeather) { if (ImGui::Begin("Weather & audio", &showWeather)) {
                ImGui::Checkbox("Auto weather", &autoWeather);
                ImGui::SliderFloat("Storm", &weather, 0.0f, 1.0f);
                ImGui::Text("Rain %.0f%%   Lightning %s", rainIntensity * 100.0f,
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
                ImGui::ColorEdit3("Tint",         &waterColor.x);
            }
            ImGui::End(); }

            terrainui::drawPanel({
                showTerrain, uiSettings, streamer, camera, look,
                texScale, normalStrength, grassDirty, treeCenter, roadDirty,
                assetDb,
            });

            if (showVegetation) { if (ImGui::Begin("Vegetation", &showVegetation)) {
                ImGui::SeparatorText("Grass");
                ImGui::Checkbox("Grass", &grassEnabled);
                bool regrow = false;
                regrow |= ImGui::SliderFloat("Density", &grassDensity, 0.1f, 3.0f);
                regrow |= ImGui::SliderFloat("Grass range", &grassRadius, 20.0f, 90.0f);
                regrow |= ImGui::SliderFloat("Blade height", &grassHeight, 0.2f, 1.2f);
                if (regrow) grassDirty = true; // baked per blade -> regrow
                ImGui::ColorEdit3("Tint", &grassTint.x);
                ImGui::Text("Blades: %d", grassCount);
                ImGui::SeparatorText("Trees");
                ImGui::Checkbox("Trees", &treeEnabled);
                bool retree = false;
                retree |= ImGui::SliderFloat("Tree density", &treeDensity, 0.0f, 2.0f);
                retree |= ImGui::SliderFloat("Tree size", &treeSize, 2.0f, 25.0f);
                if (retree) treeCenter = glm::vec2(1e9f);
                ImGui::SliderFloat("Tree LOD dist", &lodNear, 15.0f, 200.0f);
                ImGui::Text("Trees: %d", treeCount);

                ImGui::SeparatorText("Flowers");
                ImGui::Checkbox("Flowers", &flowerEnabled);
                if (ImGui::SliderFloat("Flower density", &flowerDensity, 0.0f, 2.0f))
                    grassDirty = true; // flowers regenerate with the grass pass
                ImGui::SameLine();
                if (ImGui::SmallButton("Regrow")) grassDirty = true;
                ImGui::Text("Flowers: %d", flowerCount);

                ImGui::SeparatorText("Birds");
                ImGui::Checkbox("Birds", &birdsEnabled);
                ImGui::SliderInt("Flock size", &birdCount, 0, 60);
                ImGui::SliderFloat("Bird size", &birdSize, 0.8f, 5.0f);

                ImGui::SeparatorText("Fireflies (night)");
                ImGui::Checkbox("Fireflies", &fireflyEnabled);
                ImGui::SliderInt("Count", &fireflyCount, 0, 256);
                ImGui::SliderFloat("Firefly size", &fireflySize, 0.03f, 0.25f, "%.2f");
            }
            ImGui::End(); }

            if (showCamPath) { if (ImGui::Begin("Camera path", &showCamPath)) {
                ImGui::Text("Keyframes: %d", static_cast<int>(camPath.size()));
                if (pathRecording) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1),
                                                      "RECORDING  %.1fs", pathTime);
                else if (pathPlaying) ImGui::TextColored(ImVec4(0.4f, 1, 0.5f, 1),
                                                         "PLAYING  %.1fs", pathTime);
                else ImGui::TextDisabled("idle");

                // Record continuously samples the camera while you fly; Add keyframe
                // snapshots the current pose. Both build the same spline path.
                if (ImGui::Button(pathRecording ? "Stop recording" : "Record")) {
                    pathRecording = !pathRecording;
                    if (pathRecording) {
                        camPath.clear();
                        pathPlaying = false;
                        pathTime = 0.0f;
                        recordAccum = 0.0f;
                        appendKey(0.0f); // anchor at the start pose
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Add keyframe")) {
                    const float t = camPath.empty() ? 0.0f : camPath.back().t + keySpacing;
                    appendKey(t);
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear")) {
                    camPath.clear();
                    pathPlaying = pathRecording = false;
                    pathTime = 0.0f;
                }

                ImGui::BeginDisabled(camPath.size() < 2);
                if (ImGui::Button(pathPlaying ? "Stop" : "Play")) {
                    pathPlaying = !pathPlaying;
                    pathRecording = false;
                    if (pathPlaying) pathTime = 0.0f;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::Checkbox("Loop", &pathLoop);

                ImGui::SliderFloat("Speed", &pathSpeed, 0.1f, 4.0f, "%.2fx");
                ImGui::SliderFloat("Key spacing", &keySpacing, 0.5f, 10.0f, "%.1f s");
                ImGui::SliderFloat("Rec interval", &recordInterval, 0.05f, 1.0f, "%.2f s");

                if (camPath.size() >= 2) {
                    const float tmax = camPath.back().t;
                    ImGui::Text("Duration: %.1f s", tmax);
                    // Scrubbing previews the pose and pauses playback.
                    if (ImGui::SliderFloat("Scrub", &pathTime, 0.0f, tmax, "%.2f s")) {
                        glm::vec3 p; float y, pi, fv;
                        samplePath(camPath, pathTime, p, y, pi, fv);
                        camera.setPosition(p);
                        camera.setYaw(y);
                        camera.setPitch(pi);
                        camera.setFov(fv);
                        pathPlaying = false;
                    }
                }

                ImGui::Separator();
                if (ImGui::Button("Save")) savePath();
                ImGui::SameLine();
                if (ImGui::Button("Load")) loadPath();
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", kPathFile);
            }
            ImGui::End(); }

            if (showRoads) { if (ImGui::Begin("Roads", &showRoads)) {
                ImGui::Checkbox("Show roads", &roadEnabled);
                ImGui::Checkbox("Edit mode", &roadEditMode);
                if (roadEditMode) {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                                       "Click ground = add | drag handle = move | Del = delete");
                } else {
                    ImGui::TextDisabled("Enable edit mode to place and drag handles");
                }
                ImGui::Text("Points: %d", static_cast<int>(roadPts.size()));
                ImGui::SameLine();
                if (roadSel >= 0) ImGui::Text("| selected #%d", roadSel);
                else              ImGui::TextDisabled("| none selected");

                bool rc = false;
                rc |= ImGui::SliderFloat("Width", &roadWidth, 1.0f, 20.0f, "%.1f m");
                rc |= ImGui::SliderFloat("Texture tile", &roadTexTile, 2.0f, 24.0f, "%.1f m");
                if (rc) roadDirty = true;

                // Surface texture picker (any diffuse texture in textures/).
                if (!roadTexFiles.empty() &&
                    ImGui::BeginCombo("Surface", roadTexFiles[roadTexSel].c_str())) {
                    for (int i = 0; i < static_cast<int>(roadTexFiles.size()); ++i) {
                        const bool sel = (i == roadTexSel);
                        if (ImGui::Selectable(roadTexFiles[i].c_str(), sel)) {
                            if (auto t = assetDb.loadTexture(texDir + "/" + roadTexFiles[i])) {
                                roadTex = std::move(t);
                                roadMat.setTexture("uTexture", *roadTex, 0);
                                roadTexSel = i;
                            }
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }

                ImGui::BeginDisabled(roadSel < 0 || roadSel >= static_cast<int>(roadPts.size()));
                if (ImGui::Button("Delete selected")) {
                    roadPts.erase(roadPts.begin() + roadSel);
                    roadSel = -1;
                    roadDirty = true;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(roadPts.empty());
                if (ImGui::Button("Undo point")) {
                    roadPts.pop_back();
                    if (roadSel >= static_cast<int>(roadPts.size())) roadSel = -1;
                    roadDirty = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear")) { roadPts.clear(); roadSel = -1; roadDirty = true; }
                ImGui::EndDisabled();

                ImGui::Separator();
                if (ImGui::Button("Save")) {
                    std::ofstream f("road.txt");
                    for (const glm::vec2& p : roadPts) f << p.x << ' ' << p.y << '\n';
                }
                ImGui::SameLine();
                if (ImGui::Button("Load")) {
                    std::ifstream f("road.txt");
                    if (f) {
                        roadPts.clear();
                        glm::vec2 p;
                        while (f >> p.x >> p.y) roadPts.push_back(p);
                        roadSel = -1;
                        roadDirty = true;
                    }
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(road.txt)");
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
                auto typeColor = [](EntityType t) -> ImU32 {
                    switch (t) {
                        case EntityType::Light:    return IM_COL32(255, 224, 130, 255);
                        case EntityType::Sun:      return IM_COL32(255, 200,  90, 255);
                        case EntityType::Model:    return IM_COL32(150, 200, 255, 255);
                        case EntityType::Sphere:   return IM_COL32(190, 230, 200, 255);
                        case EntityType::Cylinder: return IM_COL32(200, 210, 235, 255);
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
                    ImGui::PushStyleColor(ImGuiCol_Text, typeColor(entities[i].type));
                    const bool open = ImGui::TreeNodeEx("##n", flags, "%s", nm);
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) entitySel = i;
                    if (ImGui::BeginPopupContextItem()) {
                        entitySel = i;
                        if (ImGui::MenuItem("Duplicate")) dupReq = i;
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
                if (dupReq >= 0)      duplicateEntity(dupReq);
                else if (delReq >= 0) deleteEntity(delReq);
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
                        if (inspActive && inspEditId != selId) {
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
                        ImGui::Checkbox("Glass", &md.glass);
                        if (md.glass) {
                            ImGui::SameLine();
                            ImGui::TextDisabled("(clear centre, reflective rim)");
                        }
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

            // Asset browser: every asset in the database, grouped by source
            // (Engine vs Project) and labelled by type. Drag a Model onto the
            // viewport to place it, or a Texture onto a material's Base texture
            // slot. Double-click a Model to drop it ahead of the camera.
            if (showAssets) {
                if (ImGui::Begin("Assets", &showAssets)) {
                    ImGui::TextDisabled("Drag a model to the viewport, or a "
                                        "texture onto a material's Base texture.");
                    ImGui::Separator();
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
                        int shown = 0;
                        for (AssetId id : assetDb.allAssets()) {
                            const AssetDatabase::Entry* e = assetDb.entry(id);
                            if (!e || e->sourceIndex != si) continue;
                            ++shown;
                            const std::string lbl = std::string(assetTypeName(e->type)) +
                                                    "  " + e->relPath + "##a" + id.toString();
                            ImGui::Selectable(lbl.c_str());
                            if (ImGui::BeginDragDropSource(
                                    ImGuiDragDropFlags_SourceAllowNullID)) {
                                const std::string g = id.toString();
                                ImGui::SetDragDropPayload("ASSET_GUID", g.data(), 32);
                                ImGui::Text("%s  %s", assetTypeName(e->type),
                                            e->relPath.c_str());
                                ImGui::EndDragDropSource();
                            }
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
                if (ImGui::Begin("Script Editor", &showScriptEditor,
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
                    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                        ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))
                        doSave = true;

                    luaEditor.Render("LuaText");
                    if (luaEditor.IsTextChanged()) editorDirty = true;
                    if (doSave) saveEditor();
                }
                ImGui::End();

                // New-script modal: create scripts/<name>.lua from a template.
                if (openNewScript) ImGui::OpenPopup("New Script");
                if (ImGui::BeginPopupModal("New Script", nullptr,
                                           ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::SetNextItemWidth(260.0f);
                    ImGui::InputText("Name", newScriptName, sizeof(newScriptName));
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
                        if (out)
                            out << "-- " << file << " : entity behaviour (runs in Play)\n"
                                   "-- e: x/y/z pos, rx/ry/rz rot(deg), sx/sy/sz half, name, id\n\n"
                                   "function start(e)\nend\n\n"
                                   "function update(e, dt, t)\nend\n";
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
                    if (vehicleMode) {
                        fpsMode = false;
                        input.setCursorLocked(false);
                        if (!carPlaced) placeCar();
                        camChase = camera.position();
                    }
                }
                if (vehicleMode)
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                                       "W/S drive, A/D steer, Space brake, Esc exit");
                else
                    ImGui::TextDisabled("Press V or tick above to drive");
                ImGui::Checkbox("Show vehicle", &showVehicle);
                if (ImGui::Button("Place at camera")) placeCar();
                if (carPlaced) ImGui::Text("Speed: %.0f km/h", std::abs(carSpeed) * 3.6f);
                else           ImGui::TextDisabled("Vehicle not placed yet");
            }
            ImGui::End(); }

            } // end editor UI (skipped in presentation mode)

            // Push the (possibly edited) terrain params into the material, plus
            // the texture layers: bind each layer with a texture to its own unit
            // (3..3+N-1) and upload its height/slope band + tiling. Layers without
            // a texture are skipped, so uLayerCount is the bound count.
            terrainMat.set("uDetailScale", look.detailScale)
                      .set("uDetailStrength", look.detailStrength)
                      .set("uTexScale", texScale)
                      .set("uNormalStrength", normalStrength)
                      .set("uWaterLevel", waterLevel)
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

            // Road mesh is (re)built when points/width change (edited in the UI).
            if (roadDirty) buildRoad();
            if (roadEnabled && roadVerts > 0) {
                roadMat.set("uWaterLevel", waterLevel); // wet-darken submerged parts
                renderer.submit(roadMesh, roadMat, glm::mat4(1.0f), false);
            }

            // --- Vehicle: terrain-aligned body + steered/rolling wheels --
            if (showVehicle && carPlaced) {
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
            }
            std::vector<Material> lightMats;
            lightMats.reserve(entities.size());
            for (const Entity& b : entities) {
                if (b.type == EntityType::Sun) continue; // directional, no geometry
                // Player-start markers are authoring aids -- hidden while playing.
                if (playMode && b.components.get<PlayerStartComponent>()) continue;
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
                                        materials[mi].opacity);
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
                       .set("uAlbedo", lcol * 1.5f).set("uReflectivity", 0.0f);
                    renderer.submit(mesh, mat, m, /*castsPointShadow=*/false);
                } else {
                    // Assigned library material; reflective solids are excluded
                    // from the env probe so they don't reflect their own interior.
                    const auto* mc = b.components.get<MaterialComponent>();
                    const int mi = document.materialIndex(mc ? mc->material : AssetId{});
                    renderer.submit(mesh, gpuMats[mi], m, true,
                                    materials[mi].reflectivity > 0.0f,
                                    materials[mi].opacity);
                }
            }

            // Any entity carrying a LightComponent becomes a real point light --
            // decoupled from EntityType, so a box can glow too.
            std::vector<PointLight> pointLights;
            for (const Entity& b : entities) {
                const auto* lc = b.components.get<LightComponent>();
                if (!lc) continue;
                if (static_cast<int>(pointLights.size()) >= Renderer::kMaxPointLights) break;
                PointLight pl;
                pl.position    = b.center;
                pl.color       = lc->color * lc->intensity;  // HDR radiance
                pl.range       = lc->range;
                pl.castShadows = lc->castShadows;
                pl.shadowBias  = lc->shadowBias;
                pointLights.push_back(pl);
            }
            renderer.setPointLights(pointLights);
            renderer.preparePointShadows(); // omni shadow cubemaps (opt-in lights)

            // --- Multi-pass render with sky and planar water ------------
            // Trees cast shadows: drawn into every cascade via this callback.
            auto treeShadowCaster = [&](const glm::mat4& lightSpace) {
                if (!treeEnabled || treeCount == 0 || treePrims.empty()) return;
                glDisable(GL_CULL_FACE);
                treeDepth.bind();
                treeDepth.setMat4("uLightSpace", lightSpace);
                treeDepth.setFloat("uTime", static_cast<float>(now));
                treeDepth.setVec2("uWindDir", glm::normalize(glm::vec2(0.6f, 0.3f)));
                treeDepth.setFloat("uWindStrength", glm::mix(0.05f, 0.4f, weather));
                treeDepth.setFloat("uTreeHeight", treeLocalHeight);
                treeDepth.setInt("uTex", 0);
                glBindVertexArray(treeVAO);
                for (const TreePrim& tp : treePrims) {
                    if (tp.hasTex) tp.tex.bind(0);
                    treeDepth.setInt("uAlphaCutout", tp.cutout ? 1 : 0);
                    glDrawArraysInstanced(GL_TRIANGLES, tp.first, tp.count, treeCount);
                }
                glBindVertexArray(0);
                glEnable(GL_CULL_FACE);
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
            auto drawTrees = [&](const glm::mat4& vp, const glm::vec3& eye) {
                if (!treeEnabled || treeCount == 0 || treePrims.empty()) return;
                glDisable(GL_CULL_FACE);
                tree.bind();
                tree.setMat4("uViewProj", vp);
                tree.setFloat("uTime", static_cast<float>(now));
                tree.setVec2("uWindDir", glm::normalize(glm::vec2(0.6f, 0.3f)));
                tree.setFloat("uWindStrength", glm::mix(0.05f, 0.4f, weather));
                tree.setFloat("uTreeHeight", treeLocalHeight);
                tree.setVec3("uCamPos", eye);
                tree.setFloat("uLodNear", lodNear);
                tree.setVec3("uViewPos", eye);
                tree.setVec3("uLightDir", light.direction);
                tree.setVec3("uLightColor", light.color);
                tree.setVec3("uAmbient", light.ambient);
                tree.setVec3("uFogColor", fog.color);
                tree.setVec3("uFogSunColor", fog.sunColor);
                tree.setFloat("uFogDensity", fog.density);
                tree.setFloat("uFogHeightFalloff", fog.heightFalloff);
                tree.setFloat("uFogHeight", fog.height);
                tree.setInt("uTex", 0);
                glBindVertexArray(treeVAO);
                for (const TreePrim& tp : treePrims) {
                    if (tp.hasTex) tp.tex.bind(0);
                    tree.setInt("uAlphaCutout", tp.cutout ? 1 : 0);
                    glDrawArraysInstanced(GL_TRIANGLES, tp.first, tp.count, treeCount);
                }
                glBindVertexArray(0);
                glEnable(GL_CULL_FACE);
            };

            // 0) Environment probe: capture the scene into a cubemap from the
            //    first reflective object's position, so reflective materials
            //    mirror the surrounding world. One shared probe (v1); its parallax
            //    is only exact at that point. Skipped when nothing is reflective.
            glm::vec3 probePos(0.0f);
            bool hasReflective = false;
            for (const Entity& b : entities) {
                const auto* mc = b.components.get<MaterialComponent>();
                if (b.type != EntityType::Light && b.type != EntityType::Sun && mc &&
                    materials[document.materialIndex(mc->material)].reflectivity > 0.0f) {
                    probePos = b.center;
                    hasReflective = true;
                    break;
                }
            }
            if (hasReflective) {
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
            drawTrees(proj * reflView, reflEye); // trees mirror in the water

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

            // Grass (instanced) into the HDR buffer, lit + fogged like the terrain.
            if (grassEnabled && grassCount > 0) {
                glDisable(GL_CULL_FACE);
                grass.bind();
                grass.setMat4("uViewProj", mainVP);
                grass.setFloat("uTime", static_cast<float>(now));
                grass.setVec2("uWindDir", glm::normalize(glm::vec2(0.6f, 0.3f)));
                grass.setFloat("uWindStrength", glm::mix(0.08f, 0.55f, weather));
                grass.setVec3("uTint", grassTint);
                grass.setVec3("uViewPos", camPos);
                grass.setVec3("uLightDir", light.direction);
                grass.setVec3("uLightColor", light.color);
                grass.setVec3("uAmbient", light.ambient);
                grass.setVec3("uFogColor", fog.color);
                grass.setVec3("uFogSunColor", fog.sunColor);
                grass.setFloat("uFogDensity", fog.density);
                grass.setFloat("uFogHeightFalloff", fog.heightFalloff);
                grass.setFloat("uFogHeight", fog.height);
                grassField.draw(GL_TRIANGLE_STRIP, 7);
                glEnable(GL_CULL_FACE);
            }

            // Flowers (instanced) into the HDR buffer, lit + fogged like grass.
            if (flowerEnabled && flowerCount > 0) {
                glDisable(GL_CULL_FACE);
                flower.bind();
                flower.setMat4("uViewProj", mainVP);
                flower.setFloat("uTime", static_cast<float>(now));
                flower.setVec2("uWindDir", glm::normalize(glm::vec2(0.6f, 0.3f)));
                flower.setFloat("uWindStrength", glm::mix(0.08f, 0.55f, weather));
                flower.setVec3("uViewPos", camPos);
                flower.setVec3("uLightDir", light.direction);
                flower.setVec3("uLightColor", light.color);
                flower.setVec3("uAmbient", light.ambient);
                flower.setVec3("uFogColor", fog.color);
                flower.setVec3("uFogSunColor", fog.sunColor);
                flower.setFloat("uFogDensity", fog.density);
                flower.setFloat("uFogHeightFalloff", fog.heightFalloff);
                flower.setFloat("uFogHeight", fog.height);
                flowerField.draw(GL_TRIANGLES, flowerVerts);
                glEnable(GL_CULL_FACE);
            }

            // Trees (instanced, per-material) into the HDR buffer.
            drawTrees(mainVP, camPos);

            // Distant trees as camera-facing billboards (alpha cutout).
            if (treeEnabled && treeCount > 0 && billboardTex.isValid()) {
                glDisable(GL_CULL_FACE);
                billboard.bind();
                billboard.setMat4("uViewProj", mainVP);
                billboard.setVec3("uCamRight", camera.right());
                billboard.setVec3("uCamPos", camPos);
                billboard.setFloat("uLodNear", lodNear);
                billboard.setFloat("uTreeHeight", treeLocalHeight);
                billboard.setFloat("uAspect", bbAspect);
                billboard.setVec3("uViewPos", camPos);
                billboard.setVec3("uLightDir", light.direction);
                billboard.setVec3("uLightColor", light.color);
                billboard.setVec3("uAmbient", light.ambient);
                billboard.setVec3("uFogColor", fog.color);
                billboard.setVec3("uFogSunColor", fog.sunColor);
                billboard.setFloat("uFogDensity", fog.density);
                billboard.setFloat("uFogHeightFalloff", fog.heightFalloff);
                billboard.setFloat("uFogHeight", fog.height);
                billboard.setInt("uTex", 0);
                billboardTex.bind(0);
                glBindVertexArray(bbVAO);
                glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, treeCount);
                glBindVertexArray(0);
                glEnable(GL_CULL_FACE);
            }

            // Birds: a flock wheeling above the camera (positions on the CPU,
            // wingbeat in the shader). Drawn two-sided into the HDR buffer.
            if (birdsEnabled && birdCount > 0) {
                const float cx = camPos.x, cz = camPos.z;
                const float baseY = streamer.heightAt(cx, cz) + 95.0f; // fly higher
                std::vector<float> bi;
                bi.reserve(birdCount * 5);
                for (int i = 0; i < birdCount; ++i) {
                    const float ph = static_cast<float>(i) * 2.39996f;
                    const float R  = 50.0f + 45.0f * vhash2(static_cast<float>(i), 3.0f);
                    const float sp = 0.12f + 0.10f * vhash2(static_cast<float>(i), 9.0f);
                    const float hY = baseY + 30.0f * vhash2(static_cast<float>(i), 5.0f);
                    const float ang = static_cast<float>(now) * sp + ph;
                    const float bx = cx + std::cos(ang) * R;
                    const float bz = cz + std::sin(ang) * R;
                    const float by = hY + 3.0f * std::sin(ang * 0.7f + ph);
                    bi.insert(bi.end(), {bx, by, bz, ang, ph});
                }
                glDisable(GL_CULL_FACE);
                birdField.upload(bi);
                bird.bind();
                bird.setMat4("uViewProj", mainVP);
                bird.setFloat("uTime", static_cast<float>(now));
                bird.setFloat("uSize", birdSize);
                bird.setVec3("uColor", glm::vec3(0.02f, 0.02f, 0.03f));
                birdField.draw(GL_TRIANGLES, 18);
                glEnable(GL_CULL_FACE);
            }

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

            // --- Rain streaks (storm), into the HDR buffer --------------
            if (rainIntensity > 0.001f) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);
                rain.bind();
                rain.setMat4("uViewProj", mainVP);
                rain.setVec3("uBoxCenter", camPos);
                rain.setFloat("uBoxHeight", rainBoxH);
                rain.setFloat("uBoxHalf", rainBoxHalf);
                rain.setFloat("uStreak", glm::mix(1.2f, 3.0f, weather));
                rain.setFloat("uTime", static_cast<float>(now));
                rain.setVec3("uWind",
                             glm::normalize(glm::vec3(0.6f, 0.0f, 0.3f)) *
                                 glm::mix(0.05f, 0.6f, weather));
                rain.setVec3("uRainColor",
                             glm::clamp(light.ambient * 2.5f + light.color * 0.12f,
                                        glm::vec3(0.0f), glm::vec3(2.0f)));
                rain.setFloat("uIntensity", rainIntensity);
                glBindVertexArray(rainVAO);
                glDrawArrays(GL_LINES, 0, rainDrops * 2);
                glBindVertexArray(0);
                glDepthMask(GL_TRUE);
                glDisable(GL_BLEND);
            }

            // --- Fireflies: night-only glowing wanderers, additive into HDR ---
            const float nightF = 1.0f - dayF;
            if (fireflyEnabled && fireflyCount > 0 && nightF > 0.03f) {
                const glm::vec2 camXZ(camPos.x, camPos.z);
                std::vector<float> fi;
                fi.reserve(fireflyCount * 4);
                for (int i = 0; i < fireflyCount; ++i) {
                    glm::vec3& f = fireflies[i];
                    glm::vec2 home(f.x, f.y);
                    if (glm::length(home - camXZ) > fireflyRadius) {
                        const float ang = flyU(flyRng) * 6.2831f;
                        const float rad = std::sqrt(flyU(flyRng)) * fireflyRadius;
                        home = camXZ + rad * glm::vec2(std::cos(ang), std::sin(ang));
                        f.x = home.x; f.y = home.y;
                    }
                    const float ph = f.z;
                    const float t  = static_cast<float>(now);
                    const float wx = home.x + std::sin(t * 0.7f + ph) * 1.3f;
                    const float wz = home.y + std::cos(t * 0.9f + ph * 1.7f) * 1.3f;
                    const float hover = 0.5f + 0.5f * std::sin(t * 1.1f + ph * 2.3f);
                    const float wy = streamer.heightAt(wx, wz) + 0.4f + hover * 0.9f;
                    fi.insert(fi.end(), {wx, wy, wz, ph});
                }
                const glm::vec3 camRight = camera.right();
                const glm::vec3 camUp = glm::normalize(glm::cross(camRight, camera.front()));
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ONE); // additive glow
                glDepthMask(GL_FALSE);
                glDisable(GL_CULL_FACE);
                fireflyField.upload(fi);
                firefly.bind();
                firefly.setMat4("uViewProj", mainVP);
                firefly.setVec3("uCamRight", camRight);
                firefly.setVec3("uCamUp", camUp);
                firefly.setFloat("uSize", fireflySize);
                firefly.setFloat("uTime", static_cast<float>(now));
                firefly.setFloat("uNight", nightF);
                firefly.setVec3("uColor", glm::vec3(0.7f, 1.0f, 0.35f));
                fireflyField.draw(GL_TRIANGLE_STRIP, 4);
                glDepthMask(GL_TRUE);
                glDisable(GL_BLEND);
                glEnable(GL_CULL_FACE);
            }

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

                // Crosshair, sized to the view.
                const float ch = std::max(10.0f, vsize.y * 0.018f);
                const ImU32 white = IM_COL32(255, 255, 255, 220);
                dl->AddLine(ImVec2(c.x - ch, c.y), ImVec2(c.x + ch, c.y), white, 2.0f);
                dl->AddLine(ImVec2(c.x, c.y - ch), ImVec2(c.x, c.y + ch), white, 2.0f);

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
            }

            gui.endFrame();
            window.swapBuffers();
        }

        glDeleteBuffers(1, &rainVBO);
        glDeleteVertexArrays(1, &rainVAO);
        glDeleteBuffers(1, &treeVBO);
        glDeleteBuffers(1, &treeInstVBO);
        glDeleteVertexArrays(1, &treeVAO);
        glDeleteVertexArrays(1, &bbVAO);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        return 1;
    }

    return 0;
}
