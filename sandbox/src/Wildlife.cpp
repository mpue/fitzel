#include "Wildlife.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <glad/gl.h>

namespace {

constexpr float kPi = 3.14159265f;
constexpr int   kInstFloats = 13;   // pos3 rot3 anim2 scale colour3 belly

// One vertex: body-frame position (x right, y up, z forward; a span of 1),
// the wing coordinate (0 body, else side * fraction of the half-span), uv.
struct V { float x, y, z, wing, u, v; };

void tri(std::vector<V>& out, V a, V b, V c) { out.push_back(a); out.push_back(b); out.push_back(c); }

// A bird: a small spindle of a body, a forked tail, and two swept wings in two
// segments (arm and hand) so a wingbeat bends rather than hinges.
std::vector<V> birdMesh() {
    std::vector<V> m;
    const V nose{0, 0.005f, 0.22f, 0, 0, 0}, tail{0, 0, -0.14f, 0, 0, 0};
    const V l{-0.045f, 0, 0.04f, 0, 0, 0}, r{0.045f, 0, 0.04f, 0, 0, 0};
    const V t{0, 0.035f, 0.03f, 0, 0, 0}, b{0, -0.03f, 0.04f, 0, 0, 0};
    tri(m, nose, t, l); tri(m, nose, r, t); tri(m, nose, l, b); tri(m, nose, b, r);
    tri(m, tail, l, t); tri(m, tail, t, r); tri(m, tail, b, l); tri(m, tail, r, b);
    // Tail fan.
    tri(m, V{-0.07f, 0, -0.30f, 0, 0, 0}, V{0, 0, -0.12f, 0, 0, 0}, V{0.07f, 0, -0.30f, 0, 0, 0});
    for (int side = -1; side <= 1; side += 2) {
        const float s = static_cast<float>(side);
        // shoulder, elbow, tip -- leading and trailing edge.
        const V sL{s * 0.03f, 0, 0.09f, s * 0.06f, 0, 0}, sT{s * 0.03f, 0, -0.08f, s * 0.06f, 0, 0};
        const V eL{s * 0.25f, 0, 0.07f, s * 0.5f, 0, 0},  eT{s * 0.25f, 0, -0.07f, s * 0.5f, 0, 0};
        const V tip{s * 0.5f, 0, -0.06f, s * 1.0f, 0, 0}, tT{s * 0.42f, 0, -0.12f, s * 0.84f, 0, 0};
        if (side < 0) {
            tri(m, sL, eL, sT); tri(m, eL, eT, sT);
            tri(m, eL, tip, eT); tri(m, tip, tT, eT);
        } else {
            tri(m, sL, sT, eL); tri(m, eL, sT, eT);
            tri(m, eL, eT, tip); tri(m, tip, eT, tT);
        }
    }
    return m;
}

// A butterfly: a thin body and four wings (fore and hind each side) with
// their own uv square for the pattern the fragment shader paints.
std::vector<V> flyMesh() {
    std::vector<V> m;
    const V h{0, 0, 0.12f, 0, 0, 0}, a{0, 0, -0.16f, 0, 0, 0};
    const V bl{-0.015f, 0, 0, 0, 0, 0}, br{0.015f, 0, 0, 0, 0, 0}, bu{0, 0.012f, 0, 0, 0, 0};
    tri(m, h, bu, bl); tri(m, h, br, bu); tri(m, a, bl, bu); tri(m, a, bu, br);
    for (int side = -1; side <= 1; side += 2) {
        const float s = static_cast<float>(side);
        // Fore wing: span 0.5, from the body forward-ish; hind wing: shorter, behind.
        const V f0{s * 0.01f, 0, 0.02f, s * 0.02f, 0, 1}, f1{s * 0.5f, 0, 0.18f, s * 1.0f, 1, 1};
        const V f2{s * 0.46f, 0, -0.02f, s * 0.92f, 1, 0}, f3{s * 0.01f, 0, -0.02f, s * 0.02f, 0, 0};
        const V g0{s * 0.01f, 0, -0.01f, s * 0.02f, 0, 1}, g1{s * 0.36f, 0, -0.03f, s * 0.72f, 1, 1};
        const V g2{s * 0.30f, 0, -0.26f, s * 0.60f, 1, 0}, g3{s * 0.01f, 0, -0.14f, s * 0.02f, 0, 0};
        // Hind wings are told apart in the shader by their uv sitting in [2,3).
        V G0 = g0, G1 = g1, G2 = g2, G3 = g3;
        for (V* q : {&G0, &G1, &G2, &G3}) q->u += 2.0f;
        if (side < 0) { tri(m, f0, f1, f2); tri(m, f0, f2, f3); tri(m, G0, G1, G2); tri(m, G0, G2, G3); }
        else          { tri(m, f0, f2, f1); tri(m, f0, f3, f2); tri(m, G0, G2, G1); tri(m, G0, G3, G2); }
    }
    return m;
}

// A fish: a spindle, deeper than it is wide, with a forked tail fin and a
// dorsal fin. The wing coordinate is reused as how far a vertex swings in the
// tail beat: nothing at the head, the whole fin at the tail.
std::vector<V> fishMesh() {
    std::vector<V> m;
    struct R { float z, top, bot, half, wag; };
    const R rings[4] = {{0.30f, 0.10f, -0.08f, 0.050f, 0.00f},
                        {0.05f, 0.14f, -0.11f, 0.070f, 0.06f},
                        {-0.25f, 0.05f, -0.04f, 0.025f, 0.35f},
                        {-0.36f, 0.015f, -0.015f, 0.010f, 0.60f}};
    const auto ring = [&](const R& r, int k) {
        // top, right, bottom, left
        const float x = (k == 1) ? r.half : (k == 3) ? -r.half : 0.0f;
        const float y = (k == 0) ? r.top : (k == 2) ? r.bot : 0.5f * (r.top + r.bot);
        return V{x, y, r.z, r.wag, 0, 0};
    };
    const V nose{0, 0.0f, 0.5f, 0, 0, 0};
    for (int k = 0; k < 4; ++k) tri(m, nose, ring(rings[0], k), ring(rings[0], (k + 1) % 4));
    for (int s = 0; s < 3; ++s)
        for (int k = 0; k < 4; ++k) {
            const V a = ring(rings[s], k), b = ring(rings[s], (k + 1) % 4);
            const V c = ring(rings[s + 1], k), d = ring(rings[s + 1], (k + 1) % 4);
            tri(m, a, c, b); tri(m, b, c, d);
        }
    // Tail fin (forked) and the dorsal fin.
    // (u = 1: a fin, flat, where the body is round.)
    const V p{0, 0, -0.34f, 0.6f, 1, 0};
    tri(m, p, V{0, 0.16f, -0.53f, 1.0f, 1, 0}, V{0, 0.0f, -0.45f, 0.9f, 1, 0});
    tri(m, p, V{0, 0.0f, -0.45f, 0.9f, 1, 0}, V{0, -0.14f, -0.52f, 1.0f, 1, 0});
    tri(m, V{0, 0.13f, 0.10f, 0.03f, 1, 0}, V{0, 0.20f, -0.02f, 0.1f, 1, 0},
        V{0, 0.10f, -0.12f, 0.15f, 1, 0});
    return m;
}

void makeVao(const std::vector<V>& mesh, std::uint32_t& vao, std::uint32_t& vbo,
             std::uint32_t& inst) {
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh.size() * sizeof(V)),
                 mesh.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(V), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, sizeof(V), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(V), (void*)(4 * sizeof(float)));
    glGenBuffers(1, &inst);
    glBindBuffer(GL_ARRAY_BUFFER, inst);
    const GLsizei st = kInstFloats * sizeof(float);
    const int     sizes[6] = {3, 3, 2, 1, 3, 1};
    int off = 0;
    for (int i = 0; i < 6; ++i) {
        glEnableVertexAttribArray(3 + i);
        glVertexAttribPointer(3 + i, sizes[i], GL_FLOAT, GL_FALSE, st,
                              (void*)(static_cast<std::uintptr_t>(off) * sizeof(float)));
        glVertexAttribDivisor(3 + i, 1);
        off += sizes[i];
    }
    glBindVertexArray(0);
}

void pushInstance(std::vector<float>& d, const glm::vec3& p, float yaw, float pitch,
                  float bank, float phase, float flap, float scale, const glm::vec3& c,
                  float belly) {
    d.insert(d.end(), {p.x, p.y, p.z, yaw, pitch, bank, phase, flap, scale,
                       c.r, c.g, c.b, belly});
}

float wrapAngle(float a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}

} // namespace

bool Wildlife::init() {
    m_shader = fitzel::Shader::fromFiles("assets/shaders/creature.vert",
                                         "assets/shaders/creature.frag");
    if (!m_shader.isValid()) {
        std::fprintf(stderr, "Failed to load creature shader\n");
        return false;
    }
    const std::vector<V> bird = birdMesh(), fly = flyMesh(), fish = fishMesh();
    m_birdVerts = static_cast<int>(bird.size());
    m_flyVerts  = static_cast<int>(fly.size());
    m_fishVerts = static_cast<int>(fish.size());
    makeVao(bird, m_birdVao, m_birdVbo, m_birdInst);
    makeVao(fly, m_flyVao, m_flyVbo, m_flyInst);
    makeVao(fish, m_fishVao, m_fishVbo, m_fishInst);
    return true;
}

bool Wildlife::findWater(const World& w, float minDist, float maxDist, float depth,
                         glm::vec2& at, float* surface) {
    if (!w.water && (!w.ground || w.waterLevel < -999.0f)) return false;
    const float look = std::atan2(w.forward.x, w.forward.z);
    for (int tries = 0; tries < 40; ++tries) {
        // Mostly where the eye is looking: a fish that jumps behind you is a
        // sound, and this has none.
        const float a = (uni() < 0.75f) ? look + sym() * 0.7f : uni() * 2.0f * kPi;
        const float d = minDist + uni() * (maxDist - minDist);
        const glm::vec2 p(w.eye.x + std::sin(a) * d, w.eye.z + std::cos(a) * d);
        float surf = w.waterLevel, deep = 0.0f;
        if (w.water) {
            if (!w.water(p.x, p.y, surf, deep)) continue;
        } else {
            deep = w.waterLevel - w.ground(p.x, p.y);
        }
        if (deep < depth) continue;
        at = p;
        if (surface) *surface = surf;
        return true;
    }
    return false;
}

void Wildlife::applyRipples(const fitzel::Shader& s, bool on) const {
    static const char* const kRip[kMaxRipples] = {"uFishRing[0]", "uFishRing[1]", "uFishRing[2]",
                                                  "uFishRing[3]", "uFishRing[4]", "uFishRing[5]"};
    const int n = on ? std::min(static_cast<int>(m_ripples.size()), kMaxRipples) : 0;
    s.setInt("uFishRingCount", n);
    for (int i = 0; i < n; ++i) s.setVec4(kRip[i], m_ripples[static_cast<std::size_t>(i)]);
}

void Wildlife::addRipple(glm::vec2 xz, float strength, float y) {
    m_lastRipple = glm::vec3(xz.x, y, xz.y);
    if (static_cast<int>(m_ripples.size()) >= kMaxRipples) {
        auto oldest = std::max_element(m_ripples.begin(), m_ripples.end(),
            [](const glm::vec4& a, const glm::vec4& b) { return a.z < b.z; });
        m_ripples.erase(oldest);
    }
    m_ripples.push_back(glm::vec4(xz.x, xz.y, 0.0f, strength));
}

void Wildlife::updateFish(float dt, const World& w) {
    for (glm::vec4& r : m_ripples) r.z += dt;
    m_ripples.erase(std::remove_if(m_ripples.begin(), m_ripples.end(),
                                   [](const glm::vec4& r) { return r.z > 7.0f; }),
                    m_ripples.end());
    m_fishData.clear();
    if (!w.water && (!w.ground || w.waterLevel < -999.0f)) return;
    Fish& f = m_fish;
    if (f.flying) {
        f.vel.y -= 9.81f * dt;
        f.pos   += f.vel * dt;
        f.phase += dt * 32.0f;                 // the tail thrashing in the air
        if (f.pos.y < f.surface - 0.05f && f.vel.y < 0.0f) {
            f.flying = false;                  // back in, with a slap
            addRipple(glm::vec2(f.pos.x, f.pos.z), 1.0f, f.surface);
            m_splashes.push_back({glm::vec3(f.pos.x, f.surface, f.pos.z), 1.0f});
        }
    } else if ((f.nextJump -= dt) <= 0.0f) {
        f.nextJump = (5.0f + uni() * 14.0f) / std::max(fishRate, 0.1f);
        glm::vec2 at;
        if (w.daylight > 0.15f && findWater(w, 6.0f, 35.0f, 0.4f, at, &f.surface)) {
            f.scale   = 0.22f + uni() * 0.25f;
            const float h  = 0.25f + uni() * 0.6f;
            const float vy = std::sqrt(2.0f * 9.81f * h);
            const float hs = 1.0f + uni() * 1.4f;
            f.heading = uni() * 2.0f * kPi;
            f.vel     = glm::vec3(std::sin(f.heading) * hs, vy, std::cos(f.heading) * hs);
            f.pos     = glm::vec3(at.x, f.surface - 0.04f, at.y);
            f.phase   = uni() * 6.28f;
            f.flying  = true;
            ++m_fishJumps;
            addRipple(at, 0.6f, f.surface);
            m_splashes.push_back({glm::vec3(at.x, f.surface, at.y), 0.5f});
        }
    }
    // Between jumps, the quiet ones: a fish taking a fly off the surface,
    // nothing but a ring.
    if ((f.nextRise -= dt) <= 0.0f) {
        f.nextRise = 1.5f + uni() * 5.0f;
        glm::vec2 at;
        float surf = 0.0f;
        if (findWater(w, 5.0f, 60.0f, 0.2f, at, &surf)) addRipple(at, 0.3f + uni() * 0.3f, surf);
    }
    if (f.flying) {
        const float yaw   = std::atan2(f.vel.x, f.vel.z);
        const float pitch = std::atan2(f.vel.y, glm::length(glm::vec2(f.vel.x, f.vel.z)));
        pushInstance(m_fishData, f.pos, yaw, pitch, 0.25f * std::sin(f.phase * 0.3f), f.phase,
                     1.0f, f.scale, glm::vec3(0.20f, 0.26f, 0.25f), 0.95f);
    }
}

float Wildlife::groundAt(const World& w, float x, float z) const {
    const float g = w.ground ? w.ground(x, z) : 0.0f;
    return std::max(g, w.waterLevel);
}

void Wildlife::respawnFlock(int f, const World& w) {
    Flock& fl = m_flocks[f];
    const float a = uni() * 2.0f * kPi, d = 180.0f + uni() * 250.0f;
    fl.height = 25.0f + uni() * 45.0f;
    fl.goal = glm::vec3(w.eye.x + std::cos(a) * d, 0.0f, w.eye.z + std::sin(a) * d);
    fl.goal.y = groundAt(w, fl.goal.x, fl.goal.z) + fl.height;
    fl.goalTimer = 15.0f + uni() * 20.0f;
    // The birds arrive from over there, not out of thin air in front of you.
    const float a2 = a + kPi * 0.6f * sym();
    const glm::vec3 home(w.eye.x + std::cos(a2) * 420.0f, 0.0f, w.eye.z + std::sin(a2) * 420.0f);
    const float hy = groundAt(w, home.x, home.z) + fl.height;
    const glm::vec3 heading = glm::normalize(glm::vec3(fl.goal.x - home.x, 0.0f, fl.goal.z - home.z));
    for (Bird& b : m_birds) {
        if (b.flock != f) continue;
        b.pos = home + glm::vec3(sym() * 8.0f, hy + sym() * 4.0f, sym() * 8.0f);
        b.vel = heading * 11.0f + glm::vec3(sym(), sym() * 0.3f, sym());
        b.phase = uni() * 6.28f;
    }
}

void Wildlife::placeRaptor(Raptor& r, const World& w, bool initial) {
    const float a = uni() * 2.0f * kPi;
    const float d = initial ? 150.0f + uni() * 400.0f : 450.0f + uni() * 250.0f;
    r.centre = glm::vec2(w.eye.x + std::cos(a) * d, w.eye.z + std::sin(a) * d);
    r.radius = 35.0f + uni() * 45.0f;
    r.height = 110.0f + uni() * 110.0f;
    r.angle  = uni() * 2.0f * kPi;
    r.dir    = uni() < 0.5f ? -1.0f : 1.0f;
    r.flapTimer = 8.0f + uni() * 25.0f;
}

void Wildlife::placeFly(Fly& b, const World& w) {
    const float a = uni() * 2.0f * kPi, d = 4.0f + uni() * 26.0f;
    b.pos = glm::vec3(w.eye.x + std::cos(a) * d, 0.0f, w.eye.z + std::sin(a) * d);
    b.pos.y = groundAt(w, b.pos.x, b.pos.z) + 0.4f + uni() * 1.2f;
    b.target = b.pos;
    b.vel = glm::vec3(sym(), 0.0f, sym());
    b.rest = 0.0f;
    b.landed = false;
    b.phase = uni() * 6.28f;
    b.wander = uni() * 100.0f;
    // Cabbage white, brimstone, small tortoiseshell, common blue, peacock.
    static const glm::vec3 kColours[5] = {
        {0.93f, 0.92f, 0.86f}, {0.95f, 0.88f, 0.30f}, {0.90f, 0.42f, 0.10f},
        {0.35f, 0.55f, 0.95f}, {0.55f, 0.12f, 0.08f}};
    const float pick = uni();
    b.colour = kColours[pick < 0.32f ? 0 : pick < 0.52f ? 1 : pick < 0.72f ? 2 : pick < 0.87f ? 3 : 4];
}

void Wildlife::update(float dt, const World& w) {
    if (!enabled || !m_shader.isValid()) return;
    ++debugUpdates;
    dt = std::clamp(dt, 0.0f, 0.1f);
    m_daylight = w.daylight;
    const glm::vec2 wdir = w.wind ? w.wind->dir : glm::vec2(1.0f, 0.0f);
    const float     wstr = w.wind ? w.wind->strength : 0.2f;

    if (!m_spawned) {
        m_spawned = true;
        const int flocks = std::max(1, starlings / 30);
        m_flocks.assign(static_cast<std::size_t>(flocks), Flock{});
        m_birds.clear();
        for (int i = 0; i < starlings; ++i) {
            Bird b;
            b.flock = i % flocks;
            b.scale = 0.38f + uni() * 0.06f;
            m_birds.push_back(b);
        }
        for (int i = 0; i < swallows; ++i) {
            Bird b;
            b.flock = -1;
            b.scale = 0.32f + uni() * 0.04f;
            const float a = uni() * 6.28f, d = 10.0f + uni() * 40.0f;
            b.pos = glm::vec3(w.eye.x + std::cos(a) * d, 0.0f, w.eye.z + std::sin(a) * d);
            b.pos.y = groundAt(w, b.pos.x, b.pos.z) + 3.0f + uni() * 5.0f;
            b.vel = glm::vec3(sym() * 10.0f, 0.0f, sym() * 10.0f);
            m_birds.push_back(b);
        }
        for (int f = 0; f < flocks; ++f) respawnFlock(f, w);
        // The first flock is already overhead when the game starts.
        for (Bird& b : m_birds) {
            if (b.flock != 0) continue;
            b.pos = glm::vec3(w.eye.x + 60.0f + sym() * 8.0f, 0.0f, w.eye.z + 40.0f + sym() * 8.0f);
            b.pos.y = groundAt(w, b.pos.x, b.pos.z) + m_flocks[0].height + sym() * 3.0f;
        }
        m_raptors.assign(static_cast<std::size_t>(raptors), Raptor{});
        for (Raptor& r : m_raptors) placeRaptor(r, w, true);
        m_flies.assign(static_cast<std::size_t>(butterflies), Fly{});
        for (Fly& f : m_flies) placeFly(f, w);
    }

    // --- Starling flocks -------------------------------------------------------
    for (int f = 0; f < static_cast<int>(m_flocks.size()); ++f) {
        Flock& fl = m_flocks[f];
        fl.goalTimer -= dt;
        glm::vec3 centroid(0.0f), avgVel(0.0f);
        int n = 0;
        for (const Bird& b : m_birds)
            if (b.flock == f) { centroid += b.pos; avgVel += b.vel; ++n; }
        if (n == 0) continue;
        centroid /= static_cast<float>(n);
        avgVel   /= static_cast<float>(n);
        const float away = glm::length(glm::vec2(centroid.x - w.eye.x, centroid.z - w.eye.z));
        if (away > 1100.0f) { respawnFlock(f, w); continue; }
        if (fl.goalTimer <= 0.0f ||
            glm::distance(glm::vec2(centroid.x, centroid.z), glm::vec2(fl.goal.x, fl.goal.z)) < 30.0f) {
            // Somewhere else over the valley, never too far from the eye --
            // and now and then right over it, low, which is when a flock is
            // actually seen as birds and not as a drifting smudge.
            const float a = uni() * 2.0f * kPi;
            const float d = (uni() < 0.35f) ? 15.0f + uni() * 50.0f : 60.0f + uni() * 220.0f;
            fl.goal = glm::vec3(w.eye.x + std::cos(a) * d, 0.0f, w.eye.z + std::sin(a) * d);
            fl.height = glm::clamp(fl.height + sym() * 15.0f, 10.0f, 55.0f);
            fl.goal.y = groundAt(w, fl.goal.x, fl.goal.z) + fl.height;
            fl.goalTimer = 12.0f + uni() * 22.0f;
        }
        for (Bird& b : m_birds) {
            if (b.flock != f) continue;
            glm::vec3 acc(0.0f);
            for (const Bird& o : m_birds) {
                if (o.flock != f || &o == &b) continue;
                const glm::vec3 d = b.pos - o.pos;
                const float dist = glm::length(d);
                if (dist < 2.4f && dist > 1e-3f) acc += d / dist * (2.4f - dist) * 7.0f;
            }
            acc += (centroid - b.pos) * 0.25f;
            acc += (avgVel - b.vel) * 1.1f;
            const glm::vec3 toGoal = fl.goal - b.pos;
            acc += glm::normalize(toGoal + glm::vec3(0.0001f)) * 3.2f;
            const float above = b.pos.y - groundAt(w, b.pos.x, b.pos.z);
            if (above < 12.0f) acc.y += (12.0f - above) * 1.6f;
            if (above > 120.0f) acc.y -= (above - 120.0f) * 0.4f;
            acc += glm::vec3(wdir.x, 0.0f, wdir.y) * (wstr * 0.6f);
            const glm::vec3 oldVel = b.vel;
            b.vel += acc * dt;
            float sp = glm::length(b.vel);
            sp = glm::clamp(sp, 8.0f, 15.0f);
            b.vel = glm::normalize(b.vel) * sp;
            b.pos += b.vel * dt;
            // Wings: beat to climb or to catch up, glide otherwise.
            const float want = (b.vel.y > 0.6f || sp < 10.5f) ? 1.0f : 0.18f;
            b.flap += (want - b.flap) * std::min(1.0f, dt * 3.0f);
            b.phase += dt * (13.0f + 5.0f * b.flap);
            // Bank into the turn: the sideways part of the change in heading.
            const glm::vec3 fwd = glm::normalize(glm::vec3(b.vel.x, 0.0f, b.vel.z) + glm::vec3(1e-4f));
            const glm::vec3 side(fwd.z, 0.0f, -fwd.x);
            const float lat = glm::dot((b.vel - oldVel) / std::max(dt, 1e-4f), side);
            b.bank += (glm::clamp(-lat * 0.09f, -1.0f, 1.0f) - b.bank) * std::min(1.0f, dt * 4.0f);
        }
    }

    // --- Swallows hawking over the meadow --------------------------------------
    for (Bird& b : m_birds) {
        if (b.flock != -1) continue;
        glm::vec2 rel(b.pos.x - w.eye.x, b.pos.z - w.eye.z);
        if (glm::length(rel) > 160.0f) {
            const float a = uni() * 6.28f;
            b.pos = glm::vec3(w.eye.x + std::cos(a) * 60.0f, 0.0f, w.eye.z + std::sin(a) * 60.0f);
            b.pos.y = groundAt(w, b.pos.x, b.pos.z) + 6.0f;
        }
        // Each chases its own moving point near the eye: an insect it will
        // never catch, low over the grass.
        const float t = b.phase * 0.013f + b.scale * 91.0f;
        const glm::vec3 insect(w.eye.x + std::sin(t * 1.3f + b.scale * 17.0f) * 45.0f,
                               0.0f,
                               w.eye.z + std::cos(t * 0.9f + b.scale * 29.0f) * 45.0f);
        const float groundY = groundAt(w, b.pos.x, b.pos.z);
        if (b.pos.y < groundY + 0.8f) { b.pos.y = groundY + 0.8f; b.vel.y = std::max(b.vel.y, 2.0f); }
        const float wantY   = groundAt(w, insect.x, insect.z) +
                              2.0f + 4.0f * (0.5f + 0.5f * std::sin(t * 2.3f));
        glm::vec3 acc = glm::normalize(glm::vec3(insect.x - b.pos.x, 0.0f, insect.z - b.pos.z)
                                       + glm::vec3(1e-4f)) * 14.0f;
        acc.y = (wantY - b.pos.y) * 3.0f;
        if (b.pos.y - groundY < 1.2f) acc.y += 20.0f;
        const glm::vec3 oldVel = b.vel;
        b.vel += acc * dt;
        const float sp = glm::clamp(glm::length(b.vel), 9.0f, 17.0f);
        b.vel = glm::normalize(b.vel) * sp;
        b.pos += b.vel * dt;
        // Bursts of beats, then a glide on swept wings.
        const float burst = std::sin(b.phase * 0.07f + b.scale * 40.0f);
        b.flap += (((burst > 0.2f) ? 1.0f : 0.1f) - b.flap) * std::min(1.0f, dt * 5.0f);
        b.phase += dt * (10.0f + 8.0f * b.flap);
        const glm::vec3 fwd = glm::normalize(glm::vec3(b.vel.x, 0.0f, b.vel.z) + glm::vec3(1e-4f));
        const glm::vec3 side(fwd.z, 0.0f, -fwd.x);
        const float lat = glm::dot((b.vel - oldVel) / std::max(dt, 1e-4f), side);
        b.bank += (glm::clamp(-lat * 0.07f, -1.2f, 1.2f) - b.bank) * std::min(1.0f, dt * 6.0f);
    }

    // --- Raptors in the thermals -----------------------------------------------
    for (Raptor& r : m_raptors) {
        const float speed = 10.0f;
        r.angle += r.dir * speed / r.radius * dt;
        r.centre += wdir * (0.6f * wstr) * dt;                 // the thermal drifts
        if (glm::distance(r.centre, glm::vec2(w.eye.x, w.eye.z)) > 950.0f) placeRaptor(r, w, false);
        const glm::vec2 p = r.centre + glm::vec2(std::cos(r.angle), std::sin(r.angle)) * r.radius;
        const float gy = groundAt(w, r.centre.x, r.centre.y);
        // Slowly gaining height in the lift, then gliding off to the next one.
        r.height += dt * 0.8f;
        if (r.height > 260.0f) { r.height = 120.0f; placeRaptor(r, w, false); }
        const glm::vec3 np(p.x, gy + r.height + 4.0f * std::sin(r.angle * 0.5f), p.y);
        r.vel = (np - r.pos) / std::max(dt, 1e-4f);
        r.pos = np;
        r.flapTimer -= dt;
        if (r.flapTimer <= 0.0f) { r.flapLeft = 1.4f; r.flapTimer = 15.0f + uni() * 30.0f; }
        if (r.flapLeft > 0.0f) { r.flapLeft -= dt; r.phase += dt * 2.6f * 6.2831f; }
    }

    // --- Butterflies around the flowers ---------------------------------------
    const bool flyWeather = w.daylight > 0.35f && wstr < 1.0f;
    for (Fly& b : m_flies) {
        if (!flyWeather) continue;
        const float farAway = glm::length(glm::vec2(b.pos.x - w.eye.x, b.pos.z - w.eye.z));
        if (farAway > 48.0f) placeFly(b, w);
        if (b.landed) {
            b.rest -= dt;
            b.phase += dt * 1.6f;              // wings slowly open and close
            if (b.rest <= 0.0f) { b.landed = false; b.vel = glm::vec3(sym(), 1.2f, sym()); }
            continue;
        }
        // A new flower to visit: one of the blooms nearby, or just a spot.
        if (glm::distance(b.pos, b.target) < 0.25f || b.wander > 1e3f) {
            bool found = false;
            if (w.flowers && !w.flowers->empty()) {
                for (int tries = 0; tries < 12 && !found; ++tries) {
                    const glm::vec3& fp =
                        (*w.flowers)[static_cast<std::size_t>(uni() * w.flowers->size()) %
                                     w.flowers->size()];
                    if (glm::distance(glm::vec2(fp.x, fp.z), glm::vec2(b.pos.x, b.pos.z)) < 12.0f) {
                        if (glm::distance(b.pos, fp) < 0.3f) {   // arrived: land
                            b.landed = true;
                            b.rest = 2.0f + uni() * 6.0f;
                            b.pos = fp;
                            b.target = fp + glm::vec3(0.0f, 0.001f, 0.0f);
                            found = true;
                            break;
                        }
                        b.target = fp;
                        b.toFlower = true;
                        found = true;
                    }
                }
            }
            if (!found) {
                b.toFlower = false;
                b.target = b.pos + glm::vec3(sym() * 5.0f, 0.0f, sym() * 5.0f);
                b.target.y = groundAt(w, b.target.x, b.target.z) + 0.5f + uni() * 1.3f;
            }
            b.wander = 0.0f;
            if (b.landed) continue;
        }
        b.wander += dt;
        // Seek, with the jinking that makes a butterfly's path unmistakable.
        const glm::vec3 to = b.target - b.pos;
        glm::vec3 acc = glm::normalize(to + glm::vec3(1e-4f)) * 3.0f;
        acc += glm::vec3(std::sin(b.wander * 7.1f + b.phase), std::sin(b.wander * 9.3f) * 1.4f,
                         std::cos(b.wander * 6.3f + b.phase * 0.7f)) * 5.0f;
        acc += glm::vec3(wdir.x, 0.0f, wdir.y) * (wstr * 1.5f);
        const float gy = groundAt(w, b.pos.x, b.pos.z);
        if (b.pos.y - gy < 0.25f) acc.y += 6.0f;
        if (b.pos.y - gy > 2.5f) acc.y -= 3.0f;
        b.vel += acc * dt;
        const float sp = glm::length(b.vel);
        if (sp > 2.8f) b.vel *= 2.8f / sp;
        b.pos += b.vel * dt;
        b.phase += dt * 55.0f;                 // ~9 beats a second
        if (b.toFlower && glm::length(to) < 0.3f && b.wander > 0.5f) {
            b.landed = true;
            b.rest = 2.0f + uni() * 6.0f;
        }
        const float want = std::atan2(b.vel.x, b.vel.z);
        b.heading += wrapAngle(want - b.heading) * std::min(1.0f, dt * 6.0f);
    }

    // --- Fish in the lake --------------------------------------------------------
    updateFish(dt, w);

    // --- Instances ---------------------------------------------------------------
    m_birdData.clear();
    for (const Bird& b : m_birds) {
        const float yaw   = std::atan2(b.vel.x, b.vel.z);
        const float pitch = std::atan2(b.vel.y, glm::length(glm::vec2(b.vel.x, b.vel.z)));
        const bool swallow = b.flock == -1;
        const glm::vec3 col = swallow ? glm::vec3(0.035f, 0.04f, 0.07f) : glm::vec3(0.05f, 0.05f, 0.055f);
        pushInstance(m_birdData, b.pos, yaw, pitch, b.bank, b.phase, 0.25f + 0.75f * b.flap,
                     b.scale, col, swallow ? 0.8f : 0.15f);
    }
    for (const Raptor& r : m_raptors) {
        const float yaw  = std::atan2(r.vel.x, r.vel.z);
        const float bank = r.dir * std::atan(10.0f * 10.0f / (r.radius * 9.81f));
        const float flap = r.flapLeft > 0.0f ? 0.8f : 0.0f;
        pushInstance(m_birdData, r.pos, yaw, 0.0f, bank, r.phase, flap, 1.35f,
                     glm::vec3(0.22f, 0.15f, 0.09f), 0.55f);
    }
    m_flyData.clear();
    if (flyWeather)
        for (const Fly& b : m_flies) {
            const float flap = b.landed ? 0.5f : 1.0f;
            pushInstance(m_flyData, b.pos, b.heading, b.landed ? 0.0f : 0.25f, 0.0f, b.phase,
                         flap, 0.07f, b.colour, 0.0f);
        }
}

void Wildlife::draw(const FrameContext& c) {
    if (!enabled || !m_shader.isValid()) return;
    glDisable(GL_CULL_FACE);
    m_shader.bind();
    m_shader.setMat4("uViewProj", c.viewProj);
    m_shader.setVec3("uViewPos", c.camPos);
    m_shader.setVec3("uLightDir", c.lightDir);
    m_shader.setVec3("uLightColor", c.lightColor);
    m_shader.setVec3("uAmbient", c.ambient);
    m_shader.setVec3("uFogColor", c.fogColor);
    m_shader.setVec3("uFogSunColor", c.fogSunColor);
    m_shader.setFloat("uFogDensity", c.fogDensity);
    m_shader.setFloat("uFogHeightFalloff", c.fogHeightFalloff);
    m_shader.setFloat("uFogHeight", c.fogHeight);
    applySunShadows(m_shader, c);
    if (!m_birdData.empty()) {
        glBindBuffer(GL_ARRAY_BUFFER, m_birdInst);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_birdData.size() * sizeof(float)),
                     m_birdData.data(), GL_STREAM_DRAW);
        m_shader.setInt("uKind", 0);
        glBindVertexArray(m_birdVao);
        glDrawArraysInstanced(GL_TRIANGLES, 0, m_birdVerts,
                              static_cast<GLsizei>(m_birdData.size() / kInstFloats));
    }
    if (!m_flyData.empty()) {
        glBindBuffer(GL_ARRAY_BUFFER, m_flyInst);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_flyData.size() * sizeof(float)),
                     m_flyData.data(), GL_STREAM_DRAW);
        m_shader.setInt("uKind", 1);
        glBindVertexArray(m_flyVao);
        glDrawArraysInstanced(GL_TRIANGLES, 0, m_flyVerts,
                              static_cast<GLsizei>(m_flyData.size() / kInstFloats));
    }
    if (!m_fishData.empty()) {
        glBindBuffer(GL_ARRAY_BUFFER, m_fishInst);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_fishData.size() * sizeof(float)),
                     m_fishData.data(), GL_STREAM_DRAW);
        m_shader.setInt("uKind", 2);
        glBindVertexArray(m_fishVao);
        glDrawArraysInstanced(GL_TRIANGLES, 0, m_fishVerts,
                              static_cast<GLsizei>(m_fishData.size() / kInstFloats));
    }
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
}
