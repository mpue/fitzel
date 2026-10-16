#include "SeaSpray.hpp"

#include <algorithm>
#include <cmath>

#include "Ocean.hpp"

namespace ocean {

namespace {

float smooth01(float a, float b, float v) {
    const float t = std::clamp((v - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

void emitSeaSpray(const SeaSprayInput& in, SprayPool& pool, std::mt19937& rng) {
    if (in.amount <= 0.0f || in.dt <= 0.0f) return;
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    auto rnd = [&] { return u01(rng); };
    const float tau = 6.2831853f;

    // --- Off the breakers ----------------------------------------------------
    // Points in a disc round the eye, crowded toward it (radius linear in the
    // draw, not its root): near spray is what is seen, and the surf a hundred
    // metres off is a white line whatever is thrown off it.
    if (in.shore && in.shore->valid() && in.breakers.height > 0.0f) {
        const int   samples = 180;
        const float R       = 140.0f;
        for (int i = 0; i < samples; ++i) {
            const float r = R * rnd(), a = tau * rnd();
            const float x = in.eye.x + r * std::cos(a), z = in.eye.z + r * std::sin(a);
            const BreakerSample b = breakerAt(*in.shore, in.breakers, x, z, in.time);
            if (!b.active || b.breaking < 0.3f) continue;
            if (rnd() > b.breaking * in.dt * 28.0f * in.amount) continue;
            const glm::vec3 shore(b.dir.x, 0.0f, b.dir.y);
            const glm::vec3 side(-b.dir.y, 0.0f, b.dir.x);
            const glm::vec3 lip(x + b.offset.x, in.level + b.offset.y + 0.15f, z + b.offset.z);
            const float big = std::clamp(in.breakers.height, 0.3f, 3.0f);
            const int drops = 2 + static_cast<int>(rnd() * 3.0f * big);
            for (int k = 0; k < drops; ++k) {
                SprayP p;
                p.pos  = lip + side * ((rnd() - 0.5f) * 2.0f);
                // Thrown forward off the lip as it pitches, and up.
                p.vel  = shore * (in.breakers.speed * (0.5f + 0.6f * rnd())) +
                         glm::vec3(0.0f, (1.2f + 3.0f * rnd()) * std::sqrt(big), 0.0f) +
                         side * ((rnd() - 0.5f) * 1.5f);
                p.life = p.life0 = 0.6f + 0.8f * rnd();
                p.size = 0.5f + 0.9f * rnd();
                p.flat = 0.0f;
                pool.add(p);
            }
            // ...and the haze that hangs behind a breaking wave.
            if (rnd() < 0.55f) {
                SprayP m;
                m.pos  = lip + glm::vec3(0.0f, 0.3f, 0.0f) + side * ((rnd() - 0.5f) * 2.5f);
                m.vel  = shore * (in.breakers.speed * 0.35f) + glm::vec3(0.0f, 0.5f + rnd(), 0.0f);
                m.life = m.life0 = 1.8f + 1.6f * rnd();
                m.size = (2.0f + 3.0f * rnd()) * big;
                m.flat = 2.0f;
                pool.add(m);
            }
        }
    }

    // --- Off the crests, in a gale ---------------------------------------------
    // From a strong breeze up the wind tears the tops off the breaking crests
    // out at sea and drives them downwind as spume -- the streaks a storm draws
    // on the water, and the haze over it.
    const float gale = smooth01(9.0f, 20.0f, in.windSpeed);
    if (in.sea && gale > 0.0f) {
        const int   samples = 140;
        const float R       = 180.0f;
        const glm::vec3 wind = glm::vec3(in.windDir.x, 0.0f, in.windDir.y) * in.windSpeed;
        for (int i = 0; i < samples; ++i) {
            const float r = R * rnd(), a = tau * rnd();
            const float x = in.eye.x + r * std::cos(a), z = in.eye.z + r * std::sin(a);
            // The open sea only: the surf has its own spray, and on land the
            // ocean's foam is under the ground.
            if (in.shore && in.shore->valid() && in.shore->sample(x, z).x < 3.0f) continue;
            const float f = in.sea->foamAt(x, z);
            if (f < 0.5f) continue;
            if (rnd() > (f - 0.5f) * 2.0f * gale * in.dt * 40.0f * in.amount) continue;
            const glm::vec3 at(x, in.level + in.sea->displacementAt(x, z).y + 0.2f, z);
            const int drops = 1 + static_cast<int>(rnd() * 3.0f * gale);
            for (int k = 0; k < drops; ++k) {
                SprayP p;
                p.pos  = at + glm::vec3((rnd() - 0.5f) * 1.5f, 0.0f, (rnd() - 0.5f) * 1.5f);
                p.vel  = wind * (0.25f + 0.3f * rnd()) + glm::vec3(0.0f, 1.0f + 2.5f * rnd() * gale, 0.0f);
                p.life = p.life0 = 0.6f + 0.7f * rnd();
                p.size = 0.4f + 0.7f * rnd();
                p.flat = 0.0f;
                pool.add(p);
            }
            if (rnd() < 0.6f * gale) {
                SprayP m;
                m.pos  = at + glm::vec3(0.0f, 0.4f, 0.0f);
                m.vel  = wind * 0.5f + glm::vec3(0.0f, 0.3f, 0.0f);
                m.life = m.life0 = 1.5f + 1.5f * rnd();
                m.size = 3.0f + 4.0f * rnd() * gale;
                m.flat = 2.0f;
                pool.add(m);
            }
        }
    }
}

void emitSplash(SprayPool& pool, std::mt19937& rng, glm::vec3 at, glm::vec3 vel, float radius,
                float amount) {
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    auto rnd = [&] { return u01(rng); };
    const float hit = std::max(-vel.y, 0.0f) + 0.3f * glm::length(glm::vec2(vel.x, vel.z));
    if (hit < 0.8f || amount <= 0.0f) return;
    radius = std::clamp(radius, 0.15f, 6.0f);
    // A crown: droplets thrown up and out from the rim, as many as the blow and
    // the size of the thing deserve.
    const int drops = static_cast<int>(std::clamp(hit * radius * 22.0f, 8.0f, 220.0f) * amount);
    for (int i = 0; i < drops; ++i) {
        const float a = 6.2831853f * rnd();
        const glm::vec3 ring(std::cos(a), 0.0f, std::sin(a));
        SprayP p;
        p.pos  = at + ring * (radius * (0.6f + 0.5f * rnd()));
        p.vel  = ring * (hit * (0.15f + 0.35f * rnd())) +
                 glm::vec3(0.0f, hit * (0.35f + 0.6f * rnd()), 0.0f) + glm::vec3(vel.x, 0.0f, vel.z) * 0.3f;
        p.life = p.life0 = 0.5f + 0.9f * rnd();
        p.size = 0.5f + 1.0f * rnd() * std::sqrt(radius);
        p.flat = 0.0f;
        pool.add(p);
    }
    // The ring of foam it leaves, spreading out.
    const int foam = std::max(4, drops / 6);
    for (int i = 0; i < foam; ++i) {
        const float a = 6.2831853f * (static_cast<float>(i) + rnd()) / static_cast<float>(foam);
        const glm::vec3 ring(std::cos(a), 0.0f, std::sin(a));
        SprayP f;
        f.pos  = at + ring * radius;
        f.vel  = ring * (0.8f + 0.8f * rnd()) * std::min(hit * 0.3f, 2.0f);
        f.life = f.life0 = 2.5f + 2.0f * rnd();
        f.size = radius * (0.8f + 0.6f * rnd());
        f.flat = 1.0f;
        pool.add(f);
    }
    if (hit > 4.0f) {
        SprayP m;
        m.pos  = at + glm::vec3(0.0f, 0.5f, 0.0f);
        m.vel  = glm::vec3(0.0f, 0.6f, 0.0f);
        m.life = m.life0 = 1.5f;
        m.size = radius * 4.0f;
        m.flat = 2.0f;
        pool.add(m);
    }
}

void emitBowSpray(SprayPool& pool, std::mt19937& rng, glm::vec3 bow, glm::vec2 heading,
                  float speed, float halfWidth, float dt, float amount) {
    const float over = speed - 2.0f;   // a hull at walking pace parts the water quietly
    if (over <= 0.0f || amount <= 0.0f) return;
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    auto rnd = [&] { return u01(rng); };
    const glm::vec3 fwd(heading.x, 0.0f, heading.y);
    const glm::vec3 side(-heading.y, 0.0f, heading.x);
    // Poisson-ish: the expected count this frame, its fraction by a draw.
    const float want = over * 14.0f * dt * amount;
    int n = static_cast<int>(want);
    if (rnd() < want - static_cast<float>(n)) ++n;
    for (int i = 0; i < n; ++i) {
        const float s = rnd() < 0.5f ? -1.0f : 1.0f;   // port or starboard
        SprayP p;
        p.pos  = bow + side * (s * halfWidth * (0.3f + 0.7f * rnd()));
        // The bow wave throws water out to the side and up, a little forward.
        p.vel  = side * (s * speed * (0.15f + 0.2f * rnd())) + fwd * (speed * 0.35f) +
                 glm::vec3(0.0f, 0.8f + speed * (0.08f + 0.12f * rnd()), 0.0f);
        p.life = p.life0 = 0.5f + 0.6f * rnd();
        p.size = 0.5f + 0.8f * rnd();
        p.flat = 0.0f;
        pool.add(p);
        if (rnd() < 0.35f) {
            // ...and the foam it lays alongside, which the hull leaves behind.
            SprayP f;
            f.pos  = bow + side * (s * halfWidth * 1.1f);
            f.vel  = side * (s * 0.8f);
            f.life = f.life0 = 2.0f + 2.0f * rnd();
            f.size = 0.8f + 0.8f * rnd();
            f.flat = 1.0f;
            pool.add(f);
        }
    }
}

} // namespace ocean
