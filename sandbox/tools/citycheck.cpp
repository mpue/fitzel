// The city check: does the generated district keep out of the carriageway?
//
// A tower that overhangs the road is not a visual defect -- it is a wall you
// drive into at 200 km/h on a stretch that looked clear, and it is invisible in
// the editor because the piece that does it is a three-storey podium hidden
// behind a spray of scenery. The panel's "placed / skipped" counters cannot see
// it either: from the generator's point of view those lots were built fine.
//
// So measure it. Generate a district over a road with the shapes that actually
// break -- a straight, a sweeper, a hairpin -- and report, per style and per
// biome preset, how far the WORST solid piece reaches past the kerb. Positive
// metres are metres of road blocked.
//
// Console program, like shadercheck and audiocheck, and for the same reason:
// the editor is /SUBSYSTEM:WINDOWS in Release and has nowhere to print to.
//   build/release/bin/citycheck.exe
// Exits non-zero if anything solid crosses the kerb.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "../src/CityGen.hpp"
#include "../src/SceneTypes.hpp"  // EntityType

namespace {

constexpr float kPi = 3.14159265358979323846f;

// A road as the generator sees it: a dense polyline plus a height per point.
struct Road {
    const char*            name;
    std::vector<glm::vec2> p;
    std::vector<float>     y;
};

// Sampled at 2 m, which is about what RoadSystem hands over.
Road makeRoad(const char* name, float radius, float length) {
    Road r; r.name = name;
    for (float s = 0.0f; s <= length; s += 2.0f) {
        if (radius <= 0.0f) {                     // dead straight
            r.p.push_back({0.0f, s});
        } else {                                  // constant-radius bend
            const float a = s / radius;
            r.p.push_back({radius * (1.0f - std::cos(a)), radius * std::sin(a)});
        }
        r.y.push_back(0.0f);
    }
    return r;
}

// A figure of eight (Gerono's lemniscate): a road that crosses itself, and the
// case arc length cannot see at all -- the two branches are metres apart in the
// world and half a lap apart in station. `dip` sinks one branch, which is how a
// crossing is actually built and which must NOT cost the frontage above it.
Road makeFigureEight(const char* name, float a, float dip) {
    Road r; r.name = name;
    constexpr int kN = 900;
    for (int i = 0; i <= kN; ++i) {
        const float t = 2.0f * kPi * i / kN;
        r.p.push_back({a * std::sin(t), a * std::sin(t) * std::cos(t)});
        // Sink the second lobe, easing in and out so there is no cliff.
        const float lobe = (t > kPi) ? 1.0f : 0.0f;
        const float ease = std::sin(glm::clamp((t - kPi) / kPi, 0.0f, 1.0f) * kPi);
        r.y.push_back(-dip * lobe * ease);
    }
    return r;
}

// Roof of a car sitting on the carriageway. A piece only counts as blocking a
// stretch of road if it occupies the space a car passing along it would.
constexpr float kCarTop = 3.0f;

// Distance from `q` to the centreline, counting only the stretches this piece
// could actually be hit on -- ones whose carriageway runs through the piece's
// own height band. Without that, a district beside a road in a sixteen-metre
// cutting, or a band oversailing the street ten storeys up, reads as a wall.
// Brute force on purpose: this is a check, not a generator, and being obviously
// right matters more here than being quick.
float distToRoad(const Road& r, glm::vec2 q, float yLo, float yHi) {
    float best = 1e9f;
    for (std::size_t i = 1; i < r.p.size(); ++i) {
        const glm::vec2 a = r.p[i - 1], b = r.p[i];
        const glm::vec2 ab = b - a;
        const float l2 = glm::dot(ab, ab);
        const float t = (l2 > 1e-6f)
                            ? glm::clamp(glm::dot(q - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
        const float d = glm::length(q - (a + ab * t));
        if (d >= best) continue;
        const float segY = r.y[i - 1] + (r.y[i] - r.y[i - 1]) * t;
        if (segY + kCarTop <= yLo || segY >= yHi) continue;   // over it or under it
        best = d;
    }
    return best;
}

// How far a solid piece reaches past the kerb, in metres (<=0 = clear).
//
// The outline is walked, not just the corners: a road that crosses back over its
// own district can pass through the middle of a long facade without coming near
// either end of it, and four corner samples would call that clear. Cylinders are
// walked as the ellipse they are -- measuring a round tower by its bounding box
// reads 0.41r of empty air as a wall, which on a 45 m podium is eighteen metres
// of intrusion that does not exist.
float intrusion(const Road& r, const city::Piece& pc, float halfWidth) {
    const float c = std::cos(glm::radians(pc.yaw)), s = std::sin(glm::radians(pc.yaw));
    // Local XZ -> world, the generator's yaw convention (BuildingGen::yawXZ).
    auto toWorld = [&](glm::vec2 l) {
        return glm::vec2(pc.center.x + l.x * c + l.y * s,
                         pc.center.z - l.x * s + l.y * c);
    };

    std::vector<glm::vec2> outline;
    if (pc.type == EntityType::Cylinder) {
        constexpr int kSeg = 48;
        for (int i = 0; i < kSeg; ++i) {
            const float a = 2.0f * kPi * i / kSeg;
            outline.push_back({pc.half.x * std::cos(a), pc.half.z * std::sin(a)});
        }
    } else {
        const glm::vec2 corner[4] = {{-pc.half.x, -pc.half.z}, { pc.half.x, -pc.half.z},
                                     { pc.half.x,  pc.half.z}, {-pc.half.x,  pc.half.z}};
        for (int k = 0; k < 4; ++k) {
            const glm::vec2 a = corner[k], b = corner[(k + 1) & 3];
            const int n = std::max(1, static_cast<int>(glm::length(b - a) / 3.0f));
            for (int i = 0; i < n; ++i)
                outline.push_back(a + (b - a) * (static_cast<float>(i) / n));
        }
    }

    const float yLo = pc.center.y - pc.half.y, yHi = pc.center.y + pc.half.y;
    float worst = -1e9f;
    for (const glm::vec2& l : outline)
        worst = std::max(worst, halfWidth - distToRoad(r, toWorld(l), yLo, yHi));
    return worst;
}

// Out along one leg, round a half-turn, and back down a parallel one `spacing`
// metres away. `dip` sinks the return leg, which is how a road that doubles back
// past its own frontage is actually built.
Road makeHairpin(const char* name, float legLength, float spacing, float dip) {
    Road r; r.name = name;
    const float R = spacing * 0.5f;
    for (float s = 0.0f; s <= legLength; s += 2.0f) {   // out, along +Z at x=0
        r.p.push_back({0.0f, s});
        r.y.push_back(0.0f);
    }
    constexpr int kTurn = 60;
    for (int i = 1; i < kTurn; ++i) {                   // the half turn
        const float a = kPi * i / kTurn;
        r.p.push_back({R * (1.0f - std::cos(a)), legLength + R * std::sin(a)});
        r.y.push_back(-dip * (static_cast<float>(i) / kTurn));
    }
    for (float s = legLength; s >= 0.0f; s -= 2.0f) {   // back, at x=spacing
        r.p.push_back({spacing, s});
        r.y.push_back(-dip);
    }
    return r;
}

} // namespace

int main() {
    constexpr float kHalfWidth = 7.0f;   // a 14 m carriageway
    constexpr int   kBudget    = 400;

    // Flat ground: a slope would start rejecting lots for slope and hide the
    // very cases this is meant to catch.
    auto ground = [](float, float) { return 0.0f; };

    // Bends are cut at half a turn on purpose. Run one further and the circle
    // closes on itself, which stops being a bend and becomes the OTHER way a
    // building ends up in the road -- a lot placed against a distant stretch of
    // the same road. Worth a check of its own; mixing the two here would only
    // make both unreadable.
    const Road roads[] = {
        makeRoad("straight",     0.0f,   900.0f),
        makeRoad("sweeper R200", 200.0f, 620.0f),
        makeRoad("bend R80",     80.0f,  250.0f),
        makeRoad("hairpin R40",  40.0f,  125.0f),
        makeFigureEight("figure8 flat", 260.0f, 0.0f),
        // The case that really bites: a hairpin whose return leg runs 44 m from
        // the way out. Every lot on the inside of either leg is standing on the
        // other one, and in arc length they are half a kilometre apart. The
        // second copy sinks the return leg into a cutting -- there the frontage
        // above it must SURVIVE, or the fix has just deleted every underpass in
        // the game.
        makeHairpin("hairpin pair", 300.0f, 44.0f, 0.0f),
        makeHairpin("hairpin sunk", 300.0f, 44.0f, 16.0f),
    };

    int failures = 0;
    std::printf("%-14s %-11s %7s %8s %9s\n",
                "road", "biome", "placed", "solids", "intrusion");
    std::printf("---------------------------------------------------------\n");

    for (const Road& r : roads) {
        for (int pi = 0; pi < static_cast<int>(city::Preset::Count); ++pi) {
            const auto preset = static_cast<city::Preset>(pi);
            std::vector<city::Biome> biomes{city::preset(preset)};
            // One palette; the check never touches materials, only geometry.
            std::vector<buildings::Palette> pals(1);

            const city::District d =
                city::generate(r.p, r.y, kHalfWidth, biomes, pals, ground, kBudget);

            float worst = -1e9f, worstBase = 0.0f, worstSpan = 0.0f;
            bool  worstRound = false;
            glm::vec2 worstAt{0.0f}, worstHalf{0.0f};
            float worstYaw = 0.0f;
            for (const city::Piece& pc : d.colliders) {
                const float base = pc.center.y - pc.half.y;
                const float in = intrusion(r, pc, kHalfWidth);
                if (in > worst) {
                    worst = in; worstBase = base;
                    worstRound = (pc.type == EntityType::Cylinder);
                    worstSpan = 2.0f * std::max(pc.half.x, pc.half.z);
                    worstAt = {pc.center.x, pc.center.z};
                    worstHalf = {pc.half.x, pc.half.z};
                    worstYaw = pc.yaw;
                }
            }

            const bool bad = worst > 0.05f;   // 5 cm of slack for float noise
            if (bad) ++failures;
            char note[160] = "";
            if (bad)
                std::snprintf(note, sizeof note,
                              "   <-- BLOCKED (base y=%.1f, %.0f m %s at %.0f,%.0f"
                              " half %.0fx%.0f yaw %.0f)",
                              worstBase, worstSpan, worstRound ? "cylinder" : "box",
                              worstAt.x, worstAt.y, worstHalf.x, worstHalf.y, worstYaw);
            std::printf("%-14s %-11s %7d %8zu %8.2f m%s\n", r.name,
                        city::presetName(preset), d.placed, d.colliders.size(),
                        (worst < -1e8f) ? 0.0f : worst, note);
        }
    }

    std::printf("\n%s\n", failures ? "FAIL: solid geometry is standing in the road"
                                   : "OK: the carriageway is clear");
    return failures ? 1 : 0;
}
