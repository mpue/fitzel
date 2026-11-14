#include "PenHolder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>

#include <nlohmann/json.hpp>

#include "SolidBuild.hpp"

using fitzel::AssetId;

namespace penholder {

namespace {

using solidbuild::Builder;
using solidbuild::DV2;
using solidbuild::DV3;
using solidbuild::kPi;
using solidbuild::Ring;

constexpr int    kPer   = 6;            // corners a side of a cell's hexagon
constexpr int    kRing  = 6 * kPer;     // round a cell, and as many round its drain
constexpr double kSqrt3 = 1.7320508075688772;
constexpr double kSqrt2 = 1.4142135623730951;
constexpr double kPla   = 1.24;         // g/cm3
constexpr double kFilR  = 0.875;        // 1.75 mm filament's radius

// --- A cell's outlines -----------------------------------------------------------------

// The cells' hexagons have their flats left and right (a row runs along x) and
// their corners at 30 + 60k degrees; ring corner i lies at 30 + 10i degrees,
// on the hexagon or on the drain's round alike, so the two loft corner for
// corner. A hexagon's corners are corners 0, 6, 12, ... and the five between
// lie on its straight sides.
double angleOf(int i) { return kPi / 6.0 + 2.0 * kPi * static_cast<double>(i) / kRing; }

DV2 onHexagon(double apothem, double phi) {
    const double k = std::round(phi / (kPi / 3.0));
    return apothem / std::cos(phi - k * kPi / 3.0) * DV2(std::cos(phi), std::sin(phi));
}

Ring hexRing(Builder& b, DV2 c, double apothem, double z, bool clockwise) {
    Ring r;
    for (int i = 0; i < kRing; ++i) {
        const DV2 p = c + onHexagon(apothem, angleOf(i));
        r.push_back(b.vid({p.x, p.y, z}));
    }
    return clockwise ? solidbuild::reversed(r) : r;
}

Ring roundRing(Builder& b, DV2 c, double radius, double z, bool clockwise) {
    Ring r;
    for (int i = 0; i < kRing; ++i) {
        const double a = angleOf(i);
        r.push_back(b.vid({c.x + radius * std::cos(a), c.y + radius * std::sin(a), z}));
    }
    return clockwise ? solidbuild::reversed(r) : r;
}

// The band between two rings of as many corners, in triangles: a round below a
// hexagon is no flat quad.
void loftTris(Builder& b, const Ring& lo, const Ring& hi) {
    const std::size_t n = lo.size();
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t j = (i + 1) % n;
        b.face({lo[i], lo[j], hi[j]});
        b.face({lo[i], hi[j], hi[i]});
    }
}

// --- The honeycomb's outline -----------------------------------------------------------

// The cells on the honeycomb's lattice, in whole steps: x in half a pitch, y
// in pitch / (2 sqrt 3). A cell's middle and its tile's corners all land on
// whole numbers there, so the corners two tiles share are the same corner
// exactly -- doubles from cos and sin would differ in the last digit.
std::vector<glm::ivec2> lattice(const Params& p) {
    std::vector<glm::ivec2> out;
    if (p.layout == Layout::Round) {
        const int R = p.rings;
        for (int q = -R; q <= R; ++q)
            for (int r = std::max(-R, -q - R); r <= std::min(R, -q + R); ++r) out.emplace_back(2 * q + r, 3 * r);
    } else {
        for (int j = 0; j < p.rows; ++j)
            for (int i = 0; i < p.columns; ++i) out.emplace_back(2 * i + (j & 1), 3 * j);
    }
    return out;
}

DV2 latticePoint(glm::ivec2 k, double pitch) {
    return {0.5 * pitch * k.x, pitch / (2.0 * kSqrt3) * k.y};
}

// The outline of the tiles round the cells (the walls' middle lines: hexagons
// `pitch` across their flats), counter-clockwise, in lattice steps: the tiles'
// edges that only one tile has. Rows and rings of cells are one piece without
// holes, so this is one loop.
std::vector<glm::ivec2> tileOutline(const std::vector<glm::ivec2>& cells) {
    static const glm::ivec2 corner[6] = {{1, 1}, {0, 2}, {-1, 1}, {-1, -1}, {0, -2}, {1, -1}};   // 30, 90, ... degrees
    using Key = std::pair<int, int>;
    std::map<std::pair<Key, Key>, int> edges;
    for (const glm::ivec2& c : cells)
        for (int i = 0; i < 6; ++i) {
            const glm::ivec2 a = c + corner[i], b = c + corner[(i + 1) % 6];
            const Key u(a.x, a.y), v(b.x, b.y);
            if (const auto back = edges.find({v, u}); back != edges.end()) edges.erase(back);
            else ++edges[{u, v}];
        }
    std::map<Key, Key> next;
    for (const auto& [e, n] : edges) next[e.first] = e.second;
    std::vector<glm::ivec2> out;
    if (next.empty()) return out;
    const Key start = next.begin()->first;
    Key u = start;
    for (std::size_t guard = 0; guard <= next.size(); ++guard) {
        out.emplace_back(u.first, u.second);
        u = next[u];
        if (u == start) break;
    }
    return out;
}

// A counter-clockwise outline moved out by `d` along its edges' normals.
std::vector<DV2> offsetOut(const std::vector<DV2>& poly, double d) {
    std::vector<DV2> out;
    const std::size_t n = poly.size();
    for (std::size_t i = 0; i < n; ++i) {
        const DV2 A = poly[(i + n - 1) % n], P = poly[i], B = poly[(i + 1) % n];
        const DV2 da = glm::normalize(P - A), db = glm::normalize(B - P);
        const DV2 na(da.y, -da.x), nb(db.y, -db.x);
        out.push_back(P + d * (na + nb) / (1.0 + glm::dot(na, nb)));
    }
    return out;
}

std::vector<DV2> convexHull(std::vector<DV2> pts) {
    std::sort(pts.begin(), pts.end(), [](DV2 a, DV2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    auto cross = [](DV2 o, DV2 a, DV2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
    std::vector<DV2> h(2 * pts.size());
    std::size_t k = 0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 1e-9) --k;
        h[k++] = pts[i];
    }
    for (std::size_t i = pts.size() - 1, t = k + 1; i-- > 0;) {
        while (k >= t && cross(h[k - 2], h[k - 1], pts[i]) <= 1e-9) --k;
        h[k++] = pts[i];
    }
    h.resize(k > 1 ? k - 1 : k);
    return h;
}

// The foot: the hull round the honeycomb, `f` further out, its corners round.
std::vector<DV2> footOutline(const std::vector<DV2>& hull, double f) {
    std::vector<DV2> out;
    const std::size_t n = hull.size();
    for (std::size_t i = 0; i < n; ++i) {
        const DV2 A = hull[(i + n - 1) % n], P = hull[i], B = hull[(i + 1) % n];
        const DV2 da = glm::normalize(P - A), db = glm::normalize(B - P);
        const double a0 = std::atan2(-da.x, da.y);
        double a1 = std::atan2(-db.x, db.y);
        while (a1 < a0) a1 += 2.0 * kPi;
        const int seg = std::max(1, static_cast<int>(std::ceil((a1 - a0) / (kPi / 12.0))));
        for (int s = 0; s <= seg; ++s) {
            const double a = a0 + (a1 - a0) * s / seg;
            out.push_back(P + f * DV2(std::cos(a), std::sin(a)));
        }
    }
    return out;
}

// The honeycomb seen from above: its cells' middles and the outline of its
// outside wall, the cells' middle at the origin.
struct Plan {
    std::vector<DV2> centres, rim;
    double ap = 0.0;   // a cell's flats from its middle
};

Plan planOf(const Params& p) {
    const double cell = p.pen + p.play, w = p.wall, pitch = cell + w;
    const std::vector<glm::ivec2> cells = lattice(p);
    Plan pl;
    pl.ap = 0.5 * cell;
    DV2 lo(1e30), hi(-1e30);
    for (const glm::ivec2& k : cells) {
        pl.centres.push_back(latticePoint(k, pitch));
        lo = glm::min(lo, pl.centres.back());
        hi = glm::max(hi, pl.centres.back());
    }
    const DV2 mid = 0.5 * (lo + hi);
    for (DV2& c : pl.centres) c -= mid;
    std::vector<DV2> tiles;
    for (const glm::ivec2& k : tileOutline(cells)) tiles.push_back(latticePoint(k, pitch) - mid);
    pl.rim = offsetOut(tiles, 0.5 * w);
    return pl;
}

// Where the walls' top lies: `H` at its highest, falling by `k` (the slope's
// tangent) a millimetre further from the middle -- or towards the front. A
// mouth's corners each at their own height: the walls stay upright planes cut
// off by the slope, and the top, filled in plan, follows it.
struct Top {
    bool   front = false;
    double H = 0.0, k = 0.0, ref = 0.0;   // ref: where the top is highest
    double dist(DV2 q) const { return front ? -q.y : glm::length(q); }
    double at(DV2 q) const { return H - k * (dist(q) - ref); }
};

Top topOf(const Params& p, const Plan& pl) {
    Top t;
    t.front = p.slopeTo == SlopeTo::Front;
    t.H = p.height;
    t.k = std::tan(static_cast<double>(p.slope) * kPi / 180.0);
    t.ref = 1e30;
    for (const DV2& q : pl.rim) t.ref = std::min(t.ref, t.dist(q));
    for (const DV2& c : pl.centres)
        for (int i = 0; i < kRing; ++i) t.ref = std::min(t.ref, t.dist(c + onHexagon(pl.ap, angleOf(i))));
    return t;
}

// How far the farthest corner of a mouth lies down the slope from its top.
double farthestMouth(const Top& t, const Plan& pl) {
    double far = 0.0;
    for (const DV2& c : pl.centres)
        for (int i = 0; i < kRing; ++i) far = std::max(far, t.dist(c + onHexagon(pl.ap, angleOf(i))) - t.ref);
    return far;
}

// A cell's mouth: its hexagon's corners on the top, clockwise (a hole's).
Ring mouthRing(Builder& b, DV2 c, double apothem, const Top& t) {
    Ring r;
    for (int i = 0; i < kRing; ++i) {
        const DV2 q = c + onHexagon(apothem, angleOf(i));
        r.push_back(b.vid({q.x, q.y, t.at(q)}));
    }
    return solidbuild::reversed(r);
}

std::vector<glm::vec2> toView(const std::vector<DV2>& pts) {
    std::vector<glm::vec2> out;
    for (const DV2& p : pts) out.emplace_back(p);
    return out;
}

std::uint32_t toByte(float v) {
    return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

} // namespace

float minThroat(float drain) {
    return std::max(2.0f, 0.5f * drain / std::tan(11.0f * static_cast<float>(kPi) / 180.0f) + 1.0f);
}

const char* slopeName(SlopeTo s) {
    switch (s) {
    case SlopeTo::Rim:   return "To the rim";
    case SlopeTo::Front: return "To the front";
    default:             return "?";
    }
}

const char* layoutName(Layout l) {
    switch (l) {
    case Layout::Round: return "Round";
    case Layout::Rows:  return "Rows";
    default:            return "?";
    }
}

Params sane(const Params& in) {
    Params p = in;
    p.layout  = static_cast<Layout>(std::clamp(static_cast<int>(in.layout), 0, static_cast<int>(Layout::Count) - 1));
    p.rings   = std::clamp(in.rings, 0, 4);
    p.columns = std::clamp(in.columns, 1, 12);
    p.rows    = std::clamp(in.rows, 1, 8);
    // A cell takes one pen: never more than kMaxCell across its flats.
    p.play = std::clamp(in.play, 0.3f, 3.0f);
    p.pen  = std::clamp(in.pen, 4.0f, kMaxCell - p.play);
    const float cell = p.pen + p.play;
    // The drain's tube and the funnel's rim round it inside the cell.
    p.wall  = std::clamp(in.wall, 0.6f, std::min(3.0f, 0.5f * (cell - 2.5f)));
    p.drain = std::clamp(in.drain, 1.5f, std::max(1.5f, std::min(6.0f, cell - 2.0f * p.wall - 1.0f)));
    // A pencil's point (sharpened to 22 degrees) standing in the drain a
    // millimetre clear of the desk at least: the wider the drain, the longer
    // its throat.
    p.tipRoom = std::clamp(in.tipRoom, minThroat(p.drain), 25.0f);
    // The cells at least 10 mm deep above their funnels.
    const float funnelTop = p.tipRoom + cell / static_cast<float>(kSqrt3) - 0.5f * p.drain;
    p.height = std::clamp(in.height, funnelTop + 10.0f, 200.0f);
    // The slope takes height off the outer cells: no more than leaves the
    // shallowest one 10 mm deep above its funnel.
    p.slopeTo = static_cast<SlopeTo>(std::clamp(static_cast<int>(in.slopeTo), 0, static_cast<int>(SlopeTo::Count) - 1));
    p.slope = std::clamp(in.slope, 0.0f, 45.0f);
    if (p.slope > 0.0f) {
        const Plan pl = planOf(p);
        const double far = farthestMouth(topOf(p, pl), pl);
        const double room = std::max(0.0, static_cast<double>(p.height - funnelTop - 10.0f));
        if (far > 1e-9)
            p.slope = std::min(p.slope, static_cast<float>(std::atan(room / far) * 180.0 / kPi));
    }
    p.foot = in.foot < 0.5f ? 0.0f : std::min(in.foot, 20.0f);
    p.footHeight = std::clamp(in.footHeight, 0.6f, 5.0f);
    return p;
}

std::vector<glm::dvec2> cellCentres(const Params& in) {
    const Params p = sane(in);
    const double pitch = p.pen + p.play + p.wall;
    std::vector<DV2> out;
    for (const glm::ivec2& k : lattice(p)) out.push_back(latticePoint(k, pitch));
    DV2 lo(1e30), hi(-1e30);
    for (const DV2& c : out) {
        lo = glm::min(lo, c);
        hi = glm::max(hi, c);
    }
    const DV2 mid = 0.5 * (lo + hi);
    for (DV2& c : out) c -= mid;
    return out;
}

Model build(const Params& in) {
    const Params p = sane(in);
    Model out;
    Builder b;
    // Sizes across a cell: its flats `ap` out from its middle, its corners Rc;
    // the drain's round rd, the tube round it rt. Heights: the drain's throat
    // up to zc, the funnel from there to the walls at zf (45 degrees to the
    // corners, steeper to the flats), its underside `lift` lower all the way
    // (the shell as thick as a wall, measured square to it), from the tube's
    // top at zt to the walls at zfU.
    const double cell = p.pen + p.play, w = p.wall, pitch = cell + w;
    const double ap = 0.5 * cell, Rc = cell / kSqrt3;
    const double rd = 0.5 * p.drain, rt = rd + w;
    const double lift = w * kSqrt2;
    const double zc = p.tipRoom, zf = zc + (Rc - rd), zfU = zf - lift, zt = zc + w - lift;
    const double H = p.height;
    const bool   foot = p.foot > 0.0f;
    const double fh = foot ? p.footHeight : 0.0;

    const Plan layout = planOf(p);
    const std::vector<DV2>& centres = layout.centres;
    const std::vector<DV2>& rim = layout.rim;
    const Top top = topOf(p, layout);
    const std::vector<DV2> footPts = foot ? footOutline(convexHull(rim), p.foot) : std::vector<DV2>{};
    auto ringOf = [&](const std::vector<DV2>& pts, double z) {
        Ring r;
        for (const DV2& q : pts) r.push_back(b.vid({q.x, q.y, z}));
        return r;
    };

    // The outside walls, from the foot's top (or the desk) up.
    const Ring rimLo = ringOf(rim, fh);
    Ring rimHi;
    for (const DV2& q : rim) rimHi.push_back(b.vid({q.x, q.y, top.at(q)}));
    b.loft(rimLo, rimHi);

    // Every cell: walls down to the funnel, the funnel into the drain's throat,
    // and under it the hollow -- its walls the cell's own, its roof the
    // funnel's underside, round the drain's tube standing on the desk.
    std::vector<Ring> mouths, hollows;
    for (const DV2& c : centres) {
        const Ring drain0 = roundRing(b, c, rd, 0.0, true), drainC = roundRing(b, c, rd, zc, true);
        const Ring cupF = hexRing(b, c, ap, zf, true), cupH = mouthRing(b, c, ap, top);
        b.loft(drain0, drainC);
        loftTris(b, drainC, cupF);
        b.loft(cupF, cupH);
        const Ring tube0 = roundRing(b, c, rt, 0.0, false), tubeT = roundRing(b, c, rt, zt, false);
        const Ring roof = hexRing(b, c, ap, zfU, false);
        b.loft(tube0, tubeT);
        loftTris(b, tubeT, roof);
        const Ring hollow0 = hexRing(b, c, ap, 0.0, true);
        b.loft(hollow0, solidbuild::reversed(roof));
        b.region({tube0}, {drain0}, false);
        mouths.push_back(cupH);
        hollows.push_back(hollow0);
    }
    b.region({rimHi}, mouths, true);
    if (foot) {
        const Ring f0 = ringOf(footPts, 0.0), f1 = ringOf(footPts, fh);
        b.loft(f0, f1);
        b.region({f1}, {rimLo}, true);
        b.region({f0}, hollows, false);
    } else {
        b.region({rimLo}, hollows, false);
    }
    out.lowestTop = 1e30;
    for (const DV2& q : rim) out.lowestTop = std::min(out.lowestTop, top.at(q));
    out.shallowest = H - top.k * farthestMouth(top, layout) - zf;
    if (!b.error.empty()) {
        out.ok = false;
        out.error = b.error;
    }

    // Centred on its middle, seen from above.
    DV3 lo(1e30), hi(-1e30);
    for (const DV3& v : b.verts) {
        lo = glm::min(lo, v);
        hi = glm::max(hi, v);
    }
    const DV3 mid(0.5 * (lo.x + hi.x), 0.5 * (lo.y + hi.y), 0.0);
    for (DV3& v : b.verts) v -= mid;
    out.size = glm::vec3(hi - lo);

    out.volume = solidbuild::volume(b);
    out.grams  = out.volume / 1000.0 * kPla;
    out.metres = out.volume / (kPi * kFilR * kFilR) / 1000.0;
    out.cells  = static_cast<int>(centres.size());
    for (const DV2& c : centres) out.centres.push_back(c - DV2(mid));
    out.cell   = cell;
    out.widest = 2.0 * Rc;
    out.body   = solidbuild::toEditMesh(b);

    char hint[256];
    std::snprintf(hint, sizeof hint,
                  "Prints standing, without support (the funnels' undersides lean 45 degrees). Every wall is "
                  "%.2f mm: two lines of %.2f mm, so no infill is laid anywhere.", w, 0.5 * w);
    out.printHint = hint;

    View plan;
    plan.name = "From above";
    auto shifted = [&](std::vector<DV2> pts) {
        for (DV2& q : pts) q -= DV2(mid);
        return toView(pts);
    };
    if (foot) plan.loops.push_back(shifted(footPts));
    plan.loops.push_back(shifted(rim));
    for (const DV2& c : centres) {
        std::vector<DV2> hex;
        for (int i = 0; i < 6; ++i) hex.push_back(c + onHexagon(ap, angleOf(i * kPer)));
        plan.loops.push_back(shifted(hex));
    }
    plan.solids = static_cast<int>(plan.loops.size());
    for (const DV2& c : centres) {
        std::vector<DV2> round;
        for (int i = 0; i < kRing; ++i) round.push_back(c + rd * DV2(std::cos(angleOf(i)), std::sin(angleOf(i))));
        plan.loops.push_back(shifted(round));
    }
    out.views.push_back(std::move(plan));

    // Cut front to back through the middle of a cell in the front row's middle
    // (a hair off it, so the cut meets no corner).
    DV2 through = centres.front();
    {
        double best = 1e30;
        const double front = std::min_element(centres.begin(), centres.end(),
                                              [](DV2 a, DV2 c) { return a.y < c.y; })->y;
        for (const DV2& c : centres)
            if (c.y < front + 1e-6 && std::abs(c.x) < best) {
                best = std::abs(c.x);
                through = c;
            }
    }
    View cut;
    cut.name = "Cut";
    cut.loops = solidbuild::section(out.body, static_cast<float>(through.x - mid.x + 0.031));
    cut.solids = static_cast<int>(cut.loops.size());
    if (!cut.loops.empty()) out.views.push_back(std::move(cut));
    return out;
}

AssetId ensureMaterial(std::vector<MaterialDef>& mats, const Params& p) {
    char name[64];
    std::snprintf(name, sizeof name, "Pen holder#%02X%02X%02X", toByte(p.color.r), toByte(p.color.g), toByte(p.color.b));
    for (MaterialDef& m : mats) {
        if (m.name != name) continue;
        m.albedo = p.color;
        if (!m.assetId.valid()) m.assetId = AssetId::generate();
        return m.assetId;
    }
    MaterialDef md;
    md.assetId      = AssetId::generate();
    md.name         = name;
    md.albedo       = p.color;
    md.reflectivity = 0.0f;
    md.roughness    = 0.55f;   // printed plastic: a dull sheen
    mats.push_back(md);
    return md.assetId;
}

std::string autoName(const Params& in) {
    const Params p = sane(in);
    char b[96];
    std::snprintf(b, sizeof b, "Pen holder %d x %.4g mm", static_cast<int>(cellCentres(p).size()), p.pen);
    return b;
}

std::vector<Entity> entities(const Params& in, const AssetId& material, int& counter, const glm::vec3& at) {
    const Params p = sane(in);
    Model mdl = build(p);
    if (mdl.body.faces.empty()) return {};
    std::vector<Entity> out;
    Entity root;
    root.type = EntityType::Empty;
    root.name = autoName(p);
    root.id = counter++;
    root.parent = -1;
    // The root in the middle of the holder, so its pick box is the holder's:
    // `at` is where its bottom stands.
    glm::vec3 lo(1e9f), hi(-1e9f);
    for (const glm::vec3& v : mdl.body.verts) {
        lo = glm::min(lo, v);
        hi = glm::max(hi, v);
    }
    const glm::vec3 mid = 0.5f * (lo + hi);
    root.half = glm::max(0.5f * (hi - lo), glm::vec3(1e-4f));
    root.localCenter = root.center = at + glm::vec3(0.0f, root.half.y, 0.0f);
    {
        auto pc = std::make_unique<PenHolderComponent>();
        pc->params = p;
        root.components.items.push_back(std::move(pc));
    }
    out.push_back(std::move(root));
    const int rootId = out.front().id;
    const glm::vec3 rootC = out.front().center;

    EditMesh m = std::move(mdl.body);
    for (glm::vec3& v : m.verts) v -= mid;
    const glm::vec3 shift = editmesh::recenter(m);
    glm::vec3 mn, mx;
    m.bounds(mn, mx);
    Entity e;
    e.type = EntityType::Box;
    e.name = "Holder";
    e.id = counter++;
    e.parent = rootId;
    e.half = glm::max(0.5f * (mx - mn), glm::vec3(1e-4f));
    e.localCenter = shift;
    e.center = rootC + shift;
    auto mc = std::make_unique<MeshComponent>();
    mc->mesh = std::move(m);
    mc->touch();
    e.components.items.push_back(std::move(mc));
    auto mt = std::make_unique<MaterialComponent>();
    mt->material = material;
    e.components.items.push_back(std::move(mt));
    auto ph = std::make_unique<PhysicsComponent>();
    ph->dynamic = false;
    e.components.items.push_back(std::move(ph));
    out.push_back(std::move(e));
    return out;
}

// --- Persistence -------------------------------------------------------------------

void saveParams(nlohmann::json& j, const Params& p) {
    j["layout"]     = static_cast<int>(p.layout);
    j["rings"]      = p.rings;
    j["columns"]    = p.columns;
    j["rows"]       = p.rows;
    j["pen"]        = p.pen;
    j["play"]       = p.play;
    j["height"]     = p.height;
    j["slope"]      = p.slope;
    j["slopeTo"]    = static_cast<int>(p.slopeTo);
    j["wall"]       = p.wall;
    j["drain"]      = p.drain;
    j["tipRoom"]    = p.tipRoom;
    j["foot"]       = p.foot;
    j["footHeight"] = p.footHeight;
    j["color"]      = nlohmann::json::array({p.color.r, p.color.g, p.color.b});
}

void loadParams(const nlohmann::json& j, Params& p) {
    const Params d;
    p.layout     = static_cast<Layout>(j.value("layout", static_cast<int>(d.layout)));
    p.rings      = j.value("rings", d.rings);
    p.columns    = j.value("columns", d.columns);
    p.rows       = j.value("rows", d.rows);
    p.pen        = j.value("pen", d.pen);
    p.play       = j.value("play", d.play);
    p.height     = j.value("height", d.height);
    p.slope      = j.value("slope", d.slope);
    p.slopeTo    = static_cast<SlopeTo>(j.value("slopeTo", static_cast<int>(d.slopeTo)));
    p.wall       = j.value("wall", d.wall);
    p.drain      = j.value("drain", d.drain);
    p.tipRoom    = j.value("tipRoom", d.tipRoom);
    p.foot       = j.value("foot", d.foot);
    p.footHeight = j.value("footHeight", d.footHeight);
    p.color      = d.color;
    if (const auto c = j.find("color"); c != j.end() && c->is_array() && c->size() == 3)
        p.color = {(*c)[0].get<float>(), (*c)[1].get<float>(), (*c)[2].get<float>()};
    p = sane(p);
}

namespace {
// Scene files name components by type id; this lets a saved holder come back
// as one, parameters and all.
struct RegisterPenHolder {
    RegisterPenHolder() {
        components::registerType({"penHolder", "Pen holder",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<PenHolderComponent>()); },
            /*addable=*/false});
    }
} g_registerPenHolder;
} // namespace

} // namespace penholder
