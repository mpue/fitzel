#include "CloudField.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>

namespace clouds {
namespace {

// Atlas dimensions, derived once so nothing can disagree about them.
constexpr int kAtlasW = kSlotRes * kSlotsX;
constexpr int kAtlasH = kSlotRes;
constexpr int kAtlasD = kSlotRes * kSlotsZ;

// The same generator CloudShape uses, for the same reason: a scene stores a seed
// and expects the same sky back.
struct Rng {
    std::uint32_t s;
    explicit Rng(std::uint32_t seed) : s(seed ? seed * 747796405u + 2891336453u : 1u) {}
    std::uint32_t next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return s;
    }
    float unit() { return static_cast<float>(next() & 0xFFFFFFu) / 16777216.0f; }
    float range(float a, float b) { return a + (b - a) * unit(); }
};

// Unit cube as 36 positions. Small enough to sit here rather than in a mesh
// asset -- it is part of how this renders, not something anyone would author.
const float kCube[] = {
    -.5f,-.5f,-.5f, -.5f,-.5f, .5f, -.5f, .5f, .5f, -.5f,-.5f,-.5f, -.5f, .5f, .5f, -.5f, .5f,-.5f,
     .5f,-.5f,-.5f,  .5f, .5f,-.5f,  .5f, .5f, .5f,  .5f,-.5f,-.5f,  .5f, .5f, .5f,  .5f,-.5f, .5f,
    -.5f,-.5f,-.5f,  .5f,-.5f,-.5f,  .5f,-.5f, .5f, -.5f,-.5f,-.5f,  .5f,-.5f, .5f, -.5f,-.5f, .5f,
    -.5f, .5f,-.5f, -.5f, .5f, .5f,  .5f, .5f, .5f, -.5f, .5f,-.5f,  .5f, .5f, .5f,  .5f, .5f,-.5f,
    -.5f,-.5f,-.5f, -.5f, .5f,-.5f,  .5f, .5f,-.5f, -.5f,-.5f,-.5f,  .5f, .5f,-.5f,  .5f,-.5f,-.5f,
    -.5f,-.5f, .5f,  .5f,-.5f, .5f,  .5f, .5f, .5f, -.5f,-.5f, .5f,  .5f, .5f, .5f, -.5f, .5f, .5f,
};

cloudshape::Species pickSpecies(Rng& rng, const Settings& s) {
    const float h = std::max(s.wHumilis, 0.0f);
    const float m = std::max(s.wMediocris, 0.0f);
    const float c = std::max(s.wCongestus, 0.0f);
    const float total = h + m + c;
    if (total <= 1e-5f) return cloudshape::Species::Mediocris;
    const float r = rng.range(0.0f, total);
    if (r < h) return cloudshape::Species::Humilis;
    if (r < h + m) return cloudshape::Species::Mediocris;
    return cloudshape::Species::Congestus;
}

} // namespace

CloudField::~CloudField() { release(); }

void CloudField::release() {
    if (m_atlas) { glDeleteTextures(1, &m_atlas); m_atlas = 0; }
    if (m_inst)  { glDeleteBuffers(1, &m_inst);   m_inst = 0; }
    if (m_cube)  { glDeleteBuffers(1, &m_cube);   m_cube = 0; }
    if (m_vao)   { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
}

std::size_t CloudField::atlasBytes() const {
    return m_atlas ? static_cast<std::size_t>(kAtlasW) * kAtlasH * kAtlasD : 0;
}

bool CloudField::init(const std::string& shaderDir) {
    m_shader = fitzel::Shader::fromFiles(shaderDir + "/cloud.vert",
                                         shaderDir + "/cloud.frag");
    if (!m_shader.isValid()) {
        std::fprintf(stderr, "[clouds] shaders failed to build\n");
        return false;
    }

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_cube);
    glGenBuffers(1, &m_inst);
    glBindVertexArray(m_vao);

    glBindBuffer(GL_ARRAY_BUFFER, m_cube);
    glBufferData(GL_ARRAY_BUFFER, sizeof kCube, kCube, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);

    // Two vec4 per instance, advancing once per box rather than once per vertex.
    glBindBuffer(GL_ARRAY_BUFFER, m_inst);
    for (int i = 1; i <= 2; ++i) {
        glEnableVertexAttribArray(static_cast<GLuint>(i));
        glVertexAttribPointer(static_cast<GLuint>(i), 4, GL_FLOAT, GL_FALSE,
                              2 * sizeof(glm::vec4),
                              reinterpret_cast<void*>(static_cast<std::size_t>(i - 1) *
                                                      sizeof(glm::vec4)));
        glVertexAttribDivisor(static_cast<GLuint>(i), 1);
    }
    glBindVertexArray(0);
    return true;
}

void CloudField::bake(const Settings& s) {
    m_settings = s;
    m_instances.clear();
    if (!s.enabled || s.count <= 0) return;

    // --- The library --------------------------------------------------------
    // Every variant is grown and rasterised, then copied into its slot in one
    // big buffer. Building the atlas on the CPU and uploading once is the whole
    // of it -- GL 3.3 has no compute shader to do it on the GPU, and at a few
    // hundred milliseconds for the set there is no reason to want one.
    std::vector<unsigned char> atlas(
        static_cast<std::size_t>(kAtlasW) * kAtlasH * kAtlasD, 0);

    Rng rng(static_cast<std::uint32_t>(s.seed));
    // Aspect per variant: the instance needs it to know how tall its box is, and
    // it comes out of the bake rather than the recipe.
    float aspect[kVariants] = {};
    // ...and which species each slot holds, so an instance can pick a shape that
    // matches the size it is going to be drawn at (see the placement below).
    cloudshape::Species speciesOf[kVariants] = {};
    for (int v = 0; v < kVariants; ++v) {
        cloudshape::Recipe recipe;
        recipe.species = pickSpecies(rng, s);
        speciesOf[v]   = recipe.species;
        // Decorrelated from the placement stream: adding a cloud to the field
        // must not reshape the library.
        recipe.seed = static_cast<std::uint32_t>(s.seed) * 2654435761u +
                      static_cast<std::uint32_t>(v) * 40503u + 7u;
        const cloudshape::Volume vol = cloudshape::bake(recipe, kSlotRes);
        aspect[v] = vol.aspect;
        if (!vol.valid()) continue;

        const int sx = (v % kSlotsX) * kSlotRes;
        const int sz = (v / kSlotsX) * kSlotRes;
        for (int z = 0; z < kSlotRes; ++z) {
            for (int y = 0; y < kSlotRes; ++y) {
                const unsigned char* src =
                    &vol.density[(static_cast<std::size_t>(z) * kSlotRes + y) * kSlotRes];
                unsigned char* dst =
                    &atlas[(static_cast<std::size_t>(sz + z) * kAtlasH + y) *
                               kAtlasW + sx];
                std::copy(src, src + kSlotRes, dst);
            }
        }
    }

    if (!m_atlas) glGenTextures(1, &m_atlas);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_3D, m_atlas);
    // Force the unpack state rather than trusting it: a dependency that leaves
    // GL_UNPACK_ROW_LENGTH set reads this tightly-packed buffer with the wrong
    // stride and over-reads past its end, which is a crash inside the driver and
    // not a wrong picture. (VolumetricFog and the engine's Texture do the same.)
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_R8, kAtlasW, kAtlasH, kAtlasD, 0,
                 GL_RED, GL_UNSIGNED_BYTE, atlas.data());
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // CLAMP, not REPEAT: a ray leaving one cloud must find empty air, and in an
    // atlas a wrap would hand it the far side of a DIFFERENT cloud.
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_3D, 0);

    // --- Placement ----------------------------------------------------------
    // Scattered with a minimum separation. That separation is the whole reason
    // this system exists: a cumulus is DETACHED, and clouds that grow into each
    // other are the connected field the noise version could never stop being.
    // Rejection sampling, capped -- if the field is too crowded for the spacing
    // asked for, it places fewer rather than looping forever.
    m_instances.reserve(static_cast<std::size_t>(s.count));
    const int tries = s.count * 24;
    for (int i = 0; i < tries && static_cast<int>(m_instances.size()) < s.count; ++i) {
        const float ang = rng.range(0.0f, 6.2831853f);
        // sqrt keeps the scatter even over the disc instead of piling it up in
        // the middle, which is what a uniform radius would do.
        const float rad = std::sqrt(rng.unit()) * s.spread;
        const glm::vec2 xz(std::cos(ang) * rad, std::sin(ang) * rad);

        // --- Which cloud, and how big -------------------------------------
        // Species first, size second, because in the sky the two are the same
        // fact: a humilis IS a small cloud and a congestus IS a big one. Drawing
        // them independently -- a size out of a flat range, a shape at random --
        // makes every large cloud a scaled-up copy of a small one, which is what
        // gave the field its sameness however wide the range was set.
        const cloudshape::Species want = pickSpecies(rng, s);
        int cand[kVariants], nc = 0;
        for (int k = 0; k < kVariants; ++k)
            if (speciesOf[k] == want) cand[nc++] = k;
        // A mix so lopsided that no slot grew this species: take any slot rather
        // than place nothing.
        const int v = nc ? cand[rng.next() % static_cast<std::uint32_t>(nc)]
                         : static_cast<int>(rng.next() % kVariants);

        // Each species claims a band of the author's size range, and they
        // overlap -- a big humilis and a small mediocris are the same size in
        // reality too.
        float lo = 0.0f, hi = 1.0f;
        switch (speciesOf[v]) {
            case cloudshape::Species::Humilis:   lo = 0.00f; hi = 0.40f; break;
            case cloudshape::Species::Mediocris: lo = 0.26f; hi = 0.74f; break;
            case cloudshape::Species::Congestus: lo = 0.58f; hi = 1.00f; break;
        }
        // Within the band, small is commoner than large: cloud sizes follow a
        // power law, which is why a real sky has a scatter of little ones around
        // every big one. A flat draw gives the opposite impression -- a field of
        // middling clouds with nothing to give it scale.
        const float sMin = std::min(s.sizeMin, s.sizeMax);
        const float sMax = std::max(s.sizeMin, s.sizeMax);
        const float band = glm::mix(lo, hi, std::pow(rng.unit(), 1.7f));
        const float width  = glm::mix(sMin, sMax, band);
        const float height = width * std::max(aspect[v], 0.05f);

        // Clear of every cloud already placed, measured between edges rather
        // than centres so a big cloud claims the room it actually needs.
        bool clear = true;
        for (const Instance& o : m_instances) {
            const float need = (width + o.width) * 0.62f;
            const glm::vec2 d(o.centre.x - xz.x, o.centre.z - xz.y);
            if (glm::dot(d, d) < need * need) { clear = false; break; }
        }
        if (!clear) continue;

        const float base = s.baseHeight + rng.range(-s.baseJitter, s.baseJitter);
        m_instances.push_back({glm::vec3(xz.x, base + height * 0.5f, xz.y),
                               width, height, rng.range(0.0f, 6.2831853f),
                               v % kSlotsX, v / kSlotsX});
    }
}

void CloudField::render(const glm::mat4& viewProj, const glm::vec3& eye,
                        const glm::vec3& sunDir, const glm::vec3& sunColor,
                        const glm::vec3& ambient, float fogDensity,
                        float exposure, bool tonemap) {
    if (!valid() || m_instances.empty() || !m_settings.enabled) return;

    // Back to front. Alpha blending is order-dependent and there is no cheap way
    // around that for volumes; sorting a few dozen boxes by distance costs
    // nothing next to marching them.
    const std::size_t n = m_instances.size();
    m_order.resize(n);
    for (std::size_t i = 0; i < n; ++i) m_order[i] = static_cast<std::uint32_t>(i);
    std::sort(m_order.begin(), m_order.end(),
              [&](std::uint32_t a, std::uint32_t b) {
                  const glm::vec3 da = m_instances[a].centre - eye;
                  const glm::vec3 db = m_instances[b].centre - eye;
                  return glm::dot(da, da) > glm::dot(db, db);
              });

    m_upload.resize(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        const Instance& in = m_instances[m_order[i]];
        m_upload[i * 2 + 0] = glm::vec4(in.centre, in.width);
        m_upload[i * 2 + 1] = glm::vec4(in.height, in.yaw,
                                        static_cast<float>(in.slotX),
                                        static_cast<float>(in.slotZ));
    }
    glBindBuffer(GL_ARRAY_BUFFER, m_inst);
    // Orphan the buffer rather than waiting on last frame's draw to finish with
    // it: the contents are wholly replaced anyway.
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(m_upload.size() * sizeof(glm::vec4)),
                 nullptr, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    static_cast<GLsizeiptr>(m_upload.size() * sizeof(glm::vec4)),
                    m_upload.data());

    m_shader.bind();
    m_shader.setMat4("uViewProj", viewProj);
    m_shader.setVec3("uCameraPos", eye);
    m_shader.setVec3("uSunDir", sunDir);
    m_shader.setVec3("uSunColor", sunColor);
    m_shader.setVec3("uAmbient", ambient);
    m_shader.setVec3("uSlotScale", glm::vec3(1.0f / kSlotsX, 1.0f, 1.0f / kSlotsZ));
    m_shader.setFloat("uDensity", std::max(m_settings.density, 0.0f));
    m_shader.setFloat("uStepScale", 1.0f);
    m_shader.setFloat("uFogDensity", fogDensity);
    m_shader.setFloat("uExposure", exposure);
    m_shader.setInt("uTonemap", tonemap ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_3D, m_atlas);
    m_shader.setInt("uVolume", 0);

    // Back faces: the box still covers the screen when the camera is inside it,
    // and the march starts at the camera in that case (t0 is clamped to zero).
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // premultiplied
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    glBindVertexArray(m_vao);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 36, static_cast<GLsizei>(n));
    glBindVertexArray(0);

    glCullFace(GL_BACK);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
}

} // namespace clouds
