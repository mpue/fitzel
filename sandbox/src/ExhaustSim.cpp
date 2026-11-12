#include "ExhaustSim.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace exhaust {

namespace {

constexpr float kPi = 3.14159265f;

// Quadratic air drag on a puff, scaled by 1/radius: a small fast puff out of a
// trench stops within tens of metres, a cloud the size of a house drifts. With
// the growth below this is what makes a trench cloud travel ~100 m in the first
// few seconds and then mostly swell and rise -- the shape every launch video has.
constexpr float kDrag = 0.11f;
// How long the glow of fresh exhaust lasts, and how long a hot cloud keeps
// rising: the orange is gone in a second, the buoyancy builds towers for half a
// minute.
constexpr float kHeatTau  = 1.4f;
constexpr float kBuoyTau  = 14.0f;
constexpr float kSwellTau = 8.0f;

float smooth01(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// A unit vector at right angles to `d`, and a second one at right angles to both.
void basis(const glm::vec3& d, glm::vec3& u, glm::vec3& v) {
    const glm::vec3 ref = std::abs(d.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    u = glm::normalize(glm::cross(d, ref));
    v = glm::cross(d, u);
}

} // namespace

float Puff::fade() const {
    const float in  = std::min(1.0f, age / 0.3f);
    const float out = std::clamp((life - age) / std::max(life * 0.3f, 0.01f), 0.0f, 1.0f);
    return in * in * (3.0f - 2.0f * in) * out * out * (3.0f - 2.0f * out);
}

void Sim::clear() {
    m_puffs.clear();
    m_owed.clear();
    m_lastTip.clear();
    m_haveTip.clear();
    m_rng   = 0x9E3779B9u;
    m_seeds = 1;
    m_time  = 0.0f;
}

float Sim::rnd() {
    m_rng ^= m_rng << 13;
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    return static_cast<float>(m_rng >> 8) * (1.0f / 16777216.0f);
}

glm::vec3 Sim::rndUnit() {
    const float z = rnd() * 2.0f - 1.0f;
    const float a = rnd() * 2.0f * kPi;
    const float s = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return {s * std::cos(a), z, s * std::sin(a)};
}

glm::vec2 Sim::rndDisc() {
    const float a = rnd() * 2.0f * kPi;
    const float r = std::sqrt(rnd());
    return {r * std::cos(a), r * std::sin(a)};
}

float Sim::groundAt(const World& w, float x, float z) const {
    return w.groundAt ? w.groundAt(x, z) : 0.0f;
}

float Sim::floorAt(const World& w, const std::vector<Trench>& trenches, float x,
                   float z, int skip) const {
    float f = groundAt(w, x, z);
    for (int i = 0; i < static_cast<int>(trenches.size()); ++i) {
        if (i == skip) continue;
        const Trench& t = trenches[i];
        const glm::mat4 inv = glm::inverse(t.model);
        const glm::vec3 top = glm::vec3(t.model * glm::vec4(0.0f, 0.5f, 0.0f, 1.0f));
        const glm::vec3 l = glm::vec3(inv * glm::vec4(x, top.y, z, 1.0f));
        if (std::abs(l.x) <= 0.5f && std::abs(l.z) <= 0.5f) f = std::max(f, top.y);
    }
    return f;
}

FlameShape Sim::flameOf(const Engine& e, const std::vector<Trench>& trenches,
                        const World& world) {
    FlameShape f;
    f.a   = e.nozzle;
    f.dir = glm::length(e.dir) > 1e-6f ? glm::normalize(e.dir) : glm::vec3(0, -1, 0);
    const float th = std::clamp(e.thrust, 0.0f, 1.0f);
    // The flame grows with the throttle: at ignition a stub, at full thrust its
    // whole length. Never quite nothing, so a spool-up reads as a flame taking
    // hold rather than a cylinder being scaled from a point.
    const float L      = e.length * (0.2f + 0.8f * th);
    const float reachT = L * std::max(e.reach, 1.0f);
    const float tanS   = std::tan(std::clamp(e.spread, 0.0f, 1.2f));
    f.r0 = e.radius * (0.8f + 0.2f * th);

    float best = reachT;
    // Decks first: a box top facing the jet.
    for (int i = 0; i < static_cast<int>(trenches.size()); ++i) {
        const Trench& t = trenches[i];
        const glm::mat4 inv = glm::inverse(t.model);
        const glm::vec3 ro = glm::vec3(inv * glm::vec4(f.a, 1.0f));
        const glm::vec3 rd = glm::mat3(inv) * f.dir;   // same t as in the world
        // A nozzle sunk into the deck (the boosters' reach into its exhaust
        // holes) is already in the hole: straight down the trench.
        if (std::abs(ro.x) <= 0.5f && std::abs(ro.z) <= 0.5f && std::abs(ro.y) <= 0.5f) {
            const glm::vec3 top = glm::vec3(t.model * glm::vec4(0.0f, 0.5f, 0.0f, 1.0f));
            const glm::vec2 off(f.a.x - top.x, f.a.z - top.z);
            if (glm::length(off) < t.hole) {
                best      = 0.0f;
                f.trench  = i;
                f.hitKind = 3;
                continue;
            }
        }
        if (ro.y <= 0.5f - 1e-4f || rd.y >= -1e-6f) continue;
        const float tt = (0.5f - ro.y) / rd.y;
        const glm::vec3 h = ro + rd * tt;
        if (tt <= 0.0f || tt >= best || std::abs(h.x) > 0.5f || std::abs(h.z) > 0.5f) continue;
        best      = tt;
        f.trench  = i;
        const glm::vec3 hitW = f.a + f.dir * tt;
        const glm::vec3 top  = glm::vec3(t.model * glm::vec4(0.0f, 0.5f, 0.0f, 1.0f));
        f.hitKind = glm::length(hitW - top) < t.hole ? 3 : 2;
    }
    // The ground, by two refinements of where the line meets it -- the pad is
    // flat, and a hill under a rocket's exhaust only needs to be roughly right.
    if (f.dir.y < -1e-3f) {
        const auto g = [&](const glm::vec3& p) {
            return world.groundAt ? world.groundAt(p.x, p.z) : 0.0f;
        };
        float tg = (f.a.y - g(f.a)) / -f.dir.y;
        for (int k = 0; k < 2 && tg > 0.0f; ++k) tg = (f.a.y - g(f.a + f.dir * tg)) / -f.dir.y;
        if (tg > 0.0f && tg < best) {
            best      = tg;
            f.hitKind = 1;
            f.trench  = -1;
        }
    }
    if (f.hitKind != 0) f.hitT = best;

    f.len = L;
    if (f.hitKind == 1 || f.hitKind == 2) f.len = std::min(L, best);
    // Through the hole the flame goes on down into the trench, where the deck
    // hides it; a few metres past the deck is all that could ever be seen.
    if (f.hitKind == 3) f.len = std::min(L, best + trenches[f.trench].drop);
    f.r1 = f.r0 + f.len * tanS;
    return f;
}

void Sim::add(const Puff& p) {
    m_puffs.push_back(p);
    Puff& q = m_puffs.back();
    // A seed per puff, hashed so neighbours are unrelated: it picks the patch of
    // detail noise the renderer gives this puff.
    std::uint32_t h = m_seeds++ * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    q.seed = h;
}

void Sim::emitEngine(float dt, const Engine& e, const std::vector<Trench>& trenches,
                     const World& world) {
    const float th = std::clamp(e.thrust, 0.0f, 1.0f);
    if (th <= 0.01f) return;
    const FlameShape f = flameOf(e, trenches, world);
    const float L      = e.length * (0.2f + 0.8f * th);
    const float reachT = L * std::max(e.reach, 1.0f);
    const float tanS   = std::tan(std::clamp(e.spread, 0.0f, 1.2f));
    const float scale  = std::max(e.radius, 0.3f);

    // How hard the jet still strikes what it strikes: fully within the flame,
    // easing out to nothing at its reach. The rest of the exhaust is left in the
    // air as the trail.
    float g = 0.0f;
    if (f.hitKind != 0) g = 1.0f - smooth01(L * 0.7f, reachT, f.hitT);
    const float rHit = f.r0 + std::max(f.hitT, 0.0f) * tanS;

    // --- What the jet throws off what it hits ------------------------------
    float& owedG = m_owed[e.id * 4 + 0];
    owedG += 60.0f * th * e.smoke * scale * g * dt;
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    while (owedG >= 1.0f) {
        owedG -= 1.0f;
        // A plume wider than the hole spills the rest of itself over the deck.
        const bool throughHole =
            f.hitKind == 3 &&
            rnd() < std::min(1.0f, (trenches[f.trench].hole / std::max(rHit, 0.1f)) *
                                       (trenches[f.trench].hole / std::max(rHit, 0.1f)));
        if (throughHole) {
            const Trench& t = trenches[f.trench];
            const glm::vec3 top = glm::vec3(t.model * glm::vec4(0.0f, 0.5f, 0.0f, 1.0f));
            const glm::vec3 ax  = glm::normalize(glm::mat3(t.model) * glm::vec3(1, 0, 0));
            const glm::vec3 tup = glm::normalize(glm::mat3(t.model) * glm::vec3(0, 1, 0));
            const glm::vec3 az  = glm::normalize(glm::cross(ax, tup));
            const float side = (!t.twoSided || rnd() < 0.5f) ? 1.0f : -1.0f;
            const glm::vec3 exitP = top + ax * (side * t.reach) - tup * t.drop;
            // The exhaust and, from the deluge, steam: the same jet, one glowing
            // and one white. `steam` is how many steam puffs ride with each one.
            const int n = 1 + static_cast<int>(t.steam * 0.7f + rnd());
            for (int k = 0; k < n; ++k) {
                const bool hot = k == 0;
                Puff p;
                p.pos = exitP + az * ((rnd() - 0.5f) * t.hole * 1.6f) + tup * (rnd() * 4.0f);
                p.vel = ax * (side * t.exitSpeed * (0.65f + 0.5f * rnd()) * (hot ? 1.0f : 0.85f)) +
                        tup * (1.0f + 8.0f * rnd()) + az * ((rnd() - 0.5f) * 22.0f);
                p.r     = (2.0f + 2.5f * rnd()) * std::sqrt(scale / 1.9f);
                p.heat  = hot ? e.heat * (0.55f + 0.45f * rnd()) : 0.0f;
                p.buoy  = hot ? 2.4f : 1.2f;
                p.mass  = hot ? 1.0f : 1.1f;
                p.grow  = 0.17f;
                p.swell = 0.9f;
                p.life  = 80.0f + 60.0f * rnd();
                add(p);
            }
            // ...and some of it, flashed to steam by the deluge under the deck,
            // wells up all round the deck's edge: what engulfs the launcher's
            // base before it has moved at all.
            const int nUp = static_cast<int>(t.steam * 0.3f + rnd());
            for (int k = 0; k < nUp; ++k) {
                const glm::vec3 mid = glm::vec3(t.model * glm::vec4(0.0f, -0.5f, 0.0f, 1.0f));
                glm::vec3 l(rnd() - 0.5f, -0.5f, rnd() - 0.5f);
                if (rnd() < 0.5f) l.x = l.x < 0.0f ? -0.5f : 0.5f;
                else              l.z = l.z < 0.0f ? -0.5f : 0.5f;
                const glm::vec3 edge = glm::vec3(t.model * glm::vec4(l, 1.0f));
                glm::vec3 out = edge - mid;
                out.y = 0.0f;
                out = glm::length(out) > 1e-3f ? glm::normalize(out) : glm::vec3(1, 0, 0);
                Puff p;
                p.pos   = edge + out * (1.0f + 3.0f * rnd()) - tup * (2.0f * rnd());
                p.vel   = out * (4.0f + 10.0f * rnd()) + tup * (1.0f + 4.0f * rnd());
                p.r     = 4.5f + 4.0f * rnd();
                p.heat  = 0.0f;
                p.buoy  = 0.8f;
                p.mass  = 1.0f;
                p.grow  = 0.15f;
                p.swell = 1.1f;
                p.life  = 70.0f + 50.0f * rnd();
                add(p);
            }
        } else if (f.hitKind != 0) {
            // Out over a deck or the ground in every direction, leaning the way
            // the jet came in when it came in at a slant.
            const glm::vec3 hit = f.a + f.dir * f.hitT;
            glm::vec2 h = rndDisc();
            h = glm::length(h) > 1e-4f ? glm::normalize(h) : glm::vec2(1, 0);
            glm::vec2 lean(f.dir.x, f.dir.z);
            h = glm::normalize(h + lean * 1.5f);
            const glm::vec3 hd(h.x, 0.0f, h.y);
            const bool steam = f.trench >= 0 && rnd() < trenches[f.trench].steam * 0.4f;
            // Off a deck the gas pours over its edge and down: it is born there,
            // at the foot of the deck, rather than flung off the top to hang in
            // the air at deck height.
            glm::vec3 from = hit + hd * (rHit * (0.4f + 0.6f * rnd()));
            if (f.hitKind != 1 && f.trench >= 0) {
                const glm::mat4& m = trenches[f.trench].model;
                const glm::mat4 inv = glm::inverse(m);
                const glm::vec3 lh = glm::vec3(inv * glm::vec4(hit, 1.0f));
                const glm::vec3 ld = glm::mat3(inv) * hd;
                float te = 1e9f;
                if (std::abs(ld.x) > 1e-6f) te = std::min(te, ((ld.x > 0 ? 0.5f : -0.5f) - lh.x) / ld.x);
                if (std::abs(ld.z) > 1e-6f) te = std::min(te, ((ld.z > 0 ? 0.5f : -0.5f) - lh.z) / ld.z);
                const glm::vec3 edge = glm::vec3(m * glm::vec4(lh + ld * std::max(te, 0.0f), 1.0f));
                from = edge + hd * (2.0f + 4.0f * rnd());
                from.y = floorAt(world, trenches, from.x, from.z, f.trench);
            }
            Puff p;
            p.pos   = from + up * (rHit * 0.3f);
            p.vel   = hd * ((14.0f + 26.0f * rnd()) * (0.4f + 0.6f * g)) + up * (2.0f + 6.0f * rnd());
            p.r     = rHit * (0.3f + 0.25f * rnd()) + 1.0f;
            p.heat  = steam ? 0.0f : e.heat * g * (0.5f + 0.5f * rnd());
            p.buoy  = 1.8f;
            p.mass  = 1.0f;
            p.grow  = 0.16f;
            p.swell = 0.8f;
            p.life  = 80.0f + 60.0f * rnd();
            add(p);
        }
    }

    // --- The trail: what leaves the flame's tip into the open air ------------
    const float trailW = 1.0f - g;
    glm::vec3& last = m_lastTip[e.id];
    const glm::vec3 tip = f.a + f.dir * L;
    const bool haveLast = m_haveTip[e.id];
    m_haveTip[e.id] = true;
    const glm::vec3 from = haveLast ? last : tip;
    last = tip;
    if (trailW <= 0.02f) return;
    const float rTip = f.r0 + L * tanS;
    // Spaced by the distance the tip moved, so a rocket at speed leaves a
    // continuous column rather than beads.
    const float moved = glm::length(tip - from);
    float& owedT = m_owed[e.id * 4 + 1];
    owedT += (6.0f * dt + moved / std::max(rTip * 0.32f, 0.5f)) * trailW * e.smoke * th;
    glm::vec3 u, v;
    basis(f.dir, u, v);
    int guard = 0;
    while (owedT >= 1.0f && guard++ < 256) {
        owedT -= 1.0f;
        // Born bunched round the axis and alike, along the whole stretch the
        // tip swept this step, each as old as it would be had it left in its
        // own moment: right behind the vehicle the trail is a smooth bright
        // column, and a camera following the launch holds its start in one
        // place on screen while the trail streams through it. Scattered wide,
        // sized and sped at random, every new puff stood out of that start as a
        // bump of its own, and its outline changed from frame to frame.
        // The lumps of an older column come from what they do next: each drifts
        // off the axis at its own pace and swells at its own rate, so the
        // column breaks up into billows over seconds, not at birth.
        const glm::vec2 d = rndDisc(), side = rndDisc();
        const float s = rnd();
        Puff p;
        p.vel   = f.dir * (26.0f + 8.0f * rnd()) + (u * side.x + v * side.y) * 6.0f;
        p.age   = (1.0f - s) * dt;
        p.pos   = glm::mix(from, tip, s) + (u * d.x + v * d.y) * (rTip * 0.2f) + p.vel * p.age;
        p.r     = rTip * (1.1f + 0.2f * rnd());
        p.heat  = e.heat * 0.12f;
        p.buoy  = 0.5f;
        p.mass  = 0.9f;
        p.grow  = 0.05f;
        p.swell = 1.2f * (0.3f + 1.4f * rnd());
        p.merge = 0.45f;
        p.life  = 90.0f + 50.0f * rnd();
        add(p);
    }
}

void Sim::emitVent(float dt, const Vent& v) {
    if (v.rate <= 0.0f) return;
    float& owed = m_owed[v.id * 4 + 2];
    owed += v.rate * dt;
    const glm::vec3 d = glm::length(v.dir) > 1e-6f ? glm::normalize(v.dir) : glm::vec3(0, 1, 0);
    glm::vec3 a, b;
    basis(d, a, b);
    int guard = 0;
    while (owed >= 1.0f && guard++ < 256) {
        owed -= 1.0f;
        const glm::vec2 q = rndDisc() * std::tan(std::clamp(v.spread, 0.0f, 1.4f));
        Puff p;
        p.pos   = v.pos + (a * q.x + b * q.y) * v.size * 0.5f;
        p.vel   = glm::normalize(d + a * q.x + b * q.y) * (v.speed * (0.7f + 0.6f * rnd()));
        p.r     = v.size * (0.7f + 0.6f * rnd());
        p.heat  = v.heat;
        p.buoy  = v.buoyancy;
        // A vent's density is how thick its vapour is, not how big: the
        // shape stays full and the stuff inside it thin, so boil-off reads as
        // a veil and not as a cloud of cotton.
        p.mass    = 0.9f;
        p.opacity = v.density;
        p.grow  = 0.12f;
        p.swell = v.growth;
        p.life  = v.life * (0.8f + 0.4f * rnd());
        add(p);
    }
}

void Sim::step(float dt, const std::vector<Engine>& engines, const std::vector<Vent>& vents,
               const std::vector<Trench>& trenches, const World& world) {
    if (dt <= 0.0f) return;
    dt = std::min(dt, 0.1f);
    m_time += dt;

    for (const Engine& e : engines) emitEngine(dt, e, trenches, world);
    for (const Vent& v : vents) emitVent(dt, v);

    const float heatK  = std::exp(-dt / kHeatTau);
    const float buoyK  = std::exp(-dt / kBuoyTau);
    for (Puff& p : m_puffs) {
        p.age += dt;
        const glm::vec3 rel = p.vel - world.wind;
        const float sp = glm::length(rel);
        if (sp > 1e-4f) {
            const float decel = kDrag * sp * sp / std::max(p.r, 0.5f);
            p.vel -= rel * (std::min(decel * dt, sp) / sp);
        }
        // Buoyancy takes hold as the puff mixes and slows: a fresh puff is
        // all momentum and runs along the ground; the hot cloud it becomes
        // then rises. Without the delay the base of the cloud lifts off the
        // pad as a whole, which no launch cloud does.
        // Cold vapour is heavier than the air from the start and sinks at once.
        const float takeHold = p.buoy > 0.0f ? std::min(1.0f, p.age / 7.0f) : 1.0f;
        p.vel.y += p.buoy * takeHold * takeHold * dt;
        p.buoy  *= buoyK;
        p.pos   += p.vel * dt;
        // Growth slows as a puff gets big: past thirty metres a cloud grows by
        // new puffs piling in, not by its old ones ballooning into smooth domes
        // that no detail can break up.
        const float big = p.r / 30.0f;
        p.r     += (p.grow * sp + p.swell * std::exp(-p.age / kSwellTau)) / (1.0f + big * big) * dt;
        p.heat  *= heatK;

        // Landing on the ground or a deck: what was coming down spreads out.
        const float floorY = floorAt(world, trenches, p.pos.x, p.pos.z);
        const float lift = p.r * 0.45f;
        if (p.pos.y - lift < floorY) {
            p.pos.y = floorY + lift;
            if (p.vel.y < 0.0f) {
                glm::vec2 h(p.vel.x, p.vel.z);
                const float hl = glm::length(h);
                h = hl > 1e-3f ? h / hl : glm::normalize(rndDisc() + glm::vec2(1e-3f, 0.0f));
                const float push = -p.vel.y * 0.7f;
                p.vel.x += h.x * push;
                p.vel.z += h.y * push;
                p.vel.y  = 0.0f;
            }
        }
    }

    // Retire the dead, keeping birth order (the oldest stay at the front, which
    // is what the cap below relies on).
    m_puffs.erase(std::remove_if(m_puffs.begin(), m_puffs.end(),
                                 [](const Puff& p) { return p.age >= p.life; }),
                  m_puffs.end());
    if (static_cast<int>(m_puffs.size()) > kMaxPuffs)
        m_puffs.erase(m_puffs.begin(), m_puffs.begin() + (m_puffs.size() - kMaxPuffs));
}

} // namespace exhaust
