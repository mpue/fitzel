#include "ParticleSystem.hpp"

#include <algorithm>
#include <cmath>

#include <glad/gl.h>
#include <glm/gtc/quaternion.hpp>

#include <fitzel/asset/AssetDatabase.hpp>

#include "Component.hpp"
#include "SceneTypes.hpp"

namespace {

// Instance layout, in floats: pos(3) size(1) rot(1) colour(4) vel(3).
constexpr int kStride = 12;

// A small, fast, self-contained RNG. std::mt19937 per emitter per frame is a lot
// of state for something that only needs to look unordered, and a global one
// would make emission depend on how many other emitters ran first.
struct Rng {
    std::uint32_t s;
    explicit Rng(std::uint32_t seed) : s(seed ? seed : 0x9e3779b9u) {}
    std::uint32_t next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return s;
    }
    float unit() { return static_cast<float>(next() & 0xffffffu) / 16777215.0f; }
    float sym()  { return unit() * 2.0f - 1.0f; }
};

glm::vec3 randomInSphere(Rng& r) {
    // Rejection sampling: three lines, uniform, and no trig. The alternative
    // (spherical coordinates) bunches samples at the poles unless the cosine is
    // distributed properly, which is more care than this needs.
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 v(r.sym(), r.sym(), r.sym());
        if (glm::dot(v, v) <= 1.0f) return v;
    }
    return glm::vec3(0.0f);
}

} // namespace

bool ParticleSystem::init() {
    m_shader = fitzel::Shader::fromFiles("assets/shaders/particle.vert",
                                         "assets/shaders/particle.frag");
    if (!m_shader.isValid()) return false;
    // No base vertex data at all: the quad's corners come from gl_VertexID, so
    // every attribute is per-instance.
    m_mesh = fitzel::InstancedMesh::create(
        nullptr, 0, 0, {},
        kStride * static_cast<int>(sizeof(float)),
        {{0, 3, 0},                      // pos
         {1, 1, 3 * sizeof(float)},      // size
         {2, 1, 4 * sizeof(float)},      // rotation
         {3, 4, 5 * sizeof(float)},      // colour + alpha
         {4, 3, 9 * sizeof(float)}});    // velocity (for the stretched form)
    m_ready = true;
    return true;
}

bool ParticleSystem::speedSource(const std::vector<Entity>& entities,
                                 const Entity& e, float& topSpeed,
                                 std::string& name) {
    // Walk self, then up. An exhaust is a child of the ship, so stopping at the
    // emitter would find nothing on every rig anyone would actually build. The
    // guard is against a parent cycle -- the hierarchy should not have one, but a
    // hand-edited scene file is not something to take on trust in a render loop.
    const Entity* n = &e;
    for (int guard = 0; n && guard < 32; ++guard) {
        if (const auto* g = n->components.get<GliderComponent>()) {
            topSpeed = g->maxSpeed;
            name     = "Glider \"" + n->name + "\"";
            return true;
        }
        if (const auto* o = n->components.get<OpponentComponent>()) {
            topSpeed = o->speed;
            name     = "Opponent \"" + n->name + "\"";
            return true;
        }
        if (n->parent < 0) break;
        const Entity* up = nullptr;
        for (const Entity& c : entities) if (c.id == n->parent) { up = &c; break; }
        n = up;
    }
    topSpeed = 0.0f;
    name.clear();
    return false;
}

int ParticleSystem::liveCount() const {
    int n = 0;
    for (const auto& [id, pool] : m_pools) n += static_cast<int>(pool.parts.size());
    return n;
}

void ParticleSystem::restart(int entityId) {
    auto it = m_pools.find(entityId);
    if (it == m_pools.end()) return;
    it->second.cycleT    = 0.0f;
    it->second.burstDone = false;
}

void ParticleSystem::update(std::vector<Entity>& entities, float dt,
                            fitzel::AssetDatabase& assetDb) {
    m_batches.clear();
    if (!m_ready || dt <= 0.0f) return;
    dt = std::min(dt, 0.1f);   // a stalled frame must not teleport a whole cycle

    for (auto& [id, pool] : m_pools) pool.seen = false;

    for (Entity& e : entities) {
        const auto* pc = e.components.get<ParticleComponent>();
        if (!pc) continue;
        Pool& pool = m_pools[e.id];
        pool.seen = true;

        // The emitter's own motion, measured rather than asked for: an emitter can
        // be moved by physics, a script, a parent's rotation or a hand on the
        // gizmo, and none of those report a velocity.
        const glm::vec3 emitVel =
            pool.havePos ? (e.center - pool.lastPos) / dt : glm::vec3(0.0f);
        pool.lastPos = e.center;
        pool.havePos = true;

        const int   maxN = std::max(1, static_cast<int>(std::lround(pc->maxCount)));
        const float life = std::max(pc->lifetime, 0.02f);
        const glm::quat rot = glm::quat(glm::radians(e.rotation));

        // Local space means exactly that: positions and velocities are held in
        // the EMITTER'S frame, so the object's motion cannot reach them at all --
        // it moves, it turns, and the plume sits on the nozzle unchanged. That is
        // what a thruster needs, and it is why following the object's translation
        // alone is not enough: a craft that banks would leave its flame pointing
        // where it used to be.
        // --- Speed-driven brightness ------------------------------------------
        // Resolved per emitter, per frame, because the craft's speed is the whole
        // point and a cached one would lag it.
        float glow = 1.0f;
        if (pc->speedGlowMin != pc->speedGlowMax) {
            float top = 0.0f;
            std::string src;
            if (speedSource(entities, e, top, src) && top > 0.1f) {
                // An Opponent knows its own speed exactly; anything else is
                // measured from how far the emitter moved, which is the craft's
                // velocity for a rigidly mounted exhaust and needs no reach into
                // the race sim.
                float v = glm::length(emitVel);
                const Entity* n = &e;
                for (int guard = 0; n && guard < 32; ++guard) {
                    if (const auto* o = n->components.get<OpponentComponent>()) {
                        v = o->curSpeed;
                        break;
                    }
                    if (n->parent < 0) break;
                    const Entity* up = nullptr;
                    for (const Entity& c : entities) if (c.id == n->parent) { up = &c; break; }
                    n = up;
                }
                const float t = glm::clamp(v / top, 0.0f, 1.0f);
                glow = glm::mix(pc->speedGlowMin, pc->speedGlowMax, t);
            }
        }

        const bool localSpace = !pc->worldSpace;
        if (pool.storedLocal != localSpace) {
            // The mode was just flipped. Carry the live particles across rather
            // than letting the next frame read one frame's coordinates as the
            // other's.
            const glm::quat inv = glm::conjugate(rot);
            for (Particle& q : pool.parts) {
                if (localSpace) { q.pos = inv * (q.pos - e.center); q.vel = inv * q.vel; }
                else            { q.pos = e.center + rot * q.pos;   q.vel = rot * q.vel; }
            }
            pool.storedLocal = localSpace;
        }

        // --- Emit ------------------------------------------------------------
        // A deactivated emitter stops producing but keeps integrating below, so
        // switching one off lets its last puff finish instead of deleting it in
        // mid-air.
        int want = 0;
        if (pc->playing && e.activeInHierarchy) {
            const float dur = std::max(pc->duration, 0.01f);
            const bool  open = pc->loop || pool.cycleT < dur;
            if (open) {
                if (!pool.burstDone) {
                    want += static_cast<int>(std::lround(std::max(pc->burst, 0.0f)));
                    pool.burstDone = true;
                }
                if (pool.cycleT < dur) {
                    pool.emitAccum += pc->rate * dt;
                    const int whole = static_cast<int>(pool.emitAccum);
                    want += whole;
                    pool.emitAccum -= static_cast<float>(whole);
                }
            }
            pool.cycleT += dt;
            if (pc->loop && pool.cycleT >= dur) {
                pool.cycleT -= dur;
                pool.burstDone = false;    // every cycle gets its burst
            }
        }
        want = std::min(want, maxN - static_cast<int>(pool.parts.size()));

        for (int i = 0; i < want; ++i) {
            Rng r(static_cast<std::uint32_t>(e.id * 2654435761u) ^
                  static_cast<std::uint32_t>(pool.parts.size() * 40503u) ^
                  static_cast<std::uint32_t>(pool.cycleT * 65537.0f));
            Particle q;
            glm::vec3 local(0.0f), dir(0.0f, 1.0f, 0.0f);
            switch (pc->shape) {
                case 1:  // sphere: spawn inside it, fly outward
                    local = randomInSphere(r) * pc->radius;
                    dir   = (glm::dot(local, local) > 1e-6f) ? glm::normalize(local)
                                                             : glm::vec3(0, 1, 0);
                    break;
                case 2: { // cone about the object's +Y
                    const float a  = glm::radians(pc->coneAngle) * std::sqrt(r.unit());
                    const float th = r.unit() * 6.28318531f;
                    dir = glm::vec3(std::sin(a) * std::cos(th), std::cos(a),
                                    std::sin(a) * std::sin(th));
                    local = glm::vec3(r.sym(), 0.0f, r.sym()) * pc->radius;
                    break;
                }
                case 3:  // box
                    local = glm::vec3(r.sym(), r.sym(), r.sym()) * pc->boxSize * 0.5f;
                    dir   = glm::vec3(0.0f, 1.0f, 0.0f);
                    break;
                default: // point
                    dir = glm::normalize(glm::vec3(r.sym(), r.sym() + 1.2f, r.sym()));
                    break;
            }
            const float sp = std::max(0.0f, pc->speed + r.sym() * pc->speedVar);
            if (localSpace) {
                // Already in the emitter's frame -- and `inherit` is meaningless
                // here by construction: there is no emitter motion to inherit.
                q.pos = local;
                q.vel = dir * sp;
            } else {
                q.pos = e.center + rot * local;
                q.vel = rot * dir * sp + emitVel * glm::clamp(pc->inherit, 0.0f, 1.0f);
            }
            q.life0 = std::max(0.02f, life * (1.0f + r.sym() * pc->lifeVar));
            q.life  = q.life0;
            q.size0 = std::max(0.05f, 1.0f + r.sym() * pc->sizeVar);
            q.rot   = glm::radians(pc->rotation) * r.unit();
            q.spin  = glm::radians(pc->spin);
            pool.parts.push_back(q);
        }

        // --- Integrate --------------------------------------------------------
        // Gravity and wind are world forces, so a LOCAL pool has to receive them
        // rotated into its own frame -- otherwise a smoke plume on a banking craft
        // would fall sideways with it. One quaternion rotate per emitter, not per
        // particle.
        const glm::vec3 accel = localSpace
            ? glm::conjugate(rot) * (pc->gravity + pc->wind)
            : (pc->gravity + pc->wind);
        const float damp = std::max(0.0f, 1.0f - pc->drag * dt);
        for (Particle& q : pool.parts) {
            q.life -= dt;
            q.vel  += accel * dt;
            q.vel  *= damp;
            q.pos  += q.vel * dt;
            q.rot  += q.spin * dt;
        }

        pool.parts.erase(std::remove_if(pool.parts.begin(), pool.parts.end(),
                                        [](const Particle& q) { return q.life <= 0.0f; }),
                         pool.parts.end());
        if (pool.parts.empty()) continue;

        // --- Pack -------------------------------------------------------------
        std::shared_ptr<fitzel::Texture> tex;
        if (!pc->sprite.empty()) {
            auto it = m_sprites.find(pc->sprite);
            if (it == m_sprites.end()) {
                // Resolved once per NAME, not per frame: a miss caches the null
                // too, so a typo costs one lookup rather than one a frame.
                std::shared_ptr<fitzel::Texture> t;
                for (const fitzel::AssetId& aid : assetDb.allAssets()) {
                    if (assetDb.typeForId(aid) != fitzel::AssetType::Texture) continue;
                    const auto* ent = assetDb.entry(aid);
                    if (!ent || ent->absPath.filename().string() != pc->sprite) continue;
                    t = assetDb.loadTexture(ent->absPath.string());
                    break;
                }
                it = m_sprites.emplace(pc->sprite, std::move(t)).first;
            }
            tex = it->second;
        }

        Batch batch;
        batch.tex      = tex;
        batch.additive = pc->blend == 1;
        batch.stretch  = pc->stretch;
        batch.depth    = 0.0f;
        batch.data.reserve(pool.parts.size() * kStride);
        for (const Particle& q : pool.parts) {
            const float t = glm::clamp(1.0f - q.life / q.life0, 0.0f, 1.0f);
            const glm::vec3 c =
                glm::mix(pc->colorStart, pc->colorEnd, t) * pc->brightness * glow;
            const float a = glm::mix(pc->alphaStart, pc->alphaEnd, t);
            const float s = glm::mix(pc->sizeStart, pc->sizeEnd, t) * q.size0;
            // A local pool is lifted into the world here and nowhere else, so the
            // whole simulation above stays in one frame and only the draw has to
            // know about the other. The velocity goes too -- the stretched form
            // orients itself by it, and a local velocity would point the streak
            // wherever the object happened to be facing at the origin.
            const glm::vec3 wp = pool.storedLocal ? e.center + rot * q.pos : q.pos;
            const glm::vec3 wv = pool.storedLocal ? rot * q.vel             : q.vel;
            batch.data.insert(batch.data.end(),
                              {wp.x, wp.y, wp.z, s, q.rot,
                               c.r, c.g, c.b, a,
                               wv.x, wv.y, wv.z});
        }
        m_batches.push_back(std::move(batch));
    }

    // Emitters whose entity is gone (deleted, or Play swapping the scene) take
    // their particles with them.
    for (auto it = m_pools.begin(); it != m_pools.end();)
        it = it->second.seen ? std::next(it) : m_pools.erase(it);
}

void ParticleSystem::draw(const FrameContext& ctx) {
    if (!m_ready || m_batches.empty()) return;

    // Back to front. Only alpha batches actually need it -- additive blending is
    // order-independent -- but one sort over a handful of batches is cheaper than
    // reasoning about which half of the list needs it.
    for (Batch& b : m_batches) {
        b.depth = 0.0f;
        if (b.data.size() >= 3) {
            const glm::vec3 p(b.data[0], b.data[1], b.data[2]);
            b.depth = glm::length(p - ctx.camPos);
        }
    }
    std::sort(m_batches.begin(), m_batches.end(),
              [](const Batch& a, const Batch& b) { return a.depth > b.depth; });

    const glm::mat4 inv = glm::inverse(ctx.viewProj);
    // The camera's right/up straight out of the view matrix, so the quads face
    // the pass being drawn -- including the water mirror, where "up" is not the
    // world's.
    const glm::vec3 right = glm::normalize(glm::vec3(inv[0]));
    const glm::vec3 up    = glm::normalize(glm::vec3(inv[1]));

    GLboolean depthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    glEnable(GL_BLEND);
    glDepthMask(GL_FALSE);   // transparent sprites must not erase each other

    m_shader.bind();
    m_shader.setMat4("uViewProj", ctx.viewProj);
    m_shader.setVec3("uCamRight", right);
    m_shader.setVec3("uCamUp", up);
    m_shader.setVec3("uCamPos", ctx.camPos);
    m_shader.setVec3("uFogColor", ctx.fogColor);
    m_shader.setFloat("uFogDensity", ctx.fogDensity);

    for (const Batch& b : m_batches) {
        if (b.data.empty()) continue;
        glBlendFunc(GL_SRC_ALPHA, b.additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
        m_shader.setInt("uAdditive", b.additive ? 1 : 0);
        m_shader.setFloat("uStretch", b.stretch);
        if (b.tex) {
            b.tex->bind(0);
            m_shader.setInt("uTex", 0);
            m_shader.setInt("uHasTex", 1);
        } else {
            m_shader.setInt("uHasTex", 0);
        }
        m_mesh.upload(b.data);
        m_mesh.draw(GL_TRIANGLE_STRIP, 4);
    }

    glDepthMask(depthMask);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
