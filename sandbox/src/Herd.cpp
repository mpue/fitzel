#include "Herd.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include <glm/gtc/matrix_transform.hpp>

namespace {

// Blend two joint palettes. Linear in the matrices, which is not a rotation
// blend -- but between a graze and a walk that differ by a few degrees per
// joint, over the half second the switch takes, nobody sees the difference.
void blendPalette(std::vector<glm::mat4>& a, const std::vector<glm::mat4>& b, float w) {
    if (b.size() != a.size() || w <= 0.0f) return;
    for (std::size_t i = 0; i < a.size(); ++i) a[i] = a[i] * (1.0f - w) + b[i] * w;
}

// How fast a clip walks: the feet on the ground move backwards under the body
// at exactly the speed the body has to move forwards, or they slide. Sampled
// through one cycle, the vertices in contact (the lowest few) are followed
// from sample to sample; the median of their backward speed along the model's
// forward axis, in model units per second. Negative: the model walks the other
// way round than it was told. Zero: no contact found, or the clip carries its
// own root motion.
float stanceSpeed(const fitzel::ModelData& m, int clip, glm::vec3 fwd) {
    if (clip < 0 || clip >= static_cast<int>(m.animations.size()) || m.primitives.empty())
        return 0.0f;
    const float dur = m.animations[clip].duration;
    if (dur <= 1e-3f) return 0.0f;
    std::size_t big = 0;
    for (std::size_t i = 1; i < m.primitives.size(); ++i)
        if (m.primitives[i].vertexCount() > m.primitives[big].vertexCount()) big = i;
    const int kN = 48;
    const float dt = dur / kN;
    std::vector<fitzel::Vertex> a, b;
    fitzel::skinPrimitive(m.primitives[big], fitzel::sampleSkeleton(m, clip, 0.0f), a);
    std::vector<float> speeds;
    const float contact = 0.02f * std::max(m.height(), 1e-3f);
    for (int k = 1; k <= kN; ++k) {
        fitzel::skinPrimitive(m.primitives[big], fitzel::sampleSkeleton(m, clip, k * dt), b);
        if (a.size() != b.size() || a.empty()) return 0.0f;
        float lo = 1e30f;
        for (const fitzel::Vertex& v : a) lo = std::min(lo, v.position.y);
        double sum = 0.0;
        int n = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i].position.y > lo + contact || b[i].position.y > lo + contact) continue;
            sum += glm::dot(b[i].position - a[i].position, fwd);
            ++n;
        }
        if (n > 0) speeds.push_back(static_cast<float>(-sum / n / dt));
        a.swap(b);
    }
    if (speeds.empty()) return 0.0f;
    std::nth_element(speeds.begin(), speeds.begin() + speeds.size() / 2, speeds.end());
    return speeds[speeds.size() / 2];
}

} // namespace

bool Herd::load(const Config& cfg, fitzel::Shader& lit) {
    m_cfg = cfg;
    m_ok  = false;
    const std::string ext = std::filesystem::path(cfg.model).extension().string();
    m_model = (ext == ".fbx" || ext == ".FBX") ? fitzel::loadSkinnedModel(cfg.model)
                                                : fitzel::loadGltf(cfg.model);
    if (!m_model.animated() || m_model.primitives.empty()) return false;
    // An FBX often names its texture by a path that did not travel with it,
    // while the same asset exported as glTF carries it inside. Borrow the
    // sibling's pictures for primitives that came without one.
    bool missing = false;
    for (const fitzel::ModelPrimitive& p : m_model.primitives) missing |= p.texPixels.empty();
    if (missing) {
        for (const char* sib : {".glb", ".gltf"}) {
            const std::string other =
                std::filesystem::path(cfg.model).replace_extension(sib).string();
            if (other == cfg.model || !std::filesystem::exists(other)) continue;
            const fitzel::ModelData donor = fitzel::loadGltf(other);
            for (std::size_t i = 0; i < m_model.primitives.size(); ++i) {
                fitzel::ModelPrimitive& p = m_model.primitives[i];
                if (!p.texPixels.empty() || donor.primitives.empty()) continue;
                const fitzel::ModelPrimitive& d =
                    donor.primitives[std::min(i, donor.primitives.size() - 1)];
                if (d.texPixels.empty()) continue;
                p.texPixels = d.texPixels;
                p.texWidth  = d.texWidth;
                p.texHeight = d.texHeight;
                p.baseColor[0] = p.baseColor[1] = p.baseColor[2] = 1.0f;
            }
            break;
        }
    }
    const float h = std::max(m_model.height(), 1e-3f);
    m_scale  = cfg.height / h;
    m_offset = glm::vec3(0.0f, -m_model.minY, 0.0f);

    // The walk's own pace (see stanceSpeed), so the hooves stay planted: the
    // herd moves at the speed the clip's legs carry it, and when it slows the
    // legs slow with it. A model that walks backwards is turned round.
    {
        const float off = glm::radians(cfg.yawOffset);
        const glm::vec3 fwd(-std::sin(off), 0.0f, std::cos(off));
        m_clipSpeeds.clear();
        for (int c = 0; c < static_cast<int>(m_model.animations.size()); ++c)
            m_clipSpeeds.push_back(stanceSpeed(m_model, c, fwd) * m_scale);
        const float v = (cfg.walkClip >= 0 && cfg.walkClip < static_cast<int>(m_clipSpeeds.size()))
                            ? m_clipSpeeds[static_cast<std::size_t>(cfg.walkClip)] : 0.0f;
        m_flip = v < -0.2f;
        m_walkPace = std::abs(v) > 0.2f ? std::abs(v) : cfg.walkSpeed;
        m_paceMeasured = std::abs(v) > 0.2f;
    }

    // One material per primitive, every lit uniform written (the program is
    // shared, and whatever this does not set it inherits from the last draw).
    for (const fitzel::ModelPrimitive& p : m_model.primitives) {
        auto mat = std::make_unique<fitzel::Material>(lit);
        mat->set("uWaterLevel", -1.0e4f).set("uWetness", 0.0f)
            .set("uReflectivity", 0.0f).set("uRoughness", 0.85f).set("uGlass", 0)
            .set("uHasNormalMap", 0).set("uHasOrmMap", 0)
            .set("uEmission", glm::vec3(0.0f)).set("uEmissionStrength", 0.0f)
            .set("uHasEmissionMap", 0).set("uWindowGrid", 0).set("uMeshPaint", 0)
            .set("uTint", glm::vec3(p.baseColor[0], p.baseColor[1], p.baseColor[2]))
            // Mane and tail are alpha-blended hair cards in the file; cut out
            // they need no sorting and still cast their shadow.
            .set("uAlphaCutout", p.alphaCutout ? 1 : 0).set("uAlphaCutoff", 0.4f);
        if (!p.texPixels.empty()) {
            auto tex = std::make_unique<fitzel::Texture>(
                fitzel::Texture::fromPixels(p.texPixels.data(), p.texWidth, p.texHeight, 4));
            mat->set("uColorMode", 2).setTexture("uTexture", *tex, 0);
            m_tex.push_back(std::move(tex));
        } else {
            mat->set("uColorMode", 0)
                .set("uAlbedo", glm::vec3(p.baseColor[0], p.baseColor[1], p.baseColor[2]));
        }
        m_mats.push_back(std::move(mat));
    }

    // The herd, spread over the meadow, each on its own clock.
    const std::vector<glm::mat4> bind = fitzel::sampleSkeleton(m_model, cfg.grazeClip, 0.0f);
    m_animals.resize(static_cast<std::size_t>(std::max(0, cfg.count)));
    for (Animal& a : m_animals) {
        const float ang = uni() * 6.2831853f, r = std::sqrt(uni()) * cfg.radius * 0.5f;
        a.pos      = glm::vec3(cfg.centre.x + std::cos(ang) * r, 0.0f,
                               cfg.centre.y + std::sin(ang) * r);
        a.yaw      = uni() * 6.2831853f;
        a.clipTime = uni() * 30.0f;
        a.stateLeft = uni() * 10.0f;
        for (const fitzel::ModelPrimitive& p : m_model.primitives) {
            fitzel::skinPrimitive(p, bind, m_scratch);
            a.meshes.push_back(fitzel::Mesh::create(m_scratch));
        }
    }
    m_ok = true;
    return true;
}

void Herd::decide(Animal& a, const World& w) {
    if (a.walking) {                         // arrived or tired: graze a while
        a.walking   = false;
        a.stateLeft = 8.0f + uni() * 20.0f;
        return;
    }
    // Where the others are: a straggler walks back towards them, the rest
    // drift to fresh grass nearby -- inside the meadow, on dry ground.
    glm::vec2 c(0.0f);
    for (const Animal& o : m_animals) c += glm::vec2(o.pos.x, o.pos.z);
    c /= static_cast<float>(std::max<std::size_t>(1, m_animals.size()));
    const glm::vec2 me(a.pos.x, a.pos.z);
    for (int tries = 0; tries < 10; ++tries) {
        glm::vec2 t;
        if (glm::length(me - c) > 14.0f) {
            t = c + glm::vec2(uni() - 0.5f, uni() - 0.5f) * 10.0f;
        } else {
            const float ang = uni() * 6.2831853f, d = 4.0f + uni() * 10.0f;
            t = me + glm::vec2(std::cos(ang), std::sin(ang)) * d;
        }
        if (glm::length(t - m_cfg.centre) > m_cfg.radius) continue;
        if (w.walkable && !w.walkable(t.x, t.y)) continue;
        a.target    = t;
        a.walking   = true;
        a.stateLeft = 40.0f;                  // give up if it never gets there
        return;
    }
    a.stateLeft = 5.0f + uni() * 5.0f;       // nowhere to go: graze on
}

void Herd::update(float dt, const World& w) {
    if (!m_ok) return;
    dt = std::clamp(dt, 0.0f, 0.1f);
    // Spread over the meadow at load, where nobody could say what is water:
    // the first update moves anyone who landed in the river onto dry grass.
    if (!m_placed && w.walkable) {
        m_placed = true;
        for (Animal& a : m_animals) {
            for (int tries = 0; tries < 60 && !w.walkable(a.pos.x, a.pos.z); ++tries) {
                const float ang = uni() * 6.2831853f, r = std::sqrt(uni()) * m_cfg.radius;
                a.pos.x = m_cfg.centre.x + std::cos(ang) * r;
                a.pos.z = m_cfg.centre.y + std::sin(ang) * r;
            }
        }
    }
    for (Animal& a : m_animals) {
        a.stateLeft -= dt;
        a.clipTime  += dt;
        if (a.stateLeft <= 0.0f) decide(a, w);
        float go = 0.0f;
        if (a.walking) {
            const glm::vec2 to = a.target - glm::vec2(a.pos.x, a.pos.z);
            const float dist = glm::length(to);
            if (dist < 0.8f) {
                a.walking = false;
                a.stateLeft = 8.0f + uni() * 20.0f;
            } else {
                // Turn towards it, slowly -- a horse does not pivot on the spot.
                const float want = std::atan2(to.x, to.y);
                float d = want - a.yaw;
                while (d > 3.14159265f) d -= 6.2831853f;
                while (d < -3.14159265f) d += 6.2831853f;
                a.yaw += std::clamp(d, -0.7f * dt, 0.7f * dt);
                go = m_walkPace * a.blend * (std::abs(d) < 1.2f ? 1.0f : 0.3f);
                const float nx = a.pos.x + std::sin(a.yaw) * go * dt;
                const float nz = a.pos.z + std::cos(a.yaw) * go * dt;
                // The way there crosses the brook: stop at the bank, and
                // think again.
                const float ax = a.pos.x + to.x / dist * 1.2f;
                const float az = a.pos.z + to.y / dist * 1.2f;
                if (w.walkable && !w.walkable(ax, az)) {
                    a.walking   = false;
                    a.stateLeft = 0.5f + uni();
                    go = 0.0f;
                } else {
                    a.pos.x = nx;
                    a.pos.z = nz;
                }
            }
        }
        a.blend = std::clamp(a.blend + (a.walking ? 1.0f : -1.0f) * dt * 1.6f, 0.0f, 1.0f);
        // The legs go exactly as fast as the ground passes under them.
        a.walkTime += dt * (m_paceMeasured ? go / m_walkPace : (a.walking ? 1.0f : 0.0f));
        if (w.ground) a.pos.y = w.ground(a.pos.x, a.pos.z);

        // Pose: grazing and walking, blended through the switch.
        std::vector<glm::mat4> pal = fitzel::sampleSkeleton(m_model, m_cfg.grazeClip, a.clipTime);
        if (a.blend > 0.0f) {
            const std::vector<glm::mat4> walk =
                fitzel::sampleSkeleton(m_model, m_cfg.walkClip, a.walkTime);
            if (a.blend >= 1.0f) pal = walk;
            else                 blendPalette(pal, walk, a.blend);
        }
        for (std::size_t p = 0; p < m_model.primitives.size() && p < a.meshes.size(); ++p) {
            fitzel::skinPrimitive(m_model.primitives[p], pal, m_scratch);
            a.meshes[p].update(m_scratch);
        }
        a.model = glm::translate(glm::mat4(1.0f), a.pos) *
                  glm::rotate(glm::mat4(1.0f), a.yaw + glm::radians(m_cfg.yawOffset) +
                                                   (m_flip ? 3.14159265f : 0.0f),
                              glm::vec3(0.0f, 1.0f, 0.0f)) *
                  glm::scale(glm::mat4(1.0f), glm::vec3(m_scale)) *
                  glm::translate(glm::mat4(1.0f), m_offset);
    }
}

void Herd::submit(fitzel::Renderer& r) {
    if (!m_ok) return;
    for (const Animal& a : m_animals)
        for (std::size_t p = 0; p < a.meshes.size() && p < m_mats.size(); ++p)
            r.submit(a.meshes[p], *m_mats[p], a.model, false);
}

std::string Herd::status() const {
    std::string clips;
    for (const fitzel::AnimationClip& k : m_model.animations) {
        char b[48];
        std::snprintf(b, sizeof b, " [%s %.2fs]", k.name.c_str(), k.duration);
        clips += b;
    }
    int walking = 0;
    for (const Animal& a : m_animals) walking += a.walking ? 1 : 0;
    char w[64];
    std::snprintf(w, sizeof w, " walking %d pace %.2f%s%s", walking, m_walkPace,
                  m_paceMeasured ? " measured" : "", m_flip ? " flipped" : "");
    std::string sp = " clip speeds";
    for (float v : m_clipSpeeds) { char b[16]; std::snprintf(b, sizeof b, " %.2f", v); sp += b; }
    return statusShort() + w + sp + clips;
}

std::string Herd::statusShort() const {
    char buf[200];
    std::snprintf(buf, sizeof buf, "herd %s: %zu prims, %zu joints, %zu clips (%.2f/%.2f/%.2f s), h %.2f, scale %.4f",
                  m_ok ? "ok" : "off", m_model.primitives.size(), m_model.skeleton.size(),
                  m_model.animations.size(),
                  m_model.animations.size() > 0 ? m_model.animations[0].duration : 0.0f,
                  m_model.animations.size() > 1 ? m_model.animations[1].duration : 0.0f,
                  m_model.animations.size() > 2 ? m_model.animations[2].duration : 0.0f,
                  m_model.height(), m_scale);
    return buf;
}
