#include "WeaponSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <iterator>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include <fitzel/core/Input.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/render/Renderer.hpp>

namespace {

constexpr float kPi = 3.14159265358979f;

// --- Look -------------------------------------------------------------------
// The HUD borrows the race HUD's ink and hairline so the two read as one
// instrument panel, and adds exactly one colour of its own: the seeker's amber,
// which goes red the moment the lock is solid. Nothing else on screen is red
// while you are flying clean, so a locked target cannot be mistaken for
// anything else.
constexpr ImU32 kInk   = IM_COL32( 10,  13,  20, 175);
constexpr ImU32 kEdge  = IM_COL32(255, 255, 255,  26);
constexpr ImU32 kText  = IM_COL32(236, 240, 247, 255);
constexpr ImU32 kDim   = IM_COL32(148, 160, 180, 255);
constexpr ImU32 kAmber = IM_COL32(255, 186,  64, 255);
constexpr ImU32 kRed   = IM_COL32(255,  74,  62, 255);
constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);

ImU32 fade(ImU32 col, float a) {
    const float f = std::clamp(a, 0.0f, 1.0f);
    return (col & 0x00FFFFFFu) |
           (static_cast<ImU32>(((col >> 24) & 0xFF) * f) << 24);
}

ImU32 mixCol(ImU32 a, ImU32 b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const auto ch = [&](int shift) {
        const float x = static_cast<float>((a >> shift) & 0xFF);
        const float y = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<ImU32>(x + (y - x) * t) << shift;
    };
    return ch(0) | ch(8) | ch(16) | ch(24);
}

float easeOut(float t) { t = std::clamp(t, 0.0f, 1.0f); return 1.0f - (1.0f - t) * (1.0f - t); }

// A rotation whose +Z axis points along `fwd`. Everything in this file is
// modelled nose-down-+Z (missile, plume, brackets, rings), so one helper places
// all of it.
glm::mat4 orientTo(const glm::vec3& fwd, glm::vec3 upHint = glm::vec3(0.0f, 1.0f, 0.0f)) {
    glm::vec3 f = fwd;
    const float fl = glm::length(f);
    if (fl < 1e-5f) return glm::mat4(1.0f);
    f /= fl;
    if (std::abs(glm::dot(f, upHint)) > 0.985f) upHint = glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 r = glm::normalize(glm::cross(upHint, f));
    const glm::vec3 u = glm::cross(f, r);
    return glm::mat4(glm::vec4(r, 0.0f), glm::vec4(u, 0.0f),
                     glm::vec4(f, 0.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
}

// --- Geometry builders ------------------------------------------------------
// Triangle soup with flat normals: these are small, self-lit shapes drawn a
// handful at a time, so an index buffer and smooth normals would buy nothing
// and cost clarity. Every surface is emitted double-sided where it is a sheet
// (fins, rings, brackets) -- a targeting bracket seen from behind that vanishes
// is a bug the player experiences as "the lock disappeared".
struct Build {
    std::vector<fitzel::Vertex> v;

    void tri(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
        glm::vec3 n = glm::cross(b - a, c - a);
        const float l = glm::length(n);
        n = l > 1e-6f ? n / l : glm::vec3(0.0f, 1.0f, 0.0f);
        for (const glm::vec3& p : {a, b, c}) {
            fitzel::Vertex vt;
            vt.position = p; vt.normal = n; vt.uv = glm::vec2(0.0f);
            v.push_back(vt);
        }
    }
    void quad(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
              const glm::vec3& d) { tri(a, b, c); tri(a, c, d); }
    // Both windings: a sheet with no inside.
    void sheet(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
               const glm::vec3& d) { quad(a, b, c, d); quad(a, d, c, b); }
};

// The missile: a slim dart, nose along +Z, about 2.4 m long. Deliberately
// readable in silhouette (fat motor, thin waist, three fins) because at 135 m/s
// the shape is all anyone ever sees of it.
fitzel::Mesh buildDart() {
    Build b;
    constexpr int kSeg = 14;
    const float rBody = 0.155f, rMotor = 0.185f;
    const float zTail = -1.10f, zMotor = -0.80f, zNose = 0.55f, zTip = 1.30f;
    auto ring = [&](float z, float r, int i) {
        const float a = 2.0f * kPi * static_cast<float>(i) / kSeg;
        return glm::vec3(std::cos(a) * r, std::sin(a) * r, z);
    };
    for (int i = 0; i < kSeg; ++i) {
        const int j = (i + 1) % kSeg;
        // Motor bell -> body -> nose cone.
        b.quad(ring(zTail, rMotor, i), ring(zTail, rMotor, j),
               ring(zMotor, rMotor, j), ring(zMotor, rMotor, i));
        b.quad(ring(zMotor, rMotor, i), ring(zMotor, rMotor, j),
               ring(zNose, rBody, j), ring(zNose, rBody, i));
        b.tri(ring(zNose, rBody, i), ring(zNose, rBody, j),
              glm::vec3(0.0f, 0.0f, zTip));
        // Tail cap, so the motor isn't an open pipe from behind.
        b.tri(ring(zTail, rMotor, j), ring(zTail, rMotor, i),
              glm::vec3(0.0f, 0.0f, zTail));
    }
    // Three tail fins and three small canards up front -- the canards are what
    // make it read as "guided" rather than "firework".
    for (int k = 0; k < 3; ++k) {
        const float a = 2.0f * kPi * static_cast<float>(k) / 3.0f;
        const glm::vec3 out(std::cos(a), std::sin(a), 0.0f);
        b.sheet(out * rMotor + glm::vec3(0, 0, zTail),
                out * (rMotor + 0.30f) + glm::vec3(0, 0, zTail),
                out * (rMotor + 0.30f) + glm::vec3(0, 0, zTail + 0.28f),
                out * rMotor + glm::vec3(0, 0, zMotor + 0.30f));
        b.sheet(out * rBody + glm::vec3(0, 0, 0.16f),
                out * (rBody + 0.16f) + glm::vec3(0, 0, 0.20f),
                out * (rBody + 0.16f) + glm::vec3(0, 0, 0.42f),
                out * rBody + glm::vec3(0, 0, 0.46f));
    }
    return fitzel::Mesh::create(b.v);
}

// The motor plume: a cone blowing down -Z from the tail, apex trailing. Drawn
// twice at different scales (a white core inside an orange flare), which is the
// cheapest way to get a plume that looks hot rather than orange.
fitzel::Mesh buildFlame() {
    Build b;
    constexpr int kSeg = 14;
    const float z0 = -1.05f, z1 = -2.60f, r = 0.24f;
    for (int i = 0; i < kSeg; ++i) {
        const float a0 = 2.0f * kPi * static_cast<float>(i) / kSeg;
        const float a1 = 2.0f * kPi * static_cast<float>(i + 1) / kSeg;
        const glm::vec3 p0(std::cos(a0) * r, std::sin(a0) * r, z0);
        const glm::vec3 p1(std::cos(a1) * r, std::sin(a1) * r, z0);
        b.tri(p0, p1, glm::vec3(0.0f, 0.0f, z1));
        b.tri(p1, p0, glm::vec3(0.0f, 0.0f, z0)); // close the mouth
    }
    return fitzel::Mesh::create(b.v);
}

// A unit annulus in the XY plane (normal +Z): shockwaves and the lock's pulse.
fitzel::Mesh buildRing(float inner, int seg) {
    Build b;
    for (int i = 0; i < seg; ++i) {
        const float a0 = 2.0f * kPi * static_cast<float>(i) / seg;
        const float a1 = 2.0f * kPi * static_cast<float>(i + 1) / seg;
        const glm::vec2 d0(std::cos(a0), std::sin(a0)), d1(std::cos(a1), std::sin(a1));
        b.sheet(glm::vec3(d0 * inner, 0.0f), glm::vec3(d1 * inner, 0.0f),
                glm::vec3(d1, 0.0f), glm::vec3(d0, 0.0f));
    }
    return fitzel::Mesh::create(b.v);
}

// The gyro ring that orbits a marked craft: four arcs with gaps, plus a tab on
// each. The gaps are the point -- a plain ring spinning about its own axis is
// indistinguishable from a ring standing still, and the whole job of this thing
// is to be obviously ALIVE around the one craft the next missile is going to.
fitzel::Mesh buildGyro() {
    Build b;
    constexpr int kArcs = 4, kSteps = 9;
    const float span = kPi * 0.36f;      // ~65 deg of arc, ~25 deg of gap
    const float inner = 0.90f;
    for (int a = 0; a < kArcs; ++a) {
        const float base = 2.0f * kPi * static_cast<float>(a) / kArcs;
        for (int i = 0; i < kSteps; ++i) {
            const float t0 = base + span * static_cast<float>(i) / kSteps;
            const float t1 = base + span * static_cast<float>(i + 1) / kSteps;
            const glm::vec2 d0(std::cos(t0), std::sin(t0)), d1(std::cos(t1), std::sin(t1));
            b.sheet(glm::vec3(d0 * inner, 0.0f), glm::vec3(d1 * inner, 0.0f),
                    glm::vec3(d1, 0.0f), glm::vec3(d0, 0.0f));
        }
        // A tab standing out of the head of each arc.
        const glm::vec2 d(std::cos(base), std::sin(base));
        const glm::vec2 t(-d.y, d.x);
        b.sheet(glm::vec3(d * 0.98f - t * 0.05f, 0.0f),
                glm::vec3(d * 0.98f + t * 0.05f, 0.0f),
                glm::vec3(d * 1.20f + t * 0.05f, 0.0f),
                glm::vec3(d * 1.20f - t * 0.05f, 0.0f));
    }
    return fitzel::Mesh::create(b.v);
}

// Four corner brackets in the XY plane, at (+-1, +-1): the target cage,
// billboarded at the marked craft. Half-size 1, so the draw scales it to the
// craft it is closing in on.
fitzel::Mesh buildBracket() {
    Build b;
    const float arm = 0.46f, w = 0.085f;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2) {
            const glm::vec3 c(static_cast<float>(sx), static_cast<float>(sy), 0.0f);
            const float dx = -static_cast<float>(sx), dy = -static_cast<float>(sy);
            // Horizontal leg
            b.sheet(c + glm::vec3(0.0f, dy * w, 0.0f), c + glm::vec3(dx * arm, dy * w, 0.0f),
                    c + glm::vec3(dx * arm, 0.0f, 0.0f), c);
            // Vertical leg
            b.sheet(c + glm::vec3(dx * w, 0.0f, 0.0f), c + glm::vec3(dx * w, dy * arm, 0.0f),
                    c + glm::vec3(0.0f, dy * arm, 0.0f), c);
        }
    return fitzel::Mesh::create(b.v);
}

// A spark: a four-sided spike trailing from the origin down -Z, fat at the head
// and pinched at the tail. Scaled to the debris' own speed at draw time, which
// is what turns a cloud of dots into a burst of streaks.
fitzel::Mesh buildShard() {
    Build b;
    const float r = 0.5f;
    const glm::vec3 tail(0.0f, 0.0f, -1.0f);
    for (int i = 0; i < 4; ++i) {
        const float a0 = kPi * 0.5f * static_cast<float>(i);
        const float a1 = kPi * 0.5f * static_cast<float>(i + 1);
        const glm::vec3 p0(std::cos(a0) * r, std::sin(a0) * r, 0.0f);
        const glm::vec3 p1(std::cos(a1) * r, std::sin(a1) * r, 0.0f);
        b.tri(p0, p1, tail);
        b.tri(p1, p0, glm::vec3(0.0f));   // cap the head
    }
    return fitzel::Mesh::create(b.v);
}

// Unit sphere: the fireball and the smoke it throws.
fitzel::Mesh buildSphere() {
    Build b;
    constexpr int kU = 16, kV = 10;
    auto p = [&](int i, int j) {
        const float u = 2.0f * kPi * static_cast<float>(i) / kU;
        const float v = kPi * static_cast<float>(j) / kV;
        return glm::vec3(std::sin(v) * std::cos(u), std::cos(v), std::sin(v) * std::sin(u));
    };
    for (int j = 0; j < kV; ++j)
        for (int i = 0; i < kU; ++i)
            b.quad(p(i, j), p(i + 1, j), p(i + 1, j + 1), p(i, j + 1));
    return fitzel::Mesh::create(b.v);
}

// --- Ribbons ----------------------------------------------------------------
// The same camera-facing strip the contrails use (TrailSystem.cpp explains the
// degenerate cases in detail), factored to a free function because a missile
// needs two of them: a white-hot core that dies in a fifth of a second and the
// smoke around it that hangs for a second and a half.
glm::vec3 perpOf(const glm::vec3& d) {
    const glm::vec3 up = std::abs(d.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f)
                                              : glm::vec3(1.0f, 0.0f, 0.0f);
    return glm::normalize(glm::cross(d, up));
}

glm::vec3 sideAt(const glm::vec3& p, const glm::vec3& dir, const glm::vec3& camPos) {
    glm::vec3 toCam = camPos - p;
    const float d = glm::length(toCam);
    const glm::vec3 stable0 = perpOf(dir);
    if (d < 1e-4f) return stable0;
    toCam /= d;
    glm::vec3 cs = glm::cross(dir, toCam);
    const float sinA = glm::length(cs);
    if (sinA < 1e-4f) return stable0;
    cs /= sinA;
    const glm::vec3 stable = glm::dot(stable0, cs) < 0.0f ? -stable0 : stable0;
    const glm::vec3 mixed = glm::mix(stable, cs, glm::smoothstep(0.06f, 0.30f, sinA));
    const float ml = glm::length(mixed);
    return ml > 1e-4f ? mixed / ml : stable;
}

} // namespace

// ============================================================================

WeaponSystem::WeaponSystem(fitzel::Shader& lit) : m_lit(&lit) {
    m_dart    = buildDart();
    m_flame   = buildFlame();
    m_ring    = buildRing(0.82f, 56);
    m_gyro    = buildGyro();
    m_bracket = buildBracket();
    m_sphere  = buildSphere();
    m_shard   = buildShard();
    m_ammo    = ammoMax;
}

float WeaponSystem::rnd() {
    // xorshift32: deterministic, cheap, and independent of whatever else in the
    // frame happens to be pulling on a shared generator.
    m_rngState ^= m_rngState << 13;
    m_rngState ^= m_rngState >> 17;
    m_rngState ^= m_rngState << 5;
    return static_cast<float>(m_rngState & 0xFFFFFFu) / static_cast<float>(0x1000000u);
}

fitzel::Material& WeaponSystem::mat(const glm::vec3& albedo, const glm::vec3& emission,
                                    float glow) {
    if (m_matUsed >= m_mats.size()) m_mats.emplace_back(*m_lit);
    fitzel::Material& m = m_mats[m_matUsed++];
    // Every uniform this look depends on, set on every draw. The lit program is
    // shared with the terrain, the road and every entity, so anything left
    // unset here is inherited from whoever drew last (see the road's wetness).
    m.set("uColorMode", 0)
     .set("uAlbedo", albedo)
     .set("uTint", glm::vec3(1.0f))
     .set("uEmission", emission)
     .set("uEmissionStrength", glow)
     .set("uWetness", 0.0f)
     .set("uWaterLevel", -1.0e4f)
     .set("uRoughness", 0.4f);
    return m;
}

void WeaponSystem::reset() {
    m_missiles.clear();
    m_blasts.clear();
    m_hits.clear();
    m_tracks.clear();
    m_trackId = -1;
    m_locked  = false;
    m_lockT   = 0.0f;
    m_offCone = 0.0f;
    m_seekBlip = m_lockFlash = m_denyFlash = m_hitFlash = 0.0f;
    m_seekAnnounced = -1;
    m_pulse = 0.0f;
    m_ammo   = std::max(1, ammoMax);
    m_reload = 0.0f;
    m_fireT  = 0.0f;
    m_haveMark = false;
    m_prevFire = m_prevCycle = false;
}

// --- Acquisition ------------------------------------------------------------

void WeaponSystem::updateLock(const Frame& f, const std::vector<Racer>& racers) {
    const glm::vec3 fwd = glm::normalize(f.fwd);
    const float cosCone = std::cos(glm::radians(std::clamp(lockAngle, 2.0f, 89.0f)));

    // Who is inside the seeker cone right now, best first. "Best" is mostly how
    // centred a craft is, nudged by range, and heavily biased toward the one we
    // are already on: a seeker that swaps targets whenever two rivals cross is
    // useless in exactly the moment it matters.
    int   best = -1;
    float bestScore = -1e9f;
    bool  trackInCone = false;
    std::vector<int> inCone;
    for (const Racer& r : racers) {
        const glm::vec3 d = r.pos - f.pos;
        const float dist = glm::length(d);
        if (dist < 1e-3f || dist > lockRange) continue;
        const float c = glm::dot(d / dist, fwd);
        if (c < cosCone) continue;
        inCone.push_back(r.id);
        if (r.id == m_trackId) trackInCone = true;
        float score = c - dist * 0.0005f;
        if (r.id == m_trackId) score += 0.20f;
        if (score > bestScore) { bestScore = score; best = r.id; }
    }

    // Manual step to another craft in the cone (T / pad Y). Tremor-friendly on
    // purpose: the seeker picks for you and this is a single tap to say "no,
    // that one" -- there is no aiming to be done with it.
    if (f.input) {
        bool cycle = f.input->isKeyDown(GLFW_KEY_T);
        if (f.input->hasGamepad() && f.input->gamepadButton(GLFW_GAMEPAD_BUTTON_Y))
            cycle = true;
        if (cycle && !m_prevCycle && inCone.size() > 1) {
            std::sort(inCone.begin(), inCone.end());
            auto it = std::find(inCone.begin(), inCone.end(), m_trackId);
            const std::size_t next =
                it == inCone.end() ? 0
                                   : (static_cast<std::size_t>(it - inCone.begin()) + 1) % inCone.size();
            m_trackId = inCone[next];
            m_lockT = 0.0f; m_locked = false; m_offCone = 0.0f;
            trackInCone = true;
            m_seekAnnounced = m_trackId;   // this IS the dit; don't play a second
            m_seekBlip = 0.30f;
            cue(soundSeek, 0.35f, 1.0f);
        }
        m_prevCycle = cycle;
    }

    // Is the craft we were tracking still in the field at all? A rival that has
    // been deleted (or finished and left) drops the lock instantly -- there is
    // nothing to keep aiming at.
    bool trackAlive = false;
    for (const Racer& r : racers) if (r.id == m_trackId) { trackAlive = true; break; }
    if (!trackAlive) { m_trackId = -1; m_locked = false; m_lockT = 0.0f; }

    if (m_trackId < 0) {
        if (best >= 0) { m_trackId = best; m_lockT = 0.0f; m_offCone = 0.0f; }
    } else if (trackInCone) {
        m_offCone = 0.0f;
        const float was = m_lockT;
        m_lockT = std::min(1.0f, m_lockT + f.dt / std::max(lockTime, 0.05f));
        if (m_lockT >= 1.0f && was < 1.0f) {
            m_locked = true;
            m_lockFlash = 0.55f;
            cue(soundLock, 0.45f, 1.0f);
        }
    } else {
        // Off the cone. A solid lock coasts for `lockHold` -- that is what lets
        // a missile be sent round a corner, and it is the difference between a
        // weapon and a laser pointer. A half-built one just decays.
        m_offCone += f.dt;
        if (m_locked) {
            if (m_offCone > std::max(lockHold, 0.0f)) {
                m_locked = false; m_lockT = 0.0f;
                m_trackId = best;   // may be -1
                m_offCone = 0.0f;
            }
        } else {
            m_lockT -= f.dt / std::max(lockTime, 0.05f) * 1.6f;
            if (m_lockT <= 0.0f) {
                m_lockT = 0.0f;
                if (best >= 0 && best != m_trackId) m_trackId = best;
            }
        }
    }

    // One dit when the seeker latches onto a craft -- and then nothing until the
    // lock closes with its dit-dit. No repeating blip: the reticle's arc already
    // says how far along the lock is, and it says it better than a beep four
    // times a second, which is the kind of noise that gets a weapon muted.
    //
    // Announced on CHANGE of target (including to "none"), so re-acquiring after
    // a drop dits again, with a short re-arm guard -- a rival flickering across
    // the edge of the cone must not machine-gun the cue.
    m_seekBlip = std::max(0.0f, m_seekBlip - f.dt);
    if (m_trackId != m_seekAnnounced && m_seekBlip <= 0.0f) {
        if (m_trackId >= 0 && trackInCone) cue(soundSeek, 0.30f, 1.0f);
        m_seekAnnounced = m_trackId;
        m_seekBlip = 0.30f;
    }

    // Remember where the marked craft is: the world marker and the reticle both
    // draw long after this, by which time the caller's list is gone.
    m_haveMark = false;
    for (const Racer& r : racers)
        if (r.id == m_trackId) {
            m_haveMark   = true;
            m_markPos    = r.pos;
            m_markRadius = std::max(r.radius, 0.8f);
            m_markName   = r.name.empty() ? std::string("RIVAL") : r.name;
            m_markDist   = glm::length(r.pos - f.pos);
            auto it = m_tracks.find(r.id);
            m_markVel = it == m_tracks.end() ? glm::vec3(0.0f) : it->second.vel;
            break;
        }
}

// --- Launch -----------------------------------------------------------------

void WeaponSystem::fire(const Frame& f) {
    Missile m;
    const glm::vec3 fwd   = glm::normalize(f.fwd);
    const glm::vec3 up    = glm::length(f.up) > 1e-4f ? glm::normalize(f.up)
                                                      : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 right = glm::normalize(glm::cross(fwd, up));
    const float side = m_rail ? 1.0f : -1.0f;
    m_rail ^= 1;   // alternate rails, so a salvo fans out instead of stacking

    m.pos = f.pos + right * (side * 1.15f) - up * 0.20f + fwd * 0.5f;
    // Out of the rail sideways and a little UP, then the seeker takes over: the
    // resulting S is what makes a launch read as a launch rather than a bullet.
    // Up rather than down on purpose -- the craft flies about a metre and a half
    // over the road, and a rail that threw the missile downward would plant half
    // of them in the tarmac before the seeker ever got a say.
    m.launchDir = glm::normalize(fwd * 0.5f + right * (side * 0.78f) + up * 0.16f);
    m.dir   = m.launchDir;
    m.speed = std::max(glm::length(f.vel), 0.0f) + 26.0f;
    m.vel   = m.dir * m.speed;
    m.target = m_trackId;
    m.life   = std::max(missileLife, 1.0f);
    m.flicker = rnd() * 100.0f;
    m_missiles.push_back(std::move(m));

    --m_ammo;
    m_fireT = std::max(fireDelay, 0.05f);
    cue(soundFire, 1.0f, 0.94f + rnd() * 0.12f);
}

// --- Detonation -------------------------------------------------------------

void WeaponSystem::detonate(const Missile& m, const std::vector<Racer>& racers,
                            bool onTarget) {
    Blast b;
    b.pos  = m.pos;
    b.dir  = m.dir;
    b.life = 1.5f;
    b.spin = rnd() * 6.2831f;

    // Sparks: thrown along the missile's travel more than across it, because
    // that is what debris off a hit at 500 km/h actually does.
    const int nSp = 26;
    b.sparks.reserve(nSp);
    for (int i = 0; i < nSp; ++i) {
        glm::vec3 d(rnd(-1.0f, 1.0f), rnd(-1.0f, 1.0f), rnd(-1.0f, 1.0f));
        const float dl = glm::length(d);
        d = dl > 1e-4f ? d / dl : glm::vec3(0.0f, 1.0f, 0.0f);
        Blast::Spark s;
        s.pos   = b.pos + d * rnd(0.0f, 0.8f);
        s.vel   = d * rnd(16.0f, 46.0f) + m.dir * rnd(4.0f, 22.0f);
        s.life0 = rnd(0.32f, 0.85f);
        s.life  = s.life0;
        b.sparks.push_back(s);
    }
    const int nPf = 8;
    b.puffs.reserve(nPf);
    for (int i = 0; i < nPf; ++i) {
        glm::vec3 d(rnd(-1.0f, 1.0f), rnd(-0.3f, 1.0f), rnd(-1.0f, 1.0f));
        const float dl = glm::length(d);
        d = dl > 1e-4f ? d / dl : glm::vec3(0.0f, 1.0f, 0.0f);
        Blast::Puff p;
        p.pos   = b.pos + d * rnd(0.0f, 1.6f);
        p.vel   = d * rnd(2.5f, 8.0f) + glm::vec3(0.0f, 1.6f, 0.0f);
        p.r0    = rnd(1.0f, 2.3f);
        p.life0 = rnd(0.9f, 1.5f);
        p.life  = p.life0;
        b.puffs.push_back(p);
    }
    m_blasts.push_back(std::move(b));

    // Who was caught. A direct hit is a full hit; the blast falls off to nothing
    // at its own radius, so a near miss still costs the rival something --
    // otherwise the only reward for a well-flown shot is binary.
    const float R = std::max(blastRadius, 1.0f);
    bool connected = false;
    for (const Racer& r : racers) {
        const float dist = glm::length(r.pos - m.pos);
        if (dist > R + r.radius) continue;
        Hit h;
        h.targetId = r.id;
        h.pos = m.pos;
        h.dir = m.dir;
        h.direct = (onTarget && r.id == m.target) || dist < r.radius + 1.5f;
        h.damage = h.direct ? 1.0f
                            : std::clamp(1.0f - (dist - r.radius) / R, 0.0f, 1.0f) * 0.7f;
        if (h.damage > 0.02f) { m_hits.push_back(h); connected = true; }
    }
    if (connected) m_hitFlash = 0.6f;
    cue(soundHit, connected ? 1.0f : 0.7f, 0.9f + rnd() * 0.2f);
}

// --- Ribbons ----------------------------------------------------------------

void WeaponSystem::rebuildRibbons(Missile& m, const glm::vec3& camPos) {
    m.hotVerts.clear();
    m.smokeVerts.clear();
    if (m.pts.size() < 2) {
        m.hotMesh.update(m.hotVerts);
        m.smokeMesh.update(m.smokeVerts);
        return;
    }
    constexpr float kHotLife   = 0.26f;
    constexpr float kSmokeLife = 1.5f;
    // The core is a thin, short, white-hot streak; the smoke starts inside it
    // and swells, then pinches shut at the tail so the trail ends instead of
    // being cut off (a single material has one opacity -- the taper is the only
    // fade there is).
    auto wHot = [&](float age) {
        return age >= kHotLife ? 0.0f : 0.30f * (1.0f - age / kHotLife);
    };
    auto wSmoke = [&](float age) {
        const float t = std::clamp(age / kSmokeLife, 0.0f, 1.0f);
        return (0.22f + 1.15f * age) * glm::smoothstep(1.0f, 0.5f, t);
    };

    auto strip = [&](std::vector<fitzel::Vertex>& out,
                     const std::function<float(float)>& half) {
        auto vtx = [&](const glm::vec3& p, const glm::vec3& n) {
            fitzel::Vertex v;
            v.position = p; v.normal = n; v.uv = glm::vec2(0.0f);
            out.push_back(v);
        };
        const std::size_t n = m.pts.size();
        for (std::size_t i = 0; i + 1 < n; ++i) {
            const Pt& a = m.pts[i];
            const Pt& b = m.pts[i + 1];
            const float wa = half(a.age), wb = half(b.age);
            if (wa < 1e-4f && wb < 1e-4f) continue;
            const glm::vec3 seg = b.pos - a.pos;
            if (glm::length(seg) < 1e-5f) continue;
            const glm::vec3 dir = glm::normalize(seg);
            const glm::vec3 sa = sideAt(a.pos, dir, camPos);
            glm::vec3 sb = sideAt(b.pos, dir, camPos);
            if (glm::dot(sa, sb) < 0.0f) sb = -sb;
            const glm::vec3 aL = a.pos - sa * wa, aR = a.pos + sa * wa;
            const glm::vec3 bL = b.pos - sb * wb, bR = b.pos + sb * wb;
            const glm::vec3 nA = glm::normalize(camPos - a.pos + glm::vec3(1e-5f));
            const glm::vec3 nB = glm::normalize(camPos - b.pos + glm::vec3(1e-5f));
            vtx(aL, nA); vtx(aR, nA); vtx(bR, nB);
            vtx(aL, nA); vtx(bR, nB); vtx(bL, nB);
            vtx(aL, nA); vtx(bR, nB); vtx(aR, nA);
            vtx(aL, nA); vtx(bL, nB); vtx(bR, nB);
        }
    };
    strip(m.smokeVerts, wSmoke);
    strip(m.hotVerts, wHot);
    m.hotMesh.update(m.hotVerts);
    m.smokeMesh.update(m.smokeVerts);
}

// --- The frame --------------------------------------------------------------

void WeaponSystem::update(const Frame& f, const std::vector<Racer>& racers) {
    m_hits.clear();
    if (!enabled) {
        if (!m_missiles.empty() || !m_blasts.empty()) { m_missiles.clear(); m_blasts.clear(); }
        m_haveMark = false; m_trackId = -1; m_locked = false; m_lockT = 0.0f;
        return;
    }

    const float dt = std::clamp(f.dt, 0.0f, 0.1f);
    m_time  += dt;
    m_camPos = f.camPos;
    m_armed  = f.armed;

    // Differentiate the field's positions into velocities: the AI racers are
    // kinematic and carry no velocity of their own, and a seeker that cannot
    // pull lead is a seeker that never hits anything moving.
    for (auto& kv : m_tracks) kv.second.seen = false;
    for (const Racer& r : racers) {
        Track& t = m_tracks[r.id];
        if (!t.init || t.seen || dt <= 1e-5f) {
            t.pos = r.pos; t.seen = true; t.init = true;
            continue;
        }
        const glm::vec3 v = (r.pos - t.pos) / dt;
        // Smoothed, and sanity-capped: a craft that was respawned across the map
        // must not hand the seeker a 900 m/s lead vector.
        t.vel = glm::length(v) > 200.0f ? glm::vec3(0.0f) : glm::mix(t.vel, v, 0.35f);
        t.pos = r.pos;
        t.seen = true;
    }
    for (auto it = m_tracks.begin(); it != m_tracks.end(); )
        it = it->second.seen ? std::next(it) : m_tracks.erase(it);

    m_lockFlash = std::max(0.0f, m_lockFlash - dt);
    m_denyFlash = std::max(0.0f, m_denyFlash - dt);
    m_hitFlash  = std::max(0.0f, m_hitFlash  - dt);
    m_fireT     = std::max(0.0f, m_fireT     - dt);
    m_pulse = m_locked ? std::fmod(m_pulse + dt * 1.25f, 1.0f) : 0.0f;

    // Reload: one round at a time, so a rack is a resource rather than a tap.
    if (m_ammo < std::max(1, ammoMax)) {
        m_reload += dt / std::max(reloadTime, 0.2f);
        if (m_reload >= 1.0f) { m_reload = 0.0f; ++m_ammo; }
    } else {
        m_reload = 0.0f;
    }

    if (f.armed) {
        updateLock(f, racers);
    } else {
        m_trackId = -1; m_locked = false; m_lockT = 0.0f; m_haveMark = false;
        m_prevCycle = false;
    }

    // Trigger. Edge-detected in here so main owns no weapon state at all.
    if (f.input && f.armed) {
        bool wants = f.input->isKeyDown(GLFW_KEY_F);
        if (f.input->hasGamepad() && f.input->gamepadButton(GLFW_GAMEPAD_BUTTON_X))
            wants = true;
        if (wants && !m_prevFire) {
            if (!f.mayFire) {
                // On the grid or past the flag: silently ignored, no scolding.
            } else if (!m_locked) {
                m_denyFlash = 0.7f;
                cue(soundDeny, 0.6f, 1.0f);
            } else if (m_ammo <= 0) {
                m_denyFlash = 0.7f;
                cue(soundDeny, 0.6f, 0.75f);
            } else if (m_fireT <= 0.0f) {
                fire(f);
            }
        }
        m_prevFire = wants;
    } else {
        m_prevFire = false;
    }

    // --- Missiles ----------------------------------------------------------
    for (Missile& m : m_missiles) {
        // Sub-stepped: at 135 m/s a single 60 Hz step is over two metres, and a
        // frame hitch would fly the thing clean through the craft it is chasing.
        float left = dt;
        bool  gone = false;
        while (left > 1e-5f && !gone) {
            const float h = std::min(0.02f, left);
            left -= h;
            m.age += h;

            // Where the seeker wants to go: not at the target, but at where it
            // will be when we get there.
            glm::vec3 want = m.dir;
            const Racer* tgt = nullptr;
            for (const Racer& r : racers) if (r.id == m.target) { tgt = &r; break; }
            if (tgt) {
                glm::vec3 tv(0.0f);
                auto it = m_tracks.find(tgt->id);
                if (it != m_tracks.end()) tv = it->second.vel;
                const float dist = glm::length(tgt->pos - m.pos);
                const float tof  = std::clamp(dist / std::max(m.speed, 20.0f), 0.0f, 2.5f);
                const glm::vec3 aim = tgt->pos + tv * tof;
                const glm::vec3 d = aim - m.pos;
                if (glm::length(d) > 1e-4f) want = glm::normalize(d);
            }
            // Blend out of the launch arc into the seeker's answer.
            const float k = glm::smoothstep(0.0f, 1.0f,
                                            std::clamp(m.age / std::max(armTime, 0.01f), 0.0f, 1.0f));
            glm::vec3 desired = glm::mix(m.launchDir, want, k);
            if (glm::length(desired) > 1e-4f) desired = glm::normalize(desired);
            else desired = m.dir;

            // Finite steering authority: the missile can out-turn a racer, but
            // not by so much that dodging is pointless.
            const float maxAng = glm::radians(std::max(missileTurn, 10.0f)) * h;
            const float c = std::clamp(glm::dot(m.dir, desired), -1.0f, 1.0f);
            const float ang = std::acos(c);
            if (ang > 1e-4f) {
                const float t = std::min(1.0f, maxAng / ang);
                const glm::vec3 nd = glm::mix(m.dir, desired, t);
                if (glm::length(nd) > 1e-4f) m.dir = glm::normalize(nd);
            }
            m.speed = std::min(m.speed + missileAccel * h, std::max(missileSpeed, 10.0f));
            m.vel = m.dir * m.speed;
            m.pos += m.vel * h;

            // Proximity fuse -- including the case where it has just flown PAST
            // the target, which a distance test alone misses at these speeds.
            if (tgt) {
                const glm::vec3 to = tgt->pos - m.pos;
                const float dist = glm::length(to);
                const float fuse = tgt->radius + std::max(fuseRadius, 0.5f);
                if (dist < fuse || (dist < fuse * 2.0f && glm::dot(to, m.dir) < 0.0f)) {
                    detonate(m, racers, true);
                    gone = true;
                    break;
                }
            }
            // The ground. A missile that misses should plough in, not sail on
            // through the hillside and out the other side.
            if (groundHeight) {
                const float g = groundHeight(m.pos.x, m.pos.z, m.pos.y);
                if (m.pos.y < g + 0.35f) {
                    m.pos.y = g + 0.35f;
                    detonate(m, racers, false);
                    gone = true;
                    break;
                }
            }
            if (m.age > m.life) {
                detonate(m, racers, false);   // fuel out: it goes off anyway
                gone = true;
                break;
            }
        }
        if (gone) { m.age = m.life + 1.0e3f; continue; }   // marked for the sweep

        // Lay the trail and age it.
        if (m.pts.empty() || glm::length(m.pos - m.pts.back().pos) > 0.9f)
            m.pts.push_back({m.pos, 0.0f});
        for (Pt& p : m.pts) p.age += dt;
        while (!m.pts.empty() && m.pts.front().age > 1.5f) m.pts.pop_front();
        m.flicker += dt * 37.0f;
        rebuildRibbons(m, f.camPos);
    }
    m_missiles.erase(std::remove_if(m_missiles.begin(), m_missiles.end(),
                                    [](const Missile& m) { return m.age > m.life + 1.0f; }),
                     m_missiles.end());

    // --- Blasts ------------------------------------------------------------
    for (Blast& b : m_blasts) {
        b.age += dt;
        for (Blast::Spark& s : b.sparks) {
            s.life -= dt;
            if (s.life <= 0.0f) continue;
            s.vel -= s.vel * std::min(1.0f, 2.6f * dt);   // air drag
            s.vel.y -= 24.0f * dt;
            s.pos += s.vel * dt;
        }
        for (Blast::Puff& p : b.puffs) {
            p.life -= dt;
            if (p.life <= 0.0f) continue;
            p.vel -= p.vel * std::min(1.0f, 1.5f * dt);
            p.vel.y += 1.2f * dt;                          // smoke rises
            p.pos += p.vel * dt;
        }
    }
    m_blasts.erase(std::remove_if(m_blasts.begin(), m_blasts.end(),
                                  [](const Blast& b) { return b.age > b.life; }),
                   m_blasts.end());
}

// --- Render -----------------------------------------------------------------

void WeaponSystem::render(fitzel::Renderer& renderer) {
    if (!enabled) return;
    m_matUsed = 0;   // the pool is per-frame; references stay valid (deque)

    // --- Missiles ----------------------------------------------------------
    for (const Missile& m : m_missiles) {
        if (m.age > m.life) continue;
        const glm::mat4 model = glm::translate(glm::mat4(1.0f), m.pos) * orientTo(m.dir);
        // Body: dark, with just enough emission to keep it from turning into a
        // silhouette against a bright sky.
        renderer.submit(m_dart, mat(glm::vec3(0.10f, 0.11f, 0.13f),
                                    glm::vec3(0.55f, 0.20f, 0.06f), 0.30f),
                        model, /*castsPointShadow=*/false);
        // Plume: an orange flare with a white core, both flickering on their own
        // clock so the motor never looks like a static cone.
        const float fl = 0.80f + 0.30f * std::sin(m.flicker) + 0.12f * std::sin(m.flicker * 2.7f);
        renderer.submit(m_flame, mat(glm::vec3(1.0f, 0.45f, 0.10f),
                                     glm::vec3(1.0f, 0.42f, 0.09f), 6.0f),
                        model * glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, fl)),
                        false, false, 0.70f, /*forceTransparent=*/true);
        renderer.submit(m_flame, mat(glm::vec3(1.0f, 0.92f, 0.75f),
                                     glm::vec3(1.0f, 0.88f, 0.70f), 11.0f),
                        model * glm::scale(glm::mat4(1.0f),
                                           glm::vec3(0.52f, 0.52f, fl * 0.62f)),
                        false, false, 0.9f, true);
        // Smoke first, hot core over it: the core has to sit INSIDE the smoke.
        if (!m.smokeVerts.empty())
            renderer.submit(m.smokeMesh, mat(glm::vec3(0.30f, 0.30f, 0.33f),
                                             glm::vec3(0.0f), 0.0f),
                            glm::mat4(1.0f), false, false, 0.34f, true);
        if (!m.hotVerts.empty())
            renderer.submit(m.hotMesh, mat(glm::vec3(1.0f, 0.72f, 0.35f),
                                           glm::vec3(1.0f, 0.60f, 0.25f), 5.0f),
                            glm::mat4(1.0f), false, false, 0.65f, true);
    }

    // --- Blasts ------------------------------------------------------------
    for (const Blast& b : m_blasts) {
        // Fireball: fast out, then cooling from white through orange to a dark
        // red that fades. Emission falls faster than the size grows, which is
        // what makes it look like it is burning out rather than deflating.
        const float ft = std::clamp(b.age / 0.45f, 0.0f, 1.0f);
        if (ft < 1.0f) {
            const float R = std::max(blastRadius, 1.0f) * (0.22f + 0.80f * easeOut(ft));
            const glm::vec3 col = glm::mix(glm::vec3(1.0f, 0.95f, 0.75f),
                                           glm::vec3(0.75f, 0.14f, 0.03f), ft);
            renderer.submit(m_sphere, mat(col, col, 9.0f * (1.0f - ft) + 0.5f),
                            glm::translate(glm::mat4(1.0f), b.pos) *
                            glm::scale(glm::mat4(1.0f), glm::vec3(R)),
                            false, false, 0.92f * (1.0f - ft * ft), true);
        }
        // The flash: a very short, very bright core, the thing the eye actually
        // registers as "that went off".
        const float xt = std::clamp(b.age / 0.13f, 0.0f, 1.0f);
        if (xt < 1.0f)
            renderer.submit(m_sphere, mat(glm::vec3(1.0f), glm::vec3(1.0f), 22.0f),
                            glm::translate(glm::mat4(1.0f), b.pos) *
                            glm::scale(glm::mat4(1.0f),
                                       glm::vec3(std::max(blastRadius, 1.0f) * (0.15f + 0.55f * xt))),
                            false, false, 1.0f - xt, true);
        // Two shockwaves: one across the missile's line of travel, one flat.
        // Two, because a single ring reads as a decal and a pair reads as a
        // pressure wave.
        const float st = std::clamp(b.age / 0.60f, 0.0f, 1.0f);
        if (st < 1.0f) {
            const float R = std::max(blastRadius, 1.0f) * (0.3f + 2.0f * easeOut(st));
            const glm::vec3 col(0.85f, 0.93f, 1.0f);
            renderer.submit(m_ring, mat(col, col, 7.0f * (1.0f - st)),
                            glm::translate(glm::mat4(1.0f), b.pos) * orientTo(b.dir) *
                            glm::scale(glm::mat4(1.0f), glm::vec3(R, R, 1.0f)),
                            false, false, 0.75f * (1.0f - st), true);
            renderer.submit(m_ring, mat(col, col, 5.0f * (1.0f - st)),
                            glm::translate(glm::mat4(1.0f), b.pos) *
                            orientTo(glm::vec3(0.0f, 1.0f, 0.0f)) *
                            glm::scale(glm::mat4(1.0f), glm::vec3(R * 0.75f, R * 0.75f, 1.0f)),
                            false, false, 0.5f * (1.0f - st), true);
        }
        // Sparks: stretched along their own velocity, so they read as streaks.
        for (const Blast::Spark& s : b.sparks) {
            if (s.life <= 0.0f) continue;
            const float a = std::clamp(s.life / std::max(s.life0, 0.01f), 0.0f, 1.0f);
            const float len = std::clamp(glm::length(s.vel) * 0.035f, 0.2f, 1.6f);
            const glm::vec3 col = glm::mix(glm::vec3(1.0f, 0.25f, 0.05f),
                                           glm::vec3(1.0f, 0.95f, 0.65f), a);
            // Oriented along its own velocity, so the spike trails behind it.
            renderer.submit(m_shard, mat(col, col, 8.0f * a),
                            glm::translate(glm::mat4(1.0f), s.pos) * orientTo(s.vel) *
                            glm::scale(glm::mat4(1.0f),
                                       glm::vec3(0.13f * a + 0.03f, 0.13f * a + 0.03f, len)),
                            false, false, a, true);
        }
        // Smoke: it outlives the fire, which is what leaves a hit hanging in the
        // air behind the pack for a second.
        for (const Blast::Puff& p : b.puffs) {
            if (p.life <= 0.0f) continue;
            const float a = std::clamp(p.life / std::max(p.life0, 0.01f), 0.0f, 1.0f);
            const float R = p.r0 * (1.6f - 1.1f * a);
            const glm::vec3 col = glm::mix(glm::vec3(0.14f, 0.13f, 0.13f),
                                           glm::vec3(0.55f, 0.36f, 0.24f), a);
            renderer.submit(m_sphere, mat(col, col * 0.5f, 0.6f * a),
                            glm::translate(glm::mat4(1.0f), p.pos) *
                            glm::scale(glm::mat4(1.0f), glm::vec3(R)),
                            false, false, 0.42f * a, true);
        }
    }

    // --- The marked craft ---------------------------------------------------
    // Three cues on one target: a cage that closes in, gyros that spin around it
    // in the world, and (in drawHud) a screen reticle. Redundant on purpose --
    // this is the one thing on screen the pilot must never have to hunt for.
    if (m_armed && m_haveMark) {
        const float R   = m_markRadius;
        const float acq = std::clamp(m_lockT, 0.0f, 1.0f);
        const glm::vec3 col = glm::mix(glm::vec3(1.0f, 0.72f, 0.20f),
                                       glm::vec3(1.0f, 0.20f, 0.14f), acq);
        const glm::vec3 toCam = m_camPos - m_markPos;
        const glm::mat4 bill = glm::translate(glm::mat4(1.0f), m_markPos) *
                               orientTo(glm::length(toCam) > 1e-4f ? toCam
                                                                   : glm::vec3(0, 0, 1));
        // The cage: wide and turning while it hunts, snapping square on lock.
        const float s   = R * (2.7f - 1.45f * acq);
        const float rot = (1.0f - acq) * (1.6f + m_time * 1.1f);
        renderer.submit(m_bracket, mat(col, col, 4.0f + 4.0f * acq),
                        bill * glm::rotate(glm::mat4(1.0f), rot, glm::vec3(0, 0, 1)) *
                        glm::scale(glm::mat4(1.0f), glm::vec3(s, s, 1.0f)),
                        false, false, 0.55f + 0.40f * acq, true);
        if (m_locked) {
            // A second, tight, dead-steady cage: the lock itself.
            const float pulse = 0.82f + 0.18f * std::sin(m_time * 9.0f);
            renderer.submit(m_bracket, mat(glm::vec3(1.0f, 0.25f, 0.18f),
                                           glm::vec3(1.0f, 0.20f, 0.12f), 9.0f * pulse),
                            bill * glm::scale(glm::mat4(1.0f),
                                              glm::vec3(R * 1.25f, R * 1.25f, 1.0f)),
                            false, false, 0.95f, true);
            // ...and a ring leaving it, once a beat, so the mark reads from a
            // hundred metres back in a pack of six.
            const float pr = R * (1.0f + 2.4f * m_pulse);
            renderer.submit(m_ring, mat(glm::vec3(1.0f, 0.3f, 0.2f),
                                        glm::vec3(1.0f, 0.25f, 0.15f), 6.0f),
                            bill * glm::scale(glm::mat4(1.0f), glm::vec3(pr, pr, 1.0f)),
                            false, false, 0.55f * (1.0f - m_pulse), true);
        }
        // Two gyros around the craft in world space, counter-rotating: the cue
        // that survives the camera swinging behind the target, where a purely
        // billboarded marker collapses to a line.
        const glm::mat4 at = glm::translate(glm::mat4(1.0f), m_markPos);
        const float gs = R * 1.55f;
        renderer.submit(m_gyro, mat(col, col, 3.5f + 3.0f * acq),
                        at * glm::rotate(glm::mat4(1.0f), m_time * 1.3f, glm::vec3(0, 1, 0)) *
                        glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1, 0, 0)) *
                        glm::scale(glm::mat4(1.0f), glm::vec3(gs, gs, 1.0f)),
                        false, false, 0.5f + 0.4f * acq, true);
        renderer.submit(m_gyro, mat(col, col, 3.0f + 3.0f * acq),
                        at * glm::rotate(glm::mat4(1.0f), -m_time * 0.9f, glm::vec3(0, 1, 0)) *
                        glm::rotate(glm::mat4(1.0f), glm::radians(24.0f), glm::vec3(1, 0, 0)) *
                        glm::scale(glm::mat4(1.0f), glm::vec3(gs * 1.18f, gs * 1.18f, 1.0f)),
                        false, false, 0.35f + 0.35f * acq, true);
    }
}

void WeaponSystem::collectLights(std::vector<fitzel::PointLight>& out) const {
    if (!enabled) return;
    // A missile lights the road it flies down and a blast lights the whole
    // corner. Both are short-lived, which is exactly what the light budget can
    // afford -- and without them the effects float in front of the scene rather
    // than happening in it.
    for (const Missile& m : m_missiles) {
        if (m.age > m.life) continue;
        if (static_cast<int>(out.size()) >= fitzel::Renderer::kMaxPointLights) return;
        fitzel::PointLight pl;
        pl.position = m.pos - m.dir * 1.2f;
        pl.color    = glm::vec3(2.4f, 1.05f, 0.35f);
        pl.range    = 22.0f;
        out.push_back(pl);
    }
    for (const Blast& b : m_blasts) {
        const float t = std::clamp(b.age / 0.5f, 0.0f, 1.0f);
        if (t >= 1.0f) continue;
        if (static_cast<int>(out.size()) >= fitzel::Renderer::kMaxPointLights) return;
        fitzel::PointLight pl;
        pl.position = b.pos;
        pl.color    = glm::mix(glm::vec3(9.0f, 6.5f, 3.4f), glm::vec3(2.2f, 0.4f, 0.1f), t) *
                      (1.0f - t);
        pl.range    = std::max(blastRadius, 1.0f) * 5.0f;
        out.push_back(pl);
    }
}

// --- HUD --------------------------------------------------------------------

namespace {

// The HUD's pixel context, mirroring RaceHud's so the two match at any viewport
// size (S = 1 at ~900 px of view height).
struct Hud {
    ImDrawList* dl = nullptr;
    ImFont*     font = nullptr;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float S = 1.0f;

    float textW(float size, const char* s) const {
        return font->CalcTextSizeA(size, FLT_MAX, 0.0f, s).x;
    }
    void text(float x, float y, float size, ImU32 col, const char* s) const {
        const float o = std::max(1.0f, size * 0.06f);
        dl->AddText(font, size, ImVec2(x + o, y + o),
                    fade(IM_COL32(0, 0, 0, 205), ((col >> 24) & 0xFF) / 255.0f), s);
        dl->AddText(font, size, ImVec2(x, y), col, s);
    }
    void textC(float xMid, float y, float size, ImU32 col, const char* s) const {
        text(xMid - textW(size, s) * 0.5f, y, size, col, s);
    }
    void textR(float xR, float y, float size, ImU32 col, const char* s) const {
        text(xR - textW(size, s), y, size, col, s);
    }
    void panel(float ax, float ay, float bx, float by, ImU32 accent, float alpha) const {
        const float r = 5.0f * S;
        dl->AddRectFilled(ImVec2(ax, ay), ImVec2(bx, by), fade(kInk, alpha), r);
        dl->AddRect(ImVec2(ax, ay), ImVec2(bx, by), fade(kEdge, alpha), r, 0, 1.0f);
        if (accent)
            dl->AddRectFilled(ImVec2(ax, ay), ImVec2(ax + 3.0f * S, by),
                              fade(accent, alpha), r, ImDrawFlags_RoundCornersLeft);
    }
};

// World -> viewport pixels. Returns false behind the camera (where the
// projection turns inside out and a naive divide puts the marker on the wrong
// side of the screen).
bool project(const glm::mat4& vp, const glm::vec3& w, const ImVec2& vmin,
             const ImVec2& vsize, ImVec2& out) {
    const glm::vec4 c = vp * glm::vec4(w, 1.0f);
    if (c.w <= 0.001f) return false;
    out = ImVec2(vmin.x + (c.x / c.w * 0.5f + 0.5f) * vsize.x,
                 vmin.y + (0.5f - c.y / c.w * 0.5f) * vsize.y);
    return true;
}

// Four corner brackets around (cx, cy), arms `arm` long, turned by `rot`.
void bracketAt(const Hud& g, float cx, float cy, float r, float arm, float rot,
               ImU32 col, float thick) {
    const float c = std::cos(rot), s = std::sin(rot);
    auto P = [&](float x, float y) {
        return ImVec2(cx + x * c - y * s, cy + x * s + y * c);
    };
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2) {
            const float px = static_cast<float>(sx) * r, py = static_cast<float>(sy) * r;
            g.dl->AddLine(P(px, py), P(px - sx * arm, py), col, thick);
            g.dl->AddLine(P(px, py), P(px, py - sy * arm), col, thick);
        }
}

} // namespace

void WeaponSystem::drawHud(ImDrawList* dl, const ImVec2& vmin, const ImVec2& vsize,
                           const glm::mat4& viewProj) {
    if (!dl || !enabled || !m_armed) return;

    Hud g;
    g.dl = dl;
    g.font = ImGui::GetFont();
    g.x0 = vmin.x; g.y0 = vmin.y;
    g.x1 = vmin.x + vsize.x; g.y1 = vmin.y + vsize.y;
    g.S  = std::clamp(vsize.y / 900.0f, 0.55f, 2.2f);
    const float fSmall = 14.0f * g.S, fBody = 18.0f * g.S, fBig = 26.0f * g.S;
    const float pad = 20.0f * g.S;
    const float acq = std::clamp(m_lockT, 0.0f, 1.0f);
    const ImU32 seek = mixCol(kAmber, kRed, acq);

    // --- The reticle on the marked craft ------------------------------------
    if (m_haveMark) {
        ImVec2 p;
        const bool front = project(viewProj, m_markPos, vmin, vsize, p);
        // Screen radius from the craft's own size, so the reticle shrinks with
        // distance like the thing it is drawn around.
        float r = 26.0f * g.S;
        if (front) {
            ImVec2 pu;
            if (project(viewProj, m_markPos + glm::vec3(0.0f, m_markRadius, 0.0f),
                        vmin, vsize, pu))
                r = std::clamp(std::abs(pu.y - p.y) * 1.35f, 18.0f * g.S, 190.0f * g.S);
        }
        const bool onScreen = front && p.x > g.x0 - r && p.x < g.x1 + r &&
                                       p.y > g.y0 - r && p.y < g.y1 + r;

        if (onScreen) {
            // Hunting: a wide, slowly turning cage that closes in as the lock
            // builds. Locked: it snaps square and stops.
            const float R   = r * (2.3f - 1.15f * acq);
            const float rot = (1.0f - acq) * (0.5f + static_cast<float>(ImGui::GetTime()) * 0.9f);
            bracketAt(g, p.x, p.y, R, R * 0.42f, rot, fade(seek, 0.55f + 0.45f * acq),
                      2.0f * g.S);

            // Progress ring: how much of the lock is built, as one arc. It is
            // the only thing on the HUD that tells you whether holding the line
            // for another half second is worth it.
            if (!m_locked && acq > 0.01f) {
                dl->PathClear();
                dl->PathArcTo(p, R * 1.30f, -kPi * 0.5f, -kPi * 0.5f + 2.0f * kPi * acq, 48);
                dl->PathStroke(fade(kAmber, 0.9f), 0, 2.6f * g.S);
                dl->AddCircle(p, R * 1.30f, fade(kEdge, 0.8f), 48, 1.0f);
            }

            if (m_locked) {
                // Locked: solid brackets, a centre diamond and a flash the
                // moment it snapped shut.
                const float pulse = 0.75f + 0.25f * std::sin(static_cast<float>(ImGui::GetTime()) * 9.0f);
                bracketAt(g, p.x, p.y, r * 1.15f, r * 0.55f, 0.0f,
                          fade(kRed, pulse), 3.0f * g.S);
                const float d = r * 0.26f;
                dl->AddQuad(ImVec2(p.x, p.y - d), ImVec2(p.x + d, p.y),
                            ImVec2(p.x, p.y + d), ImVec2(p.x - d, p.y),
                            fade(kRed, pulse), 2.0f * g.S);
                if (m_lockFlash > 0.0f) {
                    const float t = std::clamp(m_lockFlash / 0.55f, 0.0f, 1.0f);
                    dl->AddCircle(p, r * (1.2f + 2.2f * (1.0f - t)), fade(kRed, t),
                                  48, 3.0f * g.S);
                }
            }

            // The readout under the reticle: who, how far, and what the seeker
            // is doing. Never more than three short lines -- this is read at
            // 500 km/h, in peripheral vision.
            char line[96];
            const float ty = p.y + R * 1.45f;
            g.textC(p.x, ty, fBody, m_locked ? kRed : kAmber,
                    m_locked ? "LOCK" : "ACQUIRING");
            std::snprintf(line, sizeof(line), "%s   %.0f m", m_markName.c_str(),
                          m_markDist);
            g.textC(p.x, ty + fBody * 1.15f, fSmall, kText, line);
            if (m_locked && m_ammo <= 0)
                g.textC(p.x, ty + fBody * 1.15f + fSmall * 1.3f, fSmall, kDim, "NO MISSILES");
        } else {
            // Off screen: an arrow at the edge, pointing the way to turn. A
            // lock you cannot see is a lock you will forget you have.
            const float cx = (g.x0 + g.x1) * 0.5f, cy = (g.y0 + g.y1) * 0.5f;
            glm::vec2 d(p.x - cx, p.y - cy);
            if (!front) d = -d;                       // behind: the far side
            if (glm::length(d) < 1e-3f) d = glm::vec2(0.0f, 1.0f);
            d = glm::normalize(d);
            const float mx = (g.x1 - g.x0) * 0.5f - 54.0f * g.S;
            const float my = (g.y1 - g.y0) * 0.5f - 54.0f * g.S;
            const float k = std::min(std::abs(d.x) > 1e-4f ? mx / std::abs(d.x) : 1e9f,
                                     std::abs(d.y) > 1e-4f ? my / std::abs(d.y) : 1e9f);
            const ImVec2 e(cx + d.x * k, cy + d.y * k);
            const float a = std::atan2(d.y, d.x);
            const float s = 15.0f * g.S;
            const ImVec2 t1(e.x + std::cos(a) * s, e.y + std::sin(a) * s);
            const ImVec2 t2(e.x + std::cos(a + 2.5f) * s, e.y + std::sin(a + 2.5f) * s);
            const ImVec2 t3(e.x + std::cos(a - 2.5f) * s, e.y + std::sin(a - 2.5f) * s);
            dl->AddTriangleFilled(t1, t2, t3, fade(seek, 0.9f));
            char line[64];
            std::snprintf(line, sizeof(line), "%s  %.0fm", m_markName.c_str(), m_markDist);
            g.textC(e.x, e.y + s * 1.1f, fSmall, seek, line);
        }
    }

    // --- Launcher panel, bottom right ---------------------------------------
    // The bottom-left is the speed block and the bottom-centre is the hull bar,
    // so the rack lives on the right, where nothing else is competing for the
    // corner of the eye.
    {
        const float w = 214.0f * g.S, h = 74.0f * g.S;
        const float ax = g.x1 - pad - w;
        float ay = g.y1 - pad - h;
        // The race HUD's hull bar wants the bottom centre, but in a narrow
        // (docked-editor) viewport it is clamped rightward and ends up under
        // this panel. Same arithmetic as RaceHud's, so the two agree about when
        // that happens; when it does, the rack stacks on top of it instead of
        // over it. Duplicated deliberately -- one number shared between two
        // files would be worse than one line of overlap test.
        const float eW = 360.0f * g.S, eH = 76.0f * g.S;
        const float eLeft = g.x0 + pad + 262.0f * g.S;
        const float eAx = std::min(std::max((g.x0 + g.x1) * 0.5f - eW * 0.5f, eLeft),
                                   g.x1 - pad - eW);
        if (eAx + eW > ax - 8.0f * g.S) ay -= eH + 8.0f * g.S;
        const ImU32 accent = m_denyFlash > 0.0f ? kRed
                           : m_locked          ? kRed
                                               : (m_ammo > 0 ? kAmber : kDim);
        g.panel(ax, ay, ax + w, ay + h, accent, 1.0f);
        if (m_denyFlash > 0.0f)
            dl->AddRectFilled(ImVec2(ax, ay), ImVec2(ax + w, ay + h),
                              fade(IM_COL32(255, 70, 60, 95),
                                   std::clamp(m_denyFlash / 0.7f, 0.0f, 1.0f)),
                              5.0f * g.S);
        if (m_hitFlash > 0.0f)
            dl->AddRectFilled(ImVec2(ax, ay), ImVec2(ax + w, ay + h),
                              fade(IM_COL32(255, 190, 80, 90),
                                   std::clamp(m_hitFlash / 0.6f, 0.0f, 1.0f)),
                              5.0f * g.S);

        const float ix = ax + 14.0f * g.S;
        g.text(ix, ay + 8.0f * g.S, fSmall, kDim, "MISSILES");
        char num[8]; std::snprintf(num, sizeof(num), "%d", m_ammo);
        g.text(ix, ay + 20.0f * g.S, fBig, m_ammo > 0 ? kText : kDim, num);

        // The rack itself: one dart per round, so "two left" is a shape, not a
        // number to read.
        const int cap = std::clamp(ammoMax, 1, 12);
        const float px0 = ix + 34.0f * g.S, px1 = ax + w - 14.0f * g.S;
        const float gap = 4.0f * g.S;
        const float pw = std::min(16.0f * g.S, ((px1 - px0) - gap * (cap - 1)) / cap);
        const float ph = 20.0f * g.S, py = ay + 24.0f * g.S;
        for (int i = 0; i < cap; ++i) {
            const float x = px0 + i * (pw + gap);
            const bool  have = i < m_ammo;
            const ImU32 c = have ? (m_locked ? kRed : kAmber) : IM_COL32(255, 255, 255, 38);
            // A dart: a body with a pointed nose.
            dl->AddRectFilled(ImVec2(x, py + ph * 0.35f), ImVec2(x + pw, py + ph),
                              c, 1.5f * g.S);
            dl->AddTriangleFilled(ImVec2(x, py + ph * 0.35f),
                                  ImVec2(x + pw, py + ph * 0.35f),
                                  ImVec2(x + pw * 0.5f, py), c);
        }

        // Reload bar: what the next round is waiting on.
        const float by = ay + h - 12.0f * g.S;
        dl->AddRectFilled(ImVec2(ix, by), ImVec2(ax + w - 14.0f * g.S, by + 5.0f * g.S),
                          IM_COL32(255, 255, 255, 30), 2.0f * g.S);
        if (m_ammo < ammoMax && m_reload > 0.001f)
            dl->AddRectFilled(ImVec2(ix, by),
                              ImVec2(ix + (ax + w - 14.0f * g.S - ix) * m_reload,
                                     by + 5.0f * g.S),
                              fade(kAmber, 0.85f), 2.0f * g.S);

        // One line of state, so the weapon is never silently doing nothing.
        const char* status = m_denyFlash > 0.0f ? (m_ammo <= 0 ? "RACK EMPTY" : "NO LOCK")
                           : m_locked           ? "LOCKED  -  F / X TO FIRE"
                           : m_haveMark         ? "TRACKING"
                                                : "NO TARGET";
        g.textR(ax + w - 14.0f * g.S, ay + 8.0f * g.S, fSmall,
                m_denyFlash > 0.0f ? kRed : (m_locked ? kRed : kDim), status);
    }

    // A hit registered: a short confirmation in the middle of the view. Firing
    // into a pack and never learning whether it connected is the one thing that
    // would make the weapon feel fake.
    if (m_hitFlash > 0.0f) {
        const float t = std::clamp(m_hitFlash / 0.6f, 0.0f, 1.0f);
        g.textC((g.x0 + g.x1) * 0.5f, g.y0 + (g.y1 - g.y0) * 0.34f, fBig,
                fade(kWhite, t), "HIT");
    }
}

// --- Editor -----------------------------------------------------------------

void WeaponSystem::settingsPanel(const SoundPicker& soundPicker) {
    ImGui::Checkbox("Enable missiles", &enabled);
    ImGui::TextWrapped(
        "F (pad X) fires at the locked craft, T (pad Y) steps to another one in "
        "the cone. A lock takes %.2f s of tracking and survives %.2f s off-cone.",
        lockTime, lockHold);
    ImGui::BeginDisabled(!enabled);
    if (ImGui::TreeNodeEx("Seeker", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Cone", &lockAngle, 5.0f, 60.0f, "%.0f deg");
        ImGui::SliderFloat("Range", &lockRange, 60.0f, 800.0f, "%.0f m");
        ImGui::SliderFloat("Lock time", &lockTime, 0.1f, 3.0f, "%.2f s");
        ImGui::SliderFloat("Lock hold", &lockHold, 0.0f, 4.0f, "%.2f s");
        ImGui::TreePop();
    }
    if (ImGui::TreeNodeEx("Launcher", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderInt("Rack size", &ammoMax, 1, 12);
        ImGui::SliderFloat("Reload", &reloadTime, 0.5f, 20.0f, "%.1f s");
        ImGui::SliderFloat("Fire delay", &fireDelay, 0.05f, 2.0f, "%.2f s");
        ImGui::TreePop();
    }
    if (ImGui::TreeNodeEx("Missile")) {
        ImGui::SliderFloat("Speed", &missileSpeed, 40.0f, 300.0f, "%.0f m/s");
        ImGui::SliderFloat("Acceleration", &missileAccel, 20.0f, 400.0f, "%.0f m/s2");
        ImGui::SliderFloat("Turn rate", &missileTurn, 20.0f, 400.0f, "%.0f deg/s");
        ImGui::SliderFloat("Fuel", &missileLife, 1.0f, 20.0f, "%.1f s");
        ImGui::SliderFloat("Launch arc", &armTime, 0.0f, 1.0f, "%.2f s");
        ImGui::SliderFloat("Fuse", &fuseRadius, 0.5f, 12.0f, "%.1f m");
        ImGui::SliderFloat("Blast", &blastRadius, 1.0f, 30.0f, "%.1f m");
        ImGui::TreePop();
    }
    if (ImGui::TreeNodeEx("Sounds")) {
        ImGui::SliderFloat("Weapon volume", &soundGain, 0.0f, 2.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Scales every missile SFX. 0 = the whole weapon\n"
                              "goes silent without clearing the fields.");
        // Each row is a picker plus an audition button: choosing a launch sound
        // by reading its filename and then flying a lap to hear it is how you
        // end up with five sounds nobody likes.
        auto row = [&](const char* label, const char* btn, std::string& field,
                       float gain, float pitch) {
            if (soundPicker) soundPicker(label, field);
            else {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "%s", field.c_str());
                if (ImGui::InputText(label, buf, sizeof(buf))) field = buf;
            }
            ImGui::BeginDisabled(field.empty() || !playCue);
            if (ImGui::Button(btn)) cue(field, gain, pitch);
            ImGui::EndDisabled();
        };
        // Auditioned at the gain the game actually plays them at, or the preview
        // would be lying about the one thing it exists to answer.
        row("Seek dit",   "Preview##seek",   soundSeek,   0.30f, 1.0f);
        row("Lock dit-dit", "Preview##lock", soundLock,   0.45f, 1.0f);
        row("Launch",     "Preview##fire",   soundFire,   1.0f,  1.0f);
        row("Detonation", "Preview##hit",    soundHit,    1.0f,  1.0f);
        row("No lock",    "Preview##deny",   soundDeny,   0.6f,  1.0f);
        ImGui::TextDisabled("Blank = silent. The defaults are generated by\n"
                            "content/sounds/gen_weapon_sounds.py.");
        ImGui::TreePop();
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("In the air: %d   blasts: %d   rack: %d/%d",
                        static_cast<int>(m_missiles.size()),
                        static_cast<int>(m_blasts.size()), m_ammo, ammoMax);
}
