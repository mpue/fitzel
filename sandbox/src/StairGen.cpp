#include "StairGen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "EditMesh.hpp"

using fitzel::AssetId;

namespace stairgen {

namespace {

constexpr float kPi = 3.14159265358979f;
// The player walks up anything to 0.40 m (PlayerStartComponent::stepHeight);
// a riser count that would ask for more is raised until it does not.
constexpr float kMaxRise   = 0.30f;
constexpr float kNosingRim = 0.04f;   // stringer top above the nosing line

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

template <class E>
E clampEnum(E e) {
    return static_cast<E>(std::clamp(static_cast<int>(e), 0, static_cast<int>(E::Count) - 1));
}

std::string fmt(const char* f, double a, double b = 0.0, double c = 0.0) {
    char buf[256];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

glm::vec3 clamp01(const glm::vec3& c) { return glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f)); }

// Every field into a range the layout can build, so no stepper and no
// hand-edited scene file can ask for a stair the turtle cannot lay.
Params sane(const Params& in) {
    Params p = in;
    p.kind      = clampEnum(p.kind);
    p.structure = clampEnum(p.structure);
    p.railStyle = clampEnum(p.railStyle);
    p.railSides = clampEnum(p.railSides);
    p.wallSides = clampEnum(p.wallSides);
    p.look      = clampEnum(p.look);
    p.height       = clampf(p.height, 0.30f, 12.0f);
    p.width        = clampf(p.width, 0.50f, 4.0f);
    p.risers       = std::clamp(p.risers, 2, 80);
    p.targetRise   = clampf(p.targetRise, 0.12f, 0.22f);
    p.going        = clampf(p.going, 0.18f, 0.45f);
    p.splitStep    = std::clamp(p.splitStep, 0, 79);
    p.landingDepth = clampf(p.landingDepth, 0.60f, 4.0f);
    p.eye          = clampf(p.eye, 0.0f, 1.5f);
    p.winders      = std::clamp(p.winders, 2, 6);
    p.innerRadius  = clampf(p.innerRadius, 0.05f, 4.0f);
    p.turn         = clampf(p.turn, 30.0f, 1080.0f);
    p.topPlatform  = clampf(p.topPlatform, 0.0f, 4.0f);
    p.treadThickness = clampf(p.treadThickness, 0.02f, 0.25f);
    p.nosing         = clampf(p.nosing, 0.0f, 0.06f);
    p.stringerWidth  = clampf(p.stringerWidth, 0.03f, 0.25f);
    p.stringerDepth  = clampf(p.stringerDepth, 0.10f, 0.60f);
    p.railHeight     = clampf(p.railHeight, 0.60f, 1.30f);
    p.balusterGap    = clampf(p.balusterGap, 0.04f, 0.30f);
    p.wallHeight     = clampf(p.wallHeight, 0.30f, 5.0f);
    p.wallThickness  = clampf(p.wallThickness, 0.06f, 0.50f);
    p.treadColor     = clamp01(p.treadColor);
    p.structureColor = clamp01(p.structureColor);
    p.railColor      = clamp01(p.railColor);
    p.wallColor      = clamp01(p.wallColor);
    return p;
}

int riserCount(const Params& p) {
    const int n  = p.autoRisers ? static_cast<int>(std::lround(p.height / p.targetRise)) : p.risers;
    const int lo = std::max(2, static_cast<int>(std::ceil(p.height / kMaxRise - 1e-4f)));
    const int hi = std::max(lo, static_cast<int>(std::floor(p.height / 0.08f)));
    return std::clamp(n, lo, hi);
}

float signedArea(const std::vector<glm::vec2>& ring) {
    float a = 0.0f;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const glm::vec2& p = ring[i];
        const glm::vec2& q = ring[(i + 1) % ring.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return 0.5f * a;
}

// --- The turtle ----------------------------------------------------------------
//
// Lays treads one after the other from the foot, always turning left. `c` is
// the middle of the next front edge, `h` the walking direction. Tread position
// k (landings count: each is reached by one riser) is walked on at (k+1)*rise.
struct Turtle {
    Plan&     plan;
    float     W, r, g;
    glm::vec2 c{0.0f}, h{0.0f, 1.0f};
    float     walk = 0.0f;
    int       pos  = 0;

    glm::vec2 R() const { return {h.y, -h.x}; }
    glm::vec2 at(const glm::vec2& o, float u, float v) const { return o + R() * u + h * v; }
    glm::vec2 frontLeft() const { return c - R() * (0.5f * W); }

    void put(Tread t, float len) {
        t.top       = static_cast<float>(pos + 1) * r;
        t.lineFront = t.top;
        t.lineBack  = t.arrival ? t.top : t.top + r;
        t.walkFront = walk;
        walk += len;
        t.walkBack = walk;
        plan.treads.push_back(std::move(t));
        ++pos;
    }

    void rect(float depth, bool landing, bool arrival) {
        const glm::vec2 FL = frontLeft();
        Tread t;
        t.landing = landing;
        t.arrival = arrival;
        t.left  = {FL, at(FL, 0.0f, depth)};
        t.right = {at(FL, W, 0.0f), at(FL, W, depth)};
        put(std::move(t), depth);
        c += h * depth;
    }

    void flight(int k) {
        for (int j = 0; j < k; ++j) rect(g, false, false);
        if (k > 0) plan.walkLine.push_back(c);
    }

    void straightLanding(float depth, bool arrival) {
        rect(depth, true, arrival);
        plan.walkLine.push_back(c);
    }

    void turnTo(const glm::vec2& newC, const glm::vec2& newH) {
        c = newC;
        h = newH;
        plan.walkLine.push_back(c);
    }

    // Quarter landing: a W x W square, left out of its left side.
    void cornerLanding() {
        const glm::vec2 FL = frontLeft();
        Tread t;
        t.landing = true;
        t.left  = {FL};
        t.right = {at(FL, W, 0.0f), at(FL, W, W), at(FL, 0.0f, W)};
        put(std::move(t), W);
        plan.walkLine.push_back(at(FL, 0.5f * W, 0.5f * W));
        turnTo(at(FL, 0.0f, 0.5f * W), {-h.y, h.x});
    }

    // Half landing across both flights and the eye between them.
    void uLanding(float depth, float eye) {
        const glm::vec2 FL = frontLeft();
        Tread t;
        t.landing = true;
        t.left  = {FL, at(FL, -eye, 0.0f)};
        t.right = {at(FL, W, 0.0f), at(FL, W, depth), at(FL, -eye - W, depth),
                   at(FL, -eye - W, 0.0f)};
        put(std::move(t), depth + W + eye);
        plan.walkLine.push_back(at(FL, 0.5f * W, 0.5f * depth));
        plan.walkLine.push_back(at(FL, -eye - 0.5f * W, 0.5f * depth));
        turnTo(at(FL, -eye - 0.5f * W, 0.0f), -h);
    }

    // Winders: `count` wedges fanned round `piv` over [0, sweep], filling the
    // rectangle [u0,u1] x [0,v1] of the frame at FL. The walking line is the
    // arc of radius `walkR` round the pivot. A pivot off the flights' inner
    // corners (the middle of a U's eye) leaves a stretch of the inner side on
    // either side of it: the first winder takes the one from `in`, the last the
    // one to `out`, so the flights still meet the winders edge to edge.
    void fan(const glm::vec2& FL, const glm::vec2& piv, float u0, float u1, float v1,
             float sweep, int count, float walkR, glm::vec2 in, glm::vec2 out) {
        auto hit = [&](float a) {
            const glm::vec2 d(std::cos(a), std::sin(a));
            float t = 1e9f;
            if (d.x > 1e-6f)  t = std::min(t, (u1 - piv.x) / d.x);
            if (d.x < -1e-6f) t = std::min(t, (u0 - piv.x) / d.x);
            if (d.y > 1e-6f)  t = std::min(t, (v1 - piv.y) / d.y);
            return piv + d * t;
        };
        const glm::vec2 corners[2] = {{u1, v1}, {u0, v1}};
        for (int i = 0; i < count; ++i) {
            const float a0 = sweep * static_cast<float>(i) / static_cast<float>(count);
            const float a1 = sweep * static_cast<float>(i + 1) / static_cast<float>(count);
            Tread t;
            auto inner = [&](const glm::vec2& q) {
                const glm::vec2 w = at(FL, q.x, q.y);
                if (t.left.empty() || glm::length(w - t.left.back()) > 1e-5f) t.left.push_back(w);
            };
            if (i == 0) inner(in);
            inner(piv);
            if (i + 1 == count) inner(out);
            glm::vec2 q = hit(a0);
            t.right.push_back(at(FL, q.x, q.y));
            for (const glm::vec2& k : corners) {
                const float ka = std::atan2(k.y - piv.y, k.x - piv.x);
                if (ka > a0 + 1e-4f && ka < a1 - 1e-4f) t.right.push_back(at(FL, k.x, k.y));
            }
            q = hit(a1);
            t.right.push_back(at(FL, q.x, q.y));
            put(std::move(t), walkR * (a1 - a0));
            const glm::vec2 w = piv + walkR * glm::vec2(std::cos(a1), std::sin(a1));
            plan.walkLine.push_back(at(FL, w.x, w.y));
        }
    }

    void lWinders(int count) {
        if (count <= 0) return;
        const glm::vec2 FL = frontLeft();
        fan(FL, {0.0f, 0.0f}, 0.0f, W, W, 0.5f * kPi, count, 0.5f * W, {0.0f, 0.0f},
            {0.0f, 0.0f});
        turnTo(at(FL, 0.0f, 0.5f * W), {-h.y, h.x});
    }

    void uWinders(int count, float eye) {
        if (count <= 0) return;
        const glm::vec2 FL = frontLeft();
        fan(FL, {-0.5f * eye, 0.0f}, -eye - W, W, W, kPi, count, 0.5f * (W + eye),
            {0.0f, 0.0f}, {-eye, 0.0f});
        turnTo(at(FL, -eye - 0.5f * W, 0.0f), -h);
    }
};

// One side of one tread as points with the height of the nosing line there.
// A step's line climbs one rise from its front to its back edge; a landing's
// stays level and climbs on its last stretch only, into the next flight, so
// a stringer or a handrail runs on through a landing without a jump.
struct SidePt {
    glm::vec2 p;
    float     line  = 0.0f;
    float     floor = 0.0f;   // walking surface of the tread this point belongs to
};

std::vector<SidePt> treadSide(const Tread& t, bool left, float r) {
    const std::vector<glm::vec2>& pl = left ? t.left : t.right;
    std::vector<SidePt> out;
    if (pl.empty()) return out;
    std::vector<float> acc(pl.size(), 0.0f);
    for (std::size_t k = 1; k < pl.size(); ++k) acc[k] = acc[k - 1] + glm::length(pl[k] - pl[k - 1]);
    const float total = acc.back();
    for (std::size_t k = 0; k < pl.size(); ++k) {
        const float u = total > 1e-6f ? acc[k] / total : 0.0f;
        float line;
        if (t.arrival)      line = t.top;
        else if (t.landing) line = (k + 1 == pl.size() && pl.size() > 1) ? t.top + r : t.top;
        else                line = t.lineFront + u * r;
        out.push_back({pl[k], line, t.top});
    }
    return out;
}

// The side of the whole stair as continuous runs. Where the side stands still
// while the stair climbs -- a winder pivot, a U landing without an eye -- the
// run breaks and a newel post is noted there instead.
struct Newel {
    glm::vec2 p;
    float     lo = 0.0f, hi = 0.0f;
};
struct Side {
    std::vector<std::vector<SidePt>> runs;
    std::vector<Newel>               newels;
};

Side buildSide(const Plan& plan, bool left) {
    Side s;
    std::vector<SidePt> cur;
    auto flush = [&] {
        if (cur.size() >= 2) s.runs.push_back(cur);
        cur.clear();
    };
    auto newel = [&](const glm::vec2& p, float a, float b) {
        for (Newel& n : s.newels)
            if (glm::length(n.p - p) < 1e-3f) {
                n.lo = std::min({n.lo, a, b});
                n.hi = std::max({n.hi, a, b});
                return;
            }
        s.newels.push_back({p, std::min(a, b), std::max(a, b)});
    };
    for (const Tread& t : plan.treads) {
        for (const SidePt& q : treadSide(t, left, plan.rise)) {
            if (!cur.empty() && glm::length(q.p - cur.back().p) < 1e-4f) {
                if (std::abs(q.line - cur.back().line) < 0.01f) continue;
                newel(q.p, q.line, cur.back().line);
                flush();
            }
            cur.push_back(q);
        }
    }
    flush();
    return s;
}

// Outward of a side (away from the treads): a left side has the treads on its
// right, a right side on its left -- a mirrored plan swapped its sides, so
// that holds for both hands.
glm::vec2 outward(const glm::vec2& d, bool left) {
    return left ? glm::vec2(-d.y, d.x) : glm::vec2(d.y, -d.x);
}

} // namespace

// --- Names -------------------------------------------------------------------

const char* kindName(Kind k) {
    switch (k) {
        case Kind::Straight: return "Straight";
        case Kind::LLanding: return "L landing";
        case Kind::LWinder:  return "L winder";
        case Kind::ULanding: return "U landing";
        case Kind::UWinder:  return "U winder";
        case Kind::Spiral:   return "Spiral";
        case Kind::Curved:   return "Curved";
        default:             return "?";
    }
}
bool isTurning(Kind k) { return k != Kind::Straight; }
bool isRound(Kind k) { return k == Kind::Spiral || k == Kind::Curved; }

const char* structureName(Structure s) {
    switch (s) {
        case Structure::Solid:     return "Solid";
        case Structure::Slab:      return "Slab";
        case Structure::Stringers: return "Stringers";
        case Structure::Spine:     return "Spine";
        case Structure::Floating:  return "Floating";
        default:                   return "?";
    }
}

const char* railStyleName(RailStyle s) {
    switch (s) {
        case RailStyle::Posts:     return "Posts";
        case RailStyle::Balusters: return "Balusters";
        case RailStyle::Glass:     return "Glass";
        case RailStyle::Parapet:   return "Parapet";
        default:                   return "?";
    }
}

const char* sidesName(Sides s) {
    switch (s) {
        case Sides::None:  return "None";
        case Sides::Left:  return "Left";
        case Sides::Right: return "Right";
        case Sides::Both:  return "Both";
        default:           return "?";
    }
}

// --- Looks ---------------------------------------------------------------------

namespace {

struct LookDef {
    const char* name;
    glm::vec3   tread, structure, rail, wall;
    float treadRefl, treadRough, structRefl, structRough, railRefl, railRough;
};

const LookDef kLooks[] = {
    {"Oak",        {0.63f, 0.48f, 0.33f}, {0.13f, 0.13f, 0.14f}, {0.74f, 0.75f, 0.77f},
                   {0.90f, 0.89f, 0.86f}, 0.00f, 0.45f, 0.35f, 0.45f, 0.70f, 0.25f},
    {"Concrete",   {0.60f, 0.59f, 0.56f}, {0.58f, 0.57f, 0.54f}, {0.08f, 0.08f, 0.09f},
                   {0.66f, 0.65f, 0.62f}, 0.00f, 0.85f, 0.00f, 0.90f, 0.30f, 0.50f},
    {"Sandstone",  {0.76f, 0.66f, 0.50f}, {0.70f, 0.61f, 0.47f}, {0.20f, 0.20f, 0.21f},
                   {0.80f, 0.72f, 0.58f}, 0.00f, 0.90f, 0.00f, 0.90f, 0.30f, 0.50f},
    {"Marble",     {0.93f, 0.92f, 0.89f}, {0.90f, 0.89f, 0.86f}, {0.80f, 0.62f, 0.30f},
                   {0.95f, 0.94f, 0.91f}, 0.15f, 0.15f, 0.10f, 0.20f, 0.80f, 0.25f},
    {"White",      {0.78f, 0.66f, 0.50f}, {0.94f, 0.94f, 0.93f}, {0.95f, 0.95f, 0.95f},
                   {0.96f, 0.96f, 0.95f}, 0.00f, 0.40f, 0.05f, 0.50f, 0.10f, 0.40f},
    {"Rustic",     {0.42f, 0.29f, 0.18f}, {0.36f, 0.24f, 0.15f}, {0.40f, 0.27f, 0.17f},
                   {0.82f, 0.78f, 0.70f}, 0.00f, 0.60f, 0.00f, 0.65f, 0.00f, 0.55f},
    {"Industrial", {0.30f, 0.31f, 0.32f}, {0.10f, 0.10f, 0.11f}, {0.62f, 0.64f, 0.66f},
                   {0.55f, 0.30f, 0.22f}, 0.40f, 0.50f, 0.35f, 0.50f, 0.60f, 0.35f},
    {"Custom",     {0.63f, 0.48f, 0.33f}, {0.13f, 0.13f, 0.14f}, {0.74f, 0.75f, 0.77f},
                   {0.90f, 0.89f, 0.86f}, 0.00f, 0.50f, 0.20f, 0.50f, 0.50f, 0.35f},
};
static_assert(sizeof(kLooks) / sizeof(kLooks[0]) == static_cast<int>(Look::Count));

} // namespace

const char* lookName(Look l) { return kLooks[static_cast<int>(clampEnum(l))].name; }

void applyLook(Params& p, Look l) {
    p.look = clampEnum(l);
    if (p.look == Look::Custom) return;
    const LookDef& d = kLooks[static_cast<int>(p.look)];
    p.treadColor     = d.tread;
    p.structureColor = d.structure;
    p.railColor      = d.rail;
    p.wallColor      = d.wall;
}

// --- Presets -------------------------------------------------------------------

const char* presetName(Preset p) {
    switch (p) {
        case Preset::Cellar:     return "Cellar";
        case Preset::Straight:   return "Straight";
        case Preset::LLanding:   return "L landing";
        case Preset::LWinder:    return "L winder";
        case Preset::ULanding:   return "U landing";
        case Preset::UWinder:    return "U winder";
        case Preset::Spiral:     return "Spiral";
        case Preset::Curved:     return "Curved";
        case Preset::Floating:   return "Floating";
        case Preset::Outdoor:    return "Outdoor";
        case Preset::FireEscape: return "Fire escape";
        case Preset::Grand:      return "Grand";
        default:                 return "?";
    }
}

const char* presetHint(Preset p) {
    switch (p) {
        case Preset::Cellar:     return "Straight concrete stair between two walls, handrail on the wall.";
        case Preset::Straight:   return "Straight timber stair between stringers, balusters both sides.";
        case Preset::LLanding:   return "Quarter turn on a square landing, oak on steel stringers.";
        case Preset::LWinder:    return "Quarter turn on winders along two walls -- the classic house stair.";
        case Preset::ULanding:   return "Half turn on a landing, concrete slab, glass at the eye.";
        case Preset::UWinder:    return "Half turn on winders, timber, compact.";
        case Preset::Spiral:     return "Steel spiral round a column, platform at the top.";
        case Preset::Curved:     return "Sweeping marble stair round an open eye.";
        case Preset::Floating:   return "Cantilevered oak treads out of a white wall, glass railing.";
        case Preset::Outdoor:    return "Wide, shallow sandstone steps for a terrace or an entrance.";
        case Preset::FireEscape: return "Open steel half-turn stair with landings and a top platform.";
        case Preset::Grand:      return "Wide marble stair with a landing half way, balusters.";
        default:                 return "";
    }
}

void applyPreset(Params& p, Preset preset) {
    const bool collider = p.collider;
    p = Params{};
    p.collider = collider;
    switch (preset) {
        case Preset::Cellar:
            p.kind = Kind::Straight; p.height = 2.60f; p.width = 0.90f; p.targetRise = 0.185f;
            p.structure = Structure::Slab; p.stringerDepth = 0.18f;
            p.railSides = Sides::Right; p.railStyle = RailStyle::Posts;
            p.wallSides = Sides::Both; p.wallHeight = 2.20f;
            applyLook(p, Look::Concrete);
            break;
        case Preset::Straight:
            p.kind = Kind::Straight; p.height = 2.80f; p.width = 0.90f;
            p.structure = Structure::Stringers; p.railSides = Sides::Both;
            p.railStyle = RailStyle::Balusters;
            applyLook(p, Look::Rustic);
            break;
        case Preset::LLanding:
            p.kind = Kind::LLanding; p.height = 2.80f; p.width = 1.00f;
            p.structure = Structure::Stringers; p.stringerWidth = 0.08f;
            p.railSides = Sides::Both; p.railStyle = RailStyle::Posts;
            applyLook(p, Look::OakSteel);
            break;
        case Preset::LWinder:
            p.kind = Kind::LWinder; p.height = 2.75f; p.width = 0.90f; p.winders = 3;
            p.structure = Structure::Stringers; p.closedRisers = true;
            p.railSides = Sides::Left; p.railStyle = RailStyle::Balusters;
            p.wallSides = Sides::Right;
            applyLook(p, Look::WhiteModern);
            break;
        case Preset::ULanding:
            p.kind = Kind::ULanding; p.height = 3.00f; p.width = 1.10f;
            p.landingDepth = 1.20f; p.eye = 0.25f;
            p.structure = Structure::Slab; p.stringerDepth = 0.20f;
            p.railSides = Sides::Left; p.railStyle = RailStyle::Glass;
            p.wallSides = Sides::Right;
            applyLook(p, Look::Concrete);
            break;
        case Preset::UWinder:
            p.kind = Kind::UWinder; p.height = 2.80f; p.width = 0.90f; p.eye = 0.12f;
            p.winders = 3; p.structure = Structure::Stringers;
            p.railSides = Sides::Both; p.railStyle = RailStyle::Balusters;
            applyLook(p, Look::Rustic);
            break;
        case Preset::Spiral:
            p.kind = Kind::Spiral; p.height = 2.80f; p.width = 0.75f; p.innerRadius = 0.10f;
            p.targetRise = 0.19f; p.column = true; p.topPlatform = 0.70f;
            p.structure = Structure::Floating; p.treadThickness = 0.06f;
            p.railSides = Sides::Right; p.railStyle = RailStyle::Balusters;
            applyLook(p, Look::Industrial);
            break;
        case Preset::Curved:
            p.kind = Kind::Curved; p.height = 3.20f; p.width = 1.30f; p.innerRadius = 0.90f;
            p.column = false; p.structure = Structure::Slab; p.stringerDepth = 0.20f;
            p.railSides = Sides::Both; p.railStyle = RailStyle::Balusters;
            applyLook(p, Look::Marble);
            break;
        case Preset::Floating:
            p.kind = Kind::Straight; p.height = 2.80f; p.width = 0.90f;
            p.structure = Structure::Floating; p.treadThickness = 0.08f; p.nosing = 0.0f;
            p.wallSides = Sides::Left; p.wallHeight = 2.40f;
            p.railSides = Sides::Right; p.railStyle = RailStyle::Glass;
            applyLook(p, Look::WhiteModern);
            break;
        case Preset::Outdoor:
            p.kind = Kind::Straight; p.height = 0.90f; p.width = 3.00f; p.targetRise = 0.15f;
            p.structure = Structure::Solid; p.nosing = 0.0f;
            p.railSides = Sides::None; p.topPlatform = 1.50f;
            applyLook(p, Look::Sandstone);
            break;
        case Preset::FireEscape:
            p.kind = Kind::ULanding; p.height = 3.20f; p.width = 0.90f; p.eye = 0.10f;
            p.landingDepth = 1.00f; p.targetRise = 0.19f; p.topPlatform = 1.00f;
            p.structure = Structure::Stringers; p.stringerWidth = 0.05f; p.stringerDepth = 0.25f;
            p.railSides = Sides::Both; p.railStyle = RailStyle::Posts;
            applyLook(p, Look::Industrial);
            break;
        case Preset::Grand:
            p.kind = Kind::Straight; p.height = 4.00f; p.width = 2.40f; p.targetRise = 0.16f;
            p.midLanding = true; p.landingDepth = 1.60f;
            p.structure = Structure::Slab; p.stringerDepth = 0.22f;
            p.railSides = Sides::Both; p.railStyle = RailStyle::Balusters; p.railHeight = 1.00f;
            applyLook(p, Look::Marble);
            break;
        default:
            break;
    }
}

// --- Layout ----------------------------------------------------------------------

std::vector<glm::vec2> Tread::polygon() const {
    std::vector<glm::vec2> ring(right.begin(), right.end());
    for (auto it = left.rbegin(); it != left.rend(); ++it) ring.push_back(*it);
    std::vector<glm::vec2> clean;
    for (const glm::vec2& v : ring)
        if (clean.empty() || glm::length(v - clean.back()) > 1e-5f) clean.push_back(v);
    while (clean.size() > 1 && glm::length(clean.front() - clean.back()) <= 1e-5f) clean.pop_back();
    if (signedArea(clean) < 0.0f) std::reverse(clean.begin(), clean.end());
    return clean;
}

Plan layout(const Params& in) {
    Plan plan;
    plan.params = sane(in);
    const Params& p = plan.params;
    const int   n = riserCount(p);
    const float r = p.height / static_cast<float>(n);
    const float g = p.autoGoing ? clampf(0.63f - 2.0f * r, 0.21f, 0.37f) : p.going;
    const int   T = n - 1;   // tread positions under the top (landings included)
    const float W = p.width;
    plan.risers = n;
    plan.rise   = r;
    plan.going  = g;

    Turtle t{plan, W, r, g};
    plan.walkLine.push_back({0.0f, 0.0f});
    auto before = [&](int avail) {   // treads before the turn
        if (avail <= 0) return 0;
        const int k = p.splitStep > 0 ? p.splitStep : (avail + 1) / 2;
        return std::clamp(k, 0, avail);
    };
    float winderGoing = 0.0f;

    switch (p.kind) {
        case Kind::Straight:
            if (p.midLanding && T >= 3) {
                const int k = before(T - 1);
                t.flight(k);
                t.straightLanding(p.landingDepth, false);
                t.flight(T - 1 - k);
            } else {
                t.flight(T);
            }
            break;
        case Kind::LLanding: {
            const int k = before(T - 1);
            t.flight(k);
            t.cornerLanding();
            t.flight(T - 1 - k);
            break;
        }
        case Kind::LWinder: {
            const int w = std::min(p.winders, T);
            const int k = before(T - w);
            t.flight(k);
            t.lWinders(w);
            t.flight(T - w - k);
            if (w > 0) winderGoing = 0.25f * kPi * W / static_cast<float>(w);
            break;
        }
        case Kind::ULanding: {
            const int k = before(T - 1);
            t.flight(k);
            t.uLanding(p.landingDepth, p.eye);
            t.flight(T - 1 - k);
            break;
        }
        case Kind::UWinder: {
            const int w = std::min(2 * p.winders, T);
            const int k = before(T - w);
            t.flight(k);
            t.uWinders(w, p.eye);
            t.flight(T - w - k);
            if (w > 0) winderGoing = 0.5f * kPi * (W + p.eye) / static_cast<float>(w);
            break;
        }
        case Kind::Spiral:
        case Kind::Curved: {
            const float ri = p.innerRadius, ro = ri + W, rw = ri + 0.5f * W;
            const float dth = p.autoTurn ? g / rw
                                         : glm::radians(p.turn) / static_cast<float>(std::max(T, 1));
            const glm::vec2 C(-rw, 0.0f);
            auto arc = [&](float rad, float a0, float a1, int m) {
                std::vector<glm::vec2> pts;
                for (int j = 0; j <= m; ++j) {
                    const float a = a0 + (a1 - a0) * static_cast<float>(j) / static_cast<float>(m);
                    pts.push_back(C + rad * glm::vec2(std::cos(a), std::sin(a)));
                }
                return pts;
            };
            auto wedge = [&](float a0, float a1, bool landing) {
                const int m = std::max(1, static_cast<int>(std::ceil((a1 - a0) / glm::radians(10.0f))));
                Tread tr;
                // The inner side as a chord: an arc there would make the tread
                // concave, and every tread is convex (see Tread).
                tr.left    = arc(ri, a0, a1, 1);
                tr.right   = arc(ro, a0, a1, m);
                tr.landing = tr.arrival = landing;
                t.put(std::move(tr), rw * (a1 - a0));
                for (const glm::vec2& q : arc(rw, a0, a1, m)) plan.walkLine.push_back(q);
            };
            for (int i = 0; i < T; ++i)
                wedge(dth * static_cast<float>(i), dth * static_cast<float>(i + 1), false);
            if (p.topPlatform > 0.01f) {
                const float a = dth * static_cast<float>(T);
                wedge(a, a + p.topPlatform / rw, true);
            }
            plan.going       = dth * rw;
            plan.turnDeg     = glm::degrees(dth * static_cast<float>(T));
            plan.walkRadius  = rw;
            plan.outerRadius = ro;
            plan.centre      = C;
            break;
        }
        default:
            break;
    }
    if (!isRound(p.kind) && p.topPlatform > 0.01f) t.straightLanding(p.topPlatform, true);
    plan.run = 0.0f;
    for (const Tread& tr : plan.treads)
        if (!tr.arrival) plan.run = tr.walkBack;

    // A right-hand stair is the left-hand one in a mirror.
    if (p.turnRight && isTurning(p.kind)) {
        auto mx = [](std::vector<glm::vec2>& v) { for (glm::vec2& q : v) q.x = -q.x; };
        for (Tread& tr : plan.treads) {
            mx(tr.left);
            mx(tr.right);
            std::swap(tr.left, tr.right);
        }
        mx(plan.walkLine);
        plan.centre.x = -plan.centre.x;
    }
    // Sides asked for in the canonical (left-turning) frame -- the inner side
    // is "left" there -- resolved to the sides of the plan as built.
    const bool mirrored = p.turnRight && isTurning(p.kind);
    auto wants = [&](Sides s, bool leftSide) {
        const bool canonLeft = mirrored ? !leftSide : leftSide;
        return s == Sides::Both || (s == Sides::Left && canonLeft) ||
               (s == Sides::Right && !canonLeft);
    };
    plan.railLeft  = wants(p.railSides, true);
    plan.railRight = wants(p.railSides, false);
    plan.wallLeft  = wants(p.wallSides, true);
    plan.wallRight = wants(p.wallSides, false);

    plan.bbMin = glm::vec2(1e9f);
    plan.bbMax = glm::vec2(-1e9f);
    for (const Tread& tr : plan.treads)
        for (const glm::vec2& q : tr.polygon()) {
            plan.bbMin = glm::min(plan.bbMin, q);
            plan.bbMax = glm::max(plan.bbMax, q);
        }
    if (plan.treads.empty()) plan.bbMin = plan.bbMax = glm::vec2(0.0f);
    plan.pitch    = glm::degrees(std::atan2(r, plan.going));
    plan.stepRule = 2.0f * r + plan.going;

    // --- Checks: problems first, then information.
    std::vector<std::string> warn, info;
    const float cm = 100.0f;
    if (plan.stepRule < 0.59f || plan.stepRule > 0.65f)
        warn.push_back(fmt("Step rule 2R + G = %.1f cm, outside 59-65 cm: the stride feels "
                           "wrong. Change the going or the riser count.", plan.stepRule * cm));
    if (r > 0.20f)
        warn.push_back(fmt("Rise %.1f cm: steep for a building stair (20 cm at most).", r * cm));
    if (plan.going < 0.23f - 1e-3f)
        warn.push_back(fmt("Going %.1f cm on the walking line: under the 23 cm a foot needs.",
                           plan.going * cm));
    if (plan.pitch > 45.0f)
        warn.push_back(fmt("Pitch %.0f\xC2\xB0: a ladder rather than a stair.", plan.pitch));
    const float minW = p.kind == Kind::Spiral ? 0.60f : 0.80f;
    if (W < minW - 1e-3f)
        warn.push_back(fmt("Width %.2f m: under the %.2f m a stair in a building needs.", W, minW));
    {
        int run = 0, worst = 0;
        for (const Tread& tr : plan.treads) {
            if (tr.landing) { run = 0; continue; }
            worst = std::max(worst, ++run);
        }
        if (worst + 1 > 18)
            warn.push_back(fmt("%.0f risers in one flight: more than 18 want a landing between "
                               "them.", worst + 1));
    }
    if (p.kind == Kind::ULanding && p.landingDepth < W - 1e-3f)
        warn.push_back(fmt("Landing %.2f m deep: shallower than the %.2f m the flights are wide.",
                           p.landingDepth, W));
    if (isRound(p.kind) && T > 0) {
        const float perTurn = 2.0f * kPi / std::max(1e-4f, glm::radians(plan.turnDeg) / static_cast<float>(T));
        const float headroom = perTurn * r - p.treadThickness;
        if (plan.turnDeg > 360.0f && headroom < 2.0f)
            warn.push_back(fmt("Headroom %.2f m under the turn above (2.00 m needed): make the "
                               "stair wider or the going longer.", headroom));
    }
    if (p.railSides == Sides::None && p.wallSides == Sides::None && p.height > 1.0f)
        warn.push_back(fmt("Open on both sides with %.2f m to fall: add a railing or a wall.",
                           p.height));
    if (p.structure == Structure::Floating && p.wallSides == Sides::None &&
        !(isRound(p.kind) && p.column))
        warn.push_back("Floating treads need something to hang from: a wall on one side "
                       "(or, round, the column).");
    if (isTurning(p.kind) && !isRound(p.kind) && p.splitStep > 0) {
        const int avail = T - (p.kind == Kind::LWinder ? p.winders
                             : p.kind == Kind::UWinder ? 2 * p.winders : 1);
        if (p.splitStep > avail)
            info.push_back(fmt("Only %.0f treads to share out: the turn comes after the last one.",
                               std::max(avail, 0)));
    }
    plan.warnings = static_cast<int>(warn.size());

    info.push_back(fmt("Safety rule R + G = %.1f cm (46 ideal), comfort rule G - R = %.1f cm "
                       "(12 ideal).", (r + plan.going) * cm, (plan.going - r) * cm));
    if (winderGoing > 0.0f)
        info.push_back(fmt("Winders: %.1f cm going on the walking line, %.0f cm at the outer "
                           "corner.", winderGoing * cm,
                           (p.kind == Kind::LWinder ? 2.0f * W / p.winders
                                                    : (3.0f * W + p.eye) / (2.0f * p.winders)) * cm));
    if (isRound(p.kind))
        info.push_back(fmt("Turns %.0f\xC2\xB0 in %.0f treads, %.1f\xC2\xB0 each.", plan.turnDeg,
                           T, plan.turnDeg / std::max(T, 1)));
    if (p.structure == Structure::Floating || p.structure == Structure::Spine)
        info.push_back("Board treads: the nosing and the tread thickness show from the side.");

    plan.notes = std::move(warn);
    plan.notes.insert(plan.notes.end(), info.begin(), info.end());
    return plan;
}

// --- Materials ---------------------------------------------------------------

namespace {

AssetId ensureMaterial(std::vector<MaterialDef>& mats, const std::string& name,
                       glm::vec3 albedo, float refl, float rough, bool glass = false,
                       float opacity = 1.0f) {
    for (MaterialDef& m : mats) {
        if (m.name != name) continue;
        m.albedo = albedo; m.reflectivity = refl; m.roughness = rough;
        m.glass = glass; m.opacity = opacity;
        if (!m.assetId.valid()) m.assetId = AssetId::generate();
        return m.assetId;
    }
    MaterialDef md;
    md.assetId      = AssetId::generate();
    md.name         = name;
    md.albedo       = albedo;
    md.reflectivity = refl;
    md.roughness    = rough;
    md.glass        = glass;
    md.opacity      = opacity;
    mats.push_back(md);
    return md.assetId;
}

} // namespace

Palette ensurePalette(std::vector<MaterialDef>& mats, const Params& pIn) {
    const Params p = sane(pIn);
    const LookDef& d = kLooks[static_cast<int>(p.look)];
    const std::string set = std::string("Stair ") + d.name + " ";
    Palette pal;
    pal.tread     = ensureMaterial(mats, set + "Tread", p.treadColor, d.treadRefl, d.treadRough);
    pal.structure = ensureMaterial(mats, set + "Structure", p.structureColor, d.structRefl,
                                   d.structRough);
    pal.rail      = ensureMaterial(mats, set + "Rail", p.railColor, d.railRefl, d.railRough);
    pal.wall      = ensureMaterial(mats, set + "Wall", p.wallColor, 0.0f, 0.90f);
    // The project's default glass: Fresnel alpha from the flag, clear head-on.
    pal.glass     = ensureMaterial(mats, "Stair Glass", {0.85f, 0.92f, 0.95f}, 0.5f, 0.03f,
                                   true, 0.28f);
    return pal;
}

// --- Geometry ----------------------------------------------------------------

namespace {

// Collects faces into one EditMesh, taking PLAN coordinates (x right, y up the
// first flight, z up) and writing model ones (X, Y up, Z = -y).
struct Builder {
    EditMesh m;

    static glm::vec3 P(const glm::vec2& q, float z) { return {q.x, z, -q.y}; }

    // One polygon, wound as given (counter-clockwise seen from outside).
    void face(const std::vector<glm::vec3>& pts, const AssetId& mat) {
        std::vector<glm::vec3> clean;
        for (const glm::vec3& v : pts)
            if (clean.empty() || glm::length(v - clean.back()) > 1e-5f) clean.push_back(v);
        while (clean.size() > 1 && glm::length(clean.front() - clean.back()) <= 1e-5f)
            clean.pop_back();
        if (clean.size() < 3) return;
        glm::vec3 n(0.0f);
        for (std::size_t i = 0; i < clean.size(); ++i) {
            const glm::vec3& a = clean[i];
            const glm::vec3& b = clean[(i + 1) % clean.size()];
            n += glm::vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x),
                           (a.x - b.x) * (a.y + b.y));
        }
        if (glm::dot(n, n) < 1e-14f) return;
        std::vector<int> f;
        for (const glm::vec3& v : clean) {
            f.push_back(static_cast<int>(m.verts.size()));
            m.verts.push_back(v);
        }
        m.faces.push_back(std::move(f));
        m.faceMat.push_back(mat);
    }

    // A convex prism over a plan ring, bottom and top heights per corner. The
    // ring may come either way round; it is turned counter-clockwise here,
    // which makes the top face up, the bottom down and the sides outward.
    void prism(std::vector<glm::vec2> ring, std::vector<float> zb, std::vector<float> zt,
               const AssetId& top, const AssetId& side, const AssetId& bottom,
               bool withBottom = true) {
        std::vector<glm::vec2> r;
        std::vector<float>     b, t;
        for (std::size_t i = 0; i < ring.size(); ++i) {
            if (!r.empty() && glm::length(ring[i] - r.back()) < 1e-5f) continue;
            r.push_back(ring[i]);
            b.push_back(std::min(zb[i], zt[i]));
            t.push_back(zt[i]);
        }
        while (r.size() > 1 && glm::length(r.front() - r.back()) < 1e-5f) {
            r.pop_back(); b.pop_back(); t.pop_back();
        }
        if (r.size() < 3 || std::abs(signedArea(r)) < 1e-7f) return;
        if (signedArea(r) < 0.0f) {
            std::reverse(r.begin(), r.end());
            std::reverse(b.begin(), b.end());
            std::reverse(t.begin(), t.end());
        }
        const std::size_t n = r.size();
        std::vector<glm::vec3> f;
        for (std::size_t i = 0; i < n; ++i) f.push_back(P(r[i], t[i]));
        face(f, top);
        if (withBottom) {
            f.clear();
            for (std::size_t i = n; i-- > 0;) f.push_back(P(r[i], b[i]));
            face(f, bottom);
        }
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i + 1) % n;
            face({P(r[i], b[i]), P(r[j], b[j]), P(r[j], t[j]), P(r[i], t[i])}, side);
        }
    }

    void prism(const std::vector<glm::vec2>& ring, float z0, float z1, const AssetId& mat) {
        prism(ring, std::vector<float>(ring.size(), z0), std::vector<float>(ring.size(), z1),
              mat, mat, mat);
    }

    // A bar of w x hh section from (a, za) to (b, zb), its sides vertical.
    void beam(glm::vec2 a, float za, glm::vec2 b, float zb, float w, float hh,
              const AssetId& mat, float ext = 0.0f) {
        glm::vec2 d = b - a;
        const float L = glm::length(d);
        if (L < 1e-4f) return;
        d /= L;
        if (ext > 0.0f) {
            const float s = (zb - za) / L;
            a -= d * ext; za -= s * ext;
            b += d * ext; zb += s * ext;
        }
        const glm::vec2 nrm(d.y, -d.x);
        const float hw = 0.5f * w, hh2 = 0.5f * hh;
        prism({a + nrm * hw, b + nrm * hw, b - nrm * hw, a - nrm * hw},
              {za - hh2, zb - hh2, zb - hh2, za - hh2}, {za + hh2, zb + hh2, zb + hh2, za + hh2},
              mat, mat, mat);
    }

    void post(const glm::vec2& c, float s, float z0, float z1, const AssetId& mat) {
        if (z1 - z0 < 1e-3f) return;
        const float h = 0.5f * s;
        prism({c + glm::vec2(-h, -h), c + glm::vec2(h, -h), c + glm::vec2(h, h),
               c + glm::vec2(-h, h)}, z0, z1, mat);
    }

    void cylinder(const glm::vec2& c, float rad, float z0, float z1, int segs, const AssetId& mat) {
        std::vector<glm::vec2> ring;
        for (int i = 0; i < segs; ++i) {
            const float a = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(segs);
            ring.push_back(c + rad * glm::vec2(std::cos(a), std::sin(a)));
        }
        prism(ring, z0, z1, mat);
    }
};

// A strip along a side run, `off0`..`off1` outward of it, its bottom and top
// worked out per point from the nosing line and the floor there. Convex
// corners get a wedge so the strip does not gape where it turns.
template <class Lo, class Hi>
void band(Builder& b, const std::vector<SidePt>& run, bool left, float off0, float off1,
          Lo zLo, Hi zHi, const AssetId& mat, bool corners = true) {
    for (std::size_t i = 0; i + 1 < run.size(); ++i) {
        const SidePt& a = run[i];
        const SidePt& c = run[i + 1];
        glm::vec2 d = c.p - a.p;
        const float L = glm::length(d);
        if (L < 1e-4f) continue;
        d /= L;
        const glm::vec2 o = outward(d, left);
        const float fl = c.floor;
        b.prism({a.p + o * off0, c.p + o * off0, c.p + o * off1, a.p + o * off1},
                {zLo(a.line, fl), zLo(c.line, fl), zLo(c.line, fl), zLo(a.line, fl)},
                {zHi(a.line, fl), zHi(c.line, fl), zHi(c.line, fl), zHi(a.line, fl)},
                mat, mat, mat);
        if (!corners || i + 2 >= run.size()) continue;
        glm::vec2 d2 = run[i + 2].p - c.p;
        if (glm::length(d2) < 1e-4f) continue;
        d2 = glm::normalize(d2);
        const glm::vec2 o2 = outward(d2, left);
        if (glm::dot(o2, d) <= 1e-3f) continue;   // concave: the strips overlap
        b.prism({c.p + o * off0, c.p + o * off1, c.p + o2 * off1, c.p + o2 * off0},
                zLo(c.line, fl), zHi(c.line, fl), mat);
    }
}

// A point `s` metres along a run, with its nosing line and floor.
SidePt alongRun(const std::vector<SidePt>& run, float s, glm::vec2* dir = nullptr) {
    for (std::size_t i = 0; i + 1 < run.size(); ++i) {
        const float L = glm::length(run[i + 1].p - run[i].p);
        if (s <= L || i + 2 == run.size()) {
            const float f = L > 1e-6f ? clampf(s / L, 0.0f, 1.0f) : 0.0f;
            if (dir && L > 1e-6f) *dir = (run[i + 1].p - run[i].p) / L;
            return {glm::mix(run[i].p, run[i + 1].p, f),
                    glm::mix(run[i].line, run[i + 1].line, f), run[i + 1].floor};
        }
        s -= L;
    }
    return run.front();
}

float runLength(const std::vector<SidePt>& run) {
    float s = 0.0f;
    for (std::size_t i = 0; i + 1 < run.size(); ++i) s += glm::length(run[i + 1].p - run[i].p);
    return s;
}

struct Meshes {
    Builder steps, structure, railing, glass, walls;
};

Meshes buildMeshes(const Plan& plan, const Palette& pal) {
    Meshes ms;
    const Params& p = plan.params;
    const float r = plan.rise;
    const Structure st = p.structure;
    const bool boards   = st == Structure::Stringers || st == Structure::Spine ||
                          st == Structure::Floating;
    const float tt      = st == Structure::Floating ? std::max(p.treadThickness, 0.06f)
                                                    : p.treadThickness;
    const float slabT   = p.stringerDepth;
    const float D       = std::max(p.stringerDepth, r + tt + 0.08f);   // stringer / spine
    const float sw      = p.stringerWidth;
    const float railH   = p.railHeight;

    // --- Steps.
    for (const Tread& t : plan.treads) {
        const std::vector<SidePt> R = treadSide(t, false, r), L = treadSide(t, true, r);
        std::vector<glm::vec2> ring;
        std::vector<float>     line;
        for (const SidePt& q : R) { ring.push_back(q.p); line.push_back(q.line); }
        for (auto it = L.rbegin(); it != L.rend(); ++it) { ring.push_back(it->p); line.push_back(it->line); }
        const std::size_t n = ring.size();
        if (st == Structure::Solid) {
            ms.steps.prism(ring, std::vector<float>(n, 0.0f), std::vector<float>(n, t.top),
                           pal.tread, pal.structure, pal.structure);
        } else if (st == Structure::Slab) {
            std::vector<float> zb(n);
            for (std::size_t i = 0; i < n; ++i)
                zb[i] = clampf(line[i] - r - slabT, 0.0f, t.top - 0.02f);
            ms.steps.prism(ring, zb, std::vector<float>(n, t.top), pal.tread, pal.structure,
                           pal.structure);
        } else {
            // A board, its front edge pushed out by the nosing (a winder's pivot
            // stays where it is: it is also the back edge's end).
            Tread nosed = t;
            const glm::vec2 fa = t.left.front(), fb = t.right.front();
            glm::vec2 e = fb - fa;
            if (p.nosing > 0.0f && glm::length(e) > 1e-4f) {
                e = glm::normalize(e);
                glm::vec2 nf(e.y, -e.x);
                glm::vec2 cen(0.0f);
                const auto poly = t.polygon();
                for (const glm::vec2& q : poly) cen += q;
                cen /= static_cast<float>(std::max<std::size_t>(poly.size(), 1));
                if (glm::dot(nf, cen - 0.5f * (fa + fb)) > 0.0f) nf = -nf;
                nosed.right.front() += nf * p.nosing;
                if (nosed.left.size() > 1) nosed.left.front() += nf * p.nosing;
            }
            ms.steps.prism(nosed.polygon(), t.top - tt, t.top, pal.tread);
        }
        // Riser boards: under the front edge, from the tread below to this one.
        if (boards && p.closedRisers) {
            const glm::vec2 fa = t.left.front(), fb = t.right.front();
            glm::vec2 cen(0.0f);
            const auto poly = t.polygon();
            for (const glm::vec2& q : poly) cen += q;
            cen /= static_cast<float>(std::max<std::size_t>(poly.size(), 1));
            glm::vec2 e = fb - fa;
            if (glm::length(e) > 1e-4f) {
                e = glm::normalize(e);
                glm::vec2 ni(e.y, -e.x);
                if (glm::dot(ni, cen - 0.5f * (fa + fb)) < 0.0f) ni = -ni;
                ms.steps.prism({fa, fb, fb + ni * 0.02f, fa + ni * 0.02f}, t.top - r,
                               t.top - tt, pal.tread);
            }
        }
    }

    // --- Spine: one beam under the middle of every tread.
    if (st == Structure::Spine) {
        const float sp = std::max(0.12f, 2.0f * sw);
        for (const Tread& t : plan.treads) {
            const glm::vec2 fm = 0.5f * (t.left.front() + t.right.front());
            const glm::vec2 bm = 0.5f * (t.left.back() + t.right.back());
            const float lf = t.top, lb = t.arrival ? t.top : t.top + r;
            const float zt = t.top - tt;
            auto piece = [&](glm::vec2 a, float la, glm::vec2 b, float lb2) {
                glm::vec2 d = b - a;
                const float L = glm::length(d);
                if (L < 1e-4f) return;
                d /= L;
                a -= d * (0.5f * sp);
                b += d * (0.5f * sp);
                const glm::vec2 nrm(d.y, -d.x);
                const float za = clampf(la - D, 0.0f, zt - 0.02f), zb = clampf(lb2 - D, 0.0f, zt - 0.02f);
                ms.structure.prism({a + nrm * (0.5f * sp), b + nrm * (0.5f * sp),
                                    b - nrm * (0.5f * sp), a - nrm * (0.5f * sp)},
                                   {za, zb, zb, za}, {zt, zt, zt, zt},
                                   pal.structure, pal.structure, pal.structure);
            };
            if (t.landing) {
                glm::vec2 cen(0.0f);
                const auto poly = t.polygon();
                for (const glm::vec2& q : poly) cen += q;
                cen /= static_cast<float>(std::max<std::size_t>(poly.size(), 1));
                piece(fm, lf, cen, 0.5f * (lf + lb));
                piece(cen, 0.5f * (lf + lb), bm, lb);
            } else {
                piece(fm, lf, bm, lb);
            }
        }
    }

    // --- Column of a round stair.
    if (isRound(p.kind) && p.column) {
        const float top = p.height + (p.railSides != Sides::None ? railH : 0.15f);
        ms.structure.cylinder(plan.centre, std::max(0.04f, p.innerRadius - 0.005f), 0.0f, top,
                              20, pal.structure);
    }

    // --- Sides: stringers, walls, railings.
    for (int si = 0; si < 2; ++si) {
        const bool left = si == 0;
        const bool wall = left ? plan.wallLeft : plan.wallRight;
        const bool rail = left ? plan.railLeft : plan.railRight;
        const Side side = buildSide(plan, left);
        const bool stringers = st == Structure::Stringers && !wall;

        if (stringers) {
            for (const auto& run : side.runs)
                band(ms.structure, run, left, 0.0f, sw,
                     [&](float l, float) { return std::max(0.0f, l - D); },
                     [&](float l, float) { return l + kNosingRim; }, pal.structure);
        }
        if (wall) {
            for (const auto& run : side.runs)
                band(ms.walls, run, left, 0.0f, p.wallThickness,
                     [](float, float) { return 0.0f; },
                     [&](float l, float) { return l + p.wallHeight; }, pal.wall);
        }

        // Newels where the side stands still: a post the winders hang on.
        for (const Newel& nw : side.newels) {
            if (rail && !wall && (p.railStyle == RailStyle::Posts ||
                                  p.railStyle == RailStyle::Balusters)) {
                const float z0 = stringers ? std::max(0.0f, nw.lo - D) : nw.lo;
                ms.railing.post(nw.p, 0.09f, z0, nw.hi + railH + 0.03f, pal.rail);
            } else if (stringers) {
                ms.structure.post(nw.p, std::max(0.09f, 1.5f * sw), std::max(0.0f, nw.lo - D),
                                  nw.hi + kNosingRim, pal.structure);
            }
        }
        if (!rail) continue;

        if (wall) {
            // A handrail on brackets off the wall.
            for (const auto& run : side.runs) {
                for (std::size_t i = 0; i + 1 < run.size(); ++i) {
                    const glm::vec2 d = run[i + 1].p - run[i].p;
                    if (glm::length(d) < 1e-4f) continue;
                    const glm::vec2 o = outward(glm::normalize(d), left);
                    ms.railing.beam(run[i].p - o * 0.075f, run[i].line + railH - 0.025f,
                                    run[i + 1].p - o * 0.075f, run[i + 1].line + railH - 0.025f,
                                    0.045f, 0.045f, pal.rail, 0.02f);
                }
                const float S = runLength(run);
                const int nb = std::max(2, static_cast<int>(std::ceil(S / 1.2f)) + 1);
                for (int k = 0; k < nb; ++k) {
                    glm::vec2 d(0.0f, 1.0f);
                    const SidePt q = alongRun(run, S * static_cast<float>(k) / (nb - 1), &d);
                    const glm::vec2 o = outward(d, left);
                    ms.railing.beam(q.p, q.line + railH - 0.06f, q.p - o * 0.075f,
                                    q.line + railH - 0.06f, 0.015f, 0.015f, pal.rail);
                }
            }
            continue;
        }

        const RailStyle rs = p.railStyle;
        // Where the railing stands relative to the edge of the treads: on the
        // stringer, on the treads, or just outside them (glass and parapets are
        // fixed to the side).
        float off;
        if (rs == RailStyle::Glass)        off = (stringers ? sw : 0.0f) + 0.015f;
        else if (rs == RailStyle::Parapet) off = (stringers ? sw : 0.0f) + 0.06f;
        else                               off = stringers ? 0.5f * sw : -0.04f;
        auto base = [&](float l, float floor) { return stringers ? l + kNosingRim : floor; };
        const float under = st == Structure::Slab ? slabT : tt;

        for (const auto& run : side.runs) {
            if (rs == RailStyle::Parapet) {
                band(ms.walls, run, left, off - 0.06f, off + 0.06f,
                     [&](float l, float) {
                         return st == Structure::Solid ? 0.0f : std::max(0.0f, l - r - under);
                     },
                     [&](float l, float) { return l + railH; }, pal.wall);
                continue;
            }
            if (rs == RailStyle::Glass) {
                band(ms.glass, run, left, off - 0.006f, off + 0.006f,
                     [&](float l, float) { return std::max(0.0f, l - r - 0.02f); },
                     [&](float l, float) { return l + railH - 0.05f; }, pal.glass, false);
            }
            // Handrail (and, for posts, two rails under it).
            for (std::size_t i = 0; i + 1 < run.size(); ++i) {
                const SidePt& a = run[i];
                const SidePt& c = run[i + 1];
                const glm::vec2 dv = c.p - a.p;
                if (glm::length(dv) < 1e-4f) continue;
                const glm::vec2 o = outward(glm::normalize(dv), left);
                const glm::vec2 pa = a.p + o * off, pc = c.p + o * off;
                ms.railing.beam(pa, a.line + railH - 0.025f, pc, c.line + railH - 0.025f, 0.05f,
                                0.05f, pal.rail, 0.025f);
                if (rs == RailStyle::Posts)
                    for (float k : {0.36f, 0.68f})
                        ms.railing.beam(pa, a.line + railH * k, pc, c.line + railH * k, 0.025f,
                                        0.025f, pal.rail, 0.0125f);
                if (rs == RailStyle::Balusters) {
                    const float L = glm::length(dv);
                    const float bw = 0.022f;
                    const int nb = std::max(1, static_cast<int>(std::ceil(L / (p.balusterGap + bw))));
                    for (int k = 0; k < nb; ++k) {
                        const float f = (static_cast<float>(k) + 0.5f) / static_cast<float>(nb);
                        const float l = glm::mix(a.line, c.line, f);
                        ms.railing.post(glm::mix(pa, pc, f), bw, base(l, c.floor),
                                        l + railH - 0.05f, pal.rail);
                    }
                }
            }
            // Posts: both ends, every corner, and no more than 1.3 m apart.
            if (rs == RailStyle::Posts || rs == RailStyle::Balusters) {
                const float S = runLength(run);
                std::vector<float> at;
                if (rs == RailStyle::Posts) {
                    const int np = std::max(2, static_cast<int>(std::ceil(S / 1.3f)) + 1);
                    for (int k = 0; k < np; ++k) at.push_back(S * static_cast<float>(k) / (np - 1));
                    float s = 0.0f;
                    for (std::size_t i = 1; i + 1 < run.size(); ++i) {
                        s += glm::length(run[i].p - run[i - 1].p);
                        const glm::vec2 d1 = run[i].p - run[i - 1].p, d2 = run[i + 1].p - run[i].p;
                        if (glm::length(d1) < 1e-4f || glm::length(d2) < 1e-4f) continue;
                        if (glm::dot(glm::normalize(d1), glm::normalize(d2)) > 0.97f) continue;
                        bool near = false;
                        for (float q : at) near |= std::abs(q - s) < 0.25f;
                        if (!near) at.push_back(s);
                    }
                } else {
                    at = {0.0f, S};   // newel posts at the ends
                }
                const float ps = rs == RailStyle::Posts ? 0.05f : 0.08f;
                for (float s : at) {
                    glm::vec2 d(0.0f, 1.0f);
                    const SidePt q = alongRun(run, s, &d);
                    const glm::vec2 o = outward(d, left);
                    ms.railing.post(q.p + o * off, ps, base(q.line, q.floor),
                                    q.line + railH + (rs == RailStyle::Posts ? -0.05f : 0.03f),
                                    pal.rail);
                }
            }
        }
    }
    return ms;
}

} // namespace

// --- Entities ------------------------------------------------------------------

std::vector<Entity> generate(const Params& pIn, const Palette& pal, int& counter,
                             const glm::vec3& footPos, float yawDeg) {
    const Plan plan = layout(pIn);
    Meshes ms = buildMeshes(plan, pal);
    const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(yawDeg), glm::vec3(0, 1, 0));

    std::vector<Entity> out;
    Entity root;
    root.type        = EntityType::Empty;
    root.name        = "Stair";
    root.id          = counter++;
    root.parent      = -1;
    root.half        = glm::vec3(0.5f);
    root.localCenter = root.center = footPos;
    root.localRotation = root.rotation = glm::vec3(0.0f, yawDeg, 0.0f);
    {
        auto sc = std::make_unique<StairComponent>();
        sc->params = plan.params;
        root.components.items.push_back(std::move(sc));
    }
    out.push_back(std::move(root));
    const int rootId = out.front().id;

    // One child per part, re-centred on its own bounds (fitScale() exactly 1,
    // so the pick box, the gizmo and the collider all describe the shape) --
    // the same deal HouseGen's parts get.
    auto add = [&](std::string name, Builder&& b, bool collide) {
        EditMesh& m = b.m;
        if (m.faces.empty()) return;
        std::map<std::string, std::pair<int, AssetId>> tally;
        for (const AssetId& id : m.faceMat) {
            auto& e = tally[id.toString()];
            ++e.first;
            e.second = id;
        }
        AssetId own;
        int best = -1;
        for (const auto& [k, v] : tally) if (v.first > best) { best = v.first; own = v.second; }
        for (AssetId& id : m.faceMat) if (id == own) id = AssetId{};
        if (!m.dressed()) m.faceMat.clear();

        const glm::vec3 shift = editmesh::recenter(m);
        glm::vec3 mn, mx;
        m.bounds(mn, mx);
        Entity e;
        e.type          = EntityType::Box;
        e.name          = std::move(name);
        e.id            = counter++;
        e.parent        = rootId;
        e.half          = glm::max(0.5f * (mx - mn), glm::vec3(0.005f));
        e.localCenter   = shift;
        e.center        = footPos + glm::vec3(rot * glm::vec4(shift, 1.0f));
        e.rotation      = glm::vec3(0.0f, yawDeg, 0.0f);
        auto mc = std::make_unique<MeshComponent>();
        mc->mesh = std::move(m);
        mc->touch();
        e.components.items.push_back(std::move(mc));
        if (own.valid()) {
            auto mat = std::make_unique<MaterialComponent>();
            mat->material = own;
            e.components.items.push_back(std::move(mat));
        }
        if (collide && plan.params.collider) {
            auto pc = std::make_unique<PhysicsComponent>();
            pc->dynamic = false;
            e.components.items.push_back(std::move(pc));
        }
        out.push_back(std::move(e));
    };
    add("Steps", std::move(ms.steps), true);
    add("Structure", std::move(ms.structure), true);
    add("Railing", std::move(ms.railing), true);
    add("Glass", std::move(ms.glass), true);
    add("Walls", std::move(ms.walls), true);
    return out;
}

int faceCount(const Params& p) {
    const Meshes ms = buildMeshes(layout(p), Palette{});
    return static_cast<int>(ms.steps.m.faces.size() + ms.structure.m.faces.size() +
                            ms.railing.m.faces.size() + ms.glass.m.faces.size() +
                            ms.walls.m.faces.size());
}

// --- Persistence ---------------------------------------------------------------

namespace {
nlohmann::json vec3Json(const glm::vec3& v) { return nlohmann::json::array({v.x, v.y, v.z}); }
glm::vec3 jsonVec3(const nlohmann::json& j, const glm::vec3& def) {
    if (!j.is_array() || j.size() != 3) return def;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}
} // namespace

void saveParams(nlohmann::json& j, const Params& p) {
    j["kind"] = static_cast<int>(p.kind);     j["turnRight"] = p.turnRight;
    j["height"] = p.height;                   j["width"] = p.width;
    j["autoRisers"] = p.autoRisers;           j["risers"] = p.risers;
    j["targetRise"] = p.targetRise;           j["autoGoing"] = p.autoGoing;
    j["going"] = p.going;                     j["splitStep"] = p.splitStep;
    j["midLanding"] = p.midLanding;           j["landingDepth"] = p.landingDepth;
    j["eye"] = p.eye;                         j["winders"] = p.winders;
    j["innerRadius"] = p.innerRadius;         j["autoTurn"] = p.autoTurn;
    j["turn"] = p.turn;                       j["topPlatform"] = p.topPlatform;
    j["structure"] = static_cast<int>(p.structure);
    j["treadThickness"] = p.treadThickness;   j["nosing"] = p.nosing;
    j["closedRisers"] = p.closedRisers;       j["stringerWidth"] = p.stringerWidth;
    j["stringerDepth"] = p.stringerDepth;     j["column"] = p.column;
    j["railSides"] = static_cast<int>(p.railSides);
    j["railStyle"] = static_cast<int>(p.railStyle);
    j["railHeight"] = p.railHeight;           j["balusterGap"] = p.balusterGap;
    j["wallSides"] = static_cast<int>(p.wallSides);
    j["wallHeight"] = p.wallHeight;           j["wallThickness"] = p.wallThickness;
    j["look"] = static_cast<int>(p.look);
    j["treadColor"] = vec3Json(p.treadColor);
    j["structureColor"] = vec3Json(p.structureColor);
    j["railColor"] = vec3Json(p.railColor);
    j["wallColor"] = vec3Json(p.wallColor);
    j["collider"] = p.collider;
}

void loadParams(const nlohmann::json& j, Params& p) {
    const Params d;   // missing fields keep the defaults
    p.kind = static_cast<Kind>(j.value("kind", static_cast<int>(d.kind)));
    p.turnRight = j.value("turnRight", d.turnRight);
    p.height = j.value("height", d.height);                 p.width = j.value("width", d.width);
    p.autoRisers = j.value("autoRisers", d.autoRisers);     p.risers = j.value("risers", d.risers);
    p.targetRise = j.value("targetRise", d.targetRise);     p.autoGoing = j.value("autoGoing", d.autoGoing);
    p.going = j.value("going", d.going);                    p.splitStep = j.value("splitStep", d.splitStep);
    p.midLanding = j.value("midLanding", d.midLanding);
    p.landingDepth = j.value("landingDepth", d.landingDepth);
    p.eye = j.value("eye", d.eye);                          p.winders = j.value("winders", d.winders);
    p.innerRadius = j.value("innerRadius", d.innerRadius);  p.autoTurn = j.value("autoTurn", d.autoTurn);
    p.turn = j.value("turn", d.turn);                       p.topPlatform = j.value("topPlatform", d.topPlatform);
    p.structure = static_cast<Structure>(j.value("structure", static_cast<int>(d.structure)));
    p.treadThickness = j.value("treadThickness", d.treadThickness);
    p.nosing = j.value("nosing", d.nosing);
    p.closedRisers = j.value("closedRisers", d.closedRisers);
    p.stringerWidth = j.value("stringerWidth", d.stringerWidth);
    p.stringerDepth = j.value("stringerDepth", d.stringerDepth);
    p.column = j.value("column", d.column);
    p.railSides = static_cast<Sides>(j.value("railSides", static_cast<int>(d.railSides)));
    p.railStyle = static_cast<RailStyle>(j.value("railStyle", static_cast<int>(d.railStyle)));
    p.railHeight = j.value("railHeight", d.railHeight);
    p.balusterGap = j.value("balusterGap", d.balusterGap);
    p.wallSides = static_cast<Sides>(j.value("wallSides", static_cast<int>(d.wallSides)));
    p.wallHeight = j.value("wallHeight", d.wallHeight);
    p.wallThickness = j.value("wallThickness", d.wallThickness);
    p.look = static_cast<Look>(j.value("look", static_cast<int>(d.look)));
    p.treadColor = jsonVec3(j.value("treadColor", nlohmann::json()), d.treadColor);
    p.structureColor = jsonVec3(j.value("structureColor", nlohmann::json()), d.structureColor);
    p.railColor = jsonVec3(j.value("railColor", nlohmann::json()), d.railColor);
    p.wallColor = jsonVec3(j.value("wallColor", nlohmann::json()), d.wallColor);
    p.collider = j.value("collider", d.collider);
    p = sane(p);
}

namespace {
// Scene files name components by type id; this is what lets a saved stair come
// back as a stair (and not lose its parameters to an unknown-component warning).
struct RegisterStair {
    RegisterStair() {
        components::registerType({"stair", "Stair",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<StairComponent>()); },
            /*addable=*/false});
    }
} g_registerStair;
} // namespace

} // namespace stairgen
