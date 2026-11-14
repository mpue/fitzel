#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "EditMesh.hpp"

// Closed solids for printing, built without boolean operations: outlines are
// stacked along an axis and lofted, flat faces are filled between the outlines
// that bound them, holes are bored as stacks of their own -- every edge between
// exactly two faces by construction. What the Gridfinity bins and the cable
// holders are made of (Gridfinity.hpp, CableHolder.hpp).
//
// Millimetres, Z up, the generator's own frame; the caller turns the result
// into the editor's metres. Pure data: no GPU, no ImGui.
namespace solidbuild {

using DV2  = glm::dvec2;
using DV3  = glm::dvec3;
using Ring = std::vector<int>;   // the corners of one outline, in order

constexpr double kPi   = 3.14159265358979323846;
constexpr int    kArc  = 6;      // segments a rounded corner
constexpr int    kHole = 40;     // segments a round hole

// A plane across an axis has its own (u, v), so that counter-clockwise there
// faces +axis: across z (x, y), across x (y, z), across y (z, x).
DV3 world(int axis, DV2 uv, double along);
DV2 plane(int axis, const DV3& p);

struct KeyHash {
    std::size_t operator()(const std::array<long long, 3>& k) const {
        std::size_t h = std::hash<long long>()(k[0]);
        h = h * 1000003u ^ std::hash<long long>()(k[1]);
        return h * 1000003u ^ std::hash<long long>()(k[2]);
    }
};

// Where a hole opens: the face it opens in finds it here (see bore()).
struct Bored {
    int  axis = 2;
    double at = 0.0;   // the plane, along the axis
    DV2  middle{0.0};  // in the plane's (u, v)
    Ring ring;         // counter-clockwise in the plane's (u, v)
};

// The mesh under construction. Corners are welded as they are made (to a
// millionth of a millimetre), so two outlines drawn through the same point share
// the corner, and an edge two outlines both run along is the same edge in both.
struct Builder {
    std::vector<DV3> verts;                  // mm, Z up
    std::vector<std::vector<int>> faces;     // counter-clockwise from outside
    std::unordered_map<std::array<long long, 3>, int, KeyHash> weld;
    std::vector<Bored> bored;
    std::string error;

    int vid(const DV3& p) {
        const std::array<long long, 3> k{std::llround(p.x * 1e6), std::llround(p.y * 1e6),
                                         std::llround(p.z * 1e6)};
        const auto [it, fresh] = weld.try_emplace(k, static_cast<int>(verts.size()));
        if (fresh) verts.push_back(p);
        return it->second;
    }

    // A face, its repeated corners (a corner drawn to a point, a step that kept
    // some corners) dropped; nothing when fewer than three are left.
    void face(const std::vector<int>& f) {
        std::vector<int> g;
        for (int v : f)
            if (g.empty() || g.back() != v) g.push_back(v);
        while (g.size() > 1 && g.front() == g.back()) g.pop_back();
        if (g.size() >= 3) faces.push_back(std::move(g));
    }
    void cap(const Ring& r, bool asIs) {
        if (asIs) face(r);
        else face(Ring(r.rbegin(), r.rend()));
    }

    // The band between two outlines of as many corners, walking from `a` to `b`
    // along the profile. With both counter-clockwise seen from +axis, a profile
    // that goes along +axis on the outside of a solid or along -axis on the
    // inside of a hollow faces out; a hole bored along +axis takes its outlines
    // clockwise.
    void loft(const Ring& a, const Ring& b) {
        const std::size_t n = a.size();
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i + 1) % n;
            face({a[i], a[j], b[j], b[i]});
        }
    }

    // The flat face in one plane across `axis` inside the `outer` outlines and
    // outside the `holes` (either way round), facing +axis or -axis. A hole may
    // lie against an outline -- a foot flush with a bin's side, a compartment
    // against its wall -- as long as the outline is cut at the hole's corners:
    // the edges they share run both ways and cancel, and the loops left over
    // bound the face. Those are filled by PolyFill, which drops corners on a
    // straight line; each one dropped is put back into the triangle along whose
    // edge it lies, so the face meets every band round it corner for corner.
    void region(const std::vector<Ring>& outer, const std::vector<Ring>& holes, int axis, bool positive);
    void region(const std::vector<Ring>& outer, const std::vector<Ring>& holes, bool up) {
        region(outer, holes, 2, up);
    }

    // The holes bored into the plane across `axis` at `at` whose middles lie
    // inside `face` (the plane's (u, v)) -- for the face to leave open.
    std::vector<Ring> openings(int axis, double at, const std::vector<DV2>& face) const;
};

// --- Rounded rectangles across Z ------------------------------------------------------

struct Rect {
    double x0, x1, y0, y1, r;
};

// Extra corners on the straight sides of a rounded rectangle, where another
// outline's corner lies on it: x along the front (y0) and back (y1), y along the
// left (x0) and right (x1).
struct Splits {
    std::vector<double> front, right, back, left;
    void tidy();
};

// A rounded rectangle, counter-clockwise from above, starting where the front
// side ends at the right-hand corner: kArc + 1 corners a rounded corner (all one
// corner where the radius has gone to nothing), and any splits on the sides.
// Rings of one profile have the same corners in the same order, so they loft.
struct Outline {
    Ring ids;
    std::array<std::size_t, 4> arc{};   // where each corner's kArc + 1 corners start
};
Outline rrect(Builder& b, const Rect& q, double z, const Splits* s = nullptr);

// The band between two rounded rectangles of one profile: corner for corner
// round the rounded corners; along a straight side, where the two are cut at
// different places, the cuts zipped together in triangles (the side is one
// flat strip, so any zip is flat).
void loft(Builder& b, const Outline& lo, const Outline& hi);

Ring circle(Builder& b, DV2 c, double r, double z, bool clockwise);
Ring reversed(const Ring& r);

// The corners of `ring` that lie on the sides of the rectangle (half sizes A, B,
// inset by d), as splits for it.
void gatherSplits(const Builder& b, const Ring& ring, double A, double B, double d, Splits& s);

// An outline as seen from above, its repeated corners once (for a panel).
std::vector<glm::vec2> plan2(const Builder& b, const Ring& r);
// Its area seen from above, counter-clockwise positive.
double area2(const Builder& b, const Ring& r);

// --- Holes and extrusions ------------------------------------------------------------

// A hole bored along an axis: round, or a rounded rectangle (a pocket for a
// pad), stepping in size along the way (a countersink, a magnet under a screw).
struct Hole {
    int axis = 2;
    DV2 middle{0.0};   // in the plane across the axis, its (u, v)
    struct Step {
        double at;       // along the axis
        double rx;       // the radius, or half the width along u
        double ry = 0.0; // half the width along v; 0 for round
        double rc = 0.0; // the rounded rectangle's corner
    };
    std::vector<Step> steps;   // from the face it enters to the face it leaves (or its bottom)
    bool through = true;       // else closed by a flat bottom at the last step
};
// Its walls (and bottom); the faces it enters and leaves find it through
// Builder::openings -- so bore before the faces are made.
void bore(Builder& b, const Hole& h);

// An outline in the plane across `axis` carried along it from level to level --
// a prism with two levels, rounded edges with more (the outline drawn in by the
// rounding at each level). Walls of edges that stay put at every level and run
// along u or v are one flat face each, so holes can open in them; caps at both
// ends. An edge `glued` says yes to has no wall: another solid's face carries on
// there (the comb on a desk clip).
struct Extrusion {
    int axis = 2;
    std::vector<double> at;                    // the levels, rising
    std::vector<std::vector<DV2>> outline;     // one per level, (u, v), corner for corner
    std::function<bool(DV2, DV2)> glued;
    bool capLo = true, capHi = true;
};
// The rings made, one per level (counter-clockwise in (u, v)).
std::vector<Ring> extrude(Builder& b, const Extrusion& e);

// `mesh` (in the editor's metres, Y up) cut by the print's plane x = `x` (mm):
// its outlines as (y, z) loops in mm, each corner once, none on a straight
// run -- a panel's cut-away view.
std::vector<std::vector<glm::vec2>> section(const EditMesh& mesh, float x);

// The solid as an EditMesh in the editor's frame: metres, Y up (x, z, -y).
EditMesh toEditMesh(const Builder& b);
// Its volume (mm3) from its faces.
double volume(const Builder& b);

} // namespace solidbuild
