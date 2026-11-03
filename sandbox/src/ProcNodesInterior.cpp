#include "ProcNodeKit.hpp"
#include "ProcInterior.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "HouseGen.hpp"
#include "PropertyMeta.hpp"   // readProps

// The procedural graphs' interiors, in three kinds that hand on to each other:
//
//   Floor plan  a room programme in, a walkable house out (HouseGen's plan and
//               model), its rooms as tagged lines.
//   Rooms       a Facade block made hollow: floors, ceilings, the walls' inside
//               with the windows cut through, partitions with doors, a stair
//               from floor to floor, its rooms as tagged lines.
//   Furnish     rooms in, the same stream out with furniture standing in them,
//               made by the Furniture nodes and placed by rules.
//
// The lines are the whole contract (Curve::tag): a closed line tagged with a
// room kind is a floor outline; "door" and "window" are upright rectangles in
// the wall; "stair" and "low" (headroom under a roof, in Curve::height) are
// rectangles on the floor. Nothing here reads a room back from faces, so a
// Rectangle with no tag, wired straight into Furnish, is a room too.
//
// Furnish does not know a catalogue of room layouts. A room's recipe (below) is
// a list of groups in order of importance -- the bed with its nightstands, the
// wardrobe, a chest -- each with sizes to try, largest first, and what it wants:
// a wall, the middle, a corner, far from the door, under a window. Each is cooked
// to learn its true footprint, then put at the best place that is inside the room,
// clear of what stands already, of the doors' swing, of the stair, not in front
// of a window if it is taller than the sill, and that still leaves a way from the
// door to everything. What does not fit is left out. The same seed gives the
// same room.

namespace proc {

const std::vector<RoomKind>& roomKinds() {
    static const std::vector<RoomKind> k = {
        {"living", "Living room"}, {"dining", "Dining room"}, {"kitchen", "Kitchen"},
        {"hall", "Hall"},          {"bath", "Bathroom"},      {"wc", "WC"},
        {"bedroom", "Bedroom"},    {"child", "Child's room"}, {"office", "Office"},
        {"guest", "Guest room"},   {"utility", "Utility"},    {"storage", "Storage"},
        {"cloakroom", "Cloakroom"}, {"dressing", "Dressing room"}, {"corridor", "Corridor"},
        {"shop", "Shop"},          {"meeting", "Meeting room"},
    };
    return k;
}

} // namespace proc

namespace {

using namespace proc::kit;
using nlohmann::json;
using fitzel::AssetId;

const glm::vec3 kUp(0.0f, 1.0f, 0.0f);

float cross2(const glm::vec2& a, const glm::vec2& b) { return a.x * b.y - a.y * b.x; }
glm::vec2 xz(const glm::vec3& p) { return {p.x, p.z}; }

bool isRoomTag(const std::string& t) {
    for (const proc::RoomKind& k : proc::roomKinds())
        if (t == k.id) return true;
    return false;
}

// A closed line's area in plan, positive when it runs so that its inside is on
// the left of (x, z) -- which way each wall's inside lies.
float signedArea(const std::vector<glm::vec2>& p) {
    float a = 0.0f;
    for (std::size_t i = 0; i < p.size(); ++i) a += cross2(p[i], p[(i + 1) % p.size()]);
    return 0.5f * a;
}

bool insidePoly(const std::vector<glm::vec2>& p, const glm::vec2& q) {
    bool in = false;
    for (std::size_t i = 0, j = p.size() - 1; i < p.size(); j = i++)
        if ((p[i].y > q.y) != (p[j].y > q.y) &&
            q.x < (p[j].x - p[i].x) * (q.y - p[i].y) / (p[j].y - p[i].y) + p[i].x)
            in = !in;
    return in;
}

// Corners on a straight stretch taken out: a floor whose edges a Facade cut.
std::vector<glm::vec2> withoutStraights(const std::vector<glm::vec2>& p) {
    std::vector<glm::vec2> o;
    const std::size_t n = p.size();
    for (std::size_t i = 0; i < n; ++i) {
        const glm::vec2 a = p[(i + n - 1) % n], b = p[i], c = p[(i + 1) % n];
        const glm::vec2 u = b - a, v = c - b;
        if (glm::length(u) < 1e-4f) continue;
        if (std::fabs(cross2(glm::normalize(u), v)) > 1e-3f || glm::dot(u, v) < 0.0f) o.push_back(b);
    }
    return o;
}

// ================================================================================
// Footprints
// ================================================================================

// A box on the floor, turned: `x` along its width, `z` toward its front.
struct Foot {
    glm::vec2 c{0.0f}, half{0.0f}, x{1.0f, 0.0f}, z{0.0f, 1.0f};
    std::array<glm::vec2, 4> corners() const {
        const glm::vec2 a = x * half.x, b = z * half.y;
        return {c - a - b, c + a - b, c + a + b, c - a + b};
    }
    Foot grown(float by) const { Foot f = *this; f.half += glm::vec2(by); return f; }
};

glm::vec2 frontAt(float deg) { const float r = glm::radians(deg); return {std::sin(r), std::cos(r)}; }
glm::vec2 widthAt(float deg) { const float r = glm::radians(deg); return {std::cos(r), -std::sin(r)}; }

bool overlap(const Foot& a, const Foot& b) {
    const auto ca = a.corners(), cb = b.corners();
    for (const glm::vec2 axis : {a.x, a.z, b.x, b.z}) {
        float a0 = 1e30f, a1 = -1e30f, b0 = 1e30f, b1 = -1e30f;
        for (const glm::vec2& p : ca) { const float d = glm::dot(p, axis); a0 = std::min(a0, d); a1 = std::max(a1, d); }
        for (const glm::vec2& p : cb) { const float d = glm::dot(p, axis); b0 = std::min(b0, d); b1 = std::max(b1, d); }
        if (a1 <= b0 + 1e-4f || b1 <= a0 + 1e-4f) return false;
    }
    return true;
}

// ================================================================================
// A room, as Furnish reads it
// ================================================================================

struct RoomWall {
    glm::vec2 a{0.0f}, dir{0.0f}, in{0.0f};
    float len = 0.0f;
    bool  window = false;
};

// Floor something may not stand on: nothing at all (a stair, a door's swing), or
// nothing taller than `maxH` (in front of a window, under a roof).
struct Zone { Foot foot; float maxH = 0.0f; bool swing = false; };

struct Room {
    std::string kind;
    std::vector<glm::vec2> poly;
    std::vector<RoomWall>  walls;
    std::vector<Zone>      zones;
    std::vector<glm::vec2> doors;     // a step inside each
    std::vector<glm::vec2> windows;   // where they are on the wall
    glm::vec2 mid{0.0f}, u{1.0f, 0.0f}, v{0.0f, 1.0f};   // u along the longest wall
    float y = 0.0f, H = 2.5f, area = 0.0f, longSide = 0.0f, shortSide = 0.0f;
};

// A marking rectangle's measure: its extent in plan and height.
struct Rectangle {
    glm::vec3 mn{0.0f}, mx{0.0f};
    glm::vec2 c{0.0f};
    float     width = 0.0f;   // the longest horizontal stretch
};
Rectangle rectangleOf(const Curve& c) {
    Rectangle r;
    r.mn = glm::vec3(1e30f);
    r.mx = glm::vec3(-1e30f);
    for (const glm::vec3& p : c.pts) { r.mn = glm::min(r.mn, p); r.mx = glm::max(r.mx, p); }
    r.c = 0.5f * (xz(r.mn) + xz(r.mx));
    for (std::size_t i = 0; i < c.pts.size(); ++i) {
        const glm::vec3 d = c.pts[(i + 1) % c.pts.size()] - c.pts[i];
        r.width = std::max(r.width, glm::length(glm::vec2(d.x, d.z)));
    }
    return r;
}

Room readRoom(const Curve& c, const Geo& g, const std::string& kind, float ceiling) {
    Room r;
    r.kind = kind;
    r.H = c.height > 0.1f ? c.height : ceiling;
    r.y = 1e30f;
    for (const glm::vec3& p : c.pts) { r.poly.push_back(xz(p)); r.y = std::min(r.y, p.y); }
    r.poly = withoutStraights(r.poly);
    if (r.poly.size() < 3) return r;
    if (signedArea(r.poly) < 0.0f) std::reverse(r.poly.begin(), r.poly.end());
    r.area = signedArea(r.poly);
    for (const glm::vec2& p : r.poly) r.mid += p;
    r.mid /= static_cast<float>(r.poly.size());
    float longest = 0.0f;
    for (std::size_t i = 0; i < r.poly.size(); ++i) {
        const glm::vec2 a = r.poly[i], b = r.poly[(i + 1) % r.poly.size()];
        RoomWall w;
        w.a = a;
        w.len = glm::length(b - a);
        if (w.len < 1e-3f) continue;
        w.dir = (b - a) / w.len;
        w.in = {-w.dir.y, w.dir.x};
        r.walls.push_back(w);
        if (w.len > longest) { longest = w.len; r.u = w.dir; r.v = w.in; }
    }
    float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
    for (const glm::vec2& p : r.poly) {
        const float a = glm::dot(p, r.u), b = glm::dot(p, r.v);
        u0 = std::min(u0, a); u1 = std::max(u1, a);
        v0 = std::min(v0, b); v1 = std::max(v1, b);
    }
    r.longSide = u1 - u0;
    r.shortSide = v1 - v0;

    // The nearest wall to a point on it, and how far along.
    auto wallAt = [&](const glm::vec2& p, float reach, float& along) -> RoomWall* {
        RoomWall* best = nullptr;
        float near = reach;
        for (RoomWall& w : r.walls) {
            const float s = glm::dot(p - w.a, w.dir);
            if (s < -0.2f || s > w.len + 0.2f) continue;
            const float d = std::fabs(glm::dot(p - w.a, w.in));
            if (d < near) { near = d; best = &w; along = s; }
        }
        return best;
    };
    for (const Curve& m : g.curves) {
        if (m.tag != "door" && m.tag != "window" && m.tag != "stair" && m.tag != "low") continue;
        const Rectangle q = rectangleOf(m);
        if (m.tag == "stair" || m.tag == "low") {
            if (std::fabs(q.mn.y - r.y) > 0.3f) continue;
            Zone z;
            z.foot.c = q.c;
            z.foot.half = 0.5f * glm::vec2(q.mx.x - q.mn.x, q.mx.z - q.mn.z);
            z.maxH = m.tag == "low" ? m.height : 0.0f;
            r.zones.push_back(z);
            continue;
        }
        if (q.mx.y < r.y + 0.2f || q.mn.y > r.y + r.H) continue;
        float along = 0.0f;
        RoomWall* w = wallAt(q.c, 0.45f, along);
        if (!w) continue;
        const glm::vec2 on = w->a + w->dir * along;
        Zone z;
        z.foot.x = w->dir;
        z.foot.z = w->in;
        const float sill = q.mn.y - r.y;
        if (m.tag == "door" || sill < 0.2f) {
            const float deep = std::min(q.width + 0.1f, 1.1f);
            z.foot.c = on + w->in * (0.5f * deep);
            z.foot.half = {0.5f * q.width + 0.1f, 0.5f * deep};
            z.swing = true;
            r.doors.push_back(on + w->in * 0.35f);
        } else {
            w->window = true;
            r.windows.push_back(on);
            z.foot.c = on + w->in * 0.3f;
            z.foot.half = {0.5f * q.width + 0.05f, 0.3f};
            z.maxH = sill - 0.02f;
        }
        r.zones.push_back(z);
    }
    return r;
}

bool footInRoom(const Room& r, const Foot& f, float margin = 0.0f) {
    const Foot g = margin > 0.0f ? f.grown(margin) : f;
    const auto cs = g.corners();
    for (const glm::vec2& p : cs)
        if (!insidePoly(r.poly, p)) return false;
    for (const glm::vec2& p : r.poly) {
        const glm::vec2 d = p - g.c;
        if (std::fabs(glm::dot(d, g.x)) < g.half.x - 1e-3f && std::fabs(glm::dot(d, g.z)) < g.half.y - 1e-3f)
            return false;
    }
    return true;
}

// ================================================================================
// Recipes
// ================================================================================

// The stuff a room is dressed in: Furnish's material slots, as they are.
struct Stuff {
    std::string woodA, woodB, white, metal, dark, fabricA, fabricB, fabricC, linen, mattress, rug, rugBorder,
                bookA, bookB, bookC, shade, stone, ceramic, glass;
};

// A piece of a group: a Furniture node's kind, its settings by file key, where
// it stands in the group. A soft one (a rug, a lamp hanging over) has no say in
// the group's footprint: what it spreads over or hangs above stays walkable.
struct Part {
    const char* kind;
    json        set;
    glm::vec3   at{0.0f};
    float       turn = 0.0f;
    bool        soft = false;
};
using Option = std::vector<Part>;

// A group and what it wants. All the weights are costs per metre (or for the
// whole wall): 0 does not care.
struct Group {
    const char*         what = "";
    std::vector<Option> options;      // tried in order: largest first
    bool  middle  = false;            // stands free, not on a wall
    float around  = 0.0f;             // middle: free floor kept all round
    float clear   = 0.6f;             // free floor kept in front
    float centred = 0.0f;             // on a wall: stay at its middle
    float cornered = 0.0f;            // on a wall: go to its end
    float farDoor = 0.0f;
    float nearDoor = 0.0f;
    float window  = 0.0f;             // above 0 under a window, below 0 not on a wall with one
    int   facing  = -1;               // on a wall: the wall across from group N (index in the recipe)
    int   beside  = -1;               // close to group N
};

float roll(int seed, int a, int b = 0) { return proc::random01(a * 131 + b * 17 + 7, seed); }

// A kitchen along one wall, its back at -Z, centred on x = 0: a fridge at the
// end, a cooker between two runs of units, the sink in the longer one.
Option kitchenRun(const Stuff& s, float W, bool upper, bool fridge) {
        Option o;
        const float fr = fridge ? 0.60f : 0.0f;
        const float cook = 0.60f;
        const float rest = W - fr - cook;
        const float x0 = -0.5f * W + fr;
        const float c1 = 0.5f * rest, c2 = rest - c1;
        const json units = {{"height", 0.9}, {"depth", 0.6}, {"fronts", 4}, {"base", 0}, {"baseHeight", 0.1},
                            {"worktop", true}, {"topThickness", 0.04}, {"overhang", 0.02}, {"handles", 1},
                            {"materialBody", s.white}, {"materialFront", s.white}, {"materialHandle", s.metal},
                            {"materialTop", s.stone}};
        auto cabinet = [&](float x, float w) {
            json u = units;
            u["width"] = w;
            u["columns"] = std::max(1, static_cast<int>(std::round(w / 0.6f)));
            o.push_back({"cabinet", u, {x + 0.5f * w, 0.0f, 0.0f}});
        };
        if (c1 > 0.25f) cabinet(x0, c1);
        if (c2 > 0.25f) cabinet(x0 + c1 + cook, c2);
        const float cookX = x0 + c1 + 0.5f * cook;
        o.push_back({"appliance", {{"kind", 1}, {"width", cook}, {"materialBody", s.white}, {"materialDark", s.dark},
                                   {"materialMetal", s.metal}, {"materialGlass", s.dark}},
                     {cookX, 0.0f, -0.01f}});
        if (c2 > 0.7f)
            o.push_back({"appliance", {{"kind", 4}, {"width", std::min(c2 - 0.15f, 0.8f)}, {"depth", 0.46}, {"height", 0.9},
                                       {"materialDark", s.dark}, {"materialMetal", s.metal}},
                         {x0 + c1 + cook + 0.5f * c2, 0.0f, 0.03f}});
        if (fridge)
            o.push_back({"appliance", {{"kind", 0}, {"width", 0.6}, {"depth", 0.62}, {"materialBody", s.white},
                                       {"materialDark", s.dark}, {"materialMetal", s.metal}},
                         {-0.5f * W + 0.3f, 0.0f, 0.0f}});
        if (upper) {
            const json wall = {{"height", 2.2}, {"depth", 0.35}, {"fronts", 0}, {"base", 2}, {"baseHeight", 1.45},
                               {"handles", 1}, {"materialBody", s.white}, {"materialFront", s.white},
                               {"materialHandle", s.metal}};
            auto up = [&](float x, float w) {
                if (w < 0.3f) return;
                json u = wall;
                u["width"] = w;
                u["columns"] = std::max(1, static_cast<int>(std::round(w / 0.6f)));
                o.push_back({"cabinet", u, {x + 0.5f * w, 0.0f, -0.125f}});
            };
            up(x0, c1);
            up(x0 + c1 + cook, c2);
            o.push_back({"appliance", {{"kind", 3}, {"width", cook}, {"depth", 0.5}, {"height", 1.6}, {"materialMetal", s.metal}},
                         {cookX, 0.0f, -0.05f}});
        }
        return o;
}

json shelfBooks(const Stuff& s, float amount, int seed) {
    return {{"books", amount}, {"seed", seed}, {"materialBooks", s.bookA}, {"materialBooksB", s.bookB},
            {"materialBooksC", s.bookC}};
}
json withAll(json a, const json& b) {
    for (const auto& [k, v] : b.items()) a[k] = v;
    return a;
}

// A room's list: most important first, so a small room keeps its bed and loses
// the armchair.
std::vector<Group> recipe(const Room& r, const Stuff& s, int seed, bool small) {
    std::vector<Group> gs;
    const float books = small ? 0.7f : 0.0f;
    const std::string& wood = roll(seed, 1) < 0.5f ? s.woodA : s.woodB;
    const std::string& carcass = roll(seed, 2) < 0.5f ? s.white : wood;
    const float fr = roll(seed, 3);
    const std::string& fabric = fr < 0.34f ? s.fabricA : fr < 0.67f ? s.fabricB : s.fabricC;
    const json cab = {{"materialBody", carcass}, {"materialFront", carcass}, {"materialHandle", s.metal}};

    auto wardrobe = [&](float w, float h, int doors) -> Part {
        return {"cabinet", withAll({{"width", w}, {"height", h}, {"depth", 0.6}, {"columns", doors}, {"rows", 1},
                                    {"fronts", 0}, {"base", 0}, {"handles", 1}}, cab)};
    };
    auto chest = [&](float w, float h, float d, int fronts, int base = 1) -> Part {
        return {"cabinet", withAll({{"width", w}, {"height", h}, {"depth", d},
                                    {"columns", std::max(1, static_cast<int>(std::round(w / 0.6f)))},
                                    {"rows", h > 0.7f ? 3 : 2}, {"fronts", fronts}, {"base", base},
                                    {"baseHeight", base == 1 ? 0.14 : 0.08}, {"handles", 2}}, cab)};
    };
    auto bookcase = [&](float w, int sd) -> Part {
        return {"shelf", withAll({{"width", w}, {"height", 2.0}, {"depth", 0.32}, {"boards", 6}, {"materialBody", wood}},
                                 shelfBooks(s, books, sd))};
    };
    auto rug = [&](float w, float d, const glm::vec3& at) -> Part {
        return {"rug", {{"width", w}, {"depth", d}, {"materialRug", s.rug}, {"materialBorder", s.rugBorder}}, at, 0.0f, true};
    };
    auto chair = [&](const glm::vec3& at, float turn, int back = 1) -> Part {
        return {"chair", {{"back", back}, {"slats", 3}, {"cushion", true}, {"materialFrame", wood},
                          {"materialSeat", fabric}}, at, turn};
    };
    auto deskSet = [&](float w, float d) -> Option {
        return {{"table", {{"width", w}, {"depth", d}, {"height", 0.74}, {"base", 3}, {"legSize", 0.05}, {"inset", 0.04},
                           {"apron", false}, {"materialTop", wood}, {"materialBase", s.dark}}},
                {"chair", {{"base", 2}, {"back", 4}, {"arms", true}, {"seatWidth", 0.5}, {"seatDepth", 0.48},
                           {"backHeight", 0.5}, {"materialFrame", s.dark}, {"materialSeat", fabric}},
                 {0.0f, 0.0f, 0.5f * d + 0.18f}, 180.0f + 12.0f},
                {"lamp", {{"kind", 1}, {"shade", 1}, {"height", 0.42}, {"shadeRadius", 0.11}, {"shadeHeight", 0.13},
                          {"materialBody", s.dark}, {"materialShade", s.shade}},
                 {0.5f * w - 0.2f, 0.74f, -0.5f * d + 0.16f}}};
    };
    auto bedSet = [&](float w, bool stands, bool withRug) -> Option {
        Option o;
        o.push_back({"bed", {{"width", w}, {"length", 2.0}, {"headboard", w > 1.2f ? 2 : 1}, {"headHeight", w > 1.2f ? 1.05 : 0.8},
                             {"materialFrame", wood}, {"materialHeadboard", w > 1.2f ? fabric : wood},
                             {"materialMattress", s.mattress}, {"materialBedding", s.linen}}});
        if (stands)
            for (const float side : {-1.0f, 1.0f}) {
                const float x = side * (0.5f * w + 0.28f);
                o.push_back({"cabinet", withAll({{"width", 0.45}, {"height", 0.5}, {"depth", 0.4}, {"columns", 1}, {"rows", 2},
                                                 {"fronts", 1}, {"base", 1}, {"baseHeight", 0.14}, {"handles", 2}}, cab),
                             {x, 0.0f, -0.82f}});
                o.push_back({"lamp", {{"kind", 1}, {"height", 0.44}, {"shadeRadius", 0.12}, {"shadeHeight", 0.16},
                                      {"materialBody", s.white}, {"materialShade", s.shade}},
                             {x, 0.5f, -0.85f}});
            }
        if (withRug) o.push_back(rug(w + 0.9f, 1.3f, {0.0f, 0.0f, 0.75f}));
        return o;
    };
    auto group = [&](const char* what, std::vector<Option> opts) -> Group& {
        Group g;
        g.what = what;
        g.options = std::move(opts);
        gs.push_back(std::move(g));
        return gs.back();
    };
    auto bedroomLike = [&](bool dbl, bool withChest) {
        std::vector<Option> beds;
        if (dbl) { beds.push_back(bedSet(1.8f, true, true)); beds.push_back(bedSet(1.6f, true, true)); beds.push_back(bedSet(1.4f, true, false)); }
        beds.push_back(bedSet(1.2f, false, false));
        beds.push_back(bedSet(0.9f, false, false));
        Group& b = group("bed", std::move(beds));
        b.centred = dbl ? 1.0f : 0.0f; b.cornered = dbl ? 0.0f : 0.4f; b.farDoor = 0.3f; b.window = -2.0f; b.clear = 0.5f;
        Group& w = group("wardrobe", {{wardrobe(2.0f, 2.2f, 3)}, {wardrobe(1.5f, 2.2f, 2)}, {wardrobe(1.0f, 2.1f, 2)}});
        w.cornered = 0.5f; w.window = -2.0f; w.clear = 0.7f;
        if (withChest) {
            Group& c = group("chest", {{chest(1.2f, 0.8f, 0.45f, 1)}, {chest(0.8f, 0.8f, 0.42f, 1)}});
            c.window = 1.0f; c.clear = 0.5f;
        }
    };

    if (r.kind == "living") {
        std::vector<Option> sets;
        for (const float w : {2.6f, 2.2f, 1.8f}) {
            Option o;
            o.push_back({"sofa", {{"width", w}, {"depth", 0.9}, {"pillows", 2}, {"materialFabric", fabric},
                                  {"materialCushion", fabric}, {"materialLegs", wood}}});
            o.push_back({"table", {{"shape", 1}, {"width", 1.1}, {"depth", 0.6}, {"height", 0.42}, {"top", 0.04}, {"corner", 0.12},
                                   {"legShape", 1}, {"legSize", 0.045}, {"apron", false}, {"shelf", true},
                                   {"materialTop", wood}, {"materialBase", wood}},
                         {0.0f, 0.0f, 1.0f}});
            o.push_back(rug(w + 0.5f, 2.1f, {0.0f, 0.0f, 0.85f}));
            sets.push_back(std::move(o));
        }
        sets.push_back({{"sofa", {{"width", 1.5}, {"depth", 0.85}, {"seats", 2}, {"pillows", 0}, {"materialFabric", fabric},
                                  {"materialCushion", fabric}, {"materialLegs", wood}}}});
        const int anchor = static_cast<int>(gs.size());
        Group& so = group("sofa", std::move(sets));
        so.centred = 1.0f; so.farDoor = 0.3f; so.window = -2.0f; so.clear = 0.4f;
        Group& tv = group("tv board", {{chest(1.8f, 0.55f, 0.42f, 1)}, {chest(1.3f, 0.55f, 0.42f, 1)}, {chest(0.9f, 0.55f, 0.4f, 1)}});
        tv.facing = anchor; tv.centred = 0.5f; tv.clear = 0.5f;
        Group& bk = group("bookcase", {{bookcase(1.2f, seed + 3)}, {bookcase(0.8f, seed + 3)}});
        bk.cornered = 0.3f; bk.window = -2.0f;
        if (r.area > 24.0f) {
            // Wohnen and Essen in one room: the table on the far side of the sofa's rug.
            const float tw = r.area > 34.0f ? 1.8f : 1.4f;
            Option t = {{"table", {{"width", tw}, {"depth", 0.9}, {"legShape", 2}, {"legSize", 0.065}, {"materialTop", wood}, {"materialBase", wood}}}};
            const int n = tw > 1.5f ? 3 : 2;
            for (int i = 0; i < n; ++i) {
                const float x = -0.5f * tw + tw * (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
                t.push_back(chair({x, 0.0f, 0.57f}, 180.0f));
                t.push_back(chair({x, 0.0f, -0.57f}, 0.0f));
            }
            Group& dt = group("dining table", {t});
            dt.middle = true; dt.around = 0.4f; dt.clear = 0.0f;
        }
        Group& ac = group("armchair", {{{"sofa", {{"width", 0.92}, {"depth", 0.86}, {"seats", 1}, {"arms", 2}, {"armWidth", 0.15},
                                                  {"pillows", 0}, {"materialFabric", s.fabricC}, {"materialCushion", s.fabricC},
                                                  {"materialLegs", wood}}}}});
        ac.beside = anchor; ac.clear = 0.5f;
        Group& lp = group("floor lamp", {{{"lamp", {{"kind", 0}, {"height", 1.6}, {"materialBody", s.dark}, {"materialShade", s.shade}}}}});
        lp.beside = anchor; lp.clear = 0.0f;
    } else if (r.kind == "dining" || r.kind == "meeting") {
        std::vector<Option> sets;
        const bool meet = r.kind == "meeting";
        auto seats = [&](float w, float d, int along, bool heads) -> Option {
            Option o;
            o.push_back({"table", {{"width", w}, {"depth", d}, {"legShape", meet ? 0 : 2}, {"legSize", 0.065},
                                   {"materialTop", wood}, {"materialBase", meet ? s.dark : wood}}});
            const float z = 0.5f * d + 0.12f;
            const float step = w / static_cast<float>(along);
            for (int i = 0; i < along; ++i) {
                const float x = -0.5f * w + step * (static_cast<float>(i) + 0.5f);
                o.push_back(chair({x, 0.0f, z}, 180.0f));
                o.push_back(chair({x, 0.0f, -z}, 0.0f));
            }
            if (heads) {
                o.push_back(chair({0.5f * w + 0.12f, 0.0f, 0.0f}, -90.0f, 4));
                o.push_back(chair({-0.5f * w - 0.12f, 0.0f, 0.0f}, 90.0f, 4));
            }
            o.push_back({"lamp", {{"kind", 2}, {"shade", 2}, {"height", r.H}, {"drop", 0.95}, {"shadeRadius", 0.26}, {"shadeHeight", 0.24},
                                  {"materialBody", s.dark}, {"materialShade", s.shade}},
                         {}, 0.0f, true});
            return o;
        };
        if (meet) sets.push_back(seats(3.2f, 1.1f, 5, true));
        sets.push_back(seats(1.8f, 0.9f, 3, true));
        sets.push_back(seats(1.4f, 0.8f, 2, false));
        sets.push_back(seats(0.8f, 0.75f, 1, false));
        Group& t = group("table", std::move(sets));
        t.middle = true; t.around = 0.35f; t.clear = 0.0f;
        Group& sb = group("sideboard", {{chest(1.8f, 0.85f, 0.45f, 0)}, {chest(1.2f, 0.85f, 0.45f, 0)}});
        sb.window = -1.0f; sb.centred = 0.2f; sb.clear = 0.5f;
    } else if (r.kind == "kitchen") {
        std::vector<Option> runs;
        for (const float w : {4.2f, 3.6f, 3.0f, 2.4f, 1.8f}) runs.push_back(kitchenRun(s, w, true, true));
        for (const float w : {3.6f, 3.0f, 2.4f, 1.8f}) runs.push_back(kitchenRun(s, w, false, true));
        for (const float w : {3.6f, 3.0f, 2.4f, 1.8f, 1.2f}) runs.push_back(kitchenRun(s, w, false, false));
        Group& g = group("kitchen run", std::move(runs));
        g.window = -0.2f; g.cornered = 0.15f; g.clear = 1.0f;
        Group& f = group("fridge", {{{"appliance", {{"kind", 0}, {"width", 0.6}, {"depth", 0.62}, {"materialBody", s.white},
                                                    {"materialDark", s.dark}, {"materialMetal", s.metal}}}}});
        f.cornered = 0.3f; f.window = -2.0f; f.clear = 0.8f;
        Option bistro = {{"table", {{"shape", 2}, {"width", 0.8}, {"base", 2}, {"materialTop", wood}, {"materialBase", s.dark}}},
                         chair({0.0f, 0.0f, 0.5f}, 180.0f), chair({0.0f, 0.0f, -0.5f}, 0.0f)};
        Group& t = group("kitchen table", {bistro});
        t.middle = true; t.around = 0.25f; t.clear = 0.0f;
    } else if (r.kind == "hall") {
        Group& c = group("shoe cabinet", {{chest(1.0f, 0.85f, 0.32f, 1, 0)}, {chest(0.7f, 0.85f, 0.3f, 1, 0)}});
        c.nearDoor = 0.1f; c.clear = 0.4f; c.window = -1.0f;
        Group& b = group("coat boards", {{{"shelf", withAll({{"style", 2}, {"width", 1.0}, {"height", 1.85}, {"depth", 0.25}, {"boards", 1},
                                                            {"materialBody", wood}}, shelfBooks(s, 0.0f, seed))}}});
        b.nearDoor = 0.1f; b.clear = 0.4f; b.window = -1.0f;
    } else if (r.kind == "bath" || r.kind == "wc") {
        const Part sb = {"sanitary", {{"kind", 2}, {"width", 1.7}, {"depth", 0.75}, {"materialCeramic", s.ceramic}, {"materialMetal", s.metal}}};
        const Part shw = {"sanitary", {{"kind", 3}, {"materialCeramic", s.ceramic}, {"materialMetal", s.metal}, {"materialGlass", s.glass}}};
        if (r.kind == "bath") {
            std::vector<Option> wet;
            if (r.area > 4.5f) wet.push_back({sb});
            wet.push_back({shw});
            Group& w = group("bath", std::move(wet));
            w.cornered = 0.6f; w.farDoor = 0.2f; w.clear = 0.55f;
        }
        const bool wide = r.kind == "bath" && r.area > 5.5f;
        std::vector<Option> basins;
        const json common = {{"materialCeramic", s.ceramic}, {"materialMetal", s.metal}, {"materialGlass", s.glass}, {"materialBody", carcass}};
        if (wide) basins.push_back({{"sanitary", withAll({{"kind", 1}, {"stand", 1}, {"width", 1.0}, {"depth", 0.5}}, common)}});
        basins.push_back({{"sanitary", withAll({{"kind", 1}, {"stand", r.kind == "wc" ? 2 : 0}, {"width", 0.6}}, common)}});
        basins.push_back({{"sanitary", withAll({{"kind", 1}, {"stand", 2}, {"width", 0.45}, {"depth", 0.34}}, common)}});
        Group& b = group("basin", std::move(basins));
        b.window = -1.0f; b.clear = 0.55f; b.nearDoor = 0.05f;
        Group& t = group("toilet", {{{"sanitary", {{"kind", 0}, {"materialCeramic", s.ceramic}, {"materialMetal", s.metal}}}}});
        t.cornered = 0.1f; t.clear = 0.5f; t.farDoor = 0.15f;
    } else if (r.kind == "bedroom") {
        bedroomLike(true, true);
    } else if (r.kind == "guest") {
        bedroomLike(true, false);
    } else if (r.kind == "child") {
        bedroomLike(false, false);
        Group& d = group("desk", {deskSet(1.2f, 0.6f), deskSet(1.0f, 0.55f)});
        d.window = 1.5f; d.clear = 0.5f;
        Group& sh = group("bookcase", {{bookcase(0.8f, seed + 5)}});
        sh.window = -2.0f; sh.cornered = 0.2f;
        Group& rg = group("rug", {{rug(1.6f, 1.2f, glm::vec3(0.0f))}});
        rg.middle = true; rg.clear = 0.0f;
    } else if (r.kind == "office") {
        Group& d = group("desk", {deskSet(1.6f, 0.8f), deskSet(1.4f, 0.7f), deskSet(1.1f, 0.6f)});
        d.window = 1.5f; d.centred = 0.3f; d.clear = 0.55f;
        Group& a = group("bookcase", {{bookcase(1.6f, seed + 7)}, {bookcase(0.9f, seed + 7)}});
        a.window = -2.0f; a.cornered = 0.3f;
        Group& c = group("cabinet", {{chest(0.8f, 0.8f, 0.42f, 1)}});
        c.clear = 0.5f; c.cornered = 0.2f;
    } else if (r.kind == "utility") {
        Group& w = group("washing machine", {{{"appliance", {{"kind", 2}, {"materialBody", s.white}, {"materialDark", s.dark},
                                                             {"materialMetal", s.metal}, {"materialGlass", s.glass}}}}});
        w.cornered = 0.3f; w.clear = 0.7f;
        Group& sh = group("shelf", {{{"shelf", withAll({{"style", 1}, {"width", 1.0}, {"height", 1.9}, {"depth", 0.4}, {"boards", 5},
                                                       {"back", false}, {"materialBody", s.metal}}, shelfBooks(s, 0.0f, seed))}}});
        sh.window = -2.0f; sh.clear = 0.5f;
    } else if (r.kind == "storage" || r.kind == "shop") {
        const int racks = r.kind == "shop" ? 8 : 4;
        for (int i = 0; i < racks; ++i) {
            Group& sh = group("rack", {{{"shelf", withAll({{"style", 1}, {"width", 1.2}, {"height", 2.0}, {"depth", 0.45}, {"boards", 5},
                                                          {"back", r.kind == "shop"}, {"materialBody", r.kind == "shop" ? wood : s.metal}},
                                                         shelfBooks(s, r.kind == "shop" ? books : 0.0f, seed + i))}}});
            sh.window = -2.0f; sh.clear = r.kind == "shop" ? 0.9f : 0.6f; sh.cornered = 0.1f;
        }
        if (r.kind == "shop") {
            Group& c = group("counter", {{{"cabinet", withAll({{"width", 2.0}, {"height", 1.05}, {"depth", 0.6}, {"columns", 3}, {"fronts", 0},
                                                               {"base", 0}, {"worktop", true}, {"topThickness", 0.04}, {"handles", 0},
                                                               {"materialTop", s.stone}}, cab)}}});
            c.nearDoor = 0.3f; c.clear = 0.8f;
            for (int i = 0; i < 4; ++i) {
                Group& t = group("display", {{{"table", {{"width", 1.4}, {"depth", 0.8}, {"height", 0.85}, {"shelf", true},
                                                         {"materialTop", wood}, {"materialBase", s.dark}}}}});
                t.middle = true; t.around = 0.6f; t.clear = 0.0f;
            }
        }
    } else if (r.kind == "cloakroom" || r.kind == "dressing") {
        for (int i = 0; i < 2; ++i) {
            Group& w = group("wardrobe", {{wardrobe(2.0f, 2.2f, 3)}, {wardrobe(1.5f, 2.2f, 2)}, {wardrobe(1.0f, 2.1f, 2)}});
            w.clear = 0.6f; w.window = -2.0f; w.cornered = 0.2f;
        }
    }
    return gs;
}

// ================================================================================
// Placing
// ================================================================================

// Part of an option, cooked: its faces at home in the group, the box it stands in.
struct CookedPart { Geo geo; bool soft = false; glm::vec3 mn{0.0f}, mx{0.0f}; };

std::vector<CookedPart> cookOption(const Option& o) {
    std::vector<CookedPart> parts;
    for (const Part& p : o) {
        std::unique_ptr<proc::Node> n = proc::make(p.kind);
        if (!n) continue;
        json set = p.set;
        set["at"]   = {p.at.x, p.at.y, p.at.z};
        set["turn"] = p.turn;
        readProps(set, n->props(), n.get());
        CookedPart c;
        c.soft = p.soft;
        n->cook({}, c.geo);
        if (c.geo.mesh.verts.empty()) continue;
        c.geo.mesh.bounds(c.mn, c.mx);
        parts.push_back(std::move(c));
    }
    return parts;
}

// Is there still a way, at shoulder width, from every door to everything's front?
struct Walk {
    const Room& r;
    float x0 = 0.0f, z0 = 0.0f;
    int   nx = 0, nz = 0;
    static constexpr float kStep = 0.2f, kBody = 0.25f;

    explicit Walk(const Room& room) : r(room) {
        float x1 = -1e30f, z1 = -1e30f;
        x0 = z0 = 1e30f;
        for (const glm::vec2& p : r.poly) {
            x0 = std::min(x0, p.x); z0 = std::min(z0, p.y);
            x1 = std::max(x1, p.x); z1 = std::max(z1, p.y);
        }
        nx = std::min(static_cast<int>((x1 - x0) / kStep) + 1, 300);
        nz = std::min(static_cast<int>((z1 - z0) / kStep) + 1, 300);
    }
    glm::vec2 at(int i, int j) const { return {x0 + (static_cast<float>(i) + 0.5f) * kStep, z0 + (static_cast<float>(j) + 0.5f) * kStep}; }
    int cell(const glm::vec2& p) const {
        const int i = std::clamp(static_cast<int>((p.x - x0) / kStep), 0, nx - 1);
        const int j = std::clamp(static_cast<int>((p.y - z0) / kStep), 0, nz - 1);
        return j * nx + i;
    }
    // Reached cells from the first door, with these footprints standing.
    std::vector<char> flood(const std::vector<Foot>& feet) const {
        std::vector<char> free(static_cast<std::size_t>(nx * nz), 0), seen(free.size(), 0);
        for (int j = 0; j < nz; ++j)
            for (int i = 0; i < nx; ++i) {
                const glm::vec2 p = at(i, j);
                if (!insidePoly(r.poly, p)) continue;
                bool ok = true;
                for (const Foot& f : feet) {
                    const glm::vec2 d = p - f.c;
                    if (std::fabs(glm::dot(d, f.x)) < f.half.x + kBody && std::fabs(glm::dot(d, f.z)) < f.half.y + kBody) { ok = false; break; }
                }
                free[static_cast<std::size_t>(j * nx + i)] = ok ? 1 : 0;
            }
        std::vector<int> open;
        if (!r.doors.empty()) {
            const int s = cell(r.doors.front());
            free[static_cast<std::size_t>(s)] = 1;
            seen[static_cast<std::size_t>(s)] = 1;
            open.push_back(s);
        }
        while (!open.empty()) {
            const int c = open.back();
            open.pop_back();
            const int i = c % nx, j = c / nx;
            const int ns[4] = {i > 0 ? c - 1 : -1, i + 1 < nx ? c + 1 : -1, j > 0 ? c - nx : -1, j + 1 < nz ? c + nx : -1};
            for (const int q : ns)
                if (q >= 0 && free[static_cast<std::size_t>(q)] && !seen[static_cast<std::size_t>(q)]) {
                    seen[static_cast<std::size_t>(q)] = 1;
                    open.push_back(q);
                }
        }
        return seen;
    }
};

struct Standing { Foot body, front; bool hasFront = false; };

// Everything in one room. What was placed goes to `plan`, its faces to `made`.
void furnishRoom(Room& r, const Stuff& stuff, const proc::FurnishSettings& set, int roomIndex,
                 Geo* made, std::vector<proc::FurnishedPiece>* plan) {
    if (r.kind == "corridor") return;
    const int seed = set.seed * 7919 + roomIndex;
    const std::vector<Group> groups = recipe(r, stuff, seed, set.small);
    std::vector<Standing> standing;
    std::vector<proc::FurnishedPiece> mine;
    std::vector<glm::vec2> anchors(groups.size(), glm::vec2(1e30f));
    std::vector<glm::vec2> anchorFront(groups.size(), glm::vec2(0.0f));
    const Walk walk(r);

    // Only what stands there for good bars the way: a stair, not a door's swing.
    std::vector<Foot> blockers;
    for (const Zone& z : r.zones)
        if (z.maxH <= 0.0f && !z.swing) blockers.push_back(z.foot);
    const std::vector<char> base = walk.flood(blockers);

    auto stillWalkable = [&](const Foot& body, const Foot* front) {
        if (r.doors.empty()) return true;
        std::vector<Foot> feet = blockers;
        for (const Standing& s : standing) feet.push_back(s.body);
        feet.push_back(body);
        const std::vector<char> seen = walk.flood(feet);
        for (const glm::vec2& d : r.doors) {
            const int c = walk.cell(d);
            if (base[static_cast<std::size_t>(c)] && !seen[static_cast<std::size_t>(c)]) return false;
        }
        auto reachable = [&](const Foot& f) {
            const float sx = std::max(f.half.x - 0.05f, 0.0f), sz = std::max(f.half.y - 0.05f, 0.0f);
            for (float a = -sx; a <= sx + 1e-3f; a += std::max(sx, 0.1f))
                for (float b = -sz; b <= sz + 1e-3f; b += std::max(sz, 0.1f)) {
                    const int c = walk.cell(f.c + f.x * a + f.z * b);
                    if (seen[static_cast<std::size_t>(c)]) return true;
                }
            return false;
        };
        if (front && !reachable(*front)) return false;
        for (const Standing& s : standing)
            if (s.hasFront && !reachable(s.front)) return false;
        return true;
    };

    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        const Group& g = groups[gi];
        for (const Option& opt : g.options) {
            std::vector<CookedPart> parts = cookOption(opt);
            glm::vec3 mn(1e30f), mx(-1e30f);
            bool any = false;
            for (const CookedPart& c : parts) {
                if (c.soft) continue;
                mn = glm::min(mn, c.mn);
                mx = glm::max(mx, c.mx);
                any = true;
            }
            const bool softOnly = !any;
            if (softOnly) {
                for (const CookedPart& c : parts) { mn = glm::min(mn, c.mn); mx = glm::max(mx, c.mx); }
                if (parts.empty()) continue;
            }
            const float W = mx.x - mn.x, D = mx.z - mn.z, tall = softOnly ? 0.0f : mx.y;

            struct Try { float score; Foot body, front; float turn; };
            std::vector<Try> tries;
            auto consider = [&](const glm::vec2& c, float turn, float score) {
                Try t;
                t.score = score;
                t.turn = turn;
                t.body.c = c;
                t.body.half = {0.5f * W, 0.5f * D};
                t.body.x = widthAt(turn);
                t.body.z = frontAt(turn);
                t.front = t.body;
                t.front.c = c + t.body.z * (0.5f * D + 0.5f * g.clear);
                t.front.half.y = 0.5f * g.clear;
                tries.push_back(t);
            };
            float jitterIndex = 0.0f;
            auto jitter = [&] { jitterIndex += 1.0f; return 0.08f * roll(seed, static_cast<int>(gi) + 11, static_cast<int>(jitterIndex)); };
            auto doorDist = [&](const glm::vec2& p) {
                float d = 1e30f;
                for (const glm::vec2& q : r.doors) d = std::min(d, glm::length(p - q));
                return r.doors.empty() ? 0.0f : d;
            };
            if (g.middle) {
                const float steps[2] = {0.0f, 90.0f};
                for (const float t0 : steps) {
                    const float base0 = glm::degrees(std::atan2(r.v.x, r.v.y));
                    const float turn = base0 + t0;
                    const float reach = 0.5f * std::max(r.longSide, r.shortSide);
                    for (float a = -reach; a <= reach; a += 0.2f)
                        for (float b = -reach; b <= reach; b += 0.2f) {
                            const glm::vec2 c = r.mid + r.u * a + r.v * b;
                            consider(c, turn, glm::length(c - r.mid) + (t0 > 0.0f ? 0.3f : 0.0f) + 0.2f * jitter());
                        }
                }
            } else {
                for (std::size_t wi = 0; wi < r.walls.size(); ++wi) {
                    const RoomWall& w = r.walls[wi];
                    if (w.len < W + 0.02f) continue;
                    const float turn = glm::degrees(std::atan2(w.in.x, w.in.y));
                    const float lo = 0.5f * W + 0.01f, hi = w.len - 0.5f * W - 0.01f;
                    const int n = std::max(1, static_cast<int>((hi - lo) / 0.1f));
                    for (int k = 0; k <= n; ++k) {
                        const float t = lo + (hi - lo) * static_cast<float>(k) / static_cast<float>(n);
                        const glm::vec2 c = w.a + w.dir * t + w.in * (0.5f * D + 0.012f);
                        float s = jitter();
                        s += g.centred * std::fabs(t - 0.5f * w.len);
                        s += g.cornered * std::min(t - 0.5f * W, w.len - t - 0.5f * W);
                        s += g.nearDoor * doorDist(c);
                        s -= g.farDoor * doorDist(c);
                        if (g.window > 0.0f) {
                            float dw = 8.0f;
                            for (const glm::vec2& q : r.windows) dw = std::min(dw, glm::length(q - (w.a + w.dir * t)));
                            s += g.window * dw;
                        } else if (g.window < 0.0f && w.window) {
                            s -= g.window;
                        }
                        if (g.facing >= 0 && anchors[static_cast<std::size_t>(g.facing)].x < 1e29f)
                            s += 3.0f * (glm::dot(w.in, anchorFront[static_cast<std::size_t>(g.facing)]) + 1.0f);
                        if (g.beside >= 0 && anchors[static_cast<std::size_t>(g.beside)].x < 1e29f)
                            s += 0.5f * glm::length(c - anchors[static_cast<std::size_t>(g.beside)]);
                        consider(c, turn, s);
                    }
                }
            }
            std::sort(tries.begin(), tries.end(), [](const Try& a, const Try& b) { return a.score < b.score; });

            const Try* found = nullptr;
            int walks = 0;
            for (const Try& t : tries) {
                if (!footInRoom(r, t.body, g.middle ? g.around : 0.0f)) continue;
                if (g.clear > 0.01f && !footInRoom(r, t.front)) continue;
                if (softOnly) { found = &t; break; }
                bool clash = false;
                for (const Zone& z : r.zones)
                    if (tall > z.maxH && overlap(t.body, z.foot)) { clash = true; break; }
                for (const Standing& s : standing) {
                    if (clash) break;
                    if (overlap(t.body, s.body) || (s.hasFront && overlap(t.body, s.front)) ||
                        (g.clear > 0.01f && overlap(t.front, s.body)))
                        clash = true;
                }
                if (clash) continue;
                if (++walks > 24) break;
                if (!stillWalkable(t.body, g.clear > 0.01f ? &t.front : nullptr)) continue;
                found = &t;
                break;
            }
            if (!found) continue;

            // Stand it there: its footprint's middle on the place, turned.
            const glm::vec2 home(0.5f * (mn.x + mx.x), 0.5f * (mn.z + mx.z));
            const glm::mat4 xf = glm::translate(glm::mat4(1.0f), glm::vec3(found->body.c.x, r.y, found->body.c.y)) *
                                 glm::rotate(glm::mat4(1.0f), glm::radians(found->turn), kUp) *
                                 glm::translate(glm::mat4(1.0f), glm::vec3(-home.x, 0.0f, -home.y));
            if (made)
                for (CookedPart& c : parts) {
                    if (c.soft) {
                        // A rug that would run up a wall is left rolled up.
                        Foot f;
                        const glm::vec3 m0 = 0.5f * (c.mn + c.mx);
                        const glm::vec4 at = xf * glm::vec4(m0, 1.0f);
                        f.c = {at.x, at.z};
                        f.half = 0.5f * glm::vec2(c.mx.x - c.mn.x, c.mx.z - c.mn.z);
                        f.x = found->body.x;
                        f.z = found->body.z;
                        if (!footInRoom(r, f)) continue;
                    }
                    transformGeo(c.geo, xf);
                    append(*made, c.geo);
                }
            if (!softOnly) {
                Standing st;
                st.body = found->body;
                st.front = found->front;
                st.hasFront = g.clear > 0.01f;
                standing.push_back(st);
                anchors[gi] = found->body.c;
                anchorFront[gi] = found->body.z;
                proc::FurnishedPiece fp;
                fp.room = roomIndex;
                fp.kind = r.kind;
                fp.what = g.what;
                fp.center = found->body.c;
                fp.half = found->body.half;
                fp.turn = found->turn;
                fp.floor = r.y;
                fp.height = tall;
                fp.clear = g.clear;
                mine.push_back(std::move(fp));
            }
            break;
        }
    }
    if (plan) plan->insert(plan->end(), mine.begin(), mine.end());
}

// The rooms in a stream: the tagged ones, and any closed line nobody tagged.
struct RoomLine { const Curve* c; std::string kind; };
std::vector<RoomLine> roomLines(const Geo& g, int defaultKind) {
    std::vector<RoomLine> out;
    const auto& kinds = proc::roomKinds();
    const std::string fallback = kinds[static_cast<std::size_t>(std::clamp(defaultKind, 0, static_cast<int>(kinds.size()) - 1))].id;
    for (const Curve& c : g.curves) {
        if (!c.closed || c.loose || c.pts.size() < 3) continue;
        if (c.tag.empty()) out.push_back({&c, fallback});
        else if (isRoomTag(c.tag)) out.push_back({&c, c.tag});
    }
    return out;
}

void furnishAll(const Geo& in, const Stuff& stuff, const proc::FurnishSettings& set, Geo* made,
                std::vector<proc::FurnishedPiece>* plan) {
    const std::vector<RoomLine> lines = roomLines(in, set.defaultKind);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (set.share < 1.0f && proc::random01(static_cast<int>(i), set.seed + 31) >= set.share) continue;
        Room r = readRoom(*lines[i].c, in, lines[i].kind, set.ceiling);
        if (r.poly.size() < 3 || r.area < 0.5f) continue;
        furnishRoom(r, stuff, set, static_cast<int>(i), made, plan);
    }
}

// ================================================================================
// Furnish
// ================================================================================

class FurnishNode : public proc::NodeOf<FurnishNode> {
public:
    int   kind    = 0;       // for a line with no tag
    int   seed    = 1;
    float ceiling = 2.5f;
    float share   = 1.0f;
    bool  small   = true;
    float light   = 0.26f;   // radius of a room's own ceiling lamp (0: none)
    std::string lightPrefab;
    std::string matWoodA, matWoodB, matWhite, matMetal, matDark, matFabricA, matFabricB, matFabricC, matLinen,
                matMattress, matRug, matRugBorder, matBookA, matBookB, matBookC, matShade, matStone, matCeramic, matGlass;

    const char* typeId() const override { return "furnish"; }
    const char* displayName() const override { return "Furnish"; }
    int inputSlots() const override { return 1; }
    const char* inputName(int) const override { return "Rooms"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            using S = FurnishNode;
            std::vector<Property> v;
            std::vector<std::string> kinds;
            for (const proc::RoomKind& k : proc::roomKinds()) kinds.push_back(k.label);
            v.push_back(choice("Line without a tag is a", "kind", &S::kind, kinds));
            v.push_back(whole("Seed", "seed", &S::seed, 0, 99999));
            v.push_back(number("Ceiling, if unsaid", "ceiling", &S::ceiling, 0.1f, 1.8f, 12.0f));
            v.push_back(number("Share of rooms", "share", &S::share, 0.05f, 0.0f, 1.0f));
            v.push_back(flag("Small things (books)", "small", &S::small));
            v.push_back(number("Ceiling lamp size (0 = none)", "light", &S::light, 0.02f, 0.0f, 1.0f));
            // Drawn as a picker over the project's prefabs (the editor knows the key).
            v.push_back(field("Light in each room (a prefab)", "prefab", PropKind::Text, &S::lightPrefab));
            v.push_back(field("Wood", "materialWoodA", PropKind::Text, &S::matWoodA));
            v.push_back(field("Wood, the other", "materialWoodB", PropKind::Text, &S::matWoodB));
            v.push_back(field("Painted white", "materialWhite", PropKind::Text, &S::matWhite));
            v.push_back(field("Metal", "materialMetal", PropKind::Text, &S::matMetal));
            v.push_back(field("Black steel", "materialDark", PropKind::Text, &S::matDark));
            v.push_back(field("Fabric", "materialFabricA", PropKind::Text, &S::matFabricA));
            v.push_back(field("Fabric 2", "materialFabricB", PropKind::Text, &S::matFabricB));
            v.push_back(field("Fabric 3", "materialFabricC", PropKind::Text, &S::matFabricC));
            v.push_back(field("Bed linen", "materialLinen", PropKind::Text, &S::matLinen));
            v.push_back(field("Mattress", "materialMattress", PropKind::Text, &S::matMattress));
            v.push_back(field("Rug", "materialRug", PropKind::Text, &S::matRug));
            v.push_back(field("Rug border", "materialRugBorder", PropKind::Text, &S::matRugBorder));
            v.push_back(field("Books", "materialBookA", PropKind::Text, &S::matBookA));
            v.push_back(field("Books 2", "materialBookB", PropKind::Text, &S::matBookB));
            v.push_back(field("Books 3", "materialBookC", PropKind::Text, &S::matBookC));
            v.push_back(field("Lamp shade", "materialShade", PropKind::Text, &S::matShade));
            v.push_back(field("Worktop", "materialStone", PropKind::Text, &S::matStone));
            v.push_back(field("Ceramic", "materialCeramic", PropKind::Text, &S::matCeramic));
            v.push_back(field("Glass", "materialGlass", PropKind::Text, &S::matGlass));
            return v;
        }();
        return p;
    }

    Stuff stuff() const {
        return {matWoodA, matWoodB.empty() ? matWoodA : matWoodB, matWhite, matMetal, matDark, matFabricA,
                matFabricB.empty() ? matFabricA : matFabricB, matFabricC.empty() ? matFabricA : matFabricC,
                matLinen, matMattress, matRug, matRugBorder, matBookA, matBookB, matBookC, matShade, matStone,
                matCeramic, matGlass};
    }

    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        out = *in[0];
        proc::FurnishSettings set;
        set.seed = seed;
        set.defaultKind = kind;
        set.ceiling = ceiling;
        set.share = share;
        set.small = small;
        const std::vector<RoomLine> lines = roomLines(*in[0], kind);
        if (lines.empty()) return "No room in the input: a closed line (a Rectangle, not filled), or Rooms, or a Floor plan";
        Geo made;
        furnishAll(*in[0], stuff(), set, &made, nullptr);
        // A lamp on each ceiling, unless the room has its own already.
        const Stuff st = stuff();
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (share < 1.0f && proc::random01(static_cast<int>(i), seed + 31) >= share) continue;
            if (lines[i].kind == "dining" || lines[i].kind == "meeting" || lines[i].kind == "storage") continue;
            Room r = readRoom(*lines[i].c, *in[0], lines[i].kind, ceiling);
            if (r.poly.size() < 3 || r.area < 0.5f) continue;
            bool overStair = false;
            for (const Zone& z : r.zones)
                if (z.maxH <= 0.0f && !z.swing) {
                    const glm::vec2 d = r.mid - z.foot.c;
                    if (std::fabs(glm::dot(d, z.foot.x)) < z.foot.half.x + 0.3f && std::fabs(glm::dot(d, z.foot.z)) < z.foot.half.y + 0.3f)
                        overStair = true;
                }
            if (overStair) continue;
            const float wet = (r.kind == "bath" || r.kind == "wc" || r.kind == "utility" || r.kind == "hall" || r.kind == "corridor" ||
                               r.kind == "cloakroom" || r.kind == "dressing") ? 0.75f : 1.0f;
            if (light > 0.01f) {
                std::unique_ptr<proc::Node> n = proc::make("lamp");
                const float rad = light * wet;
                const json set2 = {{"kind", 2}, {"shade", wet < 1.0f ? 2 : 0}, {"height", r.H},
                                   {"drop", wet < 1.0f ? 0.12 : 0.6}, {"shadeRadius", rad}, {"shadeHeight", wet < 1.0f ? 0.1 : 0.22},
                                   {"materialBody", st.dark}, {"materialShade", st.shade},
                                   {"at", {r.mid.x, r.y, r.mid.y}}};
                readProps(set2, n->props(), n.get());
                Geo g;
                n->cook({}, g);
                append(made, g);
            }
            if (!lightPrefab.empty())
                made.instances.push_back({lightPrefab, glm::translate(glm::mat4(1.0f), glm::vec3(r.mid.x, r.y + r.H - 0.5f, r.mid.y))});
        }
        append(out, made);
        out.syncSel();
        return "";
    }
};

// ================================================================================
// Floor plan
// ================================================================================

housegen::RoomType roomTypeOf(const std::string& id, bool& known) {
    using housegen::RoomType;
    static const std::map<std::string, RoomType> m = {
        {"living", RoomType::Wohnen}, {"dining", RoomType::Essen}, {"kitchen", RoomType::Kueche},
        {"hall", RoomType::Diele}, {"bath", RoomType::Bad}, {"wc", RoomType::GaesteBad},
        {"bedroom", RoomType::Schlafen}, {"child", RoomType::Kind}, {"office", RoomType::Buero},
        {"guest", RoomType::Gast}, {"utility", RoomType::HWR}, {"storage", RoomType::Abstell},
        {"cloakroom", RoomType::Garderobe}, {"dressing", RoomType::Ankleide},
    };
    const auto it = m.find(id);
    known = it != m.end();
    return known ? it->second : RoomType::Wohnen;
}
const char* roomIdOf(housegen::RoomType t) {
    using housegen::RoomType;
    switch (t) {
        case RoomType::Wohnen:    return "living";
        case RoomType::Essen:     return "dining";
        case RoomType::Kueche:    return "kitchen";
        case RoomType::Diele:     return "hall";
        case RoomType::Bad:       return "bath";
        case RoomType::GaesteBad: return "wc";
        case RoomType::Schlafen:  return "bedroom";
        case RoomType::Kind:      return "child";
        case RoomType::Buero:     return "office";
        case RoomType::Gast:      return "guest";
        case RoomType::HWR:       return "utility";
        case RoomType::Abstell:   return "storage";
        case RoomType::Garderobe: return "cloakroom";
        case RoomType::Ankleide:  return "dressing";
        default:                  return "storage";
    }
}

// "kitchen 14, living 28 | bedroom 16, bath | office" into `p`: one list per
// level from the bottom, the ones past the full storeys for the attic. "" or
// what is wrong with it.
std::string parseProgramme(const std::string& text, housegen::Params& p) {
    std::vector<std::vector<housegen::RoomSpec>> levels(1);
    std::string word, err;
    auto flush = [&]() {
        std::istringstream is(word);
        std::string id;
        is >> id;
        word.clear();
        if (id.empty()) return;
        bool known = false;
        housegen::RoomSpec r;
        r.type = roomTypeOf(id, known);
        if (!known) { err = "No room kind called '" + id + "'"; return; }
        r.area = housegen::defaultArea(r.type);
        float area = 0.0f;
        if (is >> area && area > 0.5f) r.area = area;
        levels.back().push_back(std::move(r));
    };
    for (const char ch : text) {
        if (ch == ',') flush();
        else if (ch == '|') { flush(); levels.emplace_back(); }
        else word.push_back(ch);
    }
    flush();
    if (!err.empty()) return err;
    for (auto& l : p.storeyRooms) l.clear();
    p.atticRooms.clear();
    for (std::size_t i = 0; i < levels.size(); ++i) {
        if (static_cast<int>(i) < p.storeys) p.storeyRooms[i] = std::move(levels[i]);
        else p.atticRooms.insert(p.atticRooms.end(), levels[i].begin(), levels[i].end());
    }
    return "";
}

class FloorPlanNode : public proc::NodeOf<FloorPlanNode> {
public:
    int   type    = 0;      // housegen::Preset
    float width   = 0.0f;   // 0 = the type's own
    float depth   = 0.0f;
    int   storeys = 0;
    int   attic   = 0;      // 0 the type's own, 1 rooms under the roof, 2 a loft
    float pitch   = 0.0f;
    bool  roof    = true;
    int   topLevel = 3;     // a cut-away: the highest level built
    bool  terrace = true;
    std::string programme;
    std::string matFacade, matPlinth, matRoof, matTimber, matInterior, matFloor, matFrame, matGlass, matDoor, matStair;

    const char* typeId() const override { return "floorplan"; }
    const char* displayName() const override { return "Floor plan"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            using S = FloorPlanNode;
            std::vector<Property> v;
            std::vector<std::string> types;
            for (int i = 0; i < static_cast<int>(housegen::Preset::Count); ++i)
                types.push_back(housegen::presetName(static_cast<housegen::Preset>(i)));
            v.push_back(choice("House type", "houseType", &S::type, types));
            v.push_back(number("Width (0 = the type's)", "width", &S::width, 0.25f, 0.0f, 40.0f));
            v.push_back(number("Depth (0 = the type's)", "depth", &S::depth, 0.25f, 0.0f, 40.0f));
            v.push_back(whole("Storeys (0 = the type's)", "storeys", &S::storeys, 0, housegen::kMaxStoreys));
            v.push_back(choice("Attic", "attic", &S::attic, {"The type's", "Rooms", "A loft"}));
            v.push_back(number("Roof pitch (0 = the type's)", "pitch", &S::pitch, 1.0f, 0.0f, 60.0f, "%.0f"));
            v.push_back(flag("Roof", "roof", &S::roof));
            v.push_back(whole("Highest level built", "topLevel", &S::topLevel, 0, 3));
            v.push_back(flag("Terrace", "terrace", &S::terrace));
            v.push_back(field("Rooms (kitchen 14, living | bedroom, bath)", "programme", PropKind::Text, &S::programme));
            v.push_back(field("Facade", "materialFacade", PropKind::Text, &S::matFacade));
            v.push_back(field("Plinth", "materialPlinth", PropKind::Text, &S::matPlinth));
            v.push_back(field("Roof tiles", "materialRoof", PropKind::Text, &S::matRoof));
            v.push_back(field("Timber", "materialTimber", PropKind::Text, &S::matTimber));
            v.push_back(field("Inside walls", "materialInterior", PropKind::Text, &S::matInterior));
            v.push_back(field("Floors", "materialFloor", PropKind::Text, &S::matFloor));
            v.push_back(field("Frames", "materialFrame", PropKind::Text, &S::matFrame));
            v.push_back(field("Glass", "materialGlass", PropKind::Text, &S::matGlass));
            v.push_back(field("Doors", "materialDoor", PropKind::Text, &S::matDoor));
            v.push_back(field("Stair", "materialStair", PropKind::Text, &S::matStair));
            return v;
        }();
        return p;
    }

    std::string cook(const std::vector<const Geo*>&, Geo& out) const override {
        housegen::Params p;
        housegen::applyPreset(p, static_cast<housegen::Preset>(std::clamp(type, 0, static_cast<int>(housegen::Preset::Count) - 1)));
        if (width > 1.0f) p.width = width;
        if (depth > 1.0f) p.depth = depth;
        if (storeys > 0) p.storeys = storeys;
        if (attic == 1) p.attic = true;
        if (attic == 2) p.attic = false;
        if (pitch > 1.0f) p.roofPitch = pitch;
        p.roof = roof;
        p.cutLevel = topLevel;
        p.terrace = terrace;
        p.collider = false;
        if (!programme.empty())
            if (std::string e = parseProgramme(programme, p); !e.empty()) return e;
        const housegen::Plan plan = housegen::layout(p);
        housegen::Palette pal;
        pal.facade   = AssetId::fromString(matFacade);
        pal.plinth   = AssetId::fromString(matPlinth);
        pal.roof     = AssetId::fromString(matRoof);
        pal.timber   = AssetId::fromString(matTimber);
        pal.interior = AssetId::fromString(matInterior);
        pal.floor    = AssetId::fromString(matFloor);
        pal.frame    = AssetId::fromString(matFrame);
        pal.glass    = AssetId::fromString(matGlass);
        pal.door     = AssetId::fromString(matDoor);
        pal.stair    = AssetId::fromString(matStair);
        out.mesh = housegen::meshOf(plan, pal);

        const housegen::Params& q = plan.params;
        const float W = q.width, D = q.depth;
        auto P = [&](float x, float y, float z) { return glm::vec3(x - 0.5f * W, z, y - 0.5f * D); };
        auto rect = [&](const housegen::Rect& r, float z, const char* tag, float height = 0.0f) {
            Curve c;
            c.pts = {P(r.x0, r.y0, z), P(r.x1, r.y0, z), P(r.x1, r.y1, z), P(r.x0, r.y1, z)};
            c.closed = true;
            c.tag = tag;
            c.height = height;
            out.curves.push_back(std::move(c));
        };
        for (int L = 0; L < static_cast<int>(plan.levels.size()) && L <= q.cutLevel; ++L) {
            const housegen::LevelPlan& lv = plan.levels[static_cast<std::size_t>(L)];
            if (lv.loft) continue;
            for (const housegen::PlanRoom& r : lv.rooms) {
                float head = q.clearHeight;
                if (lv.attic) head = std::min(head, housegen::roofUnderside(plan, 0.5f * (r.r.x0 + r.r.x1)) - lv.z);
                rect(r.r, lv.z, r.hall ? "hall" : roomIdOf(r.type), head);
                // Under a slope: the strips of floor too low to stand tall on.
                if (!lv.attic) continue;
                const float bands[3] = {1.0f, 1.6f, 2.3f};
                for (int side = 0; side < 2; ++side) {
                    float from = side == 0 ? r.r.x0 : r.r.x1;
                    for (int b = 0; b < 3; ++b) {
                        // Walk in from the wall until the roof clears the band's height.
                        float to = from;
                        const float inward = side == 0 ? 1.0f : -1.0f;
                        const float limit = side == 0 ? r.r.x1 : r.r.x0;
                        float low = housegen::roofUnderside(plan, from) - lv.z;
                        while ((limit - to) * inward > 0.0f && housegen::roofUnderside(plan, to) - lv.z < bands[b]) to += inward * 0.05f;
                        if (std::fabs(to - from) > 0.04f) {
                            housegen::Rect z = r.r;
                            z.x0 = std::min(from, to);
                            z.x1 = std::max(from, to);
                            rect(z, lv.z, "low", low);
                        }
                        from = to;
                    }
                }
            }
            for (const housegen::Opening& o : lv.openings) {
                const bool door = o.kind != housegen::Opening::Kind::Window;
                const float a = o.alongX ? o.r.x0 : o.r.y0, b = o.alongX ? o.r.x1 : o.r.y1;
                const float m = o.alongX ? 0.5f * (o.r.y0 + o.r.y1) : 0.5f * (o.r.x0 + o.r.x1);
                Curve c;
                auto at = [&](float s, float z) { return o.alongX ? P(s, m, z) : P(m, s, z); };
                c.pts = {at(a, lv.z + o.sill), at(b, lv.z + o.sill), at(b, lv.z + o.head), at(a, lv.z + o.head)};
                c.closed = true;
                c.tag = door ? "door" : "window";
                out.curves.push_back(std::move(c));
            }
            if (plan.hasStair && (lv.stairUp || lv.stairDown)) rect(plan.stair.r, lv.z, "stair");
        }
        return "";
    }
};

// ================================================================================
// Rooms
// ================================================================================

using housegen::Rect;

// The block's own axes: u along its long side, v across, both inside the walls.
struct BlockFrame {
    glm::vec2 o{0.0f}, u{1.0f, 0.0f}, v{0.0f, 1.0f};
    glm::vec3 at(float a, float b, float y) const { const glm::vec2 p = o + u * a + v * b; return {p.x, y, p.y}; }
    glm::vec3 dir(float a, float b) const { const glm::vec2 p = u * a + v * b; return {p.x, 0.0f, p.y}; }
};

// Faces into a mesh, each told which way is out; solids and surfaces alike.
struct Writer {
    EditMesh& m;
    void face(const std::vector<glm::vec3>& pts, const glm::vec3& away, const AssetId& mat) {
        std::vector<int> loop;
        glm::vec3 n(0.0f);
        const std::size_t k = pts.size();
        for (std::size_t i = 0; i < k; ++i) {
            const glm::vec3& a = pts[i];
            const glm::vec3& b = pts[(i + 1) % k];
            n += glm::vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
        }
        if (glm::dot(n, n) < 1e-10f) return;
        for (const glm::vec3& p : pts) loop.push_back(addCorner(m, p));
        if (glm::dot(n, away) < 0.0f) std::reverse(loop.begin() + 1, loop.end());
        m.faces.push_back(std::move(loop));
        m.syncFaceMat();
        m.faceMat.back() = mat;
    }
    // A solid box in the block's axes.
    void box(const BlockFrame& f, float a0, float a1, float b0, float b1, float y0, float y1, const AssetId& mat) {
        if (a1 - a0 < 1e-4f || b1 - b0 < 1e-4f || y1 - y0 < 1e-4f) return;
        const glm::vec3 A0B0 = f.at(a0, b0, y0), A1B0 = f.at(a1, b0, y0), A1B1 = f.at(a1, b1, y0), A0B1 = f.at(a0, b1, y0);
        const glm::vec3 up(0.0f, y1 - y0, 0.0f);
        face({A0B0, A1B0, A1B1, A0B1}, -kUp, mat);
        face({A0B0 + up, A1B0 + up, A1B1 + up, A0B1 + up}, kUp, mat);
        face({A0B0, A1B0, A1B0 + up, A0B0 + up}, f.dir(0.0f, -1.0f), mat);
        face({A1B1, A0B1, A0B1 + up, A1B1 + up}, f.dir(0.0f, 1.0f), mat);
        face({A0B1, A0B0, A0B0 + up, A0B1 + up}, f.dir(-1.0f, 0.0f), mat);
        face({A1B0, A1B1, A1B1 + up, A1B0 + up}, f.dir(1.0f, 0.0f), mat);
    }
    void floorRect(const BlockFrame& f, const Rect& r, float y, const glm::vec3& away, const AssetId& mat) {
        if (r.w() < 1e-3f || r.h() < 1e-3f) return;
        face({f.at(r.x0, r.y0, y), f.at(r.x1, r.y0, y), f.at(r.x1, r.y1, y), f.at(r.x0, r.y1, y)}, away, mat);
    }
};

// Rect `r` less `hole`, as up to four rectangles.
std::vector<Rect> minus(const Rect& r, const Rect* hole) {
    if (!hole || hole->x1 <= r.x0 || hole->x0 >= r.x1 || hole->y1 <= r.y0 || hole->y0 >= r.y1) return {r};
    const float x0 = std::max(hole->x0, r.x0), x1 = std::min(hole->x1, r.x1);
    const float y0 = std::max(hole->y0, r.y0), y1 = std::min(hole->y1, r.y1);
    std::vector<Rect> out;
    if (y0 > r.y0) out.push_back({r.x0, r.y0, r.x1, y0});
    if (y1 < r.y1) out.push_back({r.x0, y1, r.x1, r.y1});
    if (x0 > r.x0) out.push_back({r.x0, y0, x0, y1});
    if (x1 < r.x1) out.push_back({x1, y0, r.x1, y1});
    return out;
}

// An opening in a wall: along the wall s0..s1, in height y0..y1.
struct Hole { float s0, s1, y0, y1; };

// Wall from s0 to s1 between heights y0 and y1, less its holes, as strips: `put`
// gets each piece's corners along and up. A stack of openings in one strip
// shares it out between them.
void stripsAround(float s0, float s1, float y0, float y1, std::vector<Hole> holes,
                  const std::function<void(float, float, float, float)>& put) {
    std::vector<float> cuts = {s0, s1};
    for (const Hole& h : holes) { cuts.push_back(std::clamp(h.s0, s0, s1)); cuts.push_back(std::clamp(h.s1, s0, s1)); }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end(), [](float a, float b) { return b - a < 1e-3f; }), cuts.end());
    std::sort(holes.begin(), holes.end(), [](const Hole& a, const Hole& b) { return a.y0 < b.y0; });
    for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
        const float a = cuts[i], b = cuts[i + 1], m = 0.5f * (a + b);
        float y = y0;
        for (const Hole& h : holes) {
            if (m <= h.s0 || m >= h.s1) continue;
            if (h.y0 > y + 1e-3f) put(a, b, y, std::min(h.y0, y1));
            y = std::max(y, h.y1);
        }
        if (y1 > y + 1e-3f) put(a, b, y, y1);
    }
}

// The kinds in a run of rooms along a band, and how wide each is next to a bedroom.
struct Mix { const char* kind; float weight; };
const std::vector<Mix>& mixOf(int use) {
    static const std::vector<Mix> flats = {{"living", 1.35f}, {"kitchen", 0.85f}, {"bedroom", 1.0f}, {"bath", 0.62f},
                                           {"child", 0.9f}, {"dining", 1.0f}, {"wc", 0.42f}, {"bedroom", 1.0f}, {"office", 0.8f}};
    static const std::vector<Mix> offices = {{"office", 1.0f}, {"office", 1.0f}, {"meeting", 1.5f}, {"wc", 0.5f},
                                             {"office", 1.0f}, {"storage", 0.5f}};
    static const std::vector<Mix> shops = {{"shop", 2.2f}};
    return use == 1 ? offices : use == 2 ? shops : flats;
}

class RoomsNode : public proc::NodeOf<RoomsNode> {
public:
    int   use        = 0;     // 0 flats, 1 offices
    int   groundUse  = 0;     // 0 like above, 1 shops
    float wall       = 0.30f; // the outer wall's thickness inside the facade
    float slab       = 0.25f;
    float partition  = 0.12f;
    float roomWidth  = 4.2f;
    float storey     = 3.2f;  // only for a block no Facade has marked
    int   seed       = 1;
    std::string matWall, matFloor, matTile, matCeiling, matDoor, matFrame, matStair, matGlass;

    const char* typeId() const override { return "rooms"; }
    const char* displayName() const override { return "Rooms"; }
    int inputSlots() const override { return 1; }
    const char* inputName(int) const override { return "Building"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            using S = RoomsNode;
            std::vector<Property> v;
            v.push_back(choice("Used as", "use", &S::use, {"Flats", "Offices"}));
            v.push_back(choice("Ground floor", "groundUse", &S::groundUse, {"Like above", "Shops"}));
            v.push_back(number("Room width", "roomWidth", &S::roomWidth, 0.1f, 2.4f, 20.0f));
            v.push_back(whole("Seed", "seed", &S::seed, 0, 99999));
            v.push_back(number("Outer wall", "wall", &S::wall, 0.01f, 0.05f, 2.0f, "%.3f"));
            v.push_back(number("Floor slab", "slab", &S::slab, 0.01f, 0.02f, 1.0f, "%.3f"));
            v.push_back(number("Partition", "partition", &S::partition, 0.01f, 0.02f, 0.5f, "%.3f"));
            v.push_back(number("Storey height, without a Facade", "storey", &S::storey, 0.1f, 2.2f, 20.0f));
            v.push_back(field("Inside walls", "materialWall", PropKind::Text, &S::matWall));
            v.push_back(field("Floors", "materialFloor", PropKind::Text, &S::matFloor));
            v.push_back(field("Tiles (bath, kitchen)", "materialTile", PropKind::Text, &S::matTile));
            v.push_back(field("Ceilings", "materialCeiling", PropKind::Text, &S::matCeiling));
            v.push_back(field("Doors", "materialDoor", PropKind::Text, &S::matDoor));
            v.push_back(field("Frames", "materialFrame", PropKind::Text, &S::matFrame));
            v.push_back(field("Stair", "materialStair", PropKind::Text, &S::matStair));
            v.push_back(field("Glass, seen from inside", "materialGlass", PropKind::Text, &S::matGlass));
            return v;
        }();
        return p;
    }

    // A window or door the Facade marked: which wall of the block, where along
    // its inside, how far its back lies from the inside of the wall.
    struct Mark { std::size_t edge; float s0, s1, y0, y1, depth; bool door; };

    struct Block {
        std::vector<glm::vec2> outer, inner;     // the footprint, wound with its inside on the left
        std::vector<float>     levels;           // floor heights, and the top last
        std::vector<Mark>      marks;
        std::vector<int>       panes;            // the door faces a way in takes out
    };

    std::vector<Block> findBlocks(const Geo& g) const;
    void build(const Block& b, Geo& out) const;
    void shell(Writer& w, const Block& b, Geo& out, int level) const;
    void openPlan(Writer& w, const Block& b, Geo& out) const;
    bool isBox(const Block& b, BlockFrame& f, float& L, float& S) const;

    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        out = *in[0];
        const std::vector<Block> blocks = findBlocks(*in[0]);
        if (blocks.empty()) return "No block with a floor to its walls in the input: a closed building, 2 m high or more";
        // A way in: the door's own faces go; the walls' markings give way to the rooms'.
        std::vector<char> gone(out.mesh.faces.size(), 0);
        for (const Block& b : blocks)
            for (const int f : b.panes) gone[static_cast<std::size_t>(f)] = 1;
        dropFaces(out, gone);
        std::vector<Curve> kept;
        for (Curve& c : out.curves)
            if (c.tag != "door" && c.tag != "window" && c.tag != "storey") kept.push_back(std::move(c));
        out.curves = std::move(kept);
        for (const Block& b : blocks) build(b, out);
        out.syncSel();
        return "";
    }
};

std::vector<RoomsNode::Block> RoomsNode::findBlocks(const Geo& g) const {
    const EditMesh& m = g.mesh;
    // Faces that share a corner are one solid.
    std::vector<int> root(m.verts.size());
    std::iota(root.begin(), root.end(), 0);
    std::function<int(int)> find = [&](int a) {
        while (root[static_cast<std::size_t>(a)] != a) {
            root[static_cast<std::size_t>(a)] = root[static_cast<std::size_t>(root[static_cast<std::size_t>(a)])];
            a = root[static_cast<std::size_t>(a)];
        }
        return a;
    };
    for (const std::vector<int>& f : m.faces)
        for (std::size_t i = 1; i < f.size(); ++i) root[static_cast<std::size_t>(find(f[i]))] = find(f[0]);
    struct Solid { float lo = 1e30f, hi = -1e30f; int floorFace = -1; float floorY = 1e30f; bool wallTall = false; std::vector<int> faces; };
    std::map<int, Solid> solids;
    for (std::size_t fi = 0; fi < m.faces.size(); ++fi) {
        const std::vector<int>& f = m.faces[fi];
        if (f.size() < 3) continue;
        Solid& s = solids[find(f[0])];
        s.faces.push_back(static_cast<int>(fi));
        float yAvg = 0.0f;
        for (const int v : f) {
            const float y = m.verts[static_cast<std::size_t>(v)].y;
            s.lo = std::min(s.lo, y);
            s.hi = std::max(s.hi, y);
            yAvg += y;
        }
        yAvg /= static_cast<float>(f.size());
        const glm::vec3 nrm = m.faceNormal(static_cast<int>(fi));
        if (nrm.y < -0.99f && yAvg < s.floorY) { s.floorY = yAvg; s.floorFace = static_cast<int>(fi); }
        if (f.size() == 4 && std::fabs(nrm.y) < 0.05f) {
            float y0 = 1e30f, y1 = -1e30f;
            for (const int v : f) { y0 = std::min(y0, m.verts[static_cast<std::size_t>(v)].y); y1 = std::max(y1, m.verts[static_cast<std::size_t>(v)].y); }
            s.wallTall = s.wallTall || y1 - y0 >= 2.0f;
        }
    }
    std::vector<Block> blocks;
    for (const auto& [root0, s] : solids) {
        if (s.floorFace < 0 || s.hi - s.lo < 2.0f) continue;
        Block b;
        for (const int v : m.faces[static_cast<std::size_t>(s.floorFace)]) b.outer.push_back(xz(m.verts[static_cast<std::size_t>(v)]));
        b.outer = withoutStraights(b.outer);
        if (b.outer.size() < 3) continue;
        if (signedArea(b.outer) < 0.0f) std::reverse(b.outer.begin(), b.outer.end());
        if (signedArea(b.outer) < 6.0f) continue;
        // Convex, or this is more than a Rooms node can lay out.
        bool convex = true;
        const std::size_t n = b.outer.size();
        for (std::size_t i = 0; i < n; ++i)
            if (cross2(b.outer[(i + 1) % n] - b.outer[i], b.outer[(i + 2) % n] - b.outer[(i + 1) % n]) < -1e-3f) convex = false;
        if (!convex) continue;

        // The inside of the walls: each edge moved in, corners where they meet.
        const float t = wall;
        for (std::size_t i = 0; i < n; ++i) {
            const glm::vec2 a0 = b.outer[(i + n - 1) % n], a1 = b.outer[i], a2 = b.outer[(i + 1) % n];
            const glm::vec2 d1 = glm::normalize(a1 - a0), d2 = glm::normalize(a2 - a1);
            const glm::vec2 n1(-d1.y, d1.x), n2(-d2.y, d2.x);
            const glm::vec2 mitre = n1 + n2;
            const float l = 1.0f + glm::dot(n1, n2);
            b.inner.push_back(a1 + mitre * (t / std::max(l, 0.2f)));
        }
        // Markings of this block: the ones standing on its walls.
        std::vector<float> lines;
        for (const Curve& c : g.curves) {
            if (c.tag != "storey" && c.tag != "door" && c.tag != "window") continue;
            if (c.pts.size() < 2) continue;
            const glm::vec3 p = 0.5f * (c.pts[0] + c.pts[1]);
            if (p.y < s.lo - 0.05f || p.y > s.hi + 0.05f) continue;
            const glm::vec2 q = xz(p);
            bool near = insidePoly(b.outer, q);
            if (!near)
                for (std::size_t e = 0; e < n; ++e) {
                    const glm::vec2 a = b.outer[e], dir = b.outer[(e + 1) % n] - a;
                    const float len = glm::length(dir);
                    const float sp = glm::dot(q - a, dir) / std::max(len * len, 1e-6f);
                    if (sp >= -0.02f && sp <= 1.02f && std::fabs(cross2(q - a, dir)) / std::max(len, 1e-4f) < 0.06f) near = true;
                }
            if (!near) continue;
            if (c.tag == "storey") lines.push_back(p.y);
        }
        std::sort(lines.begin(), lines.end());
        lines.erase(std::unique(lines.begin(), lines.end(), [](float x, float y) { return y - x < 0.05f; }), lines.end());
        if (lines.size() < 2) {
            // No Facade has been here. A roof comes this way too -- it is a solid
            // of its own, with a floor under its eaves -- but it has no walls.
            if (!s.wallTall) continue;
            lines.clear();
            const float h = std::max(storey, 2.2f);
            const int k = std::max(1, static_cast<int>(std::floor((s.hi - s.lo) / h)));
            for (int i = 0; i <= k; ++i) lines.push_back(s.lo + static_cast<float>(i) * h);
        }
        for (std::size_t i = 0; i + 1 < lines.size(); ++i)
            if (lines[i + 1] - lines[i] >= 2.0f) b.levels.push_back(lines[i]);
        if (b.levels.empty()) continue;
        // The top of the last storey that counts.
        for (std::size_t i = 0; i < lines.size(); ++i)
            if (lines[i] > b.levels.back() + 1e-3f) { b.levels.push_back(lines[i]); break; }

        for (const Curve& c : g.curves) {
            if ((c.tag != "door" && c.tag != "window") || c.pts.size() != 4) continue;
            const glm::vec3 m0 = 0.25f * (c.pts[0] + c.pts[1] + c.pts[2] + c.pts[3]);
            if (m0.y < b.levels.front() || m0.y > b.levels.back()) continue;
            const glm::vec2 q = xz(m0);
            // On the wall it looks out of, a little behind the face.
            for (std::size_t e = 0; e < n; ++e) {
                const glm::vec2 a = b.outer[e], d = glm::normalize(b.outer[(e + 1) % n] - a);
                const glm::vec2 ni(-d.y, d.x);
                const float dist = glm::dot(q - a, ni);
                const float along = glm::dot(q - a, d);
                if (dist < -0.02f || dist > t + 0.02f) continue;
                if (along < -0.02f || along > glm::length(b.outer[(e + 1) % n] - a) + 0.02f) continue;
                const glm::vec2 ia = b.inner[e], id = glm::normalize(b.inner[(e + 1) % n] - ia);
                Mark mk;
                mk.edge = e;
                mk.s0 = 1e30f; mk.s1 = -1e30f; mk.y0 = 1e30f; mk.y1 = -1e30f;
                for (const glm::vec3& p : c.pts) {
                    const float sp = glm::dot(xz(p) - ia, id);
                    mk.s0 = std::min(mk.s0, sp);
                    mk.s1 = std::max(mk.s1, sp);
                    mk.y0 = std::min(mk.y0, p.y);
                    mk.y1 = std::max(mk.y1, p.y);
                }
                mk.depth = t - dist;
                mk.door = c.tag == "door";
                // A window past the wall's inner end (a corner): not worth a hole.
                const float ilen = glm::length(b.inner[(e + 1) % n] - ia);
                mk.s0 = std::max(mk.s0, 0.02f);
                mk.s1 = std::min(mk.s1, ilen - 0.02f);
                if (mk.s1 - mk.s0 > 0.1f) {
                    b.marks.push_back(mk);
                    if (mk.door) {
                        // The door and its frame at the back of the recess.
                        for (const int fi : s.faces) {
                            const std::vector<int>& L = m.faces[static_cast<std::size_t>(fi)];
                            bool all = !L.empty();
                            for (const int v : L) {
                                const glm::vec3 pv = m.verts[static_cast<std::size_t>(v)];
                                const float dv = glm::dot(xz(pv) - a, ni);
                                const float sv = glm::dot(xz(pv) - ia, id);
                                if (std::fabs(dv - dist) > 0.01f || sv < mk.s0 - 0.03f || sv > mk.s1 + 0.03f ||
                                    pv.y < mk.y0 - 0.01f || pv.y > mk.y1 + 0.01f) { all = false; break; }
                            }
                            if (all) b.panes.push_back(fi);
                        }
                    }
                }
                break;
            }
        }
        blocks.push_back(std::move(b));
    }
    return blocks;
}

// A four-cornered footprint with square corners: its inside as u and v, long side first.
bool RoomsNode::isBox(const Block& b, BlockFrame& f, float& L, float& S) const {
    if (b.inner.size() != 4) return false;
    const glm::vec2 e0 = b.inner[1] - b.inner[0], e1 = b.inner[2] - b.inner[1];
    if (std::fabs(glm::dot(glm::normalize(e0), glm::normalize(e1))) > 0.02f) return false;
    if (glm::length(e0) >= glm::length(e1)) {
        f.o = b.inner[0]; f.u = glm::normalize(e0); f.v = glm::normalize(e1);
        L = glm::length(e0); S = glm::length(e1);
    } else {
        f.o = b.inner[1]; f.u = glm::normalize(e1); f.v = glm::normalize(b.inner[0] - b.inner[1]);
        L = glm::length(e1); S = glm::length(e0);
    }
    return true;
}

// The inside face of the outer walls on one level, cut round the openings, with
// each opening's reveal running back to the window, and the window's inside.
void RoomsNode::shell(Writer& w, const Block& b, Geo& out, int L) const {
    const float y0 = b.levels[static_cast<std::size_t>(L)];
    const float y1 = b.levels[static_cast<std::size_t>(L) + 1] - slab;
    const AssetId wallM = AssetId::fromString(matWall), frameM = AssetId::fromString(matFrame), glassM = AssetId::fromString(matGlass);
    const std::size_t n = b.inner.size();
    for (std::size_t e = 0; e < n; ++e) {
        const glm::vec2 a = b.inner[e], c = b.inner[(e + 1) % n];
        const glm::vec2 d = glm::normalize(c - a);
        const glm::vec2 ni(-d.y, d.x);
        const float len = glm::length(c - a);
        const glm::vec3 in3(ni.x, 0.0f, ni.y), dir3(d.x, 0.0f, d.y);
        auto P = [&](float s, float y, float back = 0.0f) {
            const glm::vec2 q = a + d * s - ni * back;
            return glm::vec3(q.x, y, q.y);
        };
        std::vector<Hole> holes;
        for (const Mark& m : b.marks) {
            if (m.edge != e) continue;
            const float mid = 0.5f * (m.y0 + m.y1);
            if (mid < y0 || mid > y1) continue;
            const float top = std::min(m.y1, y1);
            holes.push_back({m.s0, m.s1, m.y0, top});

            const float back = m.depth;
            auto reveal = [&](const glm::vec3& p, const glm::vec3& q, const glm::vec3& away) {
                w.face({p, q, q - in3 * back, p - in3 * back}, away, wallM);
            };
            if (back > 1e-3f) {
                reveal(P(m.s0, m.y0), P(m.s1, m.y0), kUp);
                reveal(P(m.s1, top), P(m.s0, top), -kUp);
                reveal(P(m.s0, m.y0), P(m.s0, top), dir3);
                reveal(P(m.s1, top), P(m.s1, m.y0), -dir3);
            }
            if (m.door) {
                // The door itself, standing open into the room: two leaves if it is wide.
                const AssetId leafM = AssetId::fromString(matDoor);
                const float dwid = m.s1 - m.s0;
                const int leaves = dwid > 1.2f ? 2 : 1;
                const float lw = dwid / static_cast<float>(leaves) - 0.02f;
                for (int k = 0; k < leaves; ++k) {
                    BlockFrame lf;
                    lf.o = a + d * (k == 0 ? m.s0 : m.s1);
                    lf.u = d;
                    lf.v = ni;
                    const float x0 = k == 0 ? 0.0f : -0.05f;
                    w.box(lf, x0, x0 + 0.05f, 0.0f, lw, m.y0 + 0.01f, top - 0.01f, leafM);
                }
            } else {
                // The frame and the pane from the room's side: the facade's own
                // faces look only outward.
                const float fr = 0.07f;
                const float s0 = m.s0, s1 = m.s1, v0 = m.y0, v1 = top;
                auto bar = [&](float sa, float sb, float va, float vb) {
                    const glm::vec3 p0 = P(sa, va, back), p1 = P(sb, va, back), p2 = P(sb, vb, back), p3 = P(sa, vb, back);
                    const glm::vec3 i = in3 * 0.05f;
                    w.face({p0 + i, p1 + i, p2 + i, p3 + i}, in3, frameM);
                    w.face({p0, p1, p1 + i, p0 + i}, -kUp, frameM);
                    w.face({p3, p2, p2 + i, p3 + i}, kUp, frameM);
                    w.face({p0, p3, p3 + i, p0 + i}, -dir3, frameM);
                    w.face({p1, p2, p2 + i, p1 + i}, dir3, frameM);
                };
                bar(s0, s1, v0, v0 + fr);
                bar(s0, s1, v1 - fr, v1);
                bar(s0, s0 + fr, v0 + fr, v1 - fr);
                bar(s1 - fr, s1, v0 + fr, v1 - fr);
                if (s1 - s0 > 1.2f) bar(0.5f * (s0 + s1) - 0.5f * fr, 0.5f * (s0 + s1) + 0.5f * fr, v0 + fr, v1 - fr);
                if (glassM.valid())
                    w.face({P(s0 + fr, v0 + fr, back - 0.01f), P(s1 - fr, v0 + fr, back - 0.01f),
                            P(s1 - fr, v1 - fr, back - 0.01f), P(s0 + fr, v1 - fr, back - 0.01f)}, in3, glassM);
                // The board inside under the window.
                if (v0 - y0 > 0.3f) {
                    const glm::vec3 p0 = P(s0 - 0.04f, v0), p1 = P(s1 + 0.04f, v0);
                    const glm::vec3 o = in3 * 0.05f, u(0.0f, 0.025f, 0.0f);
                    w.face({p0 + o + u, p1 + o + u, p1 + u, p0 + u}, kUp, frameM);
                    w.face({p0 + o, p1 + o, p1 + o + u, p0 + o + u}, in3, frameM);
                    w.face({p0 + o, p1 + o, p1, p0}, -kUp, frameM);
                }
            }
            Curve mark;
            mark.pts = {P(m.s0, m.y0), P(m.s1, m.y0), P(m.s1, top), P(m.s0, top)};
            mark.closed = true;
            mark.tag = m.door ? "door" : "window";
            out.curves.push_back(std::move(mark));
        }
        stripsAround(0.0f, len, y0, y1, holes, [&](float s0, float s1, float v0, float v1) {
            w.face({P(s0, v0), P(s1, v0), P(s1, v1), P(s0, v1)}, in3, wallM);
        });
    }
}

// A footprint nothing can be laid out in, or too small for it: one room a floor.
void RoomsNode::openPlan(Writer& w, const Block& b, Geo& out) const {
    const AssetId floorM = AssetId::fromString(matFloor), ceilM = AssetId::fromString(matCeiling);
    const int levels = static_cast<int>(b.levels.size()) - 1;
    for (int L = 0; L < levels; ++L) {
        const float y0 = b.levels[static_cast<std::size_t>(L)], y1 = b.levels[static_cast<std::size_t>(L) + 1] - slab;
        shell(w, b, out, L);
        std::vector<glm::vec3> f, c;
        for (const glm::vec2& p : b.inner) { f.push_back({p.x, y0, p.y}); c.push_back({p.x, y1, p.y}); }
        w.face(f, kUp, floorM);
        w.face(c, -kUp, ceilM);
        Curve room;
        room.pts = f;
        room.closed = true;
        room.tag = use == 1 ? "office" : "living";
        room.height = y1 - y0;
        out.curves.push_back(std::move(room));
    }
}

void RoomsNode::build(const Block& b, Geo& out) const {
    Writer w{out.mesh};
    BlockFrame F;
    float L = 0.0f, S = 0.0f;
    if (!isBox(b, F, L, S) || L < 3.0f || S < 2.2f) { openPlan(w, b, out); return; }

    const AssetId floorM = AssetId::fromString(matFloor), tileM = AssetId::fromString(matTile), ceilM = AssetId::fromString(matCeiling);
    const AssetId wallM = AssetId::fromString(matWall), doorM = AssetId::fromString(matDoor), frameM = AssetId::fromString(matFrame);
    const AssetId stairM = AssetId::fromString(matStair);
    const int levels = static_cast<int>(b.levels.size()) - 1;
    const float pt = partition;

    // The bands along the block: rooms, a corridor, rooms -- or a corridor down
    // one side, or in a shallow block just rooms opening into each other.
    const float cw = 1.4f;
    struct Band { float v0, v1; int side; };   // side: 0 the v = 0 wall, 1 the v = S wall
    std::vector<Band> bands;
    Rect corridor{0.0f, 0.0f, 0.0f, 0.0f};
    bool hasCorridor = true;
    if (S >= 8.0f) {
        const float a = 0.5f * (S - cw);
        bands.push_back({0.0f, a, 0});
        bands.push_back({a + cw, S, 1});
        corridor = {0.0f, a, L, a + cw};
    } else if (S >= 5.6f) {
        bands.push_back({0.0f, S - cw, 0});
        corridor = {0.0f, S - cw, L, S};
    } else {
        bands.push_back({0.0f, S, 0});
        hasCorridor = false;
    }

    // The stair: a U of two flights and a landing, the same place on every floor.
    float tallest = 0.0f;
    for (int i = 0; i < levels; ++i) tallest = std::max(tallest, b.levels[static_cast<std::size_t>(i) + 1] - b.levels[static_cast<std::size_t>(i)]);
    const float kTread = 0.27f, kFlight = 1.0f, kLanding = 1.1f, kApron = 1.0f;
    const int   maxRisers = static_cast<int>(std::ceil(tallest / 0.185f));
    const float flightRun = static_cast<float>((maxRisers + 1) / 2) * kTread;
    const float coreLen = kApron + flightRun + kLanding;
    const float wellW = 2.0f * kFlight + 0.1f;
    bool hasStair = levels >= 2 && coreLen + 0.2f < L;
    const Band* coreBand = nullptr;
    if (hasStair) {
        coreBand = &bands.back();
        if (coreBand->v1 - coreBand->v0 < wellW + 0.05f) hasStair = false;
    }
    Rect core{0, 0, 0, 0}, well{0, 0, 0, 0};
    float apronDir = 1.0f;     // the flights run away from the apron along +u or -u
    if (hasStair) {
        const float bv0 = coreBand->v0, bv1 = coreBand->v1;
        // Hard against the corridor (or the band's start), in the middle of the block.
        const float u0 = hasCorridor ? 0.5f * (L - coreLen) : 0.0f;
        const float u1 = u0 + coreLen;
        core = {u0, bv0, u1, bv1};
        // With a corridor the apron faces it from the low end; without, the
        // rooms lie past the core, so the apron is on their side.
        apronDir = hasCorridor ? 1.0f : -1.0f;
        const float nearV = hasCorridor && coreBand->side == 1 ? bv0 : (hasCorridor ? bv1 - wellW : bv0 + 0.5f * (bv1 - bv0 - wellW));
        well = apronDir > 0.0f ? Rect{u0 + kApron, nearV, u1, nearV + wellW}
                               : Rect{u0, nearV, u1 - kApron, nearV + wellW};
    }

    struct RoomPlan { Rect r; std::string kind; };

    for (int lv = 0; lv < levels; ++lv) {
        const float y0 = b.levels[static_cast<std::size_t>(lv)], top = b.levels[static_cast<std::size_t>(lv) + 1];
        const float y1 = top - slab;
        const float H = y1 - y0;
        shell(w, b, out, lv);

        // Where the windows stand on each long wall: no partition runs into one.
        std::vector<std::pair<float, float>> taken[2];
        const glm::vec2 du = F.u;
        std::vector<std::pair<float, float>> doorsOut;
        for (const Mark& m : b.marks) {
            const float mid = 0.5f * (m.y0 + m.y1);
            if (mid < y0 || mid > y1) continue;
            const glm::vec2 ia = b.inner[m.edge], id = glm::normalize(b.inner[(m.edge + 1) % b.inner.size()] - ia);
            const glm::vec2 p0 = ia + id * m.s0, p1 = ia + id * m.s1;
            const float a0 = glm::dot(p0 - F.o, du), a1 = glm::dot(p1 - F.o, du);
            const float vv = glm::dot(0.5f * (p0 + p1) - F.o, F.v);
            const std::pair<float, float> span(std::min(a0, a1), std::max(a0, a1));
            if (std::fabs(glm::dot(id, du)) > 0.7f) {
                if (vv < 0.5f) taken[0].push_back(span);
                else if (vv > S - 0.5f) taken[1].push_back(span);
            }
            if (m.door && std::fabs(glm::dot(id, du)) > 0.7f) doorsOut.push_back({0.5f * (span.first + span.second), vv});
        }
        const bool shops = groundUse == 1 && lv == 0;
        const std::vector<Mix>& mix = mixOf(shops ? 2 : use);
        std::vector<RoomPlan> rooms;
        int cursor = static_cast<int>(proc::random01(lv * 3 + 1, seed) * static_cast<float>(mix.size()));

        // A run of rooms from u0 to u1 of one band.
        auto split = [&](const Band& bd, float u0, float u1) {
            float u = u0;
            while (u1 - u > 0.5f) {
                const Mix& mx = mix[static_cast<std::size_t>(cursor++ % static_cast<int>(mix.size()))];
                float cut = u + std::max(mx.weight * roomWidth, 1.5f);
                if (u1 - cut < 1.8f) cut = u1;
                if (cut < u1) {
                    // Off a window: to the nearer side of it.
                    for (const auto& sp : taken[bd.side])
                        if (cut > sp.first - 0.1f && cut < sp.second + 0.1f)
                            cut = (cut - sp.first < sp.second - cut && sp.first - 0.1f > u + 1.2f) ? sp.first - 0.1f : sp.second + 0.1f;
                    if (u1 - cut < 1.8f) cut = u1;
                }
                rooms.push_back({Rect{u, bd.v0, cut, bd.v1}, mx.kind});
                u = cut;
            }
        };
        for (const Band& bd : bands) {
            if (hasStair && &bd == coreBand) {
                split(bd, 0.0f, core.x0);
                split(bd, core.x1, L);
            } else {
                split(bd, 0.0f, L);
            }
        }
        // A room the street door opens into is a hall, not somebody's bedroom.
        for (RoomPlan& rp : rooms)
            for (const auto& d : doorsOut)
                if (d.first > rp.r.x0 && d.first < rp.r.x1 && d.second > rp.r.y0 - 0.5f && d.second < rp.r.y1 + 0.5f && !shops)
                    rp.kind = "hall";

        auto doorLeaf = [&](float a0, float a1, float b0, float b1) {
            w.box(F, a0, a1, b0, b1, y0 + 0.01f, y0 + 2.03f, doorM);
        };
        auto doorMark = [&](const glm::vec3& p, const glm::vec3& q) {
            Curve c;
            c.pts = {p, q, q + glm::vec3(0.0f, 2.05f, 0.0f), p + glm::vec3(0.0f, 2.05f, 0.0f)};
            c.closed = true;
            c.tag = "door";
            out.curves.push_back(std::move(c));
        };
        const float dw = 0.9f, dh = 2.05f;

        // Partition along v at u (between rooms), holes given along v.
        auto wallAcross = [&](float u, float v0, float v1, const std::vector<Hole>& holes) {
            stripsAround(v0, v1, y0, y1, holes, [&](float s0, float s1, float a, float c) {
                w.box(F, u - 0.5f * pt, u + 0.5f * pt, s0, s1, a, c, wallM);
            });
        };
        // ...and along u at v.
        auto wallAlong = [&](float v, float u0, float u1, const std::vector<Hole>& holes) {
            stripsAround(u0, u1, y0, y1, holes, [&](float s0, float s1, float a, float c) {
                w.box(F, s0, s1, v - 0.5f * pt, v + 0.5f * pt, a, c, wallM);
            });
        };
        auto trimAlong = [&](float v, float u0, float u1) {   // a door's frame in a wall along u
            const float t = 0.5f * pt + 0.015f, fw = 0.06f;
            w.box(F, u0 - fw, u0, v - t, v + t, y0, y0 + dh + fw, frameM);
            w.box(F, u1, u1 + fw, v - t, v + t, y0, y0 + dh + fw, frameM);
            w.box(F, u0, u1, v - t, v + t, y0 + dh, y0 + dh + fw, frameM);
        };
        auto trimAcross = [&](float u, float v0, float v1) {
            const float t = 0.5f * pt + 0.015f, fw = 0.06f;
            w.box(F, u - t, u + t, v0 - fw, v0, y0, y0 + dh + fw, frameM);
            w.box(F, u - t, u + t, v1, v1 + fw, y0, y0 + dh + fw, frameM);
            w.box(F, u - t, u + t, v0, v1, y0 + dh, y0 + dh + fw, frameM);
        };

        // Partitions: one between neighbours along each band, doors where a
        // corridor is missing.
        for (std::size_t i = 0; i < rooms.size(); ++i)
            for (std::size_t j = 0; j < rooms.size(); ++j) {
                if (i == j) continue;
                const Rect& a = rooms[i].r;
                const Rect& c = rooms[j].r;
                if (std::fabs(a.x1 - c.x0) > 1e-3f || a.y0 != c.y0) continue;
                std::vector<Hole> holes;
                if (!hasCorridor) {
                    const float m = 0.5f * (a.y0 + a.y1);
                    holes.push_back({m - 0.5f * dw, m + 0.5f * dw, y0, y0 + dh});
                    doorMark(F.at(a.x1, m - 0.5f * dw, y0), F.at(a.x1, m + 0.5f * dw, y0));
                    trimAcross(a.x1, m - 0.5f * dw, m + 0.5f * dw);
                    doorLeaf(a.x1 + 0.5f * pt, a.x1 + 0.5f * pt + dw - 0.02f, m + 0.5f * dw - 0.04f, m + 0.5f * dw);
                }
                wallAcross(a.x1, a.y0, a.y1, holes);
            }
        // The corridor's walls: a door for each room along them.
        if (hasCorridor) {
            for (const Band& bd : bands) {
                const float v = bd.side == 0 ? corridor.y0 : corridor.y1;
                std::vector<Hole> holes;
                for (const RoomPlan& rp : rooms) {
                    if (rp.r.y0 != bd.v0) continue;
                    const float m = std::clamp(rp.r.x0 + 0.35f * rp.r.w(), rp.r.x0 + 0.6f, std::max(rp.r.x1 - 0.6f, rp.r.x0 + 0.6f));
                    holes.push_back({m - 0.5f * dw, m + 0.5f * dw, y0, y0 + dh});
                    doorMark(F.at(m - 0.5f * dw, v, y0), F.at(m + 0.5f * dw, v, y0));
                    trimAlong(v, m - 0.5f * dw, m + 0.5f * dw);
                    const float toRoom = bd.side == 0 ? -1.0f : 1.0f;
                    const float b0 = v + toRoom * (0.5f * pt), b1 = b0 + toRoom * (dw - 0.02f);
                    doorLeaf(m + 0.5f * dw - 0.04f, m + 0.5f * dw, std::min(b0, b1), std::max(b0, b1));
                }
                if (hasStair && &bd == coreBand) {
                    // The stairs open off the corridor over their whole apron.
                    const float a = apronDir > 0.0f ? core.x0 + 0.05f : core.x1 - kApron + 0.05f;
                    holes.push_back({a, a + kApron - 0.1f, y0, y0 + dh + 0.1f});
                    Curve c;
                    c.pts = {F.at(a, v, y0), F.at(a + kApron - 0.1f, v, y0), F.at(a + kApron - 0.1f, v, y0 + dh), F.at(a, v, y0 + dh)};
                    c.closed = true;
                    c.tag = "door";
                    out.curves.push_back(std::move(c));
                }
                wallAlong(v, 0.0f, L, holes);
            }
        }
        // The stair's own walls, floor to ceiling on the sides that have rooms.
        if (hasStair) {
            if (core.x0 > 0.3f) wallAcross(core.x0, core.y0, core.y1, {});
            if (core.x1 < L - 0.3f) {
                std::vector<Hole> holes;
                if (!hasCorridor) {
                    const float m = 0.5f * (core.y0 + core.y1);
                    holes.push_back({m - 0.5f * dw, m + 0.5f * dw, y0, y0 + dh});
                    doorMark(F.at(core.x1, m - 0.5f * dw, y0), F.at(core.x1, m + 0.5f * dw, y0));
                    trimAcross(core.x1, m - 0.5f * dw, m + 0.5f * dw);
                    doorLeaf(core.x1 + 0.5f * pt, core.x1 + 0.5f * pt + dw - 0.02f, m + 0.5f * dw - 0.04f, m + 0.5f * dw);
                }
                wallAcross(core.x1, core.y0, core.y1, holes);
            }
        }

        // Floors: wood, tile where it is wet. A stairwell in the floor from
        // the second storey up, and in the ceiling on every floor but the top.
        const Rect* holeBelow = hasStair && lv > 0 ? &well : nullptr;
        const Rect* holeAbove = hasStair && lv + 1 < levels ? &well : nullptr;
        auto floorOf = [&](const Rect& r, const AssetId& mat) {
            for (const Rect& q : minus(r, holeBelow)) w.floorRect(F, q, y0, kUp, mat);
        };
        for (const RoomPlan& rp : rooms) {
            const bool wet = rp.kind == "bath" || rp.kind == "wc" || rp.kind == "kitchen" || rp.kind == "utility";
            floorOf(rp.r, wet && tileM.valid() ? tileM : floorM);
        }
        if (hasCorridor) floorOf(corridor, floorM);
        if (hasStair) floorOf(core, floorM);
        for (const Rect& q : minus({0.0f, 0.0f, L, S}, holeAbove)) w.floorRect(F, q, y1, -kUp, ceilM);
        if (holeAbove) {
            // The well's sides, from this ceiling up to the next floor.
            const Rect& h = well;
            auto side = [&](float a0, float b0, float a1, float b1, const glm::vec3& away) {
                w.face({F.at(a0, b0, y1), F.at(a1, b1, y1), F.at(a1, b1, top), F.at(a0, b0, top)}, away, wallM);
            };
            side(h.x0, h.y0, h.x1, h.y0, F.dir(0.0f, 1.0f));
            side(h.x0, h.y1, h.x1, h.y1, F.dir(0.0f, -1.0f));
            side(h.x0, h.y0, h.x0, h.y1, F.dir(1.0f, 0.0f));
            side(h.x1, h.y0, h.x1, h.y1, F.dir(-1.0f, 0.0f));
        }

        // The stair up from this floor: slabs under each tread, so a flight
        // overhead leaves the headroom of the one beneath.
        if (hasStair && lv + 1 < levels) {
            const float rise = top - y0;
            const int   risers = std::max(4, static_cast<int>(std::round(rise / 0.175f)));
            const int   first = risers / 2, second = risers - first;
            const float step = rise / static_cast<float>(risers);
            const float tread = std::min(kTread, flightRun / static_cast<float>(std::max(first, second)));
            const float start = apronDir > 0.0f ? well.x0 : well.x1;
            const float va0 = well.y0, va1 = well.y0 + kFlight, vb0 = well.y1 - kFlight, vb1 = well.y1;
            auto tread1 = [&](float at, float len, float v0, float v1, float topY) {
                const float a = apronDir > 0.0f ? at : at - len;
                w.box(F, a, a + len, v0, v1, std::max(y0, topY - step - 0.18f), topY, stairM);
            };
            for (int i = 0; i < first; ++i)
                tread1(start + apronDir * static_cast<float>(i) * tread, tread, va0, va1, y0 + static_cast<float>(i + 1) * step);
            const float landY = y0 + static_cast<float>(first) * step;
            const float landAt = start + apronDir * static_cast<float>(first) * tread;
            const float farEnd = apronDir > 0.0f ? well.x1 : well.x0;
            w.box(F, std::min(landAt, farEnd), std::max(landAt, farEnd), well.y0, well.y1, landY - 0.2f, landY, stairM);
            for (int i = 0; i < second; ++i) {
                const float topY = landY + static_cast<float>(i + 1) * step;
                const float at = landAt - apronDir * static_cast<float>(i + 1) * tread;
                tread1(at, tread, vb0, vb1, topY);
            }
        }
        // The rail round the well on the floor it arrives at.
        if (holeBelow) {
            const float yr = y0;
            auto rail = [&](float a0, float a1, float b0, float b1) {
                w.box(F, a0, a1, b0, b1, yr + 0.92f, yr + 0.97f, frameM);
                const bool alongU = a1 - a0 > b1 - b0;
                const float len = alongU ? a1 - a0 : b1 - b0;
                const int posts = std::max(2, static_cast<int>(std::ceil(len / 1.0f)) + 1);
                for (int k = 0; k < posts; ++k) {
                    const float c = (alongU ? a0 : b0) + (len - 0.04f) * static_cast<float>(k) / static_cast<float>(posts - 1);
                    if (alongU) w.box(F, c, c + 0.04f, b0, b1, yr, yr + 0.92f, frameM);
                    else        w.box(F, a0, a1, c, c + 0.04f, yr, yr + 0.92f, frameM);
                }
            };
            const float t = 0.04f;
            // The first flight's top is the open side a walker must not step off.
            const float e = apronDir > 0.0f ? well.x0 : well.x1 - t;
            rail(e, e + t, well.y0, well.y0 + kFlight);
            rail(well.x0, well.x1, well.y0 - t, well.y0);
            rail(well.x0, well.x1, well.y1, well.y1 + t);
            const float far = apronDir > 0.0f ? well.x1 : well.x0 - t;
            rail(far, far + t, well.y0, well.y1);
        }

        // The rooms as lines on the floor, the walls' half taken off.
        auto emit = [&](const Rect& r, const std::string& kind) {
            const float h = 0.5f * pt;
            const float a0 = r.x0 > 0.01f ? r.x0 + h : r.x0, a1 = r.x1 < L - 0.01f ? r.x1 - h : r.x1;
            const float b0 = r.y0 > 0.01f ? r.y0 + h : r.y0, b1 = r.y1 < S - 0.01f ? r.y1 - h : r.y1;
            Curve c;
            c.pts = {F.at(a0, b0, y0), F.at(a1, b0, y0), F.at(a1, b1, y0), F.at(a0, b1, y0)};
            c.closed = true;
            c.tag = kind;
            c.height = H;
            out.curves.push_back(std::move(c));
        };
        for (const RoomPlan& rp : rooms) emit(rp.r, rp.kind);
        if (hasCorridor) emit(corridor, "corridor");
        if (hasStair) {
            emit(core, "hall");
            Curve st;
            st.pts = {F.at(well.x0, well.y0, y0), F.at(well.x1, well.y0, y0), F.at(well.x1, well.y1, y0), F.at(well.x0, well.y1, y0)};
            st.closed = true;
            st.tag = "stair";
            out.curves.push_back(std::move(st));
        }
    }
}

// ================================================================================
// Registration
// ================================================================================

struct RegisterInteriorNodes {
    template <class T>
    static void add(const char* category, const char* tip) {
        const T t;
        proc::registerType({t.typeId(), t.displayName(), category, tip,
                            [] { return std::unique_ptr<proc::Node>(std::make_unique<T>()); }});
    }
    RegisterInteriorNodes() {
        add<FurnishNode>("Furniture", "Furniture in rooms: every closed line on the floor, by its kind. Comes\n"
                                      "from Rooms or a Floor plan, or draw one (a Rectangle, not filled). Keeps\n"
                                      "doors, stairs and windows clear; what does not fit is left out.");
        add<FloorPlanNode>("Buildings", "A walkable house from a room programme: walls with real openings, a\n"
                                        "stair, a roof, and its rooms as lines for Furnish.");
        add<RoomsNode>("Buildings", "A Facade block made hollow: floors, ceilings, partitions and doors, a\n"
                                    "stair between storeys, windows cut through, a way in by the street\n"
                                    "door. Put it after the Facade; its rooms are lines for Furnish.");
    }
} g_registerInteriorNodes;

} // namespace

std::vector<proc::FurnishedPiece> proc::furnishPlan(const Geo& in, const FurnishSettings& s) {
    std::vector<FurnishedPiece> plan;
    furnishAll(in, Stuff{}, s, nullptr, &plan);
    return plan;
}
