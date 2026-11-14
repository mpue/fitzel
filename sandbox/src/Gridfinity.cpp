#include "Gridfinity.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "SolidBuild.hpp"

using fitzel::AssetId;

namespace gridfinity {

namespace {

static_assert(kArcSegments == solidbuild::kArc, "the check counts the corners' segments");

// The mesh under construction, the outlines and the flat faces between them:
// SolidBuild.hpp.
using namespace solidbuild;

// --- A bin -------------------------------------------------------------------------

// A compartment's outline at height z: the floor's fillet on every side, the
// scoop at the front, the label shelf at the back (its top flush with the
// compartment's, a straight 1.2 mm edge, then the underside back to the wall
// at 36 deg), and under a stacking lip, on the sides against the outer wall,
// the lip's 45 deg support coming in from the wall to the lip's inner face.
struct Pocket {
    double rc = 0.0;      // corner radius up the walls
    double rf = 0.0;      // the floor's fillet
    double rs = 0.0;      // the scoop, 0 for none
    double td = 0.0;      // the shelf's depth, 0 for none
    double slope = 0.0;   // how far its underside drops
    double lipIn = 0.0;   // how far the lip's support reaches in from the wall at the top
    double zTop = 0.0, zFloor = 0.0;

    // `outer`: which sides (left, right, front, back) stand against the outer wall.
    Rect at(const Rect& q, double z, const std::array<bool, 4>& outer) const {
        const double h = z - zFloor;
        auto arcIn = [h](double r) { return (r > 0.0 && h < r) ? r - std::sqrt(std::max(0.0, r * r - (r - h) * (r - h))) : 0.0; };
        const double ef = arcIn(rf), es = arcIn(rs);
        const double ew = std::max(0.0, lipIn - (zTop - z));
        double et = 0.0;
        if (td > 0.0) {
            const double z1 = zTop - 1.2, z0 = z1 - slope;
            et = z >= z1 ? td : (z > z0 ? td * (z - z0) / slope : 0.0);
        }
        auto side = [&](int s, double e) { return outer[static_cast<std::size_t>(s)] ? std::max(e, ew) : e; };
        const bool walled = outer[0] || outer[1] || outer[2] || outer[3];
        const double r = std::max(0.0, rc - std::max(ef, walled ? ew : 0.0));
        const double left = side(0, ef), right = side(1, ef);
        const double front0 = side(2, ef), back0 = side(3, ef);
        double front = std::max(front0, es), back = std::max(back0, et);
        // The scoop and the shelf never meet: what they take beyond the walls
        // shrinks to leave 0.4 mm between them.
        const double room = (q.y1 - q.y0) - 2.0 * r - 0.4 - front0 - back0;
        const double want = (front - front0) + (back - back0);
        if (want > room && want > 0.0) {
            const double k = std::max(0.0, room) / want;
            front = front0 + (front - front0) * k;
            back  = back0 + (back - back0) * k;
        }
        return {q.x0 + left, q.x1 - right, q.y0 + front, q.y1 - back, r};
    }

    // The heights it is drawn at, top to bottom.
    std::vector<double> levels() const {
        std::vector<double> lv{zTop, zFloor};
        if (lipIn > 0.0) lv.push_back(zTop - lipIn);
        if (td > 0.0) {
            lv.push_back(zTop - 1.2);
            lv.push_back(zTop - 1.2 - slope);
        }
        for (int j = 0; j <= kArc; ++j)
            lv.push_back(zFloor + rf * (1.0 - std::sin(0.5 * kPi * j / kArc)));
        if (rs > 0.0)
            for (int j = 0; j <= 10; ++j) lv.push_back(zFloor + rs * (1.0 - std::sin(0.5 * kPi * j / 10.0)));
        std::vector<double> out;
        std::sort(lv.begin(), lv.end(), std::greater<double>());
        for (double z : lv) {
            if (z > zTop + 1e-9 || z < zFloor - 1e-9) continue;
            if (!out.empty() && out.back() - z < 0.01) {
                if (z == zFloor) out.back() = zFloor;   // the floor itself, exactly
                continue;
            }
            out.push_back(z);
        }
        return out;
    }
};

void buildBin(const Params& p, Builder& b, Model& out) {
    const int nx = p.unitsX, ny = p.unitsY;
    const double A = 21.0 * nx - 0.25, B = 21.0 * ny - 0.25, R = kBinRadius;
    const double H = kHeightU * p.unitsZ;
    const double t = p.wall;

    // --- The feet, one a grid unit -------------------------------------------------
    // Inset from the 41.5 mm top and height, from the bottom up.
    static const DV2 kFoot[] = {{2.95, 0.0}, {2.15, 0.8}, {2.15, 2.6}, {0.0, kFootH}};
    std::vector<Ring> footTops;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const DV2 c((i - 0.5 * (nx - 1)) * kPitch, (j - 0.5 * (ny - 1)) * kPitch);
            std::vector<Outline> rings;
            for (const DV2& f : kFoot)
                rings.push_back(rrect(b, {c.x - 20.75 + f.x, c.x + 20.75 - f.x, c.y - 20.75 + f.x,
                                          c.y + 20.75 - f.x, R - f.x}, f.y));
            for (std::size_t k = 0; k + 1 < rings.size(); ++k) loft(b, rings[k], rings[k + 1]);
            // Holes bored up from below: clockwise, so the same loft faces in.
            std::vector<Ring> holes;
            if (p.magnets || p.screws)
                for (const DV2 o : {DV2(13, 13), DV2(-13, 13), DV2(-13, -13), DV2(13, -13)}) {
                    const DV2 hc = c + o;
                    Ring bottom, top;
                    if (p.magnets) {
                        bottom = circle(b, hc, 3.25, 0.0, true);
                        top = circle(b, hc, 3.25, 2.4, true);
                        b.loft(bottom, top);
                    }
                    if (p.screws) {
                        const Ring s0 = circle(b, hc, 1.5, p.magnets ? 2.4 : 0.0, true);
                        const Ring s1 = circle(b, hc, 1.5, 6.0, true);
                        if (p.magnets) b.loft(top, s0);   // the step, facing down
                        else bottom = s0;
                        b.loft(s0, s1);
                        top = s1;
                    }
                    b.cap(top, true);
                    holes.push_back(reversed(bottom));
                }
            if (holes.empty()) b.cap(rings.front().ids, false);
            else b.region({rings.front().ids}, holes, false);
            footTops.push_back(rings.back().ids);
        }

    // --- The compartments --------------------------------------------------------
    const bool lip = p.lip;
    // Under a lip the compartments (and dividers) end at the bottom of its
    // inner face, 1.2 below the walls' top.
    const double zTop = lip ? H - 1.2 : H;
    const bool hollow = zTop - p.floor >= 1.0;
    std::vector<Ring> pocketTops;
    std::vector<std::vector<Outline>> pockets;
    if (hollow) {
        const int cx = p.compX, cy = p.compY;
        const double dv = p.divider;
        const double pw = (2.0 * (A - t) - (cx - 1) * dv) / cx;
        const double pd = (2.0 * (B - t) - (cy - 1) * dv) / cy;
        Pocket ps;
        ps.zTop   = zTop;
        ps.zFloor = p.floor;
        ps.rc     = R - t;
        ps.lipIn  = lip ? 2.6 - t : 0.0;
        const double depth = zTop - p.floor;
        ps.rf = std::min(ps.rc, depth);
        if (p.scoop) ps.rs = std::max(ps.rf, std::min({static_cast<double>(p.scoopRadius), depth, 0.75 * pd}));
        ps.td = p.label ? std::min(static_cast<double>(p.labelDepth), pd - 2.0 * ps.rc - 2.0) : 0.0;
        if (ps.td < 2.0) ps.td = 0.0;
        ps.slope = ps.td * std::tan(36.0 * kPi / 180.0);
        const std::vector<double> lv = ps.levels();
        for (int yy = 0; yy < cy; ++yy)
            for (int xx = 0; xx < cx; ++xx) {
                Rect q;
                q.x0 = -A + t + xx * (pw + dv);
                q.x1 = xx == cx - 1 ? A - t : q.x0 + pw;
                q.y0 = -B + t + yy * (pd + dv);
                q.y1 = yy == cy - 1 ? B - t : q.y0 + pd;
                q.r  = ps.rc;
                const std::array<bool, 4> outer{xx == 0, xx == cx - 1, yy == 0, yy == cy - 1};
                std::vector<Outline> rings;
                for (double z : lv) rings.push_back(rrect(b, ps.at(q, z, outer), z));
                pocketTops.push_back(rings.front().ids);
                pockets.push_back(std::move(rings));
            }
        out.compartments = cx * cy;
        // What one holds: the bands are flat, so between two outlines the area
        // grows as a square of the height and Simpson's rule is exact (the
        // middle outline is the two averaged corner for corner).
        const std::vector<Outline>& r0 = pockets.front();
        for (std::size_t k = 0; k + 1 < r0.size(); ++k) {
            const Ring& u = r0[k].ids;
            const Ring& l = r0[k + 1].ids;
            double mid = 0.0;
            for (std::size_t i = 0; i < u.size(); ++i) {
                const std::size_t j = (i + 1) % u.size();
                const DV3 p0 = 0.5 * (b.verts[static_cast<std::size_t>(u[i])] + b.verts[static_cast<std::size_t>(l[i])]);
                const DV3 p1 = 0.5 * (b.verts[static_cast<std::size_t>(u[j])] + b.verts[static_cast<std::size_t>(l[j])]);
                mid += 0.5 * (p0.x * p1.y - p1.x * p0.y);
            }
            out.capacity += (lv[k] - lv[k + 1]) / 6.0 * (area2(b, u) + 4.0 * mid + area2(b, l));
        }
    }

    // --- The outside: up the wall, over the lip, down to the compartments ---------
    std::vector<DV2> prof{{0.0, kFootH}};
    if (!lip) {
        prof.push_back({0.0, H});
        out.lipTop = 0.0f;
    } else {
        // The lip's point rounded by r 0.6: the arc from the outer wall over to
        // the 45 deg slope inside.
        const double fr = 0.6, fc = H + kLipH - fr - fr * std::sqrt(2.0);
        prof.push_back({0.0, fc});
        for (int i = 1; i <= 6; ++i) {
            const double a = kPi - 0.75 * kPi * i / 6.0;
            prof.push_back({fr + fr * std::cos(a), fc + fr * std::sin(a)});
        }
        prof.push_back({1.9, H + 2.5});
        prof.push_back({1.9, H + 0.7});
        prof.push_back({2.6, H});
        if (hollow) prof.push_back({2.6, zTop});
        out.lipTop = static_cast<float>(fc + fr - H);
    }
    // The bottom outline is cut where a foot's side runs along it, the last one
    // (the lip's inner face) where a compartment's does; the bands in between
    // zip the cuts together.
    Splits feet, comps;
    for (const Ring& r : footTops) gatherSplits(b, r, A, B, 0.0, feet);
    feet.tidy();
    if (lip)
        for (const Ring& r : pocketTops) gatherSplits(b, r, A, B, 2.6, comps);
    comps.tidy();
    std::vector<Outline> outside;
    for (std::size_t k = 0; k < prof.size(); ++k) {
        const DV2& f = prof[k];
        const Splits* s = k == 0 ? &feet : (k + 1 == prof.size() ? &comps : nullptr);
        outside.push_back(rrect(b, {-A + f.x, A - f.x, -B + f.x, B - f.x, R - f.x}, f.y, s));
    }
    for (std::size_t k = 0; k + 1 < outside.size(); ++k) loft(b, outside[k], outside[k + 1]);

    // Under the bin between the feet, and the top of the walls and dividers.
    b.region({outside.front().ids}, footTops, false);
    if (hollow) b.region({outside.back().ids}, pocketTops, true);
    else b.cap(outside.back().ids, true);

    for (const std::vector<Outline>& rings : pockets) {
        for (std::size_t k = 0; k + 1 < rings.size(); ++k) loft(b, rings[k], rings[k + 1]);
        b.cap(rings.back().ids, true);
    }

    out.size = glm::vec3(static_cast<float>(2.0 * A), static_cast<float>(2.0 * B),
                         static_cast<float>(H) + out.lipTop);
    out.plan.push_back(plan2(b, outside.front().ids));
    for (const std::vector<Outline>& rings : pockets) {
        out.plan.push_back(plan2(b, rings.front().ids));
        out.plan.push_back(plan2(b, rings.back().ids));
    }
}

// --- A baseplate ---------------------------------------------------------------------

void buildPlate(const Params& p, Builder& b, Model& out) {
    const int nx = p.plateX, ny = p.plateY;
    double W = kPitch * nx, D = kPitch * ny;
    if (p.fitDrawer) {
        W = std::max(W, static_cast<double>(p.drawerW));
        D = std::max(D, static_cast<double>(p.drawerD));
    }
    const double A = 0.5 * W, B = 0.5 * D, R = 4.0;
    const double f = p.plateFloor ? p.floorThickness : 0.0;
    const double top = f + 5.0;

    // A cell's opening from the top down: inset from the 42 mm cell, height.
    const DV2 kCell[] = {{0.0, f + 5.0}, {2.15, f + 2.85}, {2.15, f + 1.05}, {2.85, f + 0.35}, {2.85, f}};
    std::vector<Ring> cellTops, cellBottoms, bottomHoles;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const DV2 c((i - 0.5 * (nx - 1)) * kPitch, (j - 0.5 * (ny - 1)) * kPitch);
            std::vector<Outline> rings;
            for (const DV2& k : kCell)
                rings.push_back(rrect(b, {c.x - 21.0 + k.x, c.x + 21.0 - k.x, c.y - 21.0 + k.x,
                                          c.y + 21.0 - k.x, R - k.x}, k.y));
            for (std::size_t k = 0; k + 1 < rings.size(); ++k) loft(b, rings[k], rings[k + 1]);
            cellTops.push_back(rings.front().ids);
            if (!p.plateFloor) {
                cellBottoms.push_back(rings.back().ids);
                continue;
            }
            // The floor, with the magnets' pockets under a bin's and a
            // countersunk screw hole in the middle -- bored down from above.
            std::vector<Ring> holes;
            if (p.plateMagnets)
                for (const DV2 o : {DV2(13, 13), DV2(-13, 13), DV2(-13, -13), DV2(13, -13)}) {
                    const Ring m0 = circle(b, c + o, 3.25, f, false);
                    const Ring m1 = circle(b, c + o, 3.25, f - 2.4, false);
                    b.loft(m0, m1);
                    b.cap(m1, true);
                    holes.push_back(m0);
                    out.holes.push_back(plan2(b, m0));
                }
            if (p.plateScrews) {
                const Ring s0 = circle(b, c, 3.5, f, false);
                const Ring s1 = circle(b, c, 1.75, f - 1.75, false);
                const Ring s2 = circle(b, c, 1.75, 0.0, false);
                b.loft(s0, s1);
                b.loft(s1, s2);
                holes.push_back(s0);
                bottomHoles.push_back(s2);
                out.holes.push_back(plan2(b, s0));
                out.holes.push_back(plan2(b, s1));
            }
            b.region({rings.back().ids}, holes, true);
        }

    // The outline's top is cut where the outer cells' edges run along it (they
    // meet it at a knife edge, as they meet each other).
    Splits splits;
    for (const Ring& r : cellTops) gatherSplits(b, r, A, B, 0.0, splits);
    splits.tidy();
    const Rect o{-A, A, -B, B, R};
    const Outline o0 = rrect(b, o, 0.0), o1 = rrect(b, o, top, &splits);
    loft(b, o0, o1);
    b.region({o1.ids}, cellTops, true);
    if (p.plateFloor) {
        if (bottomHoles.empty()) b.cap(o0.ids, false);
        else b.region({o0.ids}, bottomHoles, false);
    } else {
        b.region({o0.ids}, cellBottoms, false);
    }

    out.size = glm::vec3(static_cast<float>(W), static_cast<float>(D), static_cast<float>(top));
    out.compartments = nx * ny;
    out.plan.push_back(plan2(b, o1.ids));
    for (const Ring& r : cellTops) out.plan.push_back(plan2(b, r));
}

std::uint32_t toByte(float v) {
    return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

} // namespace

int maxCompartments(int units, float wall, float divider) {
    const double inner = kPitch * units - 0.5 - 2.0 * wall;
    const double least = 2.0 * (kBinRadius - wall) + 0.6;   // the corners and a sliver of straight
    return std::max(1, static_cast<int>(std::floor((inner + divider) / (least + divider))));
}

float minFloor(const Params& p) {
    float lo = static_cast<float>(kFootH) + 0.6f;
    if (p.magnets) lo = std::max(lo, 2.4f + 0.6f);
    if (p.screws) lo = std::max(lo, 6.0f + 0.6f);
    return lo;
}

Params sane(const Params& in) {
    Params p = in;
    p.kind    = in.kind == Kind::Baseplate ? Kind::Baseplate : Kind::Bin;
    p.unitsX  = std::clamp(in.unitsX, 1, 16);
    p.unitsY  = std::clamp(in.unitsY, 1, 16);
    p.unitsZ  = std::clamp(in.unitsZ, 1, 40);
    p.wall    = std::clamp(in.wall, 0.4f, 2.4f);
    p.divider = std::clamp(in.divider, 0.4f, 5.0f);
    p.compX   = std::clamp(in.compX, 1, maxCompartments(p.unitsX, p.wall, p.divider));
    p.compY   = std::clamp(in.compY, 1, maxCompartments(p.unitsY, p.wall, p.divider));
    p.labelDepth  = std::clamp(in.labelDepth, 3.0f, 40.0f);
    p.scoopRadius = std::clamp(in.scoopRadius, 2.0f, 100.0f);
    p.floor   = std::clamp(in.floor, minFloor(p), 300.0f);

    p.drawerW = std::clamp(in.drawerW, 0.0f, 2000.0f);
    p.drawerD = std::clamp(in.drawerD, 0.0f, 2000.0f);
    if (p.fitDrawer) {
        p.plateX = std::max(1, static_cast<int>(std::floor(p.drawerW / kPitch + 1e-6)));
        p.plateY = std::max(1, static_cast<int>(std::floor(p.drawerD / kPitch + 1e-6)));
    }
    p.plateX = std::clamp(p.plateX, 1, 40);
    p.plateY = std::clamp(p.plateY, 1, 40);
    // Room under the cells for what is bored into the floor.
    float least = 1.0f;
    if (p.plateMagnets) least = std::max(least, 2.4f + 0.6f);
    if (p.plateScrews) least = std::max(least, 1.75f + 1.0f);
    p.floorThickness = std::clamp(in.floorThickness, least, 30.0f);
    if (!p.plateFloor) p.plateMagnets = p.plateScrews = false;
    return p;
}

Model build(const Params& in) {
    const Params p = sane(in);
    Model out;
    Builder b;
    if (p.kind == Kind::Bin) buildBin(p, b, out);
    else buildPlate(p, b, out);
    out.ok    = b.error.empty();
    out.error = b.error;
    // The plastic: the solid's volume from its faces.
    for (const std::vector<int>& f : b.faces)
        for (std::size_t k = 1; k + 1 < f.size(); ++k)
            out.volume += glm::dot(b.verts[static_cast<std::size_t>(f[0])],
                                   glm::cross(b.verts[static_cast<std::size_t>(f[k])],
                                              b.verts[static_cast<std::size_t>(f[k + 1])])) / 6.0;
    // To the editor's frame: metres, Y up, the front towards +Z.
    out.mesh.verts.reserve(b.verts.size());
    for (const DV3& v : b.verts)
        out.mesh.verts.emplace_back(static_cast<float>(v.x * 1e-3), static_cast<float>(v.z * 1e-3),
                                    static_cast<float>(-v.y * 1e-3));
    out.mesh.faces = std::move(b.faces);
    return out;
}

std::vector<std::vector<glm::vec2>> section(const EditMesh& m, float x) { return solidbuild::section(m, x); }

AssetId ensureMaterial(std::vector<MaterialDef>& mats, const Params& p) {
    const glm::vec3 c = p.kind == Kind::Bin ? p.binColor : p.plateColor;
    char name[64];
    std::snprintf(name, sizeof name, "Gridfinity #%02X%02X%02X", toByte(c.r), toByte(c.g), toByte(c.b));
    for (MaterialDef& m : mats) {
        if (m.name != name) continue;
        m.albedo = c;
        if (!m.assetId.valid()) m.assetId = AssetId::generate();
        return m.assetId;
    }
    MaterialDef md;
    md.assetId      = AssetId::generate();
    md.name         = name;
    md.albedo       = c;
    md.reflectivity = 0.0f;
    md.roughness    = 0.55f;   // printed plastic: a dull sheen
    mats.push_back(md);
    return md.assetId;
}

std::string autoName(const Params& in) {
    const Params p = sane(in);
    char b[96];
    if (p.kind == Kind::Bin) std::snprintf(b, sizeof b, "Gridfinity bin %dx%dx%d", p.unitsX, p.unitsY, p.unitsZ);
    else std::snprintf(b, sizeof b, "Gridfinity baseplate %dx%d", p.plateX, p.plateY);
    return b;
}

std::vector<Entity> entities(const Params& in, const AssetId& material, int& counter, const glm::vec3& at) {
    const Params p = sane(in);
    Model mdl = build(p);
    if (mdl.mesh.faces.empty()) return {};
    std::vector<Entity> out;
    Entity root;
    root.type        = EntityType::Empty;
    root.name        = autoName(p);
    root.id          = counter++;
    root.parent      = -1;
    // The root in the middle of the part, so its pick box is the part's: `at`
    // is where the part's bottom stands.
    root.half        = 0.0005f * glm::vec3(mdl.size.x, mdl.size.z, mdl.size.y);
    root.localCenter = root.center = at + glm::vec3(0.0f, root.half.y, 0.0f);
    {
        auto gc = std::make_unique<GridfinityComponent>();
        gc->params = p;
        root.components.items.push_back(std::move(gc));
    }
    out.push_back(std::move(root));
    const int rootId = out.front().id;

    // The part, re-centred on its own bounds (the Modeling panel's invariant:
    // pick box, gizmo and collider describe the shape that is there).
    EditMesh m = std::move(mdl.mesh);
    const glm::vec3 shift = editmesh::recenter(m);
    glm::vec3 mn, mx;
    m.bounds(mn, mx);
    Entity e;
    e.type        = EntityType::Box;
    e.name        = p.kind == Kind::Bin ? "Bin" : "Baseplate";
    e.id          = counter++;
    e.parent      = rootId;
    e.half        = glm::max(0.5f * (mx - mn), glm::vec3(1e-4f));
    e.localCenter = shift - glm::vec3(0.0f, out.front().half.y, 0.0f);
    e.center      = at + shift;
    auto mc = std::make_unique<MeshComponent>();
    mc->mesh = std::move(m);
    mc->touch();
    e.components.items.push_back(std::move(mc));
    auto mt = std::make_unique<MaterialComponent>();
    mt->material = material;
    e.components.items.push_back(std::move(mt));
    auto pc = std::make_unique<PhysicsComponent>();
    pc->dynamic = false;
    e.components.items.push_back(std::move(pc));
    out.push_back(std::move(e));
    return out;
}

// --- Persistence -------------------------------------------------------------------

namespace {
nlohmann::json colJson(const glm::vec3& v) { return nlohmann::json::array({v.x, v.y, v.z}); }
glm::vec3 jsonCol(const nlohmann::json& j, const glm::vec3& def) {
    if (!j.is_array() || j.size() != 3) return def;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}
} // namespace

void saveParams(nlohmann::json& j, const Params& p) {
    j["kind"]           = p.kind == Kind::Baseplate ? "baseplate" : "bin";
    j["unitsX"]         = p.unitsX;
    j["unitsY"]         = p.unitsY;
    j["unitsZ"]         = p.unitsZ;
    j["compX"]          = p.compX;
    j["compY"]          = p.compY;
    j["lip"]            = p.lip;
    j["magnets"]        = p.magnets;
    j["screws"]         = p.screws;
    j["label"]          = p.label;
    j["labelDepth"]     = p.labelDepth;
    j["scoop"]          = p.scoop;
    j["scoopRadius"]    = p.scoopRadius;
    j["wall"]           = p.wall;
    j["divider"]        = p.divider;
    j["floor"]          = p.floor;
    j["binColor"]       = colJson(p.binColor);
    j["plateX"]         = p.plateX;
    j["plateY"]         = p.plateY;
    j["plateFloor"]     = p.plateFloor;
    j["floorThickness"] = p.floorThickness;
    j["plateMagnets"]   = p.plateMagnets;
    j["plateScrews"]    = p.plateScrews;
    j["fitDrawer"]      = p.fitDrawer;
    j["drawerW"]        = p.drawerW;
    j["drawerD"]        = p.drawerD;
    j["plateColor"]     = colJson(p.plateColor);
}

void loadParams(const nlohmann::json& j, Params& p) {
    const Params d;
    p.kind           = j.value("kind", std::string("bin")) == "baseplate" ? Kind::Baseplate : Kind::Bin;
    p.unitsX         = j.value("unitsX", d.unitsX);
    p.unitsY         = j.value("unitsY", d.unitsY);
    p.unitsZ         = j.value("unitsZ", d.unitsZ);
    p.compX          = j.value("compX", d.compX);
    p.compY          = j.value("compY", d.compY);
    p.lip            = j.value("lip", d.lip);
    p.magnets        = j.value("magnets", d.magnets);
    p.screws         = j.value("screws", d.screws);
    p.label          = j.value("label", d.label);
    p.labelDepth     = j.value("labelDepth", d.labelDepth);
    p.scoop          = j.value("scoop", d.scoop);
    p.scoopRadius    = j.value("scoopRadius", d.scoopRadius);
    p.wall           = j.value("wall", d.wall);
    p.divider        = j.value("divider", d.divider);
    p.floor          = j.value("floor", d.floor);
    p.binColor       = jsonCol(j.value("binColor", nlohmann::json()), d.binColor);
    p.plateX         = j.value("plateX", d.plateX);
    p.plateY         = j.value("plateY", d.plateY);
    p.plateFloor     = j.value("plateFloor", d.plateFloor);
    p.floorThickness = j.value("floorThickness", d.floorThickness);
    p.plateMagnets   = j.value("plateMagnets", d.plateMagnets);
    p.plateScrews    = j.value("plateScrews", d.plateScrews);
    p.fitDrawer      = j.value("fitDrawer", d.fitDrawer);
    p.drawerW        = j.value("drawerW", d.drawerW);
    p.drawerD        = j.value("drawerD", d.drawerD);
    p.plateColor     = jsonCol(j.value("plateColor", nlohmann::json()), d.plateColor);
    p = sane(p);
}

namespace {
// Scene files name components by type id; this lets a saved bin come back as
// a bin, parameters and all.
struct RegisterGridfinity {
    RegisterGridfinity() {
        components::registerType({"gridfinity", "Gridfinity",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<GridfinityComponent>()); },
            /*addable=*/false});
    }
} g_registerGridfinity;
} // namespace

} // namespace gridfinity
