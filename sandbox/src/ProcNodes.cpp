#include "ProcGraph.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "EditMeshModifiers.hpp"
#include "ProcNodeKit.hpp"

// The node kinds of the procedural graphs (ProcGraph.hpp). One class each: its
// settings as members, the same settings as Property rows (the editor's fields
// and the file's keys -- `speed` is what one click of a stepper moves a value),
// and cook(). The registrar at the bottom lists them; a new kind is a class
// here and one line there.
//
// Every shape is built in its own space, standing on +Y and centred where its
// `center` says, wound counter-clockwise seen from outside like every EditMesh.
// Corners are shared between the faces of one smooth surface (a tube's side,
// a torus) and not across a crease, which is what lets "shade smooth" on the
// object round the one and keep the other sharp. The 2D shapes (circle,
// rectangle, curve) lie flat in XZ, facing +Y, unless their axis says otherwise.

namespace {

using proc::Curve;
using proc::FaceSet;
using proc::Geo;

using namespace proc::kit;

// ================================================================================
// Shapes
// ================================================================================

class BoxNode : public proc::NodeOf<BoxNode> {
public:
    glm::vec3 size{4.0f, 4.0f, 4.0f};
    glm::vec3 center{0.0f};

    const char* typeId() const override { return "box"; }
    const char* displayName() const override { return "Box"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = {
            vector3("Size", "size", &BoxNode::size, 0.5f),
            vector3("Center", "center", &BoxNode::center, 0.5f),
        };
        return p;
    }
    std::string cook(const std::vector<const Geo*>&, Geo& out) const override {
        out.mesh = EditMesh::box(glm::max(glm::abs(size), glm::vec3(0.001f)) * 0.5f);
        out.mesh.paint.clear();
        transformGeo(out, glm::translate(glm::mat4(1.0f), center));
        return "";
    }
};

// A cylinder, a cone or anything between: two radii, rings along its length
// (the rows a later step can panel or cut), and caps that can be left off.
class TubeNode : public proc::NodeOf<TubeNode> {
public:
    float     radius    = 2.0f;
    float     radiusTop = 2.0f;
    float     length    = 8.0f;
    int       segments  = 24;
    int       rows      = 1;
    bool      caps      = true;
    int       axis      = 1;
    glm::vec3 center{0.0f};

    const char* typeId() const override { return "tube"; }
    const char* displayName() const override { return "Tube"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            std::vector<Property> v;
            v.push_back(number("Radius", "radius", &TubeNode::radius, 0.25f, 0.0f, 10000.0f));
            v.push_back(number("Top radius", "radiusTop", &TubeNode::radiusTop, 0.25f, 0.0f, 10000.0f));
            v.push_back(number("Length", "length", &TubeNode::length, 0.5f, 0.01f, 100000.0f));
            v.push_back(whole("Segments", "segments", &TubeNode::segments, 3, 256));
            v.push_back(whole("Rows", "rows", &TubeNode::rows, 1, 256));
            v.push_back(flag("Caps", "caps", &TubeNode::caps));
            v.push_back(choice("Axis", "axis", &TubeNode::axis, axisLabels()));
            v.push_back(vector3("Center", "center", &TubeNode::center, 0.5f));
            return v;
        }();
        return p;
    }
    std::string cook(const std::vector<const Geo*>&, Geo& out) const override {
        EditMesh& m = out.mesh;
        const int seg = std::clamp(segments, 3, 256);
        const int nr  = std::clamp(rows, 1, 256);
        const float len = std::max(length, 0.001f);
        // Ring r's corners; a ring of radius 0 is one corner (a cone's tip).
        std::vector<std::vector<int>> ring(static_cast<std::size_t>(nr + 1));
        for (int r = 0; r <= nr; ++r) {
            const float t   = static_cast<float>(r) / static_cast<float>(nr);
            const float rad = std::max(0.0f, radius + (radiusTop - radius) * t);
            const float y   = -0.5f * len + len * t;
            std::vector<int>& rv = ring[static_cast<std::size_t>(r)];
            if (rad < 1e-5f) {
                const int tip = addCorner(m, {0.0f, y, 0.0f});
                rv.assign(static_cast<std::size_t>(seg), tip);
                continue;
            }
            for (int i = 0; i < seg; ++i) {
                const float a = kTau * static_cast<float>(i) / static_cast<float>(seg);
                rv.push_back(addCorner(m, {rad * std::cos(a), y, rad * std::sin(a)}));
            }
        }
        for (int r = 0; r < nr; ++r)
            for (int i = 0; i < seg; ++i) {
                const std::size_t a = static_cast<std::size_t>(i);
                const std::size_t b = static_cast<std::size_t>((i + 1) % seg);
                const std::vector<int>& lo = ring[static_cast<std::size_t>(r)];
                const std::vector<int>& hi = ring[static_cast<std::size_t>(r + 1)];
                addLoop(m, {lo[a], hi[a], hi[b], lo[b]});   // a tip ring makes it a triangle
            }
        if (caps) {
            const std::vector<int>& lo = ring.front();
            const std::vector<int>& hi = ring.back();
            if (lo.front() != lo.back()) m.faces.push_back(lo);   // facing -Y
            if (hi.front() != hi.back())
                m.faces.push_back(std::vector<int>(hi.rbegin(), hi.rend()));   // facing +Y
        }
        transformGeo(out, glm::translate(glm::mat4(1.0f), center) * axisFrame(axis));
        return "";
    }
};

// The plain cylinder: one radius, a height, closed at both ends unless told
// otherwise. What a Tube with equal radii already makes -- it is the Tube that
// makes it -- under the name everyone looks for first.
class CylinderNode : public proc::NodeOf<CylinderNode> {
public:
    float     radius   = 2.0f;
    float     height   = 4.0f;
    int       segments = 24;
    int       rows     = 1;
    bool      caps     = true;
    int       axis     = 1;
    glm::vec3 center{0.0f};

    const char* typeId() const override { return "cylinder"; }
    const char* displayName() const override { return "Cylinder"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            std::vector<Property> v;
            v.push_back(number("Radius", "radius", &CylinderNode::radius, 0.25f, 0.001f, 10000.0f));
            v.push_back(number("Height", "height", &CylinderNode::height, 0.5f, 0.01f, 100000.0f));
            v.push_back(whole("Segments", "segments", &CylinderNode::segments, 3, 256));
            v.push_back(whole("Rows", "rows", &CylinderNode::rows, 1, 256));
            v.push_back(flag("Caps", "caps", &CylinderNode::caps));
            v.push_back(choice("Axis", "axis", &CylinderNode::axis, axisLabels()));
            v.push_back(vector3("Center", "center", &CylinderNode::center, 0.5f));
            return v;
        }();
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        TubeNode t;
        t.radius    = radius;
        t.radiusTop = radius;
        t.length    = height;
        t.segments  = segments;
        t.rows      = rows;
        t.caps      = caps;
        t.axis      = axis;
        t.center    = center;
        return t.cook(in, out);
    }
};

class SphereNode : public proc::NodeOf<SphereNode> {
public:
    float     radius   = 3.0f;
    int       rings    = 12;
    int       segments = 24;
    glm::vec3 center{0.0f};

    const char* typeId() const override { return "sphere"; }
    const char* displayName() const override { return "Sphere"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = {
            number("Radius", "radius", &SphereNode::radius, 0.25f, 0.01f, 10000.0f),
            whole("Rings", "rings", &SphereNode::rings, 2, 128),
            whole("Segments", "segments", &SphereNode::segments, 3, 256),
            vector3("Center", "center", &SphereNode::center, 0.5f),
        };
        return p;
    }
    std::string cook(const std::vector<const Geo*>&, Geo& out) const override {
        out.mesh = EditMesh::sphere(glm::vec3(std::max(radius, 0.001f)),
                                    std::clamp(rings, 2, 128), std::clamp(segments, 3, 256));
        out.mesh.paint.clear();
        transformGeo(out, glm::translate(glm::mat4(1.0f), center));
        return "";
    }
};

// A ring with a cross-section of `sides` corners, `width` across and `height`
// tall. The section is turned half a step, so with four sides it is a box
// section with a flat floor -- a station's habitat ring -- and with many it is
// round.
class TorusNode : public proc::NodeOf<TorusNode> {
public:
    float     radius   = 20.0f;
    float     width    = 4.0f;
    float     height   = 4.0f;
    int       segments = 48;
    int       sides    = 12;
    int       axis     = 1;
    glm::vec3 center{0.0f};

    const char* typeId() const override { return "torus"; }
    const char* displayName() const override { return "Torus"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            std::vector<Property> v;
            v.push_back(number("Radius", "radius", &TorusNode::radius, 1.0f, 0.01f, 100000.0f));
            v.push_back(number("Width", "width", &TorusNode::width, 0.25f, 0.01f, 10000.0f));
            v.push_back(number("Height", "height", &TorusNode::height, 0.25f, 0.01f, 10000.0f));
            v.push_back(whole("Segments", "segments", &TorusNode::segments, 3, 512));
            v.push_back(whole("Sides", "sides", &TorusNode::sides, 3, 64));
            v.push_back(choice("Axis", "axis", &TorusNode::axis, axisLabels()));
            v.push_back(vector3("Center", "center", &TorusNode::center, 0.5f));
            return v;
        }();
        return p;
    }
    std::string cook(const std::vector<const Geo*>&, Geo& out) const override {
        EditMesh& m = out.mesh;
        const int ns = std::clamp(segments, 3, 512);
        const int nk = std::clamp(sides, 3, 64);
        // Corners on an ellipse wide enough that the FLAT sides land on
        // +/- width/2 and height/2, not the corners.
        const float grow = 1.0f / std::cos(kPi / static_cast<float>(nk));
        const float a = 0.5f * std::max(width, 0.001f) * grow;
        const float b = 0.5f * std::max(height, 0.001f) * grow;
        for (int i = 0; i < ns; ++i) {
            const float phi = kTau * static_cast<float>(i) / static_cast<float>(ns);
            const glm::vec3 d(std::cos(phi), 0.0f, std::sin(phi));
            for (int k = 0; k < nk; ++k) {
                const float th = kTau * static_cast<float>(k) / static_cast<float>(nk) +
                                 kPi / static_cast<float>(nk);
                addCorner(m, d * (radius + a * std::cos(th)) + glm::vec3(0.0f, b * std::sin(th), 0.0f));
            }
        }
        auto at = [&](int i, int k) { return (i % ns) * nk + (k % nk); };
        for (int i = 0; i < ns; ++i)
            for (int k = 0; k < nk; ++k)
                m.faces.push_back({at(i, k), at(i, k + 1), at(i + 1, k + 1), at(i + 1, k)});
        transformGeo(out, glm::translate(glm::mat4(1.0f), center) * axisFrame(axis));
        return "";
    }
};

// A flat sheet facing +Y, cut into cells: a solar wing, a deck, a landing pad.
class GridNode : public proc::NodeOf<GridNode> {
public:
    float     sizeX = 10.0f, sizeZ = 10.0f;
    int       cellsX = 4, cellsZ = 4;
    glm::vec3 center{0.0f};

    const char* typeId() const override { return "grid"; }
    const char* displayName() const override { return "Grid"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = {
            number("Size X", "sizeX", &GridNode::sizeX, 0.5f, 0.01f, 100000.0f),
            number("Size Z", "sizeZ", &GridNode::sizeZ, 0.5f, 0.01f, 100000.0f),
            whole("Cells X", "cellsX", &GridNode::cellsX, 1, 256),
            whole("Cells Z", "cellsZ", &GridNode::cellsZ, 1, 256),
            vector3("Center", "center", &GridNode::center, 0.5f),
        };
        return p;
    }
    std::string cook(const std::vector<const Geo*>&, Geo& out) const override {
        EditMesh& m = out.mesh;
        const int cx = std::clamp(cellsX, 1, 256), cz = std::clamp(cellsZ, 1, 256);
        for (int ix = 0; ix <= cx; ++ix)
            for (int iz = 0; iz <= cz; ++iz)
                addCorner(m, {sizeX * (static_cast<float>(ix) / cx - 0.5f), 0.0f,
                              sizeZ * (static_cast<float>(iz) / cz - 0.5f)});
        auto at = [&](int ix, int iz) { return ix * (cz + 1) + iz; };
        for (int ix = 0; ix < cx; ++ix)
            for (int iz = 0; iz < cz; ++iz)
                m.faces.push_back({at(ix, iz), at(ix, iz + 1), at(ix + 1, iz + 1), at(ix + 1, iz)});
        transformGeo(out, glm::translate(glm::mat4(1.0f), center));
        return "";
    }
};

// --- 2D shapes ----------------------------------------------------------------------
// Lines, or filled: a face. As lines they are what Sweep runs a profile
// along (or uses AS the profile), what Revolve turns, what Copy onto points
// lines things up on; filled, Extrude makes a solid of them.

class CircleNode : public proc::NodeOf<CircleNode> {
public:
    float     radius   = 4.0f;
    int       segments = 24;
    float     sweep    = 360.0f;   // less than a full turn: an open arc
    bool      filled   = false;
    int       axis     = 1;        // the axis it faces: Y lies flat
    glm::vec3 center{0.0f};

    const char* typeId() const override { return "circle"; }
    const char* displayName() const override { return "Circle"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            std::vector<Property> v;
            v.push_back(number("Radius", "radius", &CircleNode::radius, 0.25f, 0.001f, 100000.0f));
            v.push_back(whole("Segments", "segments", &CircleNode::segments, 3, 512));
            v.push_back(number("Arc (deg)", "sweep", &CircleNode::sweep, 15.0f, 1.0f, 360.0f, "%.0f"));
            v.push_back(flag("Filled", "filled", &CircleNode::filled));
            v.push_back(choice("Facing axis", "axis", &CircleNode::axis, axisLabels()));
            v.push_back(vector3("Center", "center", &CircleNode::center, 0.5f));
            return v;
        }();
        return p;
    }
    std::string cook(const std::vector<const Geo*>&, Geo& out) const override {
        const int n = std::clamp(segments, 3, 512);
        const bool closed = sweep >= 359.99f;
        const float arc = glm::radians(std::clamp(sweep, 1.0f, 360.0f));
        std::vector<glm::vec3> pts;
        const int count = closed ? n : n + 1;
        // Counter-clockwise seen from +Y: the angle runs from +X towards -Z.
        for (int i = 0; i < count; ++i) {
            const float a = arc * static_cast<float>(i) / static_cast<float>(n);
            pts.emplace_back(radius * std::cos(a), 0.0f, -radius * std::sin(a));
        }
        emitLoop(out, pts, closed, filled);
        transformGeo(out, glm::translate(glm::mat4(1.0f), center) * axisFrame(axis));
        return "";
    }
};

// A rectangle -- a square when both sides match.
class RectNode : public proc::NodeOf<RectNode> {
public:
    float     sizeX  = 6.0f, sizeZ = 6.0f;
    bool      filled = false;
    int       axis   = 1;
    glm::vec3 center{0.0f};

    const char* typeId() const override { return "rect"; }
    const char* displayName() const override { return "Rectangle"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = {
            number("Size X", "sizeX", &RectNode::sizeX, 0.5f, 0.001f, 100000.0f),
            number("Size Z", "sizeZ", &RectNode::sizeZ, 0.5f, 0.001f, 100000.0f),
            flag("Filled", "filled", &RectNode::filled),
            choice("Facing axis", "axis", &RectNode::axis, axisLabels()),
            vector3("Center", "center", &RectNode::center, 0.5f),
        };
        return p;
    }
    std::string cook(const std::vector<const Geo*>&, Geo& out) const override {
        const float x = 0.5f * sizeX, z = 0.5f * sizeZ;
        emitLoop(out, {{x, 0.0f, z}, {x, 0.0f, -z}, {-x, 0.0f, -z}, {-x, 0.0f, z}}, true, filled);
        transformGeo(out, glm::translate(glm::mat4(1.0f), center) * axisFrame(axis));
        return "";
    }
};

// A line through points you give: straight from point to point, or smooth
// through them. A path to sweep a pipe along, a profile to turn into a dome.
class CurveNode : public proc::NodeOf<CurveNode> {
public:
    // "x y z; x y z; ..." -- the editor shows them as a list of points.
    std::string points = "0 0 0; 10 0 6; 20 0 -6; 30 0 0";
    bool        closed = false;
    int         smooth = 0;       // samples per span; 0 = straight lines
    bool        filled = false;

    const char* typeId() const override { return "curve"; }
    const char* displayName() const override { return "Curve"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            std::vector<Property> v;
            v.push_back(field("Points", "points", PropKind::Text, &CurveNode::points));
            v.push_back(flag("Closed", "closed", &CurveNode::closed));
            Property s = whole("Smooth", "smooth", &CurveNode::smooth, 0, 32);
            v.push_back(std::move(s));
            Property f = flag("Filled", "filled", &CurveNode::filled);
            f.visible = [](const void* o) { return static_cast<const CurveNode*>(o)->closed; };
            v.push_back(std::move(f));
            return v;
        }();
        return p;
    }
    std::string cook(const std::vector<const Geo*>&, Geo& out) const override {
        std::vector<glm::vec3> pts = proc::parsePoints(points);
        if (pts.size() < 2) return "A curve needs two points or more";
        const bool loop = closed && pts.size() >= 3;
        if (smooth >= 2) pts = smoothLine(pts, loop, std::clamp(smooth, 2, 32));
        if (filled && loop) {
            // Facing up when it lies flat, however the points were placed.
            glm::vec3 n(0.0f);
            for (std::size_t i = 0; i < pts.size(); ++i) {
                const glm::vec3& a = pts[i];
                const glm::vec3& b = pts[(i + 1) % pts.size()];
                n += glm::vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
            }
            if (n.y < 0.0f) std::reverse(pts.begin(), pts.end());
        }
        emitLoop(out, pts, loop, filled);
        return "";
    }
};
// A prefab of the project, placed by the graph: moved, turned, copied in a
// row, round a hub or onto points like any shape -- and put into the scene as
// real objects under the procedural one, models, lights and scripts and all.
class PrefabNode : public proc::NodeOf<PrefabNode> {
public:
    std::string prefab;   // its name, as the project's prefabs folder lists it
    glm::vec3   move{0.0f};
    glm::vec3   turn{0.0f};
    float       scale = 1.0f;

    const char* typeId() const override { return "prefab"; }
    const char* displayName() const override { return "Prefab"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = {
            // Drawn as a picker over the project's prefabs (the editor knows the key).
            field("Prefab", "prefab", PropKind::Text, &PrefabNode::prefab),
            vector3("Move", "move", &PrefabNode::move, 0.5f),
            vector3("Turn (deg)", "turn", &PrefabNode::turn, 15.0f, "%.0f"),
            number("Scale", "scale", &PrefabNode::scale, 0.1f, 0.01f, 1000.0f),
        };
        return p;
    }
    std::string cook(const std::vector<const Geo*>&, Geo& out) const override {
        if (prefab.empty()) return "Pick a prefab";
        out.instances.push_back({prefab, glm::translate(glm::mat4(1.0f), move) * eulerDeg(turn) *
                                         glm::scale(glm::mat4(1.0f), glm::vec3(std::max(scale, 0.01f)))});
        return "";
    }
};

// ================================================================================
// Combining and copying
// ================================================================================

// Everything wired in, as one piece of geometry.
class MergeNode : public proc::NodeOf<MergeNode> {
public:
    const char* typeId() const override { return "merge"; }
    const char* displayName() const override { return "Merge"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> none; return none;
    }
    bool variadic() const override { return true; }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        for (const Geo* g : in)
            if (g) append(out, *g);
        return in.empty() ? "Nothing wired in yet" : "";
    }
};

class TransformNode : public proc::NodeOf<TransformNode> {
public:
    glm::vec3 move{0.0f};
    glm::vec3 turn{0.0f};          // degrees
    glm::vec3 scale{1.0f};
    float     uniform = 1.0f;
    bool      onlySelected = false;

    const char* typeId() const override { return "transform"; }
    const char* displayName() const override { return "Transform"; }
    int inputSlots() const override { return 1; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = {
            vector3("Move", "move", &TransformNode::move, 0.5f),
            vector3("Turn (deg)", "turn", &TransformNode::turn, 15.0f, "%.0f"),
            vector3("Scale", "scale", &TransformNode::scale, 0.1f, "%.2f"),
            number("Uniform scale", "uniform", &TransformNode::uniform, 0.1f, 0.001f, 1000.0f),
            flag("Only selected points", "onlySelected", &TransformNode::onlySelected),
        };
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        out = *in[0];
        const glm::mat4 xf = glm::translate(glm::mat4(1.0f), move) * eulerDeg(turn) *
                             glm::scale(glm::mat4(1.0f), scale * uniform);
        if (!onlySelected || !out.hasSel) {
            transformGeo(out, xf);
            return onlySelected ? "No points selected: moved all of them" : "";
        }
        // Only the selected points move: a deformation, so nothing is turned
        // round even by a mirroring scale -- the faces around them stretch.
        for (int i = 0; i < static_cast<int>(out.mesh.verts.size()); ++i)
            if (out.meshPicked(i)) {
                glm::vec3& v = out.mesh.verts[static_cast<std::size_t>(i)];
                v = glm::vec3(xf * glm::vec4(v, 1.0f));
            }
        const glm::mat3 nx = normalMatrix(xf);
        for (Curve& c : out.curves)
            for (int i = 0; i < static_cast<int>(c.pts.size()); ++i)
                if (Geo::curvePicked(c, i, out.hasSel)) {
                    glm::vec3& p = c.pts[static_cast<std::size_t>(i)];
                    p = glm::vec3(xf * glm::vec4(p, 1.0f));
                    if (i < static_cast<int>(c.nrm.size()))
                        c.nrm[static_cast<std::size_t>(i)] = turnNormal(nx, c.nrm[static_cast<std::size_t>(i)]);
                }
        return "";
    }
};

// Copies turned round an axis: spokes round a hub, modules round a ring,
// petals of solar wings. Each copy is pushed `radius` out from the axis first.
class CopyRadialNode : public proc::NodeOf<CopyRadialNode> {
public:
    int   count  = 4;
    float radius = 0.0f;
    int   axis   = 1;
    float start  = 0.0f;     // degrees
    float sweep  = 360.0f;   // degrees; a full turn spaces the copies evenly
    bool  turnCopies = true;

    const char* typeId() const override { return "copyradial"; }
    const char* displayName() const override { return "Copy radial"; }
    int inputSlots() const override { return 1; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            std::vector<Property> v;
            v.push_back(whole("Count", "count", &CopyRadialNode::count, 1, 512));
            v.push_back(number("Radius", "radius", &CopyRadialNode::radius, 0.5f, 0.0f, 100000.0f));
            v.push_back(choice("Axis", "axis", &CopyRadialNode::axis, axisLabels()));
            v.push_back(number("Start (deg)", "start", &CopyRadialNode::start, 15.0f, -360.0f, 360.0f, "%.0f"));
            v.push_back(number("Sweep (deg)", "sweep", &CopyRadialNode::sweep, 15.0f, -360.0f, 360.0f, "%.0f"));
            v.push_back(flag("Turn the copies", "turnCopies", &CopyRadialNode::turnCopies));
            return v;
        }();
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        const int n = std::clamp(count, 1, 512);
        // A full turn would put the last copy on the first: spread over n gaps
        // then, over n - 1 for an arc that ends where it says.
        const bool closed = std::fabs(sweep) >= 359.99f;
        const float gaps = closed ? static_cast<float>(n) : static_cast<float>(std::max(n - 1, 1));
        const glm::vec3 ax   = axisVec(axis);
        const glm::vec3 out0 = glm::vec3(axisFrame(axis) * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
        for (int i = 0; i < n; ++i) {
            const float deg = start + sweep * static_cast<float>(i) / gaps;
            const glm::mat4 r = glm::rotate(glm::mat4(1.0f), glm::radians(deg), ax);
            glm::mat4 xf;
            if (turnCopies) xf = r * glm::translate(glm::mat4(1.0f), out0 * radius);
            else            xf = glm::translate(glm::mat4(1.0f), glm::vec3(r * glm::vec4(out0 * radius, 0.0f)));
            Geo c = *in[0];
            transformGeo(c, xf);
            append(out, c);
        }
        return "";
    }
};

// Copies in a row, each `step` on from the one before: modules along a truss,
// panels of a solar wing, lights down a corridor.
class CopyLinearNode : public proc::NodeOf<CopyLinearNode> {
public:
    int       count = 3;
    glm::vec3 step{4.0f, 0.0f, 0.0f};
    bool      centred = true;

    const char* typeId() const override { return "copylinear"; }
    const char* displayName() const override { return "Copy in a row"; }
    int inputSlots() const override { return 1; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = {
            whole("Count", "count", &CopyLinearNode::count, 1, 512),
            vector3("Step", "step", &CopyLinearNode::step, 0.5f),
            flag("Centre the row", "centred", &CopyLinearNode::centred),
        };
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        const int n = std::clamp(count, 1, 512);
        const float first = centred ? -0.5f * static_cast<float>(n - 1) : 0.0f;
        for (int i = 0; i < n; ++i) {
            Geo c = *in[0];
            transformGeo(c, glm::translate(glm::mat4(1.0f), step * (first + static_cast<float>(i))));
            append(out, c);
        }
        return "";
    }
};

// The input reflected through the plane across `axis` at the origin, by
// default next to the original: build one half of something, get both.
class MirrorNode : public proc::NodeOf<MirrorNode> {
public:
    int  axis = 0;
    bool keep = true;

    const char* typeId() const override { return "mirror"; }
    const char* displayName() const override { return "Mirror"; }
    int inputSlots() const override { return 1; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = {
            choice("Across", "axis", &MirrorNode::axis, axisLabels()),
            flag("Keep the original", "keep", &MirrorNode::keep),
        };
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        if (keep) out = *in[0];
        Geo c = *in[0];
        glm::vec3 s(1.0f);
        s[std::clamp(axis, 0, 2)] = -1.0f;
        transformGeo(c, glm::scale(glm::mat4(1.0f), s));
        append(out, c);
        return "";
    }
};

// A copy of the first input on points of the second: on its points (the
// selected ones, when a selection was made) or on the middle of its faces,
// stood up along the surface there -- and along a curve, turned to follow it.
// Antennas on every module, lights along a path, greebles where nobody
// placed them.
class CopyToPointsNode : public proc::NodeOf<CopyToPointsNode> {
public:
    int   where  = 0;       // 0 points, 1 face centres
    bool  align  = true;
    float scale  = 1.0f;
    bool  spin   = false;   // each copy turned at random about its own up
    float share  = 1.0f;
    int   seed   = 1;

    const char* typeId() const override { return "copytopoints"; }
    const char* displayName() const override { return "Copy onto points"; }
    int inputSlots() const override { return 2; }
    const char* inputName(int slot) const override { return slot == 0 ? "Shape" : "Points of"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            std::vector<Property> v;
            v.push_back(choice("On", "where", &CopyToPointsNode::where,
                               {"Points (the selected ones)", "Face centres"}));
            v.push_back(flag("Stand on the surface / follow curves", "align", &CopyToPointsNode::align));
            v.push_back(number("Scale", "scale", &CopyToPointsNode::scale, 0.1f, 0.001f, 1000.0f));
            v.push_back(flag("Random spin", "spin", &CopyToPointsNode::spin));
            v.push_back(number("Share", "share", &CopyToPointsNode::share, 0.05f, 0.0f, 1.0f));
            Property s = whole("Seed", "seed", &CopyToPointsNode::seed, 0, 99999);
            s.visible = [](const void* o) {
                const auto* n = static_cast<const CopyToPointsNode*>(o);
                return n->share < 1.0f || n->spin;
            };
            v.push_back(std::move(s));
            return v;
        }();
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (in.size() < 2 || !in[0]) return "Nothing wired into Shape";
        if (!in[1]) return "Nothing wired into Points of";
        const Geo& src = *in[1];
        // Each target: where, which way is up, and (on a curve) which way on.
        struct Spot { glm::vec3 p, up, along; bool curve; };
        std::vector<Spot> spots;
        if (where == 0) {
            const std::vector<glm::vec3> nrm = proc::cornerNormals(src.mesh);
            for (int i = 0; i < static_cast<int>(src.mesh.verts.size()); ++i)
                if (src.meshPicked(i))
                    spots.push_back({src.mesh.verts[static_cast<std::size_t>(i)], nrm[static_cast<std::size_t>(i)],
                                     glm::vec3(0.0f), false});
            for (const Curve& c : src.curves) {
                const int n = static_cast<int>(c.pts.size());
                for (int i = 0; i < n; ++i) {
                    if (!Geo::curvePicked(c, i, src.hasSel)) continue;
                    if (c.loose) {
                        spots.push_back({c.pts[static_cast<std::size_t>(i)], c.normalAt(i), glm::vec3(0.0f), false});
                        continue;
                    }
                    const int a = c.closed ? (i - 1 + n) % n : std::max(i - 1, 0);
                    const int b = c.closed ? (i + 1) % n : std::min(i + 1, n - 1);
                    glm::vec3 t = c.pts[static_cast<std::size_t>(b)] - c.pts[static_cast<std::size_t>(a)];
                    t = glm::dot(t, t) > 1e-12f ? glm::normalize(t) : glm::vec3(1.0f, 0.0f, 0.0f);
                    spots.push_back({c.pts[static_cast<std::size_t>(i)], glm::vec3(0.0f, 1.0f, 0.0f), t, true});
                }
            }
        } else {
            for (int f : proc::pickFaces(src, src.hasSel ? FaceSet::SelectedPoints : FaceSet::All, 1.0f, 0))
                spots.push_back({src.mesh.faceCenter(f), src.mesh.faceNormal(f), glm::vec3(0.0f), false});
        }
        const float keep = std::clamp(share, 0.0f, 1.0f);
        int made = 0;
        for (std::size_t i = 0; i < spots.size(); ++i) {
            if (keep < 1.0f && proc::random01(static_cast<int>(i), seed) >= keep) continue;
            if (++made > 20000) return "Stopped at 20000 copies";
            const Spot& s = spots[i];
            glm::mat4 xf = glm::translate(glm::mat4(1.0f), s.p);
            if (align && s.curve) {
                // Along the curve: the copy's +X runs with it, +Y as near up
                // as the slope allows.
                glm::vec3 up = s.up - s.along * glm::dot(s.up, s.along);
                up = glm::dot(up, up) > 1e-8f ? glm::normalize(up) : glm::vec3(0.0f, 0.0f, 1.0f);
                const glm::vec3 side = glm::cross(s.along, up);
                glm::mat4 r(1.0f);
                r[0] = glm::vec4(s.along, 0.0f);
                r[1] = glm::vec4(up, 0.0f);
                r[2] = glm::vec4(side, 0.0f);
                xf = xf * r;
            } else if (align) {
                xf = xf * upTo(s.up);
            }
            if (spin)
                xf = xf * glm::rotate(glm::mat4(1.0f),
                                      kTau * proc::random01(static_cast<int>(i), seed + 7919),
                                      glm::vec3(0.0f, 1.0f, 0.0f));
            xf = xf * glm::scale(glm::mat4(1.0f), glm::vec3(scale));
            Geo c = *in[0];
            transformGeo(c, xf);
            append(out, c);
        }
        return spots.empty() ? "No points to copy onto" : "";
    }
};
// ================================================================================
// Curves into surfaces
// ================================================================================

// Every curve re-cut into points an even distance apart -- what Copy onto
// points needs to line things up at a spacing, and Sweep to bend smoothly.
class ResampleNode : public proc::NodeOf<ResampleNode> {
public:
    float spacing = 2.0f;

    const char* typeId() const override { return "resample"; }
    const char* displayName() const override { return "Resample"; }
    int inputSlots() const override { return 1; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = {
            number("Spacing", "spacing", &ResampleNode::spacing, 0.25f, 0.01f, 10000.0f),
        };
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        out = *in[0];
        if (!firstLine(out)) return "No curves to resample";
        const float step = std::max(spacing, 0.01f);
        for (Curve& c : out.curves) {
            if (c.loose) continue;   // points, not a line: nothing to cut
            float len = 0.0f;
            const std::size_t n = c.pts.size();
            for (std::size_t i = 0; i + 1 < n; ++i) len += glm::length(c.pts[i + 1] - c.pts[i]);
            if (c.closed && n > 2) len += glm::length(c.pts.front() - c.pts.back());
            int count = static_cast<int>(std::round(len / step));
            count = c.closed ? std::max(count, 3) : std::max(count + 1, 2);
            c.pts = resampleLine(c.pts, c.closed, std::min(count, 100000));
            // New points: none of them is one that was selected.
            if (out.hasSel) c.sel.assign(c.pts.size(), 0);
        }
        return "";
    }
};

// A profile swept along a path: a pipe, a corridor, a rail, a cable. The path
// is every curve of the first input; the profile is the first curve (or face)
// of the second, as drawn lying flat at the origin -- its X runs across the
// path, its Z up -- or, with nothing wired there, a round one of `radius`.
class SweepNode : public proc::NodeOf<SweepNode> {
public:
    float radius = 1.0f;
    int   sides  = 12;
    float scale  = 1.0f;
    float twist  = 0.0f;   // degrees over the whole path
    bool  caps   = true;
    bool  flip   = false;

    const char* typeId() const override { return "sweep"; }
    const char* displayName() const override { return "Sweep"; }
    int inputSlots() const override { return 2; }
    const char* inputName(int slot) const override { return slot == 0 ? "Path" : "Profile (optional)"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            std::vector<Property> v;
            v.push_back(number("Radius (round)", "radius", &SweepNode::radius, 0.1f, 0.001f, 10000.0f));
            v.push_back(whole("Sides (round)", "sides", &SweepNode::sides, 3, 128));
            v.push_back(number("Profile scale", "scale", &SweepNode::scale, 0.1f, 0.001f, 1000.0f));
            v.push_back(number("Twist (deg)", "twist", &SweepNode::twist, 15.0f, -36000.0f, 36000.0f, "%.0f"));
            v.push_back(flag("Caps", "caps", &SweepNode::caps));
            v.push_back(flag("Flip faces", "flip", &SweepNode::flip));
            return v;
        }();
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (in.empty() || !in[0]) return "Nothing wired into Path";
        if (!firstLine(*in[0])) return "The path has no curves";
        // The profile, in its own plane.
        std::vector<glm::vec2> prof;
        bool profClosed = true;
        const Geo* pg = in.size() > 1 ? in[1] : nullptr;
        if (const Curve* pc = pg ? firstLine(*pg) : nullptr) {
            for (const glm::vec3& p : pc->pts) prof.emplace_back(p.x, p.z);
            profClosed = pc->closed;
        } else if (pg && !pg->mesh.faces.empty()) {
            for (int v : pg->mesh.faces.front()) prof.emplace_back(pg->mesh.verts[static_cast<std::size_t>(v)].x,
                                                                   pg->mesh.verts[static_cast<std::size_t>(v)].z);
        } else {
            const int n = std::clamp(sides, 3, 128);
            for (int k = 0; k < n; ++k) {
                const float a = kTau * static_cast<float>(k) / static_cast<float>(n);
                prof.emplace_back(radius * std::cos(a), radius * std::sin(a));
            }
        }
        if (prof.size() < 2) return "The profile needs two points or more";
        // Which way round the profile runs decides which way its walls face:
        // turned so they look away from its inside, whichever way it was drawn.
        float area = 0.0f;
        for (std::size_t k = 0; k < prof.size(); ++k) {
            const glm::vec2& a = prof[k];
            const glm::vec2& b = prof[(k + 1) % prof.size()];
            area += a.x * b.y - b.x * a.y;
        }
        const bool reverse = (area > 0.0f) != flip;
        const int m = static_cast<int>(prof.size());

        for (const Curve& path : in[0]->curves) {
            const int n = static_cast<int>(path.pts.size());
            if (n < 2 || path.loose) continue;
            const bool closed = path.closed && n >= 3;
            const std::vector<glm::vec3>& P = path.pts;
            // Tangents, and frames carried along the path without spinning
            // (parallel transport): the profile does not corkscrew round a bend.
            std::vector<glm::vec3> T(static_cast<std::size_t>(n)), N(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i) {
                const int a = closed ? (i - 1 + n) % n : std::max(i - 1, 0);
                const int b = closed ? (i + 1) % n : std::min(i + 1, n - 1);
                glm::vec3 t = P[static_cast<std::size_t>(b)] - P[static_cast<std::size_t>(a)];
                T[static_cast<std::size_t>(i)] = glm::dot(t, t) > 1e-12f ? glm::normalize(t) : glm::vec3(1, 0, 0);
            }
            auto perp = [](const glm::vec3& v, const glm::vec3& t) {
                glm::vec3 r = v - t * glm::dot(v, t);
                return glm::dot(r, r) > 1e-8f ? glm::normalize(r) : glm::vec3(0.0f);
            };
            glm::vec3 n0 = perp(glm::vec3(0, 1, 0), T[0]);
            if (glm::dot(n0, n0) < 0.5f) n0 = perp(glm::vec3(1, 0, 0), T[0]);
            N[0] = n0;
            for (int i = 1; i < n; ++i) {
                glm::vec3 nn = perp(N[static_cast<std::size_t>(i - 1)], T[static_cast<std::size_t>(i)]);
                N[static_cast<std::size_t>(i)] = glm::dot(nn, nn) > 0.5f ? nn : N[static_cast<std::size_t>(i - 1)];
            }
            // A closed path's frame has to meet itself again: whatever turn the
            // trip round left, spread evenly back over the loop.
            float fix = 0.0f;
            if (closed) {
                const glm::vec3 back = perp(N[static_cast<std::size_t>(n - 1)], T[0]);
                fix = std::atan2(glm::dot(glm::cross(back, N[0]), T[0]), glm::dot(back, N[0]));
            }
            std::vector<float> along(static_cast<std::size_t>(n), 0.0f);
            for (int i = 1; i < n; ++i)
                along[static_cast<std::size_t>(i)] = along[static_cast<std::size_t>(i - 1)] +
                    glm::length(P[static_cast<std::size_t>(i)] - P[static_cast<std::size_t>(i - 1)]);
            const float total = std::max(along.back(), 1e-6f);

            const int base = static_cast<int>(out.mesh.verts.size());
            for (int i = 0; i < n; ++i) {
                const float u = closed ? static_cast<float>(i) / static_cast<float>(n) : along[static_cast<std::size_t>(i)] / total;
                const float turn = glm::radians(twist) * along[static_cast<std::size_t>(i)] / total + fix * u;
                const glm::vec3 t  = T[static_cast<std::size_t>(i)];
                glm::vec3 nn = N[static_cast<std::size_t>(i)];
                nn = nn * std::cos(turn) + glm::cross(t, nn) * std::sin(turn);
                const glm::vec3 side = glm::cross(t, nn);
                for (const glm::vec2& q : prof)
                    addCorner(out.mesh, P[static_cast<std::size_t>(i)] + (side * q.x + nn * q.y) * scale);
            }
            auto at = [&](int i, int k) { return base + (i % n) * m + (k % m); };
            const int segI = closed ? n : n - 1;
            const int segK = profClosed ? m : m - 1;
            for (int i = 0; i < segI; ++i)
                for (int k = 0; k < segK; ++k) {
                    if (reverse) addLoop(out.mesh, {at(i, k), at(i + 1, k), at(i + 1, k + 1), at(i, k + 1)});
                    else         addLoop(out.mesh, {at(i, k), at(i, k + 1), at(i + 1, k + 1), at(i + 1, k)});
                }
            if (caps && !closed && profClosed && m >= 3) {
                // Ends that face out of the tube, however the profile ran.
                for (int end = 0; end < 2; ++end) {
                    const int i = end == 0 ? 0 : n - 1;
                    std::vector<int> loop;
                    for (int k = 0; k < m; ++k) loop.push_back(at(i, k));
                    glm::vec3 nrm(0.0f);
                    for (int k = 0; k < m; ++k) {
                        const glm::vec3& a = out.mesh.verts[static_cast<std::size_t>(loop[static_cast<std::size_t>(k)])];
                        const glm::vec3& b = out.mesh.verts[static_cast<std::size_t>(loop[static_cast<std::size_t>((k + 1) % m)])];
                        nrm += glm::vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
                    }
                    const glm::vec3 want = end == 0 ? -T[0] : T[static_cast<std::size_t>(n - 1)];
                    if ((glm::dot(nrm, want) < 0.0f) != flip) std::reverse(loop.begin() + 1, loop.end());
                    out.mesh.faces.push_back(std::move(loop));
                }
            }
        }
        return "";
    }
};

// Every curve of the input turned round an axis through the origin, like on
// a lathe: a profile drawn from the axis outwards becomes a dome, a tank, a
// nozzle, a dish. Points on the axis stay one point (the tip of a dome).
class RevolveNode : public proc::NodeOf<RevolveNode> {
public:
    int   segments = 32;
    float sweep    = 360.0f;
    int   axis     = 1;
    bool  flip     = false;

    const char* typeId() const override { return "revolve"; }
    const char* displayName() const override { return "Revolve"; }
    int inputSlots() const override { return 1; }
    const char* inputName(int) const override { return "Profile"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = {
            whole("Segments", "segments", &RevolveNode::segments, 3, 512),
            number("Turn (deg)", "sweep", &RevolveNode::sweep, 15.0f, 1.0f, 360.0f, "%.0f"),
            choice("Axis", "axis", &RevolveNode::axis, axisLabels()),
            flag("Flip faces", "flip", &RevolveNode::flip),
        };
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (in.empty() || !in[0]) return "Nothing wired into Profile";
        if (!firstLine(*in[0])) return "The profile has no curves";
        const glm::vec3 ax = axisVec(axis);
        const int segs = std::clamp(segments, 3, 512);
        const bool full = sweep >= 359.99f;
        const int rings = full ? segs : segs + 1;
        for (const Curve& c : in[0]->curves) {
            const int m = static_cast<int>(c.pts.size());
            if (m < 2 || c.loose) continue;
            const std::size_t first = out.mesh.faces.size();
            std::vector<std::vector<int>> idx(static_cast<std::size_t>(rings), std::vector<int>(static_cast<std::size_t>(m)));
            for (int j = 0; j < m; ++j) {
                const glm::vec3 p = c.pts[static_cast<std::size_t>(j)];
                const glm::vec3 r = p - ax * glm::dot(p, ax);
                const bool onAxis = glm::dot(r, r) < 1e-10f;
                const int tip = onAxis ? addCorner(out.mesh, p) : -1;
                for (int i = 0; i < rings; ++i) {
                    const float a = glm::radians(std::clamp(sweep, 1.0f, 360.0f)) * static_cast<float>(i) /
                                    static_cast<float>(segs);
                    idx[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] =
                        onAxis ? tip
                               : addCorner(out.mesh, glm::vec3(glm::rotate(glm::mat4(1.0f), a, ax) * glm::vec4(p, 1.0f)));
                }
            }
            const int segJ = c.closed ? m : m - 1;
            for (int i = 0; i < segs; ++i)
                for (int j = 0; j < segJ; ++j) {
                    const int i1 = (i + 1) % rings, j1 = (j + 1) % m;
                    addLoop(out.mesh, {idx[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)],
                                       idx[static_cast<std::size_t>(i)][static_cast<std::size_t>(j1)],
                                       idx[static_cast<std::size_t>(i1)][static_cast<std::size_t>(j1)],
                                       idx[static_cast<std::size_t>(i1)][static_cast<std::size_t>(j)]});
                }
            // Facing away from the axis, whichever way the profile was drawn:
            // the area of the faces, weighted by how much each looks outward.
            double outward = 0.0;
            for (std::size_t f = first; f < out.mesh.faces.size(); ++f) {
                const glm::vec3 ctr = out.mesh.faceCenter(static_cast<int>(f));
                glm::vec3 r = ctr - ax * glm::dot(ctr, ax);
                if (glm::dot(r, r) < 1e-10f) continue;
                outward += glm::dot(out.mesh.faceNormal(static_cast<int>(f)), glm::normalize(r)) *
                           out.mesh.faceArea(static_cast<int>(f));
            }
            if ((outward < 0.0) != flip) flipFaces(out.mesh, first);
        }
        return "";
    }
};

// ================================================================================
// Points
// ================================================================================

// Pick points: inside a box or a sphere, facing a way, every n-th, a random
// share -- replacing the selection, adding to it, taking from it, or keeping
// only what both agree on. Chained, they filter: a box, then every second
// point of what is in it. Every step after reads the selection (see Geo).
class SelectPointsNode : public proc::NodeOf<SelectPointsNode> {
public:
    int       by     = 0;   // 0 box, 1 sphere, 2 facing, 3 every n-th, 4 random, 5 all
    int       mode   = 0;   // 0 replace, 1 add, 2 remove, 3 keep only matching
    bool      invert = false;
    int       scope  = 0;   // 0 all points, 1 face corners, 2 curve points
    glm::vec3 center{0.0f};
    glm::vec3 size{10.0f, 10.0f, 10.0f};
    float     radius = 5.0f;
    int       facing = 0;   // FaceSet::Up + this
    int       every  = 2;
    int       offset = 0;
    float     share  = 0.5f;
    int       seed   = 1;

    const char* typeId() const override { return "selectpoints"; }
    const char* displayName() const override { return "Select points"; }
    int inputSlots() const override { return 1; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            using S = SelectPointsNode;
            std::vector<Property> v;
            v.push_back(choice("By", "by", &S::by,
                               {"Inside a box", "Inside a sphere", "Facing", "Every n-th",
                                "Random share", "All"}));
            v.push_back(choice("Then", "mode", &S::mode,
                               {"Replace the selection", "Add to it", "Take from it",
                                "Keep only these"}));
            v.push_back(flag("Invert the test", "invert", &S::invert));
            v.push_back(choice("Points of", "scope", &S::scope, {"Everything", "Faces", "Curves"}));
            auto when = [](Property pr, std::initializer_list<int> bys) {
                const std::vector<int> ok(bys);
                pr.visible = [ok](const void* o) {
                    return std::find(ok.begin(), ok.end(), static_cast<const S*>(o)->by) != ok.end();
                };
                return pr;
            };
            v.push_back(when(vector3("Center", "center", &S::center, 0.5f), {0, 1}));
            v.push_back(when(vector3("Size", "size", &S::size, 0.5f), {0}));
            v.push_back(when(number("Radius", "radius", &S::radius, 0.5f, 0.0f, 100000.0f), {1}));
            v.push_back(when(choice("Facing", "facing", &S::facing,
                                    {"Up (+Y)", "Down (-Y)", "Sides (level)", "+X", "-X", "+Z", "-Z",
                                     "Outside (away from Y axis)", "Inside (toward Y axis)"}), {2}));
            v.push_back(when(whole("Every", "every", &S::every, 1, 10000), {3}));
            v.push_back(when(whole("Starting at", "offset", &S::offset, 0, 10000), {3}));
            v.push_back(when(number("Share", "share", &S::share, 0.05f, 0.0f, 1.0f), {4}));
            v.push_back(when(whole("Seed", "seed", &S::seed, 0, 99999), {4}));
            return v;
        }();
        return p;
    }

    bool test(const glm::vec3& p, const glm::vec3& n, int index) const {
        bool hit = true;
        switch (by) {
            case 0: {
                const glm::vec3 h = 0.5f * glm::abs(size);
                hit = glm::all(glm::lessThanEqual(glm::abs(p - center), h));
                break;
            }
            case 1: hit = glm::length(p - center) <= radius; break;
            case 2: {
                constexpr float k = 0.7f;
                switch (static_cast<FaceSet>(facing + 1)) {
                    case FaceSet::Up:    hit = n.y >  k; break;
                    case FaceSet::Down:  hit = n.y < -k; break;
                    case FaceSet::Sides: hit = std::fabs(n.y) < 0.3f; break;
                    case FaceSet::PosX:  hit = n.x >  k; break;
                    case FaceSet::NegX:  hit = n.x < -k; break;
                    case FaceSet::PosZ:  hit = n.z >  k; break;
                    case FaceSet::NegZ:  hit = n.z < -k; break;
                    default: {
                        const glm::vec3 r(p.x, 0.0f, p.z);
                        if (glm::dot(r, r) < 1e-8f) { hit = false; break; }
                        const float d = glm::dot(glm::vec3(n.x, 0.0f, n.z), glm::normalize(r));
                        hit = static_cast<FaceSet>(facing + 1) == FaceSet::AwayFromAxis ? d > 0.5f : d < -0.5f;
                    }
                }
                break;
            }
            case 3: hit = index >= offset && (index - offset) % std::max(every, 1) == 0; break;
            case 4: hit = proc::random01(index, seed) < std::clamp(share, 0.0f, 1.0f); break;
            default: hit = true; break;
        }
        return hit != invert;
    }
    char combine(bool was, bool hit) const {
        switch (mode) {
            case 1:  return (was || hit) ? 1 : 0;
            case 2:  return (was && !hit) ? 1 : 0;
            case 3:  return (was && hit) ? 1 : 0;
            default: return hit ? 1 : 0;
        }
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        out = *in[0];
        const bool had = out.hasSel;
        // With no selection yet, "add to it" starts from nothing and "take
        // from it" from everything -- which is what "no selection" means.
        auto before = [&](bool picked) { return had ? picked : mode != 1; };
        out.hasSel = true;
        out.meshSel.resize(out.mesh.verts.size(), 0);
        const std::vector<glm::vec3> nrm = proc::cornerNormals(out.mesh);
        for (int i = 0; i < static_cast<int>(out.mesh.verts.size()); ++i) {
            const bool was = before(in[0]->meshPicked(i));
            char& s = out.meshSel[static_cast<std::size_t>(i)];
            if (scope == 2) { s = mode == 0 ? 0 : (was ? 1 : 0); continue; }
            s = combine(was, test(out.mesh.verts[static_cast<std::size_t>(i)], nrm[static_cast<std::size_t>(i)], i));
        }
        for (std::size_t ci = 0; ci < out.curves.size(); ++ci) {
            Curve& c = out.curves[ci];
            const Curve& src = in[0]->curves[ci];
            c.sel.resize(c.pts.size(), 0);
            for (int i = 0; i < static_cast<int>(c.pts.size()); ++i) {
                const bool was = before(Geo::curvePicked(src, i, had));
                char& s = c.sel[static_cast<std::size_t>(i)];
                if (scope == 1) { s = mode == 0 ? 0 : (was ? 1 : 0); continue; }
                // Counted along each curve: "every second point" of a path.
                s = combine(was, test(c.pts[static_cast<std::size_t>(i)], c.normalAt(i), i));
            }
        }
        return "";
    }
};

// Take the selected points away -- or keep only them. A face loses itself
// with any of its corners; a curve just runs on without the point.
class DeletePointsNode : public proc::NodeOf<DeletePointsNode> {
public:
    bool keepSelected = false;

    const char* typeId() const override { return "deletepoints"; }
    const char* displayName() const override { return "Delete points"; }
    int inputSlots() const override { return 1; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = {
            flag("Keep only the selected", "keepSelected", &DeletePointsNode::keepSelected),
        };
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        out = *in[0];
        if (!out.hasSel) return "Nothing selected yet: put a Select points node before it";
        auto gone = [&](bool picked) { return keepSelected ? !picked : picked; };
        std::vector<char> drop(out.mesh.faces.size(), 0);
        for (std::size_t f = 0; f < out.mesh.faces.size(); ++f)
            for (int v : out.mesh.faces[f])
                if (gone(out.meshPicked(v))) { drop[f] = 1; break; }
        dropFaces(out, drop);
        std::vector<Curve> kept;
        for (Curve& c : out.curves) {
            Curve k;
            k.closed = c.closed;
            k.loose  = c.loose;
            for (int i = 0; i < static_cast<int>(c.pts.size()); ++i) {
                const bool picked = Geo::curvePicked(c, i, true);
                if (gone(picked)) continue;
                k.pts.push_back(c.pts[static_cast<std::size_t>(i)]);
                k.sel.push_back(picked ? 1 : 0);
                if (!c.nrm.empty()) k.nrm.push_back(c.normalAt(i));
            }
            if (k.pts.size() < 3) k.closed = false;
            // A line needs two points; a single loose point is still a point.
            if (k.pts.size() >= (k.loose ? 1u : 2u)) kept.push_back(std::move(k));
        }
        out.curves = std::move(kept);
        return "";
    }
};
// The faces turned into points: their corners, the middle of each face, or
// points strewn over the surface -- each with the way the surface faced
// there, so Copy onto points still stands things up on it. The faces are gone
// afterwards (unless kept), so what comes out is something to select, thin
// out and copy onto without the surface showing under it: rivets on every
// corner, lamps in the middle of every panel, rocks strewn over a slope.
// With a selection, only the selected corners (the faces between them) count;
// the points made start unselected.
class MeshToPointsNode : public proc::NodeOf<MeshToPointsNode> {
public:
    int  from      = 0;     // 0 corners, 1 face centres, 2 strewn over the surface
    int  count     = 100;
    int  seed      = 1;
    bool keepFaces = false;

    const char* typeId() const override { return "meshtopoints"; }
    const char* displayName() const override { return "Mesh to points"; }
    int inputSlots() const override { return 1; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            using S = MeshToPointsNode;
            std::vector<Property> v;
            v.push_back(choice("From", "from", &S::from, {"Corners", "Face centres", "Strewn over the surface"}));
            auto strewn = [](Property pr) {
                pr.visible = [](const void* o) { return static_cast<const S*>(o)->from == 2; };
                return pr;
            };
            v.push_back(strewn(whole("Count", "count", &S::count, 1, 100000, 10)));
            v.push_back(strewn(whole("Seed", "seed", &S::seed, 0, 99999)));
            v.push_back(flag("Keep the faces", "keepFaces", &S::keepFaces));
            return v;
        }();
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        const Geo& src = *in[0];
        const EditMesh& m = src.mesh;
        Curve pts;
        pts.loose = true;
        auto put = [&](const glm::vec3& p, const glm::vec3& n) {
            pts.pts.push_back(p);
            pts.nrm.push_back(n);
        };
        const std::vector<int> faces =
            proc::pickFaces(src, src.hasSel ? FaceSet::SelectedPoints : FaceSet::All, 1.0f, 0);
        if (from == 0) {
            // Only corners a face uses: the others are not part of the surface.
            const std::vector<glm::vec3> nrm = proc::cornerNormals(m);
            std::vector<char> used(m.verts.size(), 0);
            for (const std::vector<int>& f : m.faces)
                for (int v : f) used[static_cast<std::size_t>(v)] = 1;
            for (int i = 0; i < static_cast<int>(m.verts.size()); ++i)
                if (used[static_cast<std::size_t>(i)] && src.meshPicked(i))
                    put(m.verts[static_cast<std::size_t>(i)], nrm[static_cast<std::size_t>(i)]);
        } else if (from == 1) {
            for (int f : faces) put(m.faceCenter(f), m.faceNormal(f));
        } else {
            // Evenly over the area: each face fanned into triangles, a triangle
            // drawn by its share of the total, a spot drawn inside it.
            struct Tri { glm::vec3 a, b, c, n; };
            std::vector<Tri>    tris;
            std::vector<double> upTo;   // running total of the areas
            double total = 0.0;
            for (int f : faces) {
                const std::vector<int>& loop = m.faces[static_cast<std::size_t>(f)];
                const glm::vec3 n = m.faceNormal(f);
                for (std::size_t k = 1; k + 1 < loop.size(); ++k) {
                    const glm::vec3 a = m.verts[static_cast<std::size_t>(loop[0])];
                    const glm::vec3 b = m.verts[static_cast<std::size_t>(loop[k])];
                    const glm::vec3 c = m.verts[static_cast<std::size_t>(loop[k + 1])];
                    const double area = 0.5 * glm::length(glm::cross(b - a, c - a));
                    if (area <= 1e-12) continue;
                    total += area;
                    tris.push_back({a, b, c, n});
                    upTo.push_back(total);
                }
            }
            const int n = std::clamp(count, 1, 100000);
            for (int i = 0; i < n && total > 0.0; ++i) {
                const double r = static_cast<double>(proc::random01(3 * i, seed)) * total;
                const std::size_t t = std::min(
                    static_cast<std::size_t>(std::upper_bound(upTo.begin(), upTo.end(), r) - upTo.begin()),
                    tris.size() - 1);
                float u = proc::random01(3 * i + 1, seed), w = proc::random01(3 * i + 2, seed);
                if (u + w > 1.0f) { u = 1.0f - u; w = 1.0f - w; }
                const Tri& tr = tris[t];
                put(tr.a + u * (tr.b - tr.a) + w * (tr.c - tr.a), tr.n);
            }
        }
        if (keepFaces) {
            out = src;
        } else {
            out.curves    = src.curves;
            out.instances = src.instances;
        }
        // The selection has done its work in choosing; the points are new.
        out.hasSel = false;
        out.meshSel.clear();
        for (Curve& c : out.curves) c.sel.clear();
        if (pts.pts.empty())
            return m.faces.empty() ? "No faces to make points of" : "No points: nothing selected";
        out.curves.push_back(std::move(pts));
        return "";
    }
};

// ================================================================================
// Detail
// ================================================================================

// Pull faces out along their normals, after an optional inset: the cap of a
// module becomes a docking collar, the top of a box a roof structure.
//
// Lines are pulled too, the way Blender pulls edges: each becomes a band of
// faces `distance` along one axis -- an open line a wall, a closed one a
// sleeve (open at both ends; a solid comes from a *filled* shape). A line so
// pulled is faces from then on and leaves the curves; loose points stay.
class ExtrudeNode : public proc::NodeOf<ExtrudeNode> {
public:
    int   faces    = static_cast<int>(FaceSet::Up);
    float share    = 1.0f;
    int   seed     = 1;
    float distance = 1.0f;
    float inset    = 0.0f;   // metres
    bool  separate = false;
    bool  back     = true;
    int   lineAxis = 1;      // which way lines are pulled: Y stands them up

    const char* typeId() const override { return "extrude"; }
    const char* displayName() const override { return "Extrude"; }
    int inputSlots() const override { return 1; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            std::vector<Property> v;
            faceRows<ExtrudeNode>(v);
            v.push_back(number("Distance", "distance", &ExtrudeNode::distance, 0.25f, -10000.0f, 10000.0f));
            v.push_back(number("Inset first", "inset", &ExtrudeNode::inset, 0.1f, 0.0f, 10000.0f));
            v.push_back(flag("Each face on its own", "separate", &ExtrudeNode::separate));
            v.push_back(flag("Close the back", "back", &ExtrudeNode::back));
            v.push_back(choice("Lines along", "lineAxis", &ExtrudeNode::lineAxis, axisLabels()));
            return v;
        }();
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        out = *in[0];
        const std::vector<int> fs = proc::pickFaces(out, static_cast<FaceSet>(faces), share, seed);
        const bool lines = std::any_of(out.curves.begin(), out.curves.end(),
                                       [](const Curve& c) { return !c.loose && c.pts.size() >= 2; });
        if (fs.empty() && !lines) return "No faces of that kind, and no lines";
        if (std::fabs(distance) > 1e-6f) pullLines(out);
        if (fs.empty()) return "";
        // A face standing free -- a filled circle, a sheet -- has nothing behind
        // it: pulled out, it would leave its back open. Those (each of whose
        // edges borders only faces being extruded) get their old loop back,
        // turned round, so a filled shape becomes a closed solid.
        std::vector<std::vector<int>> backs;
        std::vector<int> backOf;
        if (back) {
            std::vector<char> picked(out.mesh.faces.size(), 0);
            for (int f : fs) picked[static_cast<std::size_t>(f)] = 1;
            std::unordered_map<std::uint64_t, int> others;   // edge -> faces not being extruded
            for (std::size_t f = 0; f < out.mesh.faces.size(); ++f) {
                if (picked[f]) continue;
                const auto& fv = out.mesh.faces[f];
                for (std::size_t i = 0; i < fv.size(); ++i) {
                    const int a = fv[i], b = fv[(i + 1) % fv.size()];
                    ++others[(static_cast<std::uint64_t>(std::min(a, b)) << 32) | static_cast<std::uint32_t>(std::max(a, b))];
                }
            }
            for (int f : fs) {
                const auto& fv = out.mesh.faces[static_cast<std::size_t>(f)];
                bool loose = true;
                for (std::size_t i = 0; i < fv.size() && loose; ++i) {
                    const int a = fv[i], b = fv[(i + 1) % fv.size()];
                    loose = others.count((static_cast<std::uint64_t>(std::min(a, b)) << 32) |
                                         static_cast<std::uint32_t>(std::max(a, b))) == 0;
                }
                if (!loose) continue;
                backs.push_back(std::vector<int>(fv.rbegin(), fv.rend()));
                backOf.push_back(f);
            }
        }
        // Both add corners at the end and keep the old ones' numbers, so the
        // selection survives (the new corners come in unselected).
        if (inset > 0.0f) editmesh::insetFaces(out.mesh, fs, inset);
        if (std::fabs(distance) > 1e-6f) {
            if (separate) for (int f : fs) editmesh::extrude(out.mesh, f, distance);
            else          editmesh::extrudeFaces(out.mesh, fs, distance);
            for (std::size_t i = 0; i < backs.size(); ++i) addFace(out.mesh, backs[i], backOf[i]);
        }
        return "";
    }

private:
    // Every line of `g` a band of quads `distance` along the axis. A closed
    // line is turned so its band faces out (counter-clockwise round the pull);
    // an open one faces along its run crossed with the pull. The line's points
    // become the band's foot and keep their selection; the top comes in
    // unselected.
    void pullLines(Geo& g) const {
        const glm::vec3 d = axisVec(lineAxis) * distance;
        std::vector<Curve> kept;
        g.syncSel();
        for (Curve& c : g.curves) {
            if (c.loose || c.pts.size() < 2) { kept.push_back(std::move(c)); continue; }
            const int n = static_cast<int>(c.pts.size());
            const bool closed = c.closed && n >= 3;
            std::vector<int> order(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i) order[static_cast<std::size_t>(i)] = i;
            if (closed) {
                glm::vec3 area(0.0f);   // Newell: the loop's own facing
                for (int i = 0; i < n; ++i)
                    area += glm::cross(c.pts[static_cast<std::size_t>(i)], c.pts[static_cast<std::size_t>((i + 1) % n)]);
                if (glm::dot(area, d) < 0.0f) std::reverse(order.begin(), order.end());
            }
            std::vector<int> foot(static_cast<std::size_t>(n)), top(static_cast<std::size_t>(n));
            for (int k = 0; k < n; ++k) {
                const int i = order[static_cast<std::size_t>(k)];
                foot[static_cast<std::size_t>(k)] = addCorner(g.mesh, c.pts[static_cast<std::size_t>(i)]);
                if (g.hasSel) g.meshSel.push_back(Geo::curvePicked(c, i, true) ? 1 : 0);
            }
            for (int k = 0; k < n; ++k) {
                top[static_cast<std::size_t>(k)] = addCorner(g.mesh, c.pts[static_cast<std::size_t>(order[static_cast<std::size_t>(k)])] + d);
                if (g.hasSel) g.meshSel.push_back(0);
            }
            const int segs = closed ? n : n - 1;
            for (int k = 0; k < segs; ++k) {
                const std::size_t a = static_cast<std::size_t>(k), b = static_cast<std::size_t>((k + 1) % n);
                g.mesh.faces.push_back({foot[a], foot[b], top[b], top[a]});
            }
        }
        g.curves = std::move(kept);
        if (!g.mesh.faceMat.empty()) g.mesh.syncFaceMat();
        if (!g.mesh.faceUV.empty())  g.mesh.syncFaceUv();
    }
};

// Sci-fi surface detail: every picked face becomes a raised (or sunk) plate,
// each a random height between the two depths. Rows on a tube or cells on a
// grid give it the faces to work with -- one huge face makes one huge plate.
class PanelsNode : public proc::NodeOf<PanelsNode> {
public:
    int   faces    = static_cast<int>(FaceSet::All);
    float share    = 0.6f;
    int   seed     = 1;
    float inset    = 0.12f;   // of each face's own size
    float depthMin = 0.1f;
    float depthMax = 0.4f;

    const char* typeId() const override { return "panels"; }
    const char* displayName() const override { return "Panels"; }
    int inputSlots() const override { return 1; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            std::vector<Property> v;
            faceRows<PanelsNode>(v);
            v.push_back(number("Rim", "inset", &PanelsNode::inset, 0.02f, 0.0f, 0.9f));
            v.push_back(number("Depth from", "depthMin", &PanelsNode::depthMin, 0.05f, -1000.0f, 1000.0f));
            v.push_back(number("Depth to", "depthMax", &PanelsNode::depthMax, 0.05f, -1000.0f, 1000.0f));
            return v;
        }();
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        out = *in[0];
        const std::vector<int> fs = proc::pickFaces(out, static_cast<FaceSet>(faces), share, seed);
        if (fs.empty()) return "No faces of that kind";
        if (out.mesh.faces.size() + fs.size() * 10 > 400000) return "Too many faces (over 400000)";
        for (int f : fs) {
            const float t = proc::random01(f, seed + 104729);
            panelFace(out.mesh, f, inset, depthMin + (depthMax - depthMin) * t);
        }
        return "";
    }
};

class SubdivideNode : public proc::NodeOf<SubdivideNode> {
public:
    int levels = 1;
    int mode   = 0;   // 0 smooth (Catmull-Clark), 1 cut only

    const char* typeId() const override { return "subdivide"; }
    const char* displayName() const override { return "Subdivide"; }
    int inputSlots() const override { return 1; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = {
            whole("Levels", "levels", &SubdivideNode::levels, 0, 4),
            choice("Mode", "mode", &SubdivideNode::mode, {"Smooth", "Cut only"}),
        };
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        out = *in[0];
        const int want = std::clamp(levels, 0, 4);
        const int done = editmesh::subdivideSurface(out.mesh, want, mode == 0);
        out.dropMeshSel();   // the corners were made anew
        return done < want ? "Stopped early: too many faces" : "";
    }
};

// A surface given thickness: a grid becomes a slab, an open shell a wall.
class SolidifyNode : public proc::NodeOf<SolidifyNode> {
public:
    float thickness = 0.2f;
    float offset    = 0.0f;

    const char* typeId() const override { return "solidify"; }
    const char* displayName() const override { return "Thicken"; }
    int inputSlots() const override { return 1; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = {
            number("Thickness", "thickness", &SolidifyNode::thickness, 0.05f, -1000.0f, 1000.0f),
            number("Offset", "offset", &SolidifyNode::offset, 0.5f, -1.0f, 1.0f, "%.1f"),
        };
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        out = *in[0];
        editmesh::solidify(out.mesh, thickness, offset, true, true);
        out.dropMeshSel();
        return "";
    }
};

// Every edge a strut: a tube with a few segments and rows becomes a truss,
// a sphere a geodesic frame.
class LatticeNode : public proc::NodeOf<LatticeNode> {
public:
    float thickness = 0.2f;

    const char* typeId() const override { return "lattice"; }
    const char* displayName() const override { return "Lattice"; }
    int inputSlots() const override { return 1; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = {
            number("Strut thickness", "thickness", &LatticeNode::thickness, 0.05f, 0.001f, 1000.0f),
        };
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        out = *in[0];
        editmesh::wireframe(out.mesh, thickness, 0.0f, true);
        out.dropMeshSel();
        return "";
    }
};

class DeleteFacesNode : public proc::NodeOf<DeleteFacesNode> {
public:
    int   faces = static_cast<int>(FaceSet::Up);
    float share = 1.0f;
    int   seed  = 1;
    bool  keepOnly = false;

    const char* typeId() const override { return "deletefaces"; }
    const char* displayName() const override { return "Delete faces"; }
    int inputSlots() const override { return 1; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            std::vector<Property> v;
            faceRows<DeleteFacesNode>(v);
            v.push_back(flag("Keep only these", "keepOnly", &DeleteFacesNode::keepOnly));
            return v;
        }();
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        out = *in[0];
        const std::vector<int> fs = proc::pickFaces(out, static_cast<FaceSet>(faces), share, seed);
        std::vector<char> drop(out.mesh.faces.size(), keepOnly ? 1 : 0);
        for (int f : fs) drop[static_cast<std::size_t>(f)] = keepOnly ? 0 : 1;
        dropFaces(out, drop);
        return "";
    }
};

// ================================================================================
// Look
// ================================================================================

// A library material on the picked faces. Faces nobody dresses wear the
// object's own material, so the hull can be the object's and only the
// windows, the solar cells and the gold foil need a node.
class MaterialNode : public proc::NodeOf<MaterialNode> {
public:
    std::string material;   // the material's GUID; "" = the object's own
    int   faces = static_cast<int>(FaceSet::All);
    float share = 1.0f;
    int   seed  = 1;

    const char* typeId() const override { return "material"; }
    const char* displayName() const override { return "Material"; }
    int inputSlots() const override { return 1; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            std::vector<Property> v;
            // Drawn as a picker over the scene's materials (the editor knows
            // the key); stored as the GUID, so renaming a material is harmless.
            v.push_back(field("Material", "material", PropKind::Text, &MaterialNode::material));
            faceRows<MaterialNode>(v);
            return v;
        }();
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        out = *in[0];
        const fitzel::AssetId id = fitzel::AssetId::fromString(material);
        for (int f : proc::pickFaces(out, static_cast<FaceSet>(faces), share, seed))
            out.mesh.setFaceMaterial(f, id);
        return "";
    }
};

// ================================================================================
// Registration
// ================================================================================

struct RegisterNodes {
    template <class T>
    static void add(const char* category, const char* tip) {
        const T t;
        proc::registerType({t.typeId(), t.displayName(), category, tip,
                            [] { return std::unique_ptr<proc::Node>(std::make_unique<T>()); }});
    }
    RegisterNodes() {
        add<BoxNode>("Shapes", "A box of any size.");
        add<CylinderNode>("Shapes", "A cylinder: one radius, a height, closed at both ends.");
        add<TubeNode>("Shapes", "A cylinder or a cone: hub, module, spoke, mast.\n"
                                "Rows give later steps faces to work on.");
        add<SphereNode>("Shapes", "A sphere: tanks, domes, pods.");
        add<TorusNode>("Shapes", "A ring. Four sides make a box section with a flat floor --\n"
                                 "a habitat ring; many sides make it round.");
        add<GridNode>("Shapes", "A flat sheet in cells: solar wings, decks, pads.");
        add<CircleNode>("Curves", "A circle or an arc: a line, or a filled disc.\n"
                                  "A profile to sweep, a path, a ring of points.");
        add<RectNode>("Curves", "A rectangle -- or a square: a line, or a filled face.");
        add<CurveNode>("Curves", "A line through points you give, straight or smooth:\n"
                                 "a path for a pipe, a profile for a dome.");
        add<ResampleNode>("Curves", "Curves cut into evenly spaced points.");
        add<SweepNode>("Curves", "A profile run along a path: pipes, corridors, rails,\n"
                                 "cables. Round, or the shape wired into Profile.");
        add<RevolveNode>("Curves", "A profile turned round an axis like on a lathe:\n"
                                   "domes, tanks, nozzles, dishes.");
        add<PrefabNode>("Shapes", "A prefab of the project, placed as real objects under\n"
                                  "this one: move it, copy it, put it onto points.");
        add<MergeNode>("Combine", "Several pieces as one.");
        add<TransformNode>("Combine", "Move, turn and scale what comes in -- or only its\n"
                                      "selected points.");
        add<MirrorNode>("Combine", "Reflect across an axis, keeping the original: build half, get both.");
        add<CopyRadialNode>("Copies", "Copies turned round an axis: spokes, modules round a ring.");
        add<CopyLinearNode>("Copies", "Copies in a row: modules on a truss, panels on a wing.");
        add<CopyToPointsNode>("Copies", "A shape copied onto the points (the selected ones) or\n"
                                        "faces of another: antennas, lights along a path.");
        add<SelectPointsNode>("Points", "Pick points by a box, a sphere, a direction, every\n"
                                        "n-th or at random -- and chain them to filter.");
        add<DeletePointsNode>("Points", "Remove the selected points (or all the others).");
        add<MeshToPointsNode>("Points", "The faces as points: their corners, their middles, or strewn\n"
                                        "over the surface -- to copy onto without the surface showing.");
        add<PanelsNode>("Detail", "Hull plating: faces become raised or sunk plates\n"
                                  "of random height.");
        add<ExtrudeNode>("Detail", "Pull faces out (or push them in), after an optional inset.\n"
                                   "Lines are pulled into bands: a wall, a sleeve.");
        add<LatticeNode>("Detail", "Every edge a strut: a tube becomes a truss.");
        add<SolidifyNode>("Detail", "Give a flat sheet thickness.");
        add<SubdivideNode>("Detail", "Split every face; smooth rounds the shape off.");
        add<DeleteFacesNode>("Detail", "Remove faces -- or keep only them.");
        add<MaterialNode>("Look", "A material on some or all faces.");
    }
} g_registerNodes;

} // namespace