#include "ProcGraph.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "ProcNodeKit.hpp"
#include "ProcNoise.hpp"
#include "ProcRockTex.hpp"

// The node kinds that shape things the way nature does rather than the way a
// drawing board does (ProcGraph.hpp): an even ball of triangles to start from,
// noise to push its points around, smoothing to wear it down, flat facets
// knocked off it where it would have split -- and a texture painted onto what
// came out. Together: a rock. Separately: a lumpy asteroid, a hill out of a
// grid, a cave wall, a worn statue base.
//
// Everything that moves points by where they are moves corners at the same
// place as one point (kit::weld): a box's corners are three corners, one per
// face, and noise that moved them apart would open the box at every edge.

namespace {

using proc::Curve;
using proc::Geo;

using namespace proc::kit;

// ================================================================================
// Shapes
// ================================================================================

// An icosahedron with every triangle split into four, `detail` times, and
// pushed out onto the sphere: triangles all of a size, with no poles where a
// UV sphere bunches its rows. What noise wants to work on.
class IcosphereNode : public proc::NodeOf<IcosphereNode> {
public:
    float     radius = 2.0f;
    int       detail = 4;
    glm::vec3 center{0.0f};

    const char* typeId() const override { return "icosphere"; }
    const char* displayName() const override { return "Icosphere"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = {
            number("Radius", "radius", &IcosphereNode::radius, 0.25f, 0.01f, 10000.0f),
            whole("Detail", "detail", &IcosphereNode::detail, 0, 6),
            vector3("Center", "center", &IcosphereNode::center, 0.5f),
        };
        return p;
    }
    std::string cook(const std::vector<const Geo*>&, Geo& out) const override {
        const float t = 0.5f * (1.0f + std::sqrt(5.0f));
        std::vector<glm::vec3> v = {
            {-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
            {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
        for (glm::vec3& p : v) p = glm::normalize(p);
        std::vector<glm::ivec3> f = {
            {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
            {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8},
            {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
        for (int d = 0; d < std::clamp(detail, 0, 6); ++d) {
            std::map<std::pair<int, int>, int> mid;
            auto half = [&](int a, int b) {
                const std::pair<int, int> k(std::min(a, b), std::max(a, b));
                auto it = mid.find(k);
                if (it != mid.end()) return it->second;
                v.push_back(glm::normalize(v[static_cast<std::size_t>(a)] + v[static_cast<std::size_t>(b)]));
                return mid[k] = static_cast<int>(v.size()) - 1;
            };
            std::vector<glm::ivec3> next;
            next.reserve(f.size() * 4);
            for (const glm::ivec3& tri : f) {
                const int ab = half(tri.x, tri.y), bc = half(tri.y, tri.z), ca = half(tri.z, tri.x);
                next.push_back({tri.x, ab, ca});
                next.push_back({tri.y, bc, ab});
                next.push_back({tri.z, ca, bc});
                next.push_back({ab, bc, ca});
            }
            f.swap(next);
        }
        const float r = std::max(radius, 0.001f);
        EditMesh& m = out.mesh;
        m.verts.reserve(v.size());
        for (const glm::vec3& p : v) m.verts.push_back(p * r);
        for (const glm::ivec3& tri : f) m.faces.push_back({tri.x, tri.y, tri.z});
        if (signedVolume(m) < 0.0) flipFaces(m);
        transformGeo(out, glm::translate(glm::mat4(1.0f), center));
        return "";
    }
};

// ================================================================================
// Deform
// ================================================================================

// Houdini's Mountain: every point pushed by noise -- out along the surface,
// straight up, or every way at once. The selected points only, when a Select
// points node before it picked some. Curves move with it, along the way their
// points face.
class NoiseNode : public proc::NodeOf<NoiseNode> {
public:
    int       direction  = 0;   // 0 along the normals, 1 up (+Y), 2 every way
    float     amplitude  = 0.5f;
    int       kind       = 0;   // proc::noise::Kind
    float     size       = 2.0f;
    int       octaves    = 4;
    float     roughness  = 0.5f;
    float     lacunarity = 2.0f;
    float     warp       = 0.0f;
    float     crackWidth = 0.1f;
    glm::vec3 offset{0.0f};
    int       seed       = 1;

    const char* typeId() const override { return "noise"; }
    const char* displayName() const override { return "Noise"; }
    int inputSlots() const override { return 1; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            using N = NoiseNode;
            std::vector<Property> v;
            v.push_back(choice("Move", "direction", &N::direction,
                               {"Along the surface normals", "Up (+Y)", "Every way"}));
            v.push_back(number("Amplitude", "amplitude", &N::amplitude, 0.05f, -10000.0f, 10000.0f));
            v.push_back(choice("Noise", "kind", &N::kind, proc::noise::kindLabels()));
            v.push_back(number("Size", "size", &N::size, 0.1f, 0.001f, 100000.0f));
            v.push_back(whole("Octaves", "octaves", &N::octaves, 1, 12));
            v.push_back(number("Roughness", "roughness", &N::roughness, 0.05f, 0.0f, 1.0f));
            v.push_back(number("Lacunarity", "lacunarity", &N::lacunarity, 0.1f, 1.0f, 8.0f));
            v.push_back(number("Warp", "warp", &N::warp, 0.1f, 0.0f, 10.0f));
            Property w = number("Crack width", "crackWidth", &N::crackWidth, 0.01f, 0.001f, 1.0f);
            w.visible = [](const void* o) {
                return static_cast<const N*>(o)->kind == static_cast<int>(proc::noise::Kind::Cracks);
            };
            v.push_back(std::move(w));
            v.push_back(vector3("Offset", "offset", &N::offset, 0.1f, "%.2f"));
            v.push_back(whole("Seed", "seed", &N::seed, 0, 99999));
            return v;
        }();
        return p;
    }
    proc::noise::Fractal fractal() const {
        proc::noise::Fractal f;
        f.kind       = static_cast<proc::noise::Kind>(
            std::clamp(kind, 0, static_cast<int>(proc::noise::kindLabels().size()) - 1));
        f.size       = size;
        f.octaves    = octaves;
        f.roughness  = roughness;
        f.lacunarity = lacunarity;
        f.warp       = warp;
        f.crackWidth = crackWidth;
        f.offset     = offset;
        f.seed       = seed;
        return f;
    }
    glm::vec3 push(const glm::vec3& p, const glm::vec3& n, const proc::noise::Fractal& f) const {
        if (direction == 2) {
            proc::noise::Fractal fy = f, fz = f;
            fy.seed += 7001;
            fz.seed += 14002;
            return amplitude * glm::vec3(proc::noise::fractal(f, p), proc::noise::fractal(fy, p),
                                         proc::noise::fractal(fz, p));
        }
        const glm::vec3 d = direction == 1 ? glm::vec3(0.0f, 1.0f, 0.0f) : n;
        return d * (amplitude * proc::noise::fractal(f, p));
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        out = *in[0];
        const proc::noise::Fractal f = fractal();
        const Welded w = weld(out.mesh, false);
        // Once per point, so twins at one place stay at one place.
        std::vector<glm::vec3> by(w.pos.size());
        std::vector<char> done(w.pos.size(), 0);
        for (int i = 0; i < static_cast<int>(out.mesh.verts.size()); ++i) {
            if (!out.meshPicked(i)) continue;
            const std::size_t g = static_cast<std::size_t>(w.group[static_cast<std::size_t>(i)]);
            if (!done[g]) { by[g] = push(w.pos[g], w.nrm[g], f); done[g] = 1; }
            out.mesh.verts[static_cast<std::size_t>(i)] += by[g];
        }
        for (Curve& c : out.curves)
            for (int i = 0; i < static_cast<int>(c.pts.size()); ++i)
                if (Geo::curvePicked(c, i, out.hasSel)) c.pts[static_cast<std::size_t>(i)] += push(c.pts[static_cast<std::size_t>(i)], c.normalAt(i), f);
        return "";
    }
};

// Wear the shape down: every point moved towards the middle of its
// neighbours, a share at a time. Laplace's smoothing shrinks what it smooths;
// Taubin's way pushes back out after every step and keeps the size. Points on
// an open edge stay where they are, so a sheet keeps its outline.
class SmoothNode : public proc::NodeOf<SmoothNode> {
public:
    int   iterations = 4;
    float strength   = 0.5f;
    bool  keepSize   = true;

    const char* typeId() const override { return "smooth"; }
    const char* displayName() const override { return "Smooth"; }
    int inputSlots() const override { return 1; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = {
            whole("Iterations", "iterations", &SmoothNode::iterations, 0, 200),
            number("Strength", "strength", &SmoothNode::strength, 0.05f, 0.0f, 1.0f),
            flag("Keep the size", "keepSize", &SmoothNode::keepSize),
        };
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        out = *in[0];
        const float lam = std::clamp(strength, 0.0f, 1.0f);
        const float mu  = -lam / (1.0f - 0.1f * lam);   // Taubin's pass back out
        const int   its = std::clamp(iterations, 0, 200);
        if (!out.mesh.verts.empty() && its > 0 && lam > 0.0f) {
            const Welded w = weld(out.mesh, true);
            std::vector<char> moves(w.pos.size(), 0);
            for (int i = 0; i < static_cast<int>(out.mesh.verts.size()); ++i)
                if (out.meshPicked(i)) moves[static_cast<std::size_t>(w.group[static_cast<std::size_t>(i)])] = 1;
            std::vector<glm::vec3> p = w.pos, q(p.size());
            auto step = [&](float k) {
                for (std::size_t v = 0; v < p.size(); ++v) {
                    q[v] = p[v];
                    if (!moves[v] || w.rim[v] || w.next[v].empty()) continue;
                    glm::vec3 mid(0.0f);
                    for (int u : w.next[v]) mid += p[static_cast<std::size_t>(u)];
                    mid /= static_cast<float>(w.next[v].size());
                    q[v] = p[v] + k * (mid - p[v]);
                }
                p.swap(q);
            };
            for (int it = 0; it < its; ++it) {
                step(lam);
                if (keepSize) step(mu);
            }
            for (int i = 0; i < static_cast<int>(out.mesh.verts.size()); ++i)
                out.mesh.verts[static_cast<std::size_t>(i)] = p[static_cast<std::size_t>(w.group[static_cast<std::size_t>(i)])];
        }
        for (Curve& c : out.curves) {
            const int n = static_cast<int>(c.pts.size());
            if (n < 3 || c.loose) continue;
            for (int it = 0; it < its; ++it)
                for (float k : keepSize ? std::vector<float>{lam, mu} : std::vector<float>{lam}) {
                    std::vector<glm::vec3> s = c.pts;
                    for (int i = 0; i < n; ++i) {
                        if (!c.closed && (i == 0 || i == n - 1)) continue;
                        if (!Geo::curvePicked(c, i, out.hasSel)) continue;
                        const glm::vec3& a = c.pts[static_cast<std::size_t>((i - 1 + n) % n)];
                        const glm::vec3& b = c.pts[static_cast<std::size_t>((i + 1) % n)];
                        s[static_cast<std::size_t>(i)] += k * (0.5f * (a + b) - c.pts[static_cast<std::size_t>(i)]);
                    }
                    c.pts.swap(s);
                }
        }
        return "";
    }
};

// Where a stone split, it is flat: planes knocked off each piece at random,
// every point beyond one pushed back onto it, so a round lump comes out with
// the flat faces and sharp edges of broken rock. Each piece (a rock among a
// pile of them) gets cuts of its own. The fracture is not glass: Unevenness
// lets it wander. Flat base cuts the bottom off level, to stand on.
class FacetsNode : public proc::NodeOf<FacetsNode> {
public:
    int   cuts       = 8;
    float depth      = 0.3f;    // share of a piece's radius, the deepest a cut goes
    float unevenness = 0.04f;   // share of a piece's radius
    bool  flatBase   = true;
    float base       = 0.15f;   // share of a piece's height cut off the bottom
    int   seed       = 1;

    const char* typeId() const override { return "facets"; }
    const char* displayName() const override { return "Facets"; }
    int inputSlots() const override { return 1; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            std::vector<Property> v;
            v.push_back(whole("Cuts", "cuts", &FacetsNode::cuts, 0, 64));
            v.push_back(number("Depth", "depth", &FacetsNode::depth, 0.05f, 0.0f, 1.0f));
            v.push_back(number("Unevenness", "unevenness", &FacetsNode::unevenness, 0.01f, 0.0f, 0.5f));
            v.push_back(flag("Flat base", "flatBase", &FacetsNode::flatBase));
            Property b = number("Base cut", "base", &FacetsNode::base, 0.05f, 0.0f, 0.9f);
            b.visible = [](const void* o) { return static_cast<const FacetsNode*>(o)->flatBase; };
            v.push_back(std::move(b));
            v.push_back(whole("Seed", "seed", &FacetsNode::seed, 0, 99999));
            return v;
        }();
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        out = *in[0];
        if (out.mesh.verts.empty()) return "No faces to cut";
        const Welded w = weld(out.mesh, true);
        std::vector<char> moves(w.pos.size(), 0);
        for (int i = 0; i < static_cast<int>(out.mesh.verts.size()); ++i)
            if (out.meshPicked(i)) moves[static_cast<std::size_t>(w.group[static_cast<std::size_t>(i)])] = 1;
        std::vector<glm::vec3> p = w.pos;
        std::vector<std::vector<int>> pieces(static_cast<std::size_t>(w.pieces));
        for (std::size_t v = 0; v < p.size(); ++v) pieces[static_cast<std::size_t>(w.piece[v])].push_back(static_cast<int>(v));
        for (std::size_t k = 0; k < pieces.size(); ++k) {
            const std::vector<int>& pts = pieces[k];
            if (pts.size() < 4) continue;
            glm::vec3 c(0.0f), lo(1e30f), hi(-1e30f);
            for (int v : pts) {
                c += p[static_cast<std::size_t>(v)];
                lo = glm::min(lo, p[static_cast<std::size_t>(v)]);
                hi = glm::max(hi, p[static_cast<std::size_t>(v)]);
            }
            c /= static_cast<float>(pts.size());
            float r = 0.0f;
            for (int v : pts) r = std::max(r, glm::length(p[static_cast<std::size_t>(v)] - c));
            if (r < 1e-6f) continue;
            const int s = seed * 7919 + static_cast<int>(k) * 131;
            // A plane facing `d`, `into` metres in from the furthest point that
            // way; every point beyond it put back on it.
            auto cut = [&](const glm::vec3& d, float into, int salt) {
                float far = -1e30f;
                for (int v : pts) far = std::max(far, glm::dot(p[static_cast<std::size_t>(v)] - c, d));
                const float at = far - into;
                for (int v : pts) {
                    if (!moves[static_cast<std::size_t>(v)]) continue;
                    glm::vec3& q = p[static_cast<std::size_t>(v)];
                    const float wob = unevenness * r *
                                      proc::noise::perlin(q / (0.4f * r) + glm::vec3(13.7f * static_cast<float>(salt)), s + salt);
                    const float over = glm::dot(q - c, d) - (at + wob);
                    if (over > 0.0f) q -= d * over;
                }
            };
            if (flatBase) cut(glm::vec3(0.0f, -1.0f, 0.0f), std::clamp(base, 0.0f, 0.9f) * (hi.y - lo.y), 1);
            for (int i = 0; i < std::clamp(cuts, 0, 64); ++i) {
                // A direction over the whole sphere -- with a flat base, none
                // straight down, where the base already is.
                const float z = 2.0f * proc::random01(i * 3, s) - 1.0f;
                const float a = kTau * proc::random01(i * 3 + 1, s);
                const float q = std::sqrt(std::max(0.0f, 1.0f - z * z));
                glm::vec3 d(q * std::cos(a), z, q * std::sin(a));
                if (flatBase && d.y < -0.6f) d.y = -d.y;
                d = glm::normalize(d);
                const float into = depth * r * (0.35f + 0.65f * proc::random01(i * 3 + 2, s));
                cut(d, into, i + 2);
            }
        }
        for (int i = 0; i < static_cast<int>(out.mesh.verts.size()); ++i)
            out.mesh.verts[static_cast<std::size_t>(i)] = p[static_cast<std::size_t>(w.group[static_cast<std::size_t>(i)])];
        return "";
    }
};

// ================================================================================
// Look
// ================================================================================

// A rock's maps painted onto the faces (ProcRockTex.hpp): each face gets
// coordinates of its own, and the colour, normal and roughness maps of a
// material of the object's own are made to fit them -- on save, PNGs in the
// project's textures/procedural folder. Put it last, or nearly: a step that
// rebuilds faces after it (Subdivide, Extrude) loses their coordinates.
class RockTextureNode : public proc::NodeOf<RockTextureNode> {
public:
    int       rock       = 0;
    glm::vec3 tint{1.0f};
    float     grain      = 0.012f;
    float     bumps      = 1.0f;
    float     layers     = 0.5f;
    float     cracks     = 0.3f;
    float     wear       = 0.5f;
    float     dirt       = 0.5f;
    float     lichen     = 0.3f;
    float     moss       = 0.15f;
    int       resolution = 2;   // 256 << this
    int       seed       = 1;

    const char* typeId() const override { return "rocktexture"; }
    const char* displayName() const override { return "Rock texture"; }
    int inputSlots() const override { return 1; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            using R = RockTextureNode;
            std::vector<Property> v;
            v.push_back(choice("Rock", "rock", &R::rock, proc::rock::rockLabels()));
            v.push_back(field("Tint", "tint", PropKind::Color, &R::tint));
            v.push_back(number("Grain (m)", "grain", &R::grain, 0.005f, 0.002f, 1.0f, "%.3f"));
            v.push_back(number("Bumps", "bumps", &R::bumps, 0.1f, 0.0f, 10.0f));
            v.push_back(number("Layers", "layers", &R::layers, 0.05f, 0.0f, 1.0f));
            v.push_back(number("Cracks", "cracks", &R::cracks, 0.05f, 0.0f, 1.0f));
            v.push_back(number("Worn edges", "wear", &R::wear, 0.05f, 0.0f, 1.0f));
            v.push_back(number("Dirt", "dirt", &R::dirt, 0.05f, 0.0f, 1.0f));
            v.push_back(number("Lichen", "lichen", &R::lichen, 0.05f, 0.0f, 1.0f));
            v.push_back(number("Moss", "moss", &R::moss, 0.05f, 0.0f, 1.0f));
            v.push_back(choice("Texture size", "resolution", &R::resolution, {"256", "512", "1024", "2048"}));
            v.push_back(whole("Seed", "seed", &R::seed, 0, 99999));
            return v;
        }();
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        out = *in[0];
        if (out.mesh.faces.empty()) return "No faces to paint";
        proc::rock::Look l;
        l.rock = rock; l.tint = tint; l.grain = grain; l.bumps = bumps; l.layers = layers;
        l.cracks = cracks; l.wear = wear; l.dirt = dirt; l.lichen = lichen; l.moss = moss;
        l.size = 256 << std::clamp(resolution, 0, 3);
        l.seed = seed;
        proc::Bake b;
        b.key  = name.empty() ? std::string("rocktexture") : name;
        b.slot = proc::bakeSlot(b.key);
        b.maps = proc::rock::bake(out.mesh, l, &b.hash);
        if (!b.maps) return "No faces to paint";
        for (int f = 0; f < static_cast<int>(out.mesh.faces.size()); ++f)
            if (out.mesh.validFace(f)) out.mesh.setFaceMaterial(f, b.slot);
        out.bakes.erase(std::remove_if(out.bakes.begin(), out.bakes.end(),
                                       [&](const proc::Bake& o) { return o.slot == b.slot; }),
                        out.bakes.end());
        out.bakes.insert(out.bakes.begin(), std::move(b));
        return "";
    }
};

// ================================================================================
// Registration
// ================================================================================

struct RegisterRockNodes {
    template <class T>
    static void add(const char* category, const char* tip) {
        const T t;
        proc::registerType({t.typeId(), t.displayName(), category, tip,
                            [] { return std::unique_ptr<proc::Node>(std::make_unique<T>()); }});
    }
    RegisterRockNodes() {
        add<IcosphereNode>("Shapes", "A ball of even triangles, with no poles: what Noise,\n"
                                     "Facets and Smooth want to start from -- a rock, an asteroid.");
        add<NoiseNode>("Deform", "Push points by noise -- out along the surface, up, or every way:\n"
                                 "Perlin hills, ridged crests, billows, cells, cracks. Only the\n"
                                 "selected ones after a Select points (which can pick by noise too).");
        add<SmoothNode>("Deform", "Wear the shape down: points eased towards their neighbours,\n"
                                  "keeping the size if asked. Open edges stay put.");
        add<FacetsNode>("Deform", "Flat fracture faces knocked off at random, every piece its own:\n"
                                  "a round lump becomes broken stone. Flat base to stand on.");
        add<RockTextureNode>("Look", "A rock's colour, normal and roughness maps painted onto the\n"
                                     "faces: granite, sandstone, limestone, basalt or slate, with\n"
                                     "worn edges, dirt in the hollows, lichen and moss on top.");
    }
} g_registerRockNodes;

} // namespace
