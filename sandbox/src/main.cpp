#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <future>
#include <random>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <imgui_internal.h> // DockBuilder API for the default panel layout

#include <fitzel/Fitzel.hpp>

using namespace fitzel;

// On laptops with hybrid graphics (NVIDIA Optimus / AMD PowerXpress), ask the
// driver to run us on the discrete high-performance GPU instead of the iGPU.
#if defined(_WIN32)
extern "C" {
    __declspec(dllexport) unsigned long NvOptimusEnablement = 1;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

// --- Camera path recorder/player -------------------------------------------
// A keyframe of camera state at a point in time along the path.
struct CamKey {
    float     t;     // seconds from the path start
    glm::vec3 pos;
    float     yaw;   // degrees (unwrapped across keys for smooth interpolation)
    float     pitch;
    float     fov;
};

// Centripetal-ish Catmull-Rom: a smooth curve passing through b and c, using the
// neighbours a and d for tangents. Works for float and glm::vec3 alike.
template <typename T>
static T catmull(const T& a, const T& b, const T& c, const T& d, float t) {
    const float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * b) + (-a + c) * t +
                   (2.0f * a - 5.0f * b + 4.0f * c - d) * t2 +
                   (-a + 3.0f * b - 3.0f * c + d) * t3);
}

// Sample the path at time `time`, writing the interpolated camera pose. Position
// and pose channels are Catmull-Rom smoothed; the path is clamped at both ends.
static void samplePath(const std::vector<CamKey>& k, float time,
                       glm::vec3& pos, float& yaw, float& pitch, float& fov) {
    const int n = static_cast<int>(k.size());
    if (n == 0) return;
    if (n == 1 || time <= k.front().t) {
        pos = k.front().pos; yaw = k.front().yaw;
        pitch = k.front().pitch; fov = k.front().fov; return;
    }
    if (time >= k.back().t) {
        pos = k.back().pos; yaw = k.back().yaw;
        pitch = k.back().pitch; fov = k.back().fov; return;
    }
    int i = 0;
    while (i < n - 1 && k[i + 1].t <= time) ++i;
    const float seg = k[i + 1].t - k[i].t;
    const float u   = seg > 1e-6f ? (time - k[i].t) / seg : 0.0f;
    const int i0 = std::max(0, i - 1), i1 = i, i2 = i + 1, i3 = std::min(n - 1, i + 2);
    pos   = catmull(k[i0].pos,   k[i1].pos,   k[i2].pos,   k[i3].pos,   u);
    yaw   = catmull(k[i0].yaw,   k[i1].yaw,   k[i2].yaw,   k[i3].yaw,   u);
    pitch = catmull(k[i0].pitch, k[i1].pitch, k[i2].pitch, k[i3].pitch, u);
    fov   = catmull(k[i0].fov,   k[i1].fov,   k[i2].fov,   k[i3].fov,   u);
}

// Squared distance from (x,z) to a polyline in the XZ plane. Returns a huge
// value for an empty/degenerate line, so callers can test "< clearance^2"
// unconditionally. Used to keep vegetation off the road.
static float roadDistanceSq(const std::vector<glm::vec2>& line, float x, float z) {
    if (line.size() < 2) return 1e30f;
    const glm::vec2 p(x, z);
    float best = 1e30f;
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        const glm::vec2 a = line[i], b = line[i + 1];
        const glm::vec2 ab = b - a;
        const float len2 = glm::dot(ab, ab);
        float t = len2 > 1e-8f ? glm::dot(p - a, ab) / len2 : 0.0f;
        t = glm::clamp(t, 0.0f, 1.0f);
        const glm::vec2 d = p - (a + ab * t);
        best = std::min(best, glm::dot(d, d));
    }
    return best;
}

// Smooth 2D value noise (~0..1) for meadow patchiness in grass placement.
static float vhash2(float x, float z) {
    const float h = std::sin(x * 127.1f + z * 311.7f) * 43758.5453f;
    return h - std::floor(h);
}
static float valNoise2(float x, float z) {
    const float xi = std::floor(x), zi = std::floor(z);
    const float xf = x - xi, zf = z - zi;
    const float u = xf * xf * (3.0f - 2.0f * xf);
    const float v = zf * zf * (3.0f - 2.0f * zf);
    const float a = vhash2(xi, zi),     b = vhash2(xi + 1.0f, zi);
    const float c = vhash2(xi, zi + 1.0f), d = vhash2(xi + 1.0f, zi + 1.0f);
    return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}

// A small flower: a green stem (crossed quads, tint 0), a ring of cupped petals
// tinted per-instance (tint 1) and a yellow centre disc (tint 2). Petals tilt up
// and outward so the bloom reads as a flower from across the meadow, not a disc.
// Returns interleaved floats: pos3, normal3, tint1.
static std::vector<float> makeFlowerMesh() {
    std::vector<float> v;
    auto push = [&](glm::vec3 p, glm::vec3 n, float t) {
        v.insert(v.end(), {p.x, p.y, p.z, n.x, n.y, n.z, t});
    };
    auto tri = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, float t) {
        const glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
        push(a, n, t); push(b, n, t); push(c, n, t);
    };
    auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, glm::vec3 n, float t) {
        push(a, n, t); push(b, n, t); push(c, n, t);
        push(a, n, t); push(c, n, t); push(d, n, t);
    };
    const float sh = 0.55f, hw = 0.018f;
    quad({-hw, 0, 0}, {hw, 0, 0}, {hw, sh, 0}, {-hw, sh, 0}, {0, 0, 1}, 0.0f);
    quad({0, 0, -hw}, {0, 0, hw}, {0, sh, hw}, {0, sh, -hw}, {1, 0, 0}, 0.0f);

    // Petals: rounded lobes, nearly flat and wide (buttercup/daisy look) so the
    // open bloom reads as a soft colour speck rather than a cupped star.
    const int   P = 6, ARC = 5;
    const float rb = 0.04f, rt = 0.30f, lift = 0.05f;
    const float PI = 3.14159265f;
    for (int i = 0; i < P; ++i) {
        const float am = static_cast<float>(i) / P * 6.2831853f;
        const float d  = (PI / P) * 1.2f; // wide, overlapping petals -> full bloom
        const glm::vec3 baseC(std::cos(am) * rb, sh, std::sin(am) * rb);
        glm::vec3 prev(0.0f);
        for (int k = 0; k <= ARC; ++k) {
            const float f = static_cast<float>(k) / ARC;   // 0..1 across the petal
            const float a = am - d + 2.0f * d * f;
            const float shape = std::sin(f * PI);          // round: 0 edges, 1 middle
            const float r = rb + (rt - rb) * shape;
            const float y = sh + lift * shape;
            const glm::vec3 p(std::cos(a) * r, y, std::sin(a) * r);
            if (k > 0) tri(baseC, prev, p, 1.0f);
            prev = p;
        }
    }
    // Small yellow centre disc, almost flush with the petals.
    const int   C = 10;
    const float rc = 0.08f;
    const glm::vec3 cc(0.0f, sh + 0.012f, 0.0f);
    for (int i = 0; i < C; ++i) {
        const float a0 = static_cast<float>(i) / C * 6.2831853f;
        const float a1 = static_cast<float>(i + 1) / C * 6.2831853f;
        const glm::vec3 p0(std::cos(a0) * rc, sh + 0.008f, std::sin(a0) * rc);
        const glm::vec3 p1(std::cos(a1) * rc, sh + 0.008f, std::sin(a1) * rc);
        tri(cc, p0, p1, 2.0f);
    }
    return v;
}

// A capped cylinder with its axle along local X (for vehicle wheels). Radius r,
// half-thickness ht, `seg` sides. Normals point outward; UVs are placeholders.
static MeshData makeCylinderX(float r, float ht, int seg) {
    MeshData m;
    const float TAU = 6.28318530718f;
    for (int i = 0; i < seg; ++i) {
        const float a0 = static_cast<float>(i) / seg * TAU;
        const float a1 = static_cast<float>(i + 1) / seg * TAU;
        const glm::vec3 n0(0.0f, std::cos(a0), std::sin(a0));
        const glm::vec3 n1(0.0f, std::cos(a1), std::sin(a1));
        const auto base = static_cast<std::uint32_t>(m.vertices.size());
        m.vertices.push_back({{-ht, r * n0.y, r * n0.z}, n0, {0.0f, 0.0f}});
        m.vertices.push_back({{ ht, r * n0.y, r * n0.z}, n0, {0.0f, 1.0f}});
        m.vertices.push_back({{ ht, r * n1.y, r * n1.z}, n1, {1.0f, 1.0f}});
        m.vertices.push_back({{-ht, r * n1.y, r * n1.z}, n1, {1.0f, 0.0f}});
        m.indices.insert(m.indices.end(),
                         {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    for (int side = 0; side < 2; ++side) {
        const float x = side ? ht : -ht;
        const glm::vec3 nc(side ? 1.0f : -1.0f, 0.0f, 0.0f);
        const auto c = static_cast<std::uint32_t>(m.vertices.size());
        m.vertices.push_back({{x, 0.0f, 0.0f}, nc, {0.5f, 0.5f}});
        for (int i = 0; i <= seg; ++i) {
            const float a = static_cast<float>(i) / seg * TAU;
            m.vertices.push_back({{x, r * std::cos(a), r * std::sin(a)}, nc, {0.0f, 0.0f}});
        }
        for (int i = 0; i < seg; ++i) {
            const std::uint32_t a = c + 1 + i, b = c + 2 + i;
            if (side) m.indices.insert(m.indices.end(), {c, a, b});
            else      m.indices.insert(m.indices.end(), {c, b, a});
        }
    }
    return m;
}

int main() {
    try {
        Window window(WindowConfig{
            .width  = 1280,
            .height = 720,
            .title  = "Fitzel - Infinite Terrain, CSM, Materials",
            .vsync  = true,
        });

        Input  input(window);                  // before Gui (callback chaining)
        Gui    gui(window);
        Camera camera({0.0f, 10.0f, 30.0f}, -90.0f, -5.0f);
        camera.moveSpeed = 20.0f;

        Shader lit = Shader::fromFiles("assets/shaders/lit.vert",
                                       "assets/shaders/lit.frag");
        if (!lit.isValid()) {
            std::fprintf(stderr, "Failed to load lit shader\n");
            return 1;
        }

        // Slope/height-driven terrain palette, exposed as material parameters.
        struct TerrainLook {
            glm::vec3 sand{0.76f, 0.70f, 0.48f};
            glm::vec3 grass{0.23f, 0.42f, 0.16f};
            glm::vec3 rock{0.38f, 0.34f, 0.30f};
            glm::vec3 snow{0.92f, 0.94f, 0.98f};
            float snowLevel      = 16.0f;
            float rockSlope      = 0.62f; // flatter than this -> rock
            float slopeSharpness = 0.14f;
            float detailScale    = 0.35f; // micro-detail frequency
            float detailStrength = 1.5f;  // normal-perturbation strength
        } look;

        // Terrain PBR-ish albedo textures (triplanar), loaded from the repo's
        // textures/ folder (path injected by CMake).
        const std::string texDir = FITZEL_TEXTURE_DIR;
        Texture texSand   = Texture::fromFile(texDir + "/coast_sand_01_diff_4k.jpg");
        Texture texGround = Texture::fromFile(texDir + "/aerial_rocks_01_diff_4k.jpg");
        Texture texCliff  = Texture::fromFile(texDir + "/rocky_terrain_02_diff_4k.jpg");
        Texture texSnow   = Texture::fromFile(texDir + "/snow_02_diff_4k.jpg");
        // Matching triplanar normal maps. PolyHaven ships these as DWAA-compressed
        // EXR (which most loaders, incl. tinyexr, can't decode), so they're
        // converted to PNG once (see README); no vertical flip.
        Texture texSandN   = Texture::fromFile(texDir + "/coast_sand_01_nor_gl_4k.png", false);
        Texture texGroundN = Texture::fromFile(texDir + "/aerial_rocks_01_nor_gl_4k.png", false);
        Texture texCliffN  = Texture::fromFile(texDir + "/rocky_terrain_02_nor_gl_4k.png", false);
        Texture texSnowN   = Texture::fromFile(texDir + "/snow_02_nor_gl_4k.png", false);
        if (!texSand.isValid() || !texGround.isValid() ||
            !texCliff.isValid() || !texSnow.isValid()) {
            std::fprintf(stderr, "Warning: some terrain textures failed to load from %s\n",
                         texDir.c_str());
        }
        float texScale       = 0.08f; // world units -> texture tiling
        float normalStrength = 1.0f;

        // Materials describe surface appearance; the renderer feeds in lighting.
        Material terrainMat(lit);
        terrainMat.set("uColorMode", 1)
                  .setTexture("uTexSand", texSand, 6)
                  .setTexture("uTexGround", texGround, 3)
                  .setTexture("uTexCliff", texCliff, 4)
                  .setTexture("uTexSnow", texSnow, 5)
                  .setTexture("uTexSandN", texSandN, 8)
                  .setTexture("uTexGroundN", texGroundN, 9)
                  .setTexture("uTexCliffN", texCliffN, 10)
                  .setTexture("uTexSnowN", texSnowN, 11);

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
        int  viewW = hdrW, viewH = hdrH;
        bool viewportHovered = false;
        glm::vec2 viewportMouseNdc(0.0f); // cursor within the viewport, NDC [-1,1]
        bool viewportClicked = false;     // left-click landed on the viewport image
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

        // Cloud controls.
        float cloudCoverage = 0.5f;
        float cloudDensity  = 1.0f;
        float cloudScale    = 0.0025f;
        float cloudSpeed    = 5.0f;
        float cloudBottom   = 140.0f;
        float cloudTop      = 320.0f;

        // Weather: 0 = clear .. 1 = storm. Drives clouds, light, fog, waves, rain.
        float weather     = 0.0f;
        bool  autoWeather = true;

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
        GLuint grassVAO = 0, bladeVBO = 0, grassInstVBO = 0;
        {
            const float blade[] = { // triangle strip: (x, h01)
                -0.5f, 0.0f,  0.5f, 0.0f,  -0.45f, 0.33f,  0.45f, 0.33f,
                -0.30f, 0.66f, 0.30f, 0.66f,  0.0f, 1.0f };
            glGenVertexArrays(1, &grassVAO);
            glBindVertexArray(grassVAO);
            glGenBuffers(1, &bladeVBO);
            glBindBuffer(GL_ARRAY_BUFFER, bladeVBO);
            glBufferData(GL_ARRAY_BUFFER, sizeof(blade), blade, GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
            glGenBuffers(1, &grassInstVBO);
            glBindBuffer(GL_ARRAY_BUFFER, grassInstVBO);
            const GLsizei is = 7 * sizeof(float);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, is, (void*)0);
            glVertexAttribDivisor(1, 1);
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, is, (void*)(3 * sizeof(float)));
            glVertexAttribDivisor(2, 1);
            glEnableVertexAttribArray(3);
            glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, is, (void*)(4 * sizeof(float)));
            glVertexAttribDivisor(3, 1);
            glEnableVertexAttribArray(4);
            glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, is, (void*)(5 * sizeof(float)));
            glVertexAttribDivisor(4, 1);
            glEnableVertexAttribArray(5);
            glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, is, (void*)(6 * sizeof(float)));
            glVertexAttribDivisor(5, 1);
            glBindVertexArray(0);
        }
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
                    const float rim   = 1.0f - glm::smoothstep(R * 0.82f, R,
                                                               std::sqrt(x * x + z * z));
                    const int   count = static_cast<int>(per * dens
                                        * glm::mix(0.35f, 1.0f, lush) * rim);
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
        {
            ModelData md = loadGltf(std::string(FITZEL_MODEL_DIR) + "/tree1.glb");
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
        Texture  roadTex = Texture::fromFile(texDir + "/asphalt_02_diff_4k.jpg");
        Material roadMat(lit);
        roadMat.set("uColorMode", 2).setTexture("uTexture", roadTex, 0);
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
            if (n < 2) { roadMesh = Mesh(); roadVerts = 0; return; }

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

        // Tree instances live here (filled by regenTrees below); declared early
        // so flower placement can cluster blooms around the trees.
        std::vector<float> treeInst;
        int                treeCount = 0;

        // --- Flowers: GPU-instanced blooms scattered through lush grass ---
        Shader flower = Shader::fromFiles("assets/shaders/flower.vert",
                                          "assets/shaders/flower.frag");
        if (!flower.isValid()) { std::fprintf(stderr, "Failed to load flower shader\n"); return 1; }
        GLuint flowerVAO = 0, flowerBaseVBO = 0, flowerInstVBO = 0;
        int    flowerVerts = 0;
        {
            const std::vector<float> fm = makeFlowerMesh();
            flowerVerts = static_cast<int>(fm.size() / 7);
            glGenVertexArrays(1, &flowerVAO);
            glBindVertexArray(flowerVAO);
            glGenBuffers(1, &flowerBaseVBO);
            glBindBuffer(GL_ARRAY_BUFFER, flowerBaseVBO);
            glBufferData(GL_ARRAY_BUFFER, fm.size() * sizeof(float), fm.data(), GL_STATIC_DRAW);
            const GLsizei bs = 7 * sizeof(float);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, bs, (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, bs, (void*)(3 * sizeof(float)));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, bs, (void*)(6 * sizeof(float)));
            glGenBuffers(1, &flowerInstVBO);
            glBindBuffer(GL_ARRAY_BUFFER, flowerInstVBO);
            const GLsizei is = 8 * sizeof(float); // iPos3, iYaw, iScale, iColor3
            glEnableVertexAttribArray(3);
            glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, is, (void*)0);
            glVertexAttribDivisor(3, 1);
            glEnableVertexAttribArray(4);
            glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, is, (void*)(3 * sizeof(float)));
            glVertexAttribDivisor(4, 1);
            glEnableVertexAttribArray(5);
            glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, is, (void*)(4 * sizeof(float)));
            glVertexAttribDivisor(5, 1);
            glEnableVertexAttribArray(6);
            glVertexAttribPointer(6, 3, GL_FLOAT, GL_FALSE, is, (void*)(5 * sizeof(float)));
            glVertexAttribDivisor(6, 1);
            glBindVertexArray(0);
        }
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
            flowerCount = static_cast<int>(out.size() / 8);
            glBindBuffer(GL_ARRAY_BUFFER, flowerInstVBO);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(out.size() * sizeof(float)),
                         out.data(), GL_DYNAMIC_DRAW);
        };

        // --- Birds: a small flock of flapping billboards circling overhead --
        Shader bird = Shader::fromFiles("assets/shaders/bird.vert",
                                        "assets/shaders/bird.frag");
        if (!bird.isValid()) { std::fprintf(stderr, "Failed to load bird shader\n"); return 1; }
        GLuint birdVAO = 0, birdBaseVBO = 0, birdInstVBO = 0;
        {
            const float bm[] = { // pos3, flap ; two triangles (left + right wing)
                0,0, 0.6f,0,  -1,0,0,1,   0,0,-0.4f,0,
                0,0, 0.6f,0,   0,0,-0.4f,0, 1,0,0,1 };
            glGenVertexArrays(1, &birdVAO);
            glBindVertexArray(birdVAO);
            glGenBuffers(1, &birdBaseVBO);
            glBindBuffer(GL_ARRAY_BUFFER, birdBaseVBO);
            glBufferData(GL_ARRAY_BUFFER, sizeof(bm), bm, GL_STATIC_DRAW);
            const GLsizei bs = 4 * sizeof(float);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, bs, (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, bs, (void*)(3 * sizeof(float)));
            glGenBuffers(1, &birdInstVBO);
            glBindBuffer(GL_ARRAY_BUFFER, birdInstVBO);
            const GLsizei is = 5 * sizeof(float); // iPos3, iYaw, iPhase
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, is, (void*)0);
            glVertexAttribDivisor(2, 1);
            glEnableVertexAttribArray(3);
            glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, is, (void*)(3 * sizeof(float)));
            glVertexAttribDivisor(3, 1);
            glEnableVertexAttribArray(4);
            glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, is, (void*)(4 * sizeof(float)));
            glVertexAttribDivisor(4, 1);
            glBindVertexArray(0);
        }
        bool  birdsEnabled = true;
        int   birdCount    = 18;
        float birdSize     = 2.2f;

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
        Audio audio;
        const std::string soundDir = FITZEL_SOUND_DIR;
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

        // --- Preset system ------------------------------------------------
        // Every tunable is bound by name to a getter/setter, so presets are a
        // simple "key value" text file per scene look. Unknown/missing keys are
        // ignored, so old presets keep working as fields come and go.
        struct PField {
            std::string key;
            std::function<float()>     get;
            std::function<void(float)> set;
        };
        std::vector<PField> fields;
        auto addF = [&](const char* k, float& r) {
            fields.push_back({k, [&r] { return r; }, [&r](float v) { r = v; }});
        };
        auto addB = [&](const char* k, bool& r) {
            fields.push_back({k, [&r] { return r ? 1.0f : 0.0f; },
                                 [&r](float v) { r = v > 0.5f; }});
        };
        auto addI = [&](const char* k, int& r) {
            fields.push_back({k, [&r] { return static_cast<float>(r); },
                                 [&r](float v) { r = static_cast<int>(std::lround(v)); }});
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

        namespace fs = std::filesystem;
        const fs::path presetDir = "presets";
        auto listPresets = [&] {
            std::vector<std::string> out;
            std::error_code ec;
            if (fs::exists(presetDir, ec))
                for (const auto& e : fs::directory_iterator(presetDir, ec))
                    if (e.path().extension() == ".fzp") out.push_back(e.path().stem().string());
            std::sort(out.begin(), out.end());
            return out;
        };
        auto savePreset = [&](const std::string& name) {
            std::error_code ec; fs::create_directories(presetDir, ec);
            std::ofstream f(presetDir / (name + ".fzp"));
            for (const PField& fld : fields) f << fld.key << ' ' << fld.get() << '\n';
        };
        auto loadPreset = [&](const std::string& name) {
            std::ifstream f(presetDir / (name + ".fzp"));
            if (!f) return false;
            std::map<std::string, float> vals;
            std::string k; float v;
            while (f >> k >> v) vals[k] = v;
            for (const PField& fld : fields) {
                auto it = vals.find(fld.key);
                if (it != vals.end()) fld.set(it->second);
            }
            return true;
        };
        auto deletePreset = [&](const std::string& name) {
            std::error_code ec; fs::remove(presetDir / (name + ".fzp"), ec);
        };
        std::vector<std::string> presetList = listPresets();
        int  presetSel = -1;
        char presetName[64] = "my scene";

        double lastTime = window.time();

        while (window.isOpen()) {
            window.pollEvents();
            input.update();

            const double now = window.time();
            const float  dt  = static_cast<float>(now - lastTime);
            lastTime = now;

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
                if (presentMode) {
                    presentMode = false;
                    glfwSetWindowMonitor(window.nativeHandle(), nullptr,
                                         savedWX, savedWY, savedWW, savedWH, 0);
                } else if (vehicleMode)  { vehicleMode = false; }
                else if (fpsMode) { fpsMode = false; input.setCursorLocked(false); }
                else              { window.requestClose(); }
            }
            prevEsc = escDown;

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

                glm::vec3 pos = camera.position();
                pos += move * camera.moveSpeed * dt;

                // Gravity + jump.
                const bool space = input.isKeyDown(GLFW_KEY_SPACE);
                if (space && !prevSpace && grounded) fpsVelY = 9.0f;
                prevSpace = space;
                fpsVelY -= 25.0f * dt;
                pos.y += fpsVelY * dt;

                const float ground = streamer.heightAt(pos.x, pos.z) + eyeHeight;
                if (pos.y <= ground) { pos.y = ground; fpsVelY = 0.0f; grounded = true; }
                else                 { grounded = false; }
                camera.setPosition(pos);
            } else {
                // Look only when dragging over the viewport panel (or already
                // locked into a drag); the surrounding dock panels keep the mouse.
                const bool mouseLook = input.isMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)
                                       && (viewportHovered || presentMode || input.isCursorLocked());
                if (mouseLook != input.isCursorLocked()) input.setCursorLocked(mouseLook);
                if (mouseLook) {
                    const glm::vec2 d = input.mouseDelta();
                    camera.processMouse(d.x, d.y);
                }
                if (viewportHovered || presentMode) camera.processScroll(input.scrollDelta());
                if (!gui.wantsKeyboard()) {
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
                    grassCount = static_cast<int>(data.size() / 7);
                    glBindBuffer(GL_ARRAY_BUFFER, grassInstVBO);
                    glBufferData(GL_ARRAY_BUFFER,
                                 static_cast<GLsizeiptr>(data.size() * sizeof(float)),
                                 data.data(), GL_DYNAMIC_DRAW);
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
            // fresh lightning flash.
            audio.setMasterVolume(muted ? 0.0f : masterVolume);
            rainSnd.setVolume(rainIntensity);
            windSnd.setVolume(glm::smoothstep(0.15f, 1.0f, weather) * 0.9f);
            breezeSnd.setVolume((1.0f - glm::smoothstep(0.0f, 0.5f, weather)) * 0.5f);
            const bool flashOn = flash > 0.25f;
            if (flashOn && !prevFlashOn) {
                thunderSnd.setVolume(glm::clamp(weather, 0.3f, 1.0f));
                thunderSnd.play();
            }
            prevFlashOn = flashOn;

            // --- Day/night: advance time, derive sun direction and lighting ---
            if (dayLength > 0.1f) {
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
            // HDR radiance: the sun is much brighter than 1 so tonemapping
            // produces highlights and contrast instead of a flat look.
            light.color   = sunCol * (0.12f + 0.95f * dayF) * 3.4f * lightDim;
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

            // --- UI ------------------------------------------------------
            gui.beginFrame();
            if (presentMode) {
                // Presentation: hide the editor UI, render the scene full-window.
                window.framebufferSize(viewW, viewH);
                viewportHovered = true;
            } else {
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

                ImGuiID central = 0;
                ImGuiID column = ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Right, 0.26f,
                                                             nullptr, &central);
                ImGuiID bottom = 0;
                ImGuiID top = ImGui::DockBuilderSplitNode(column, ImGuiDir_Up, 0.40f,
                                                          nullptr, &bottom);

                ImGui::DockBuilderDockWindow("Viewport", central);
                ImGui::DockBuilderDockWindow("Stats", top);
                ImGui::DockBuilderDockWindow("Camera", top);
                ImGui::DockBuilderDockWindow("Weather & audio", top);
                ImGui::DockBuilderDockWindow("Sky & atmosphere", bottom);
                ImGui::DockBuilderDockWindow("Colour grade", bottom);
                ImGui::DockBuilderDockWindow("Water", bottom);
                ImGui::DockBuilderDockWindow("Terrain", bottom);
                ImGui::DockBuilderDockWindow("Vegetation", bottom);
                ImGui::DockBuilderDockWindow("Camera path", bottom);
                ImGui::DockBuilderDockWindow("Roads", bottom);
                ImGui::DockBuilderDockWindow("Vehicle", bottom);
                ImGui::DockBuilderDockWindow("Presets", bottom);
                ImGui::DockBuilderFinish(dockId);
            }

            // Central scene viewport: shows the composited render texture. Its
            // content size drives the render resolution (set below for next pass).
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            if (ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar
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
                const ImVec2 mp   = ImGui::GetIO().MousePos;
                viewportMouseNdc = glm::vec2(
                    (rsz.x > 0.0f ? (mp.x - rmin.x) / rsz.x : 0.5f) * 2.0f - 1.0f,
                    1.0f - (rsz.y > 0.0f ? (mp.y - rmin.y) / rsz.y : 0.5f) * 2.0f);
                viewportClicked = viewportHovered &&
                                  ImGui::IsMouseClicked(ImGuiMouseButton_Left);

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
            } else {
                viewportHovered = false;
                viewportClicked = false;
            }
            ImGui::End();
            ImGui::PopStyleVar();

            if (ImGui::Begin("Stats")) {
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
            ImGui::End();

            if (ImGui::Begin("Camera")) {
                if (ImGui::Checkbox("First-person (F)", &fpsMode)) {
                    input.setCursorLocked(fpsMode);
                    fpsVelY = 0.0f;
                    if (fpsMode) {
                        const glm::vec3 p = camera.position();
                        camera.setPosition({p.x, streamer.heightAt(p.x, p.z) + eyeHeight, p.z});
                    }
                }
                ImGui::SameLine();
                ImGui::TextDisabled(fpsMode ? "(walk + jump, Esc to exit)" : "(free fly)");
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
            ImGui::End();

            if (ImGui::Begin("Weather & audio")) {
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
            ImGui::End();

            if (ImGui::Begin("Sky & atmosphere")) {
                ImGui::SliderFloat("Time of day", &timeOfDay, 0.0f, 24.0f, "%.1f h");
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
            }
            ImGui::End();

            if (ImGui::Begin("Colour grade")) {
                ImGui::SliderFloat("Hue",        &hueShift, -180.0f, 180.0f, "%.0f");
                ImGui::SliderFloat("Saturation", &saturation, 0.0f, 2.0f);
                ImGui::SliderFloat("Brightness", &valueGain, 0.3f, 2.0f);
                ImGui::SliderFloat("Warmth",     &warmth, -0.5f, 0.5f);
                ImGui::SliderFloat("Contrast",   &contrast, 0.0f, 0.6f);
            }
            ImGui::End();

            if (ImGui::Begin("Water")) {
                ImGui::SliderFloat("Level",       &waterLevel, -15.0f, 15.0f);
                ImGui::SliderFloat("Swell height",&waveHeight, 0.0f, 2.5f);
                ImGui::SliderFloat("Choppiness",  &waveChoppy, 0.0f, 1.0f);
                ImGui::SliderFloat("Ripples",     &waveStrength, 0.0f, 0.05f, "%.3f");
                ImGui::SliderFloat("Ripple size", &waveScale, 0.01f, 0.2f, "%.3f");
                ImGui::SliderFloat("Shore foam",  &foamWidth, 0.0f, 8.0f);
                ImGui::ColorEdit3("Tint",         &waterColor.x);
            }
            ImGui::End();

            if (ImGui::Begin("Terrain")) {
                ImGui::SeparatorText("Generator");
                ImGui::SliderFloat("Height",     &uiSettings.heightScale, 0.0f, 30.0f);
                ImGui::SliderFloat("Ridges",     &uiSettings.ridgeScale, 0.0f, 50.0f);
                ImGui::SliderFloat("Continents", &uiSettings.continentAmp, 0.0f, 3.0f);
                ImGui::SliderFloat("Biome size", &uiSettings.biomeFreq, 0.0005f, 0.004f, "%.4f");
                ImGui::SliderFloat("Terraces",   &uiSettings.terrace, 0.0f, 1.0f);
                ImGui::SliderFloat("Warp",       &uiSettings.warpStrength, 0.0f, 40.0f);
                ImGui::SliderFloat("Frequency",  &uiSettings.frequency, 0.003f, 0.05f, "%.3f");
                ImGui::SliderInt  ("Octaves",    &uiSettings.octaves, 1, 8);
                ImGui::SliderFloat("Seed",       &uiSettings.seed, 0.0f, 100.0f);
                if (ImGui::Button("Regenerate")) {
                    streamer.settings() = uiSettings;
                    streamer.rebuild();
                    streamer.update(camera.position());
                    grassDirty = true; // regrow vegetation on the new terrain
                    treeCenter = glm::vec2(1e9f);
                    roadDirty  = true; // re-drape roads on the new heights
                }
                ImGui::SeparatorText("Material (slope)");
                ImGui::SliderFloat("Texture scale", &texScale, 0.02f, 0.2f, "%.3f");
                ImGui::SliderFloat("Normal strength", &normalStrength, 0.0f, 1.0f);
                ImGui::SliderFloat("Rock slope",    &look.rockSlope, 0.0f, 1.0f);
                ImGui::SliderFloat("Slope blend",   &look.slopeSharpness, 0.02f, 0.4f);
                ImGui::SliderFloat("Snow level",    &look.snowLevel, 0.0f, 40.0f);
                ImGui::SliderFloat("Detail bump",   &look.detailStrength, 0.0f, 4.0f);
            }
            ImGui::End();

            if (ImGui::Begin("Vegetation")) {
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
            }
            ImGui::End();

            if (ImGui::Begin("Camera path")) {
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
            ImGui::End();

            if (ImGui::Begin("Roads")) {
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
                            Texture t = Texture::fromFile(texDir + "/" + roadTexFiles[i]);
                            if (t.isValid()) {
                                roadTex = std::move(t);
                                roadMat.setTexture("uTexture", roadTex, 0);
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
            ImGui::End();

            if (ImGui::Begin("Vehicle")) {
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
            ImGui::End();

            if (ImGui::Begin("Presets")) {
                // Applying a preset that changes terrain requires a rebuild+regrow,
                // exactly like the Regenerate button does.
                auto applyLoaded = [&] {
                    streamer.settings() = uiSettings;
                    streamer.rebuild();
                    streamer.update(camera.position());
                    grassDirty = true;
                    treeCenter = glm::vec2(1e9f);
                    roadDirty  = true;
                };

                ImGui::TextDisabled("Save the full scene look (terrain, sky, water, "
                                    "grade, vegetation...) as a named preset.");
                ImGui::InputText("Name", presetName, sizeof(presetName));
                if (ImGui::Button("Save preset")) {
                    if (presetName[0]) {
                        savePreset(presetName);
                        presetList = listPresets();
                        for (int i = 0; i < static_cast<int>(presetList.size()); ++i)
                            if (presetList[i] == presetName) presetSel = i;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Refresh")) { presetList = listPresets(); presetSel = -1; }

                ImGui::SeparatorText("Saved presets");
                if (presetList.empty()) {
                    ImGui::TextDisabled("(none yet)");
                }
                if (ImGui::BeginListBox("##presetlist", ImVec2(-1.0f, 150.0f))) {
                    for (int i = 0; i < static_cast<int>(presetList.size()); ++i) {
                        const bool sel = (presetSel == i);
                        if (ImGui::Selectable(presetList[i].c_str(), sel))
                            presetSel = i;
                        // Double-click loads immediately.
                        if (sel && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
                                && ImGui::IsItemHovered()) {
                            if (loadPreset(presetList[i])) applyLoaded();
                        }
                    }
                    ImGui::EndListBox();
                }

                const bool hasSel = presetSel >= 0 && presetSel < static_cast<int>(presetList.size());
                ImGui::BeginDisabled(!hasSel);
                if (ImGui::Button("Load")) {
                    if (loadPreset(presetList[presetSel])) applyLoaded();
                }
                ImGui::SameLine();
                if (ImGui::Button("Delete")) {
                    deletePreset(presetList[presetSel]);
                    presetList = listPresets();
                    presetSel = -1;
                }
                ImGui::EndDisabled();
            }
            ImGui::End();
            } // end editor UI (skipped in presentation mode)

            // Push the (possibly edited) terrain blend params into the material.
            terrainMat.set("uSnowLevel", look.snowLevel)
                      .set("uRockSlope", look.rockSlope)
                      .set("uSlopeSharpness", look.slopeSharpness)
                      .set("uDetailScale", look.detailScale)
                      .set("uDetailStrength", look.detailStrength)
                      .set("uTexScale", texScale)
                      .set("uSandLevel", waterLevel + 1.0f)
                      .set("uNormalStrength", normalStrength)
                      .set("uWaterLevel", waterLevel);

            // --- Submit the opaque scene once ---------------------------
            // Render at the docked viewport panel's size, not the whole window.
            const int   fbW = viewW, fbH = viewH;
            const float aspect = static_cast<float>(fbW) / static_cast<float>(fbH);
            const glm::mat4 proj = camera.projectionMatrix(aspect);

            renderer.setViewport(fbW, fbH);
            renderer.begin(camera, aspect, light);

            for (const TerrainChunk* chunk : streamer.visibleChunks()) {
                renderer.submit(chunk->mesh(), terrainMat, glm::mat4(1.0f));
            }

            // Road mesh is (re)built when points/width change (edited in the UI).
            if (roadDirty) buildRoad();
            if (roadEnabled && roadVerts > 0) {
                roadMat.set("uWaterLevel", waterLevel); // wet-darken submerged parts
                renderer.submit(roadMesh, roadMat, glm::mat4(1.0f));
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
            drawSky(glm::inverse(proj * reflView), reflEye, false);
            glCullFace(GL_FRONT); // mirroring flips winding
            renderer.renderScene(reflView, proj, reflEye,
                                 glm::vec4(0, 1, 0, -waterLevel + 0.1f), false);
            glCullFace(GL_BACK);

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
            }
            hdrRT.bind();
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            drawSky(glm::inverse(mainVP), camPos, false);
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
                glBindVertexArray(grassVAO);
                glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 7, grassCount);
                glBindVertexArray(0);
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
                glBindVertexArray(flowerVAO);
                glDrawArraysInstanced(GL_TRIANGLES, 0, flowerVerts, flowerCount);
                glBindVertexArray(0);
                glEnable(GL_CULL_FACE);
            }

            // Trees (instanced, per-material) into the HDR buffer.
            if (treeEnabled && treeCount > 0 && !treePrims.empty()) {
                glDisable(GL_CULL_FACE);
                tree.bind();
                tree.setMat4("uViewProj", mainVP);
                tree.setFloat("uTime", static_cast<float>(now));
                tree.setVec2("uWindDir", glm::normalize(glm::vec2(0.6f, 0.3f)));
                tree.setFloat("uWindStrength", glm::mix(0.05f, 0.4f, weather));
                tree.setFloat("uTreeHeight", treeLocalHeight);
                tree.setVec3("uCamPos", camPos);
                tree.setFloat("uLodNear", lodNear);
                tree.setVec3("uViewPos", camPos);
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
            }

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
                const float baseY = streamer.heightAt(cx, cz) + 55.0f;
                std::vector<float> bi;
                bi.reserve(birdCount * 5);
                for (int i = 0; i < birdCount; ++i) {
                    const float ph = static_cast<float>(i) * 2.39996f;
                    const float R  = 45.0f + 35.0f * vhash2(static_cast<float>(i), 3.0f);
                    const float sp = 0.12f + 0.10f * vhash2(static_cast<float>(i), 9.0f);
                    const float hY = baseY + 18.0f * vhash2(static_cast<float>(i), 5.0f);
                    const float ang = static_cast<float>(now) * sp + ph;
                    const float bx = cx + std::cos(ang) * R;
                    const float bz = cz + std::sin(ang) * R;
                    const float by = hY + 3.0f * std::sin(ang * 0.7f + ph);
                    bi.insert(bi.end(), {bx, by, bz, ang, ph});
                }
                glDisable(GL_CULL_FACE);
                glBindBuffer(GL_ARRAY_BUFFER, birdInstVBO);
                glBufferData(GL_ARRAY_BUFFER,
                             static_cast<GLsizeiptr>(bi.size() * sizeof(float)),
                             bi.data(), GL_DYNAMIC_DRAW);
                bird.bind();
                bird.setMat4("uViewProj", mainVP);
                bird.setFloat("uTime", static_cast<float>(now));
                bird.setFloat("uSize", birdSize);
                bird.setVec3("uColor", glm::vec3(0.02f, 0.02f, 0.03f));
                glBindVertexArray(birdVAO);
                glDrawArraysInstanced(GL_TRIANGLES, 0, 6, birdCount);
                glBindVertexArray(0);
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

            // Composite: to the viewport texture (editor) or straight to the
            // screen (presentation mode, no dock panel to display it in).
            if (presentMode) {
                int winW = 0, winH = 0;
                window.framebufferSize(winW, winH);
                RenderTarget::unbind(winW, winH);
            } else {
                viewportRT.bind();
            }
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

            gui.endFrame();
            window.swapBuffers();
        }

        glDeleteBuffers(1, &rainVBO);
        glDeleteVertexArrays(1, &rainVAO);
        glDeleteBuffers(1, &bladeVBO);
        glDeleteBuffers(1, &grassInstVBO);
        glDeleteVertexArrays(1, &grassVAO);
        glDeleteBuffers(1, &treeVBO);
        glDeleteBuffers(1, &treeInstVBO);
        glDeleteVertexArrays(1, &treeVAO);
        glDeleteVertexArrays(1, &bbVAO);
        glDeleteBuffers(1, &flowerBaseVBO);
        glDeleteBuffers(1, &flowerInstVBO);
        glDeleteVertexArrays(1, &flowerVAO);
        glDeleteBuffers(1, &birdBaseVBO);
        glDeleteBuffers(1, &birdInstVBO);
        glDeleteVertexArrays(1, &birdVAO);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        return 1;
    }

    return 0;
}
