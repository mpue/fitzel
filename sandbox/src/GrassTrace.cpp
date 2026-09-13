#include "GrassTrace.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <unordered_map>

#include "SandboxMath.hpp"
#include "Wind.hpp"

using fitzel::TerrainSettings;
using fitzel::terrainHeight;
using fitzel::terrainMoisture;

namespace grassfield {
namespace {

// --- The blade, as grass.vert builds it ------------------------------------

// The blade strip: x in [-0.5, 0.5], h01 in [0, 1]. Seven vertices, so five
// triangles. Byte-for-byte the array VegetationSystem uploads as the base mesh --
// if one of them changes, both pictures have to.
const float kBlade[14] = {
    -0.5f,  0.0f,   0.5f,  0.0f,
    -0.45f, 0.33f,  0.45f, 0.33f,
    -0.30f, 0.66f,  0.30f, 0.66f,
     0.0f,  1.0f
};

// GLSL fract(sin(x) * k) hashing, transliterated. Not valNoise2: the shader
// computes the blade's colour with THIS noise, and a different one would be a
// different meadow -- the same field in two colours.
float ghash(glm::vec2 p) {
    const float s = std::sin(p.x * 127.1f + p.y * 311.7f) * 43758.5453f;
    return s - std::floor(s);
}

float gnoise(glm::vec2 p) {
    const glm::vec2 i(std::floor(p.x), std::floor(p.y));
    glm::vec2 f = p - i;
    f = f * f * (3.0f - 2.0f * f);
    const float a = ghash(i);
    const float b = ghash(i + glm::vec2(1.0f, 0.0f));
    const float c = ghash(i + glm::vec2(0.0f, 1.0f));
    const float d = ghash(i + glm::vec2(1.0f, 1.0f));
    return glm::mix(glm::mix(a, b, f.x), glm::mix(c, d, f.x), f.y);
}

float fractSin(float x, float k) {
    const float s = std::sin(x) * k;
    return s - std::floor(s);
}

// One blade's colours, as grass.vert computes them into vBaseCol / vTipCol.
// sRGB, pre-tint -- the tracer applies pow(2.2) itself and the tint is the
// material's, so both stay in the vocabulary pathtrace already speaks.
struct BladeColor { glm::vec3 base, tip; };

BladeColor bladeColor(const glm::vec3& pos, float lush, float r1, float r2) {
    const glm::vec2 xz(pos.x, pos.z);
    const float meadow = gnoise(xz * 0.05f);
    const float fine   = gnoise(xz * 0.27f + 11.0f);
    const float hueN   = gnoise(xz * 0.11f + 31.0f);
    const float shade  = gnoise(xz * 0.035f + 5.0f);

    float green = glm::clamp(lush - (1.0f - meadow) * 0.55f + (fine - 0.5f) * 0.3f
                                  - r1 * 0.25f, 0.0f, 1.0f);
    green = glm::clamp(green - glm::smoothstep(0.86f, 0.99f, r2) * 0.6f, 0.0f, 1.0f);

    const glm::vec3 dryBase(0.16f, 0.14f, 0.06f);
    const glm::vec3 dryTip(0.50f, 0.45f, 0.22f);
    const glm::vec3 lushBase = glm::mix(glm::vec3(0.03f, 0.10f, 0.03f),
                                        glm::vec3(0.07f, 0.16f, 0.05f), fine);
    const glm::vec3 lushTipC(0.14f, 0.36f, 0.13f);
    const glm::vec3 lushTipW(0.44f, 0.54f, 0.16f);
    const glm::vec3 lushTip = glm::mix(lushTipC, lushTipW, hueN * hueN);

    const float bright = (0.55f + 0.55f * shade) * (0.82f + 0.32f * r1)
                       * glm::mix(0.85f, 1.05f, lush);
    return {glm::mix(dryBase, lushBase, green) * bright,
            glm::mix(dryTip,  lushTip,  green) * bright};
}

// The colour palette. A blade's colour has nowhere to live on a pathtrace
// Triangle -- there is a material index and no vertex colour -- so quantised
// colours BECOME materials and the index is the colour. Coarse on purpose: the
// eye separates far fewer greens than a float does, and every distinct entry is
// a material the GPU tracer has to carry.
class Palette {
public:
    Palette(pathtrace::Scene& scene, const glm::vec3& tint, int steps,
            float translucency)
        : m_scene(scene), m_tint(tint),
          m_steps(static_cast<float>(std::max(2, steps))),
          m_translucency(translucency) {}

    int index(const glm::vec3& srgb) {
        const std::uint32_t key =
            (quant(srgb.r) << 20) | (quant(srgb.g) << 10) | quant(srgb.b);
        const auto it = m_map.find(key);
        if (it != m_map.end()) return it->second;

        pathtrace::Material m;
        // Snap to the bin's centre so the material IS the quantisation, rather
        // than whichever blade happened to ask for it first.
        m.albedo = glm::vec3(unquant(quant(srgb.r)),
                             unquant(quant(srgb.g)),
                             unquant(quant(srgb.b)));
        m.tint = m_tint;
        // A leaf is matte and not a mirror. Grass in the raster path has a small
        // specular term; at the roughness a blade actually has it contributes
        // almost nothing to a still, and giving it reflectivity would put chrome
        // glints on a meadow.
        m.roughness    = 0.85f;
        m.reflectivity = 0.0f;
        m.translucency = m_translucency;
        const int id = static_cast<int>(m_scene.materials.size());
        m_scene.materials.push_back(std::move(m));
        m_map.emplace(key, id);
        return id;
    }

    int size() const { return static_cast<int>(m_map.size()); }

private:
    // Quantised on a square-root curve, not linearly.
    //
    // Measured, not assumed: linear bins put a whole meadow in 14 colours at 16
    // steps. Grass lives between about 0.02 and 0.35, so linear bins spend
    // nine tenths of their range on colours no blade ever has, and the few that
    // land in the dark end have to carry every green there is. The square root
    // spreads them where the values actually are -- the same curve, and for the
    // same reason, that makes 8-bit sRGB usable for dark colours at all.
    std::uint32_t quant(float v) const {
        const float q = std::floor(std::sqrt(glm::clamp(v, 0.0f, 1.0f)) * m_steps);
        return static_cast<std::uint32_t>(std::min(q, m_steps - 1.0f));
    }
    float unquant(std::uint32_t q) const {
        const float t = (static_cast<float>(q) + 0.5f) / m_steps;
        return t * t;
    }

    pathtrace::Scene& m_scene;
    glm::vec3         m_tint;
    float             m_steps;
    float             m_translucency = 0.0f;
    std::unordered_map<std::uint32_t, int> m_map;
};

} // namespace

// ---------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------

void generateTile(std::int32_t tx, std::int32_t tz, glm::vec2 origin, float size,
                  const Field& f, std::vector<float>& out) {
    const TerrainSettings& s = f.terrain;
    const float waterLvl = f.waterLevel, snowLvl = f.snowLevel;
    const float gHeight = f.height, gDensity = f.density, chaos = f.chaos;
    const std::vector<glm::vec2>& road = f.road;
    const float roadClear = f.roadClear;
    const std::vector<glm::vec3>& wet = f.wet;

    std::uint32_t seed = static_cast<std::uint32_t>(tx) * 73856093u
                       ^ static_cast<std::uint32_t>(tz) * 19349663u ^ 0x9E3779B9u;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float spacing = 0.6f; // sampling grid (one ground query per cell)
    const int   per = std::max(1, static_cast<int>(120.0f * gDensity));
    for (float lz = 0.0f; lz < size; lz += spacing) {
        for (float lx = 0.0f; lx < size; lx += spacing) {
            const float wx = origin.x + lx, wz = origin.y + lz;
            if (roadDistanceSq(road, wx, wz) < roadClear * roadClear) continue;
            if (inDiscs(wet, wx, wz)) continue;   // in a brook, not beside one
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
            // Dry ground: nothing, as it always was -- or, with dryGrowth, a thin
            // straw-coloured sward, which is what a dry meadow actually is. A
            // valley floor with bare earth between green patches reads as desert.
            float dryThin = 1.0f;
            if (lush < 0.22f) {
                if (f.dryGrowth <= 0.0f) continue;
                dryThin = f.dryGrowth * glm::mix(0.35f, 0.75f, lush / 0.22f);
            }
            // In the woods the canopy takes the light: a few tufts, not a lawn.
            // The edge keeps most of its grass -- that is where the sun gets in.
            float woods = 0.0f;
            if (f.eco.enabled) woods = ecology::sample(f.eco, wx, wz, h, n.y).forest;
            // Meadow patchiness at several scales. `chaos` scales how much each
            // irregularity kicks in: 0 = near-uniform lawn, 1 = wild meadow,
            // higher piles on taller outliers and more gaps.
            const float patch  = valNoise2(wx * 0.05f, wz * 0.05f);
            const float patch2 = valNoise2(wx * 0.17f + 60.0f, wz * 0.17f + 60.0f);
            const float bare   = valNoise2(wx * 0.13f + 19.0f, wz * 0.13f + 7.0f);
            const float bare2  = valNoise2(wx * 0.31f + 3.0f,  wz * 0.31f + 23.0f);
            // Broad bare patches always apply; the finer holes only with chaos.
            if (bare < 0.26f || bare2 < 0.12f * chaos) continue;
            const float densJit = 1.0f + (glm::mix(0.55f, 1.20f, patch2) - 1.0f) * chaos;
            const float dens    = glm::mix(0.25f, 1.25f, patch) * densJit;
            // Per-cell count jitter breaks the even grid density (chaos-scaled).
            const float cellJit = 1.0f + (glm::mix(0.60f, 1.30f, u(rng)) - 1.0f) * chaos;
            const int   count = static_cast<int>(per * dens
                                * glm::mix(0.35f, 1.0f, lush) * cellJit
                                * (1.0f - 0.85f * woods) * dryThin);
            // Height clumps have their OWN frequency (independent of density), so
            // tall tufts and low turf don't line up with thick/thin.
            const float tuft = valNoise2(wx * 0.11f + 40.0f, wz * 0.11f + 40.0f);
            const float jitPos = spacing * (2.2f + 0.4f * chaos);
            for (int b = 0; b < count; ++b) {
                const float tuftF = 1.0f + (glm::mix(0.50f, 1.40f, tuft) - 1.0f) * chaos;
                const float jitF  = 1.0f + (glm::mix(0.65f, 1.30f, u(rng)) - 1.0f) * chaos;
                float bh = gHeight * glm::max(0.15f, tuftF * jitF);
                // A few stalks shoot well above the canopy (grass gone to seed).
                if (u(rng) < 0.05f * chaos) bh *= glm::mix(1.4f, 2.0f, u(rng));
                out.insert(out.end(), {
                    wx + (u(rng) - 0.5f) * jitPos, h,
                    wz + (u(rng) - 0.5f) * jitPos,
                    u(rng) * 6.2831f,
                    bh,
                    u(rng) * 6.2831f,
                    glm::clamp(lush + (patch - 0.5f) * 0.4f
                                    + (u(rng) - 0.5f) * 0.12f, 0.0f, 1.0f)});
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Expansion into triangles
// ---------------------------------------------------------------------------

void appendToScene(pathtrace::Scene& scene, const Field& f,
                   const TraceOptions& opt, TraceReport* report) {
    const auto t0 = std::chrono::steady_clock::now();
    TraceReport rep;

    const float radius = std::max(0.0f, opt.radius);
    if (radius <= 0.0f) { if (report) *report = rep; return; }

    Palette palette(scene, f.tint, opt.colorSteps, opt.translucency);

    // The tile range covering the disc. Generated tile by tile because that is
    // the unit the placement is seeded in: asking for an arbitrary rectangle
    // would put different blades in it than the field has.
    const float ts = Field::kTileSize;
    const std::int32_t t0x = static_cast<std::int32_t>(
        std::floor((opt.centerXZ.x - radius) / ts));
    const std::int32_t t1x = static_cast<std::int32_t>(
        std::floor((opt.centerXZ.x + radius) / ts));
    const std::int32_t t0z = static_cast<std::int32_t>(
        std::floor((opt.centerXZ.y - radius) / ts));
    const std::int32_t t1z = static_cast<std::int32_t>(
        std::floor((opt.centerXZ.y + radius) / ts));

    // The fade band, in the shader's own terms: uFadeEnd is the disc's edge and
    // uFadeStart is one margin inside it.
    const float fadeEnd   = radius;
    const float fadeStart = std::max(0.0f, radius - std::max(0.0f, opt.fadeMargin));

    const glm::vec2 windDir = (glm::length(opt.windDir) > 1e-6f)
                                  ? glm::normalize(opt.windDir)
                                  : glm::vec2(1.0f, 0.0f);

    std::vector<float> inst;
    bool stop = false;

    for (std::int32_t tz = t0z; tz <= t1z && !stop; ++tz) {
        for (std::int32_t tx = t0x; tx <= t1x && !stop; ++tx) {
            inst.clear();
            generateTile(tx, tz, glm::vec2(tx * ts, tz * ts), ts, f, inst);

            for (std::size_t i = 0; i + 6 < inst.size(); i += 7) {
                const glm::vec3 iPos(inst[i], inst[i + 1], inst[i + 2]);
                const float iRot    = inst[i + 3];
                const float iHeight = inst[i + 4];
                const float iPhase  = inst[i + 5];
                const float iLush   = inst[i + 6];

                // The disc is round; the tiles that cover it are not.
                const float fdist = glm::length(glm::vec2(iPos.x, iPos.z) - opt.centerXZ);
                if (fdist > radius) continue;

                if (rep.triangles + 5 > opt.maxTriangles) { stop = true; break; }

                // grass.vert, from here down. The frustum cull is deliberately
                // NOT ported: a blade behind the camera still lights what is in
                // front of it, and culling it would leave a hole in every
                // reflection and every bounce.
                const float r1 = fractSin(iPhase * 91.17f + iRot * 13.30f, 43758.5453f);
                const float r2 = fractSin(iPhase * 44.53f + iRot * 7.13f + 2.7f, 24634.6345f);
                const float r3 = fractSin(iPos.x * 12.9898f + iPos.z * 78.233f, 43758.5453f);

                const float hs = 1.0f - glm::smoothstep(fadeStart, fadeEnd, fdist);
                if (hs <= 1e-3f) continue;   // faded to nothing at the edge

                const float widthVar = glm::mix(0.70f, 1.65f, r2);
                const float c = std::cos(iRot), s = std::sin(iRot);

                const float leanAng = iRot + (r3 - 0.5f) * 2.5f;
                const glm::vec2 leanDir(std::cos(leanAng), std::sin(leanAng));
                const float leanAmt = glm::mix(0.03f, 0.17f, r1);

                const float along = glm::dot(glm::vec2(iPos.x, iPos.z), windDir);
                // wind.glsl's gust field (Wind.hpp), posed at the tracer's time.
                wind::State ws;
                ws.dir = windDir;
                ws.gustiness = opt.windGust;
                ws.time = opt.windTime;
                const float gust = wind::gust(ws, glm::vec2(iPos.x, iPos.z));
                const float sway = std::sin(opt.windTime * (1.35f + 0.6f * r2)
                                            + iPhase + along * 0.25f);
                const float stiff = glm::mix(0.65f, 1.25f, r1);
                const float bladeH = iHeight * hs;
                const float bendGain = glm::clamp(bladeH / 0.35f, 0.20f, 1.70f);
                const float bendAmt = opt.windStrength * gust * stiff
                                    * (0.30f + 0.70f * sway) * bendGain;

                // The blade's seven world-space vertices.
                glm::vec3 v[7];
                for (int k = 0; k < 7; ++k) {
                    const float ax  = kBlade[k * 2];
                    const float h01 = kBlade[k * 2 + 1];
                    const float w   = 0.016f * widthVar * (1.0f - 0.4f * h01);
                    glm::vec3 local(ax * 2.0f * w, h01 * iHeight * hs, 0.0f);
                    local = glm::vec3(local.x * c, local.y, local.x * s);
                    local.x += leanDir.x * (leanAmt * h01 * h01);
                    local.z += leanDir.y * (leanAmt * h01 * h01);
                    const float bend = bendAmt * h01 * h01;
                    local.x += windDir.x * bend;
                    local.z += windDir.y * bend;
                    v[k] = iPos + local;
                }

                // One normal for the whole blade -- the ribbon's own, tilted
                // toward up by however much the caller asked for (see
                // TraceOptions::normalUpBias).
                const glm::vec3 N =
                    glm::normalize(glm::vec3(-s, opt.normalUpBias, c));

                const BladeColor bc = bladeColor(iPos, iLush, r1, r2);

                // Five triangles from the strip. Each takes the colour at its own
                // height, which is the raster path's per-fragment gradient
                // resolved to five steps -- a blade is a few pixels tall in a
                // still and the difference does not survive the pixel.
                for (int k = 0; k + 2 < 7; ++k) {
                    pathtrace::Triangle t;
                    // Strip winding: flip every other triangle so the fronts
                    // agree. The tracer is two-sided, so this costs nothing and
                    // keeps the geometry honest for anything that reads it later.
                    if (k & 1) { t.p0 = v[k + 1]; t.p1 = v[k]; t.p2 = v[k + 2]; }
                    else       { t.p0 = v[k];     t.p1 = v[k + 1]; t.p2 = v[k + 2]; }
                    t.n0 = t.n1 = t.n2 = N;
                    const float hMid = (kBlade[k * 2 + 1] + kBlade[(k + 1) * 2 + 1]
                                        + kBlade[(k + 2) * 2 + 1]) / 3.0f;
                    t.material = palette.index(glm::mix(bc.base, bc.tip, hMid));
                    scene.triangles.push_back(t);
                    ++rep.triangles;
                }
                ++rep.blades;
            }
        }
    }

    rep.truncated = stop;
    rep.materials = palette.size();
    rep.seconds = std::chrono::duration<double>(
                      std::chrono::steady_clock::now() - t0).count();

    // The tracer keeps THREE paint weights per triangle, parallel to the
    // triangle list. Grass has none, but the array still has to grow with it:
    // the CPU tracer would merely read zero past the end, while the GPU one
    // tests `vertexPaint.size() >= triangles * 3` and, finding it short, drops
    // the paint for the WHOLE scene -- the terrain would lose its layers
    // because grass was added, and only in one of the two renderers.
    if (!scene.vertexPaint.empty())
        scene.vertexPaint.resize(scene.triangles.size() * 3, glm::vec4(0.0f));

    if (report) *report = rep;
}

} // namespace grassfield
