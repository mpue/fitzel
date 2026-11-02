#pragma once

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// A hull that rides the sea as something you can walk on.
//
// A Buoyancy body on a DYNAMIC Physics component floats by forces at its
// corners (Buoyancy.hpp), and that is right for a crate or a dinghy. A ship is
// different in the two ways that decide whether a player can stand on it:
//
//  - The walking player is carried only by what moves KINEMATICALLY (a
//    platform, a tram): a dynamic deck slides away under the feet. And Jolt
//    cannot simulate a moving triangle mesh, so a dynamic hull is a hull-shaped
//    BOX, with the player standing on its lid a metre above the planks.
//  - A long hull averages the sea. A fifty-metre ship does not feel the chop,
//    only the waves about as long as itself, and it answers them slowly.
//
// So the hull is LED rather than pushed: every frame the water under its
// waterplane is fitted with a plane, and the hull's heave, pitch and roll each
// follow that plane through a damped spring. The lag and the overshoot are its
// inertia; the periods come from its size -- the depth it sits at for heave and
// pitch, its beam for roll -- so a raft bobs and a ship of the line rolls slowly.
//
// The author places the hull at its waterline in still water: what the sea adds
// is an offset from there, so what was placed is what floats. Pure arithmetic:
// Platforms.cpp leads the hull and its colliders with it, oceancheck measures it.
namespace ocean {

struct HullRide {
    // The waterplane in the hull's own frame at rest: x across, z along.
    glm::vec2 center{0.0f};    // its middle (local x, z)
    glm::vec2 half{1.0f};      // half its beam (x) and half its length (z)
    float     waterline = 0.0f; // local y of the still water
    float     draft     = 1.0f; // m from the waterline to the lowest point

    // Offsets from rest, each with its rate. The tilts are the rotations as
    // they are applied: `pitch` about the local X axis (positive puts +Z down),
    // `roll` about the local Z axis (positive puts +X up).
    float heave = 0.0f, heaveV = 0.0f;   // m up
    float pitch = 0.0f, pitchV = 0.0f;   // rad
    float roll  = 0.0f, rollV  = 0.0f;   // rad

    // Natural periods, seconds. Heave and pitch as a body sitting `draft` deep
    // (with the water it drags along, half again its own mass); roll from the
    // beam the way a naval architect's rule of thumb has it (T ~ 2.6 sqrt(B)
    // for a stiff hull). Clamped to what still reads as floating.
    float heavePeriod() const {
        return std::clamp(6.2831853f * std::sqrt(1.5f * draft / 9.81f), 0.8f, 8.0f);
    }
    float rollPeriod() const {
        return std::clamp(2.6f * std::sqrt(std::max(2.0f * half.x, 0.1f)), 1.2f, 14.0f);
    }

    // The sea under the waterplane as a plane, from a grid of samples:
    // `level` + h0 at the middle, rising `slope` metres per metre across (x) and
    // along (z) the hull. `surface(x, z)` answers the water's world height;
    // `home` is the hull's world matrix at rest (no scale).
    template <class Surface>
    void fitSea(const Surface& surface, float level, const glm::mat4& home,
                float& h0, glm::vec2& slope) const {
        const int nx = half.x > 2.0f ? 3 : 2;
        const int nz = half.y > 6.0f ? 5 : (half.y > 2.0f ? 3 : 2);
        float sum = 0.0f, sx = 0.0f, sz = 0.0f, xx = 0.0f, zz = 0.0f;
        for (int i = 0; i < nx; ++i)
            for (int k = 0; k < nz; ++k) {
                const float u = half.x * (2.0f * i / (nx - 1) - 1.0f);
                const float v = half.y * (2.0f * k / (nz - 1) - 1.0f);
                const glm::vec3 w = glm::vec3(home * glm::vec4(center.x + u, waterline,
                                                               center.y + v, 1.0f));
                const float h = surface(w.x, w.z) - level;
                sum += h;  sx += h * u;  sz += h * v;
                xx  += u * u;  zz += v * v;
            }
        h0    = sum / static_cast<float>(nx * nz);
        slope = glm::vec2(xx > 0.0f ? sx / xx : 0.0f, zz > 0.0f ? sz / zz : 0.0f);
    }

    // Move on by `dt` against the sea.
    template <class Surface>
    void step(const Surface& surface, float level, const glm::mat4& home, float dt) {
        if (dt <= 0.0f) return;
        float h0 = 0.0f;
        glm::vec2 slope(0.0f);
        fitSea(surface, level, home, h0, slope);
        // Where the plane would hold the hull if it had no inertia: lifted to
        // the water, its deck parallel to the plane.
        const float toHeave = h0;
        const float toPitch = -std::atan(slope.y);
        const float toRoll  =  std::atan(slope.x);
        const float wH = 6.2831853f / heavePeriod();
        const float wR = 6.2831853f / rollPeriod();
        // Damping as a fraction of critical: heave and pitch die quickly in
        // water (a hull pushes a lot of it about), a roll lingers.
        const auto spring = [](float& x, float& v, float to, float w, float zeta, float h) {
            v += (w * w * (to - x) - 2.0f * zeta * w * v) * h;
            x += v * h;
        };
        // Sub-steps: a long frame must not throw a short-period raft about.
        const int n = std::max(1, static_cast<int>(std::ceil(std::min(dt, 0.1f) * 120.0f)));
        const float h = std::min(dt, 0.1f) / static_cast<float>(n);
        for (int i = 0; i < n; ++i) {
            spring(heave, heaveV, toHeave, wH, 0.45f, h);
            spring(pitch, pitchV, toPitch, wH, 0.40f, h);
            spring(roll,  rollV,  toRoll,  wR, 0.18f, h);
        }
    }

    // The hull's world matrix now: `home` tilted about the middle of its
    // waterplane, then lifted.
    glm::mat4 pose(const glm::mat4& home) const {
        const glm::vec3 pivot(center.x, waterline, center.y);
        glm::mat4 tilt = glm::translate(glm::mat4(1.0f), pivot);
        tilt = glm::rotate(tilt, pitch, glm::vec3(1.0f, 0.0f, 0.0f));
        tilt = glm::rotate(tilt, roll,  glm::vec3(0.0f, 0.0f, 1.0f));
        tilt = glm::translate(tilt, -pivot);
        return glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, heave, 0.0f)) * home * tilt;
    }
};

} // namespace ocean
