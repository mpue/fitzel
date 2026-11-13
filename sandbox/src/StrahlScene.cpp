// What Strahl's objects mean: the kinds the language knows, the properties
// each accepts, and the translation of everything a script emitted into a
// pathtrace::Scene.
//
// Objects are recorded loosely while the script runs (an Obj is a kind and the
// properties it was given) and only interpreted here. That keeps the
// interpreter ignorant of what a sphere is, and it means a property can be
// given in any order, overridden by a later one, or added by a modifier block
// long after the object was first made -- `pillar { material: gold }` -- with
// the checks still pointing at the line that set it.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include <glm/gtc/matrix_transform.hpp>

#include <stb_image.h>
#include <tinyexr.h>

#include "StrahlInternal.hpp"

namespace strahl {
namespace {

constexpr double kPiD = 3.14159265358979323846;

enum class Cat { Shape, Csg, Group, Mesh, Material, Light, Sun, Sky, Fog, Camera, Render };

struct KindDef {
    const char* name;
    Cat         cat;
    std::vector<const char*> positional;
    std::vector<const char*> props;
};

// Every kind, what it takes by position, and every property it accepts. The
// transforms (translate, rotate, scale, matrix) are accepted by all of them
// and handled as they are set; `material` by every shape.
const std::vector<KindDef>& kinds() {
    static const std::vector<KindDef> k = {
        {"sphere",       Cat::Shape, {"center", "radius"}, {"center", "radius"}},
        {"box",          Cat::Shape, {"min", "max"}, {"min", "max", "size", "center"}},
        {"plane",        Cat::Shape, {"normal", "offset"}, {"normal", "offset", "point"}},
        {"disc",         Cat::Shape, {"center", "normal", "radius"}, {"center", "normal", "radius", "hole"}},
        {"cylinder",     Cat::Shape, {"from", "to", "radius"}, {"from", "to", "radius", "open"}},
        {"cone",         Cat::Shape, {"from", "to", "radius", "top_radius"},
                                     {"from", "to", "radius", "top_radius", "open"}},
        {"torus",        Cat::Shape, {"major", "minor"}, {"major", "minor"}},
        {"isosurface",   Cat::Shape, {"function"},
                                     {"function", "bound", "min", "max", "threshold", "max_gradient",
                                      "accuracy", "open"}},
        {"mesh",         Cat::Mesh,  {"vertices", "faces"}, {"vertices", "faces", "normals", "uvs", "smooth"}},
        {"union",        Cat::Csg,   {}, {}},
        {"merge",        Cat::Csg,   {}, {}},
        {"intersection", Cat::Csg,   {}, {}},
        {"difference",   Cat::Csg,   {}, {}},
        {"group",        Cat::Group, {}, {}},
        {"object",       Cat::Group, {}, {}},
        {"material",     Cat::Material, {"color"},
                                     {"color", "roughness", "metallic", "glass", "ior", "absorption",
                                      "medium_color", "emission", "strength", "translucency", "opacity",
                                      "bump", "bump_strength", "shadow", "additive"}},
        {"light",        Cat::Light, {"position", "color"},
                                     {"position", "color", "intensity", "radius", "range", "visible"}},
        {"spot",         Cat::Light, {"position", "look_at"},
                                     {"position", "look_at", "direction", "color", "intensity", "angle",
                                      "inner", "radius", "range", "visible"}},
        {"sun",          Cat::Sun,   {"direction", "color"}, {"direction", "color", "intensity", "angle"}},
        {"sky",          Cat::Sky,   {"zenith", "horizon", "ground"},
                                     {"zenith", "horizon", "ground", "intensity", "hdri", "rotate",
                                      "background", "procedural"}},
        {"fog",          Cat::Fog,   {"density", "color"}, {"density", "color", "height", "falloff", "sun_color"}},
        {"camera",       Cat::Camera, {"position", "look_at"},
                                     {"position", "look_at", "direction", "up", "fov", "aperture", "focus",
                                      "roll"}},
        {"render",       Cat::Render, {},
                                     {"width", "height", "samples", "bounces", "batch", "seed", "threads",
                                      "clamp", "exposure", "tonemap", "saturation", "contrast", "warmth",
                                      "hue", "vignette", "output"}},
    };
    return k;
}

const KindDef* kindDef(const std::string& name) {
    for (const KindDef& k : kinds())
        if (name == k.name) return &k;
    return nullptr;
}

bool isTransform(const std::string& n) {
    return n == "translate" || n == "rotate" || n == "scale" || n == "matrix" || n == "transform";
}

bool takesMaterial(Cat c) { return c == Cat::Shape || c == Cat::Csg || c == Cat::Group || c == Cat::Mesh; }

int editDistance(const std::string& a, const std::string& b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<int>(j);
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = static_cast<int>(i);
        for (std::size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] != b[j - 1])});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

// The nearest of `from` to a misspelt name, or "" when nothing is close.
std::string closest(const std::string& name, const std::vector<std::string>& from) {
    std::string best;
    int bestD = 3;
    for (const std::string& c : from) {
        const int d = editDistance(name, c);
        if (d < bestD) { bestD = d; best = c; }
    }
    return best;
}

std::string suggest(const std::string& name, const std::vector<std::string>& from) {
    const std::string best = closest(name, from);
    return best.empty() ? std::string() : " -- did you mean '" + best + "'?";
}

glm::dmat4 rotationXYZ(const glm::dvec3& deg) {
    const glm::dvec3 r = glm::radians(deg);
    return glm::rotate(glm::dmat4(1.0), r.z, glm::dvec3(0, 0, 1)) *
           glm::rotate(glm::dmat4(1.0), r.y, glm::dvec3(0, 1, 0)) *
           glm::rotate(glm::dmat4(1.0), r.x, glm::dvec3(1, 0, 0));
}

// The rotation taking +y onto `n`.
glm::dmat4 alignY(glm::dvec3 n) {
    const double l = glm::length(n);
    if (l < 1e-12) return glm::dmat4(1.0);
    n /= l;
    const glm::dvec3 y(0, 1, 0);
    const double c = glm::dot(y, n);
    if (c > 1.0 - 1e-12) return glm::dmat4(1.0);
    if (c < -1.0 + 1e-12) return glm::rotate(glm::dmat4(1.0), kPiD, glm::dvec3(1, 0, 0));
    return glm::rotate(glm::dmat4(1.0), std::acos(std::clamp(c, -1.0, 1.0)),
                       glm::normalize(glm::cross(y, n)));
}

glm::vec3 linear(const glm::dvec3& srgb) {
    return glm::vec3(glm::pow(glm::max(srgb, glm::dvec3(0.0)), glm::dvec3(2.2)));
}

} // namespace

std::vector<std::string> objectKinds() {
    std::vector<std::string> out;
    for (const KindDef& k : kinds()) out.push_back(k.name);
    return out;
}

std::vector<std::string> propertyNames() {
    std::vector<std::string> out = {"material", "translate", "rotate", "scale", "matrix", "transform"};
    for (const KindDef& k : kinds())
        for (const char* p : k.props)
            if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    return out;
}

std::vector<KindSchema> kindSchemas() {
    std::vector<KindSchema> out;
    for (const KindDef& k : kinds()) {
        KindSchema s;
        s.name = k.name;
        s.positional.assign(k.positional.begin(), k.positional.end());
        s.props.assign(k.props.begin(), k.props.end());
        s.shape = takesMaterial(k.cat);
        out.push_back(std::move(s));
    }
    return out;
}

bool Interp::isShapeKind(const std::string& kind) {
    const KindDef* d = kindDef(kind);
    return d && (d->cat == Cat::Shape || d->cat == Cat::Csg || d->cat == Cat::Group || d->cat == Cat::Mesh);
}

void Interp::setProp(Obj& o, const std::string& name, const Value& v, const Loc& loc) {
    if (isTransform(name)) {
        glm::dmat4 m(1.0);
        if (name == "translate") {
            if (!v.isNumeric()) throw Error(loc, "translate: wants a vector, like [0, 1, 0]");
            m = glm::translate(glm::dmat4(1.0), v.vec3());
        } else if (name == "rotate") {
            if (!v.isNumeric()) throw Error(loc, "rotate: wants degrees about x, y and z, like [0, 45, 0]");
            if (v.t == T::Num) throw Error(loc, "rotate: wants three angles -- [0, " + describe(v) + ", 0] turns about y");
            m = rotationXYZ(v.vec3());
        } else if (name == "scale") {
            if (!v.isNumeric()) throw Error(loc, "scale: wants a number or a vector");
            const glm::dvec3 s = v.vec3();
            if (std::fabs(s.x * s.y * s.z) < 1e-30) throw Error(loc, "scale: of zero flattens the object to nothing");
            m = glm::scale(glm::dmat4(1.0), s);
        } else {
            if (v.t != T::Mat) throw Error(loc, name + ": wants a matrix (translation(), rotation() ...)");
            m = v.matrix();
        }
        o.xf = m * o.xf;
        return;
    }
    o.props.push_back({name, v, loc});
}

void Interp::finishObj(Obj& o) {
    const KindDef* d = kindDef(o.kind);
    if (!d) throw Error(o.loc, "unknown kind of object '" + o.kind + "'");
    // A material among a shape's unnamed values is its material, wherever it
    // stands -- sphere { [0, 1, 0], 0.5, gold }, as POV-Ray would have it.
    if (takesMaterial(d->cat))
        for (auto it = o.positional.begin(); it != o.positional.end();) {
            if (it->t == T::Obj && it->objRef().kind == "material") {
                o.props.push_back({"material", *it, o.loc});
                it = o.positional.erase(it);
            } else {
                ++it;
            }
        }
    // ...and among a material's, it is what the material starts from --
    // material { gold, roughness: 0.3 }, material { matte(checker()) } -- as
    // POV-Ray's texture { T_Stone ... } is: its properties first, so whatever
    // the block says wins, and its pattern transforms before the block's.
    std::size_t based = 0;
    if (d->cat == Cat::Material)
        for (auto it = o.positional.begin(); it != o.positional.end();) {
            if (it->t == T::Obj && it->objRef().kind == "material") {
                const Obj& base = it->objRef();
                o.props.insert(o.props.begin() + static_cast<std::ptrdiff_t>(based), base.props.begin(),
                               base.props.end());
                based += base.props.size();
                o.xf = o.xf * base.xf;
                it = o.positional.erase(it);
            } else {
                ++it;
            }
        }
    if (!o.positional.empty()) {
        if (o.positional.size() > d->positional.size()) {
            std::string names;
            for (const char* p : d->positional) names += (names.empty() ? "" : ", ") + std::string(p);
            if (d->positional.empty())
                throw Error(o.loc, "a " + o.kind + " takes no values without a name; " +
                                       describe(o.positional[0]) + " needs one (name: value)");
            throw Error(o.loc, "a " + o.kind + " takes at most " + std::to_string(d->positional.size()) +
                                   " values without a name (" + names + "), this one has " +
                                   std::to_string(o.positional.size()));
        }
        // Before the named ones, so a name given explicitly still wins (and
        // after a material started from, which they change).
        std::vector<Prop> pos;
        for (std::size_t i = 0; i < o.positional.size(); ++i)
            pos.push_back({d->positional[i], o.positional[i], o.loc});
        o.props.insert(o.props.begin() + static_cast<std::ptrdiff_t>(based), pos.begin(), pos.end());
        o.positional.clear();
    }
    std::vector<std::string> allowed(d->props.begin(), d->props.end());
    if (takesMaterial(d->cat)) allowed.push_back("material");
    for (const Prop& p : o.props) {
        if (std::find(allowed.begin(), allowed.end(), p.name) != allowed.end()) continue;
        if (takesMaterial(d->cat)) {
            // color: on a sphere is the commonest slip of all: a surface's
            // looks belong to its material.
            const KindDef* m = kindDef("material");
            std::vector<std::string> mat(m->props.begin(), m->props.end());
            const std::string near = std::find(mat.begin(), mat.end(), p.name) != mat.end()
                                   ? p.name : closest(p.name, mat);
            if (!near.empty())
                throw Error(p.loc, "a " + o.kind + " has no property '" + p.name + "' -- how a surface "
                                   "looks belongs to its material: material: plastic(red), or "
                                   "material: material { " + near +
                                   ": ... }");
        }
        throw Error(p.loc, "a " + o.kind + " has no property '" + p.name + "'" + suggest(p.name, allowed));
    }
    if (!o.children.empty() && d->cat != Cat::Csg && d->cat != Cat::Group)
        throw Error(o.children.front().t == T::Obj ? o.children.front().objRef().loc : o.loc,
                    "a " + o.kind + " cannot contain other objects -- only union, merge, "
                    "intersection, difference and group can");
}

Value loadImage(Interp& in, const std::string& path, const Loc& loc) {
    const std::filesystem::path p = in.resolve(path, loc);
    const std::string key = p.string();
    auto it = in.images.find(key);
    if (it == in.images.end()) {
        int w = 0, h = 0, n = 0;
        unsigned char* px = stbi_load(key.c_str(), &w, &h, &n, 4);
        if (!px) throw Error(loc, "cannot read the image '" + path + "'");
        auto img = std::make_shared<pathtrace::Image>();
        img->width  = w;
        img->height = h;
        img->pixels.assign(px, px + static_cast<std::size_t>(w) * h * 4);
        stbi_image_free(px);
        it = in.images.emplace(key, img).first;
    }
    Value v;
    v.t = T::Image;
    v.p = it->second;
    return v;
}

namespace {

// --- The scene builder -------------------------------------------------------

class Builder {
public:
    Builder(Interp& in, Result& out) : m_in(in), m_out(out) {
        m_out.scene = std::make_shared<pathtrace::Scene>();
        m_shapes = std::make_shared<pathtrace::Shapes>();
        pathtrace::Scene& sc = *m_out.scene;
        sc.sun.enabled = false;   // only when the script asks for one
        sc.grade = pathtrace::Grade{};
        sc.exposure = 1.0f;
        // A light grey plastic, for shapes given no material at all.
        pathtrace::Material def;
        def.albedo = glm::vec3(0.75f);
        def.roughness = 0.45f;
        sc.materials.push_back(def);
        m_out.settings = pathtrace::Settings{};
        m_out.settings.width = 1280;
        m_out.settings.height = 720;
        m_out.settings.samples = 128;
        m_out.settings.maxBounces = 8;
    }

    void run() {
        const glm::dmat4 I(1.0);
        for (const Value& v : m_in.scene) item(v, I, 0, I);
        finishCamera();
        pathtrace::Scene& sc = *m_out.scene;
        if (!m_shapes->empty()) {
            m_shapes->finalize();
            sc.shapes = m_shapes;
        }
        m_out.shapes = static_cast<int>(m_shapes->nodes.size());
        m_out.meshes = static_cast<int>(sc.meshes.size());
        m_out.materials = static_cast<int>(sc.materials.size());
        m_out.lights = static_cast<int>(sc.lamps.size()) + (sc.sun.enabled ? 1 : 0);
        m_out.triangles = sc.triangleCount();
        m_out.functions = m_in.compiled;
    }

private:
    Interp&  m_in;
    Result&  m_out;
    std::shared_ptr<pathtrace::Shapes> m_shapes;
    std::unordered_map<const Obj*, int> m_materials;
    bool m_camera = false, m_sun = false, m_sky = false, m_render = false;
    const Obj* m_cameraObj = nullptr;
    glm::dmat4 m_cameraXf{1.0};

    // --- Property access, with the error pointing at the property --------
    const Prop* prop(const Obj& o, const char* name) const { return o.find(name); }

    double num(const Obj& o, const char* name, double def) const {
        const Prop* p = o.find(name);
        if (!p) return def;
        if (p->value.t != T::Num && p->value.t != T::Bool)
            throw Error(p->loc, std::string(name) + ": should be a number, not " + typeName(p->value.t));
        return p->value.v[0];
    }
    bool flag(const Obj& o, const char* name, bool def) const {
        const Prop* p = o.find(name);
        if (!p) return def;
        if (p->value.t != T::Bool && p->value.t != T::Num)
            throw Error(p->loc, std::string(name) + ": should be true or false");
        return p->value.truthy();
    }
    glm::dvec3 vec(const Obj& o, const char* name, const glm::dvec3& def) const {
        const Prop* p = o.find(name);
        if (!p) return def;
        if (p->value.t == T::Num) return glm::dvec3(p->value.v[0]);
        if (p->value.t != T::Vec || p->value.n < 3)
            throw Error(p->loc, std::string(name) + ": should be a vector of three, not " +
                                    m_in.describe(p->value));
        return p->value.vec3();
    }
    std::string str(const Obj& o, const char* name, const std::string& def) const {
        const Prop* p = o.find(name);
        if (!p) return def;
        if (p->value.t != T::Str) throw Error(p->loc, std::string(name) + ": should be a string");
        return p->value.string();
    }
    double positive(const Obj& o, const char* name, double def) const {
        const double v = num(o, name, def);
        if (!(v > 0.0)) throw Error(o.find(name) ? o.find(name)->loc : o.loc,
                                    std::string(name) + ": must be greater than zero");
        return v;
    }

    // --- Items -------------------------------------------------------------
    void item(const Value& v, const glm::dmat4& parent, int mat, const glm::dmat4& patternToWorld) {
        if (v.t == T::List) {
            for (const Value& e : v.listRef()) item(e, parent, mat, patternToWorld);
            return;
        }
        if (v.t != T::Obj) return;
        const Obj& o = v.objRef();
        const KindDef* d = kindDef(o.kind);
        if (!d) throw Error(o.loc, "unknown object '" + o.kind + "'");
        const glm::dmat4 world = parent * o.xf;
        switch (d->cat) {
        case Cat::Group:
        case Cat::Csg:
            if (d->cat == Cat::Group || o.kind == "union") {
                // A group at this level is only a way of moving several things
                // together: its members go into the scene one by one, which is
                // what lets the accelerator tell them apart.
                int m = mat;
                glm::dmat4 pw = patternToWorld;
                materialOf(o, world, m, pw);
                for (const Value& c : o.children) item(c, world, m, pw);
                return;
            }
            [[fallthrough]];
        case Cat::Shape: {
            const int node = shape(v, parent, mat, patternToWorld);
            if (node >= 0) m_shapes->roots.push_back(node);
            return;
        }
        case Cat::Mesh:     mesh(o, world, mat); return;
        case Cat::Light:    light(o, world); return;
        case Cat::Sun:      sun(o, world); return;
        case Cat::Sky:      sky(o); return;
        case Cat::Fog:      fog(o); return;
        case Cat::Camera:
            if (m_camera) throw Error(o.loc, "a second camera -- a scene has one");
            m_camera = true;
            m_cameraObj = &o;
            m_cameraXf = world;
            return;
        case Cat::Render:   render(o); return;
        case Cat::Material:
            throw Error(o.loc, "a material on its own does nothing -- give it to a shape (material: ...)");
        }
    }

    // The material an object carries, if it carries one: sets `mat` and the
    // space its patterns live in (the object's own, before its transforms --
    // and moved by the material's own transforms, if it has any).
    void materialOf(const Obj& o, const glm::dmat4& world, int& mat, glm::dmat4& patternToWorld) {
        const Prop* p = o.find("material");
        if (!p) return;
        glm::dmat4 own(1.0);
        mat = material(p->value, p->loc, own);
        patternToWorld = world * own;
    }

    // --- Shapes ------------------------------------------------------------
    // One shape (or CSG) as a node; -1 when it turned out to be nothing (a
    // light inside a CSG is taken out and placed as a light).
    int shape(const Value& v, const glm::dmat4& parent, int mat, glm::dmat4 patternToWorld) {
        const Obj& o = v.objRef();
        const KindDef* d = kindDef(o.kind);
        const glm::dmat4 world = parent * o.xf;
        if (d->cat == Cat::Light) { light(o, world); return -1; }
        if (d->cat == Cat::Mesh)
            throw Error(o.loc, "a mesh has no inside, so it cannot be part of a union, merge, "
                               "intersection or difference -- put it in a group");
        if (d->cat != Cat::Shape && d->cat != Cat::Csg && d->cat != Cat::Group)
            throw Error(o.loc, "a " + o.kind + " cannot be part of a shape");
        materialOf(o, world, mat, patternToWorld);

        using K = pathtrace::ShapeNode::Kind;
        if (d->cat == Cat::Csg || d->cat == Cat::Group) {
            std::vector<int> kids;
            for (const Value& c : o.children) {
                if (c.t != T::Obj) continue;
                const int k = shape(c, world, mat, patternToWorld);
                if (k >= 0) kids.push_back(k);
            }
            const std::string& kind = o.kind;
            if (kind == "difference" && kids.empty())
                throw Error(o.loc, "a difference needs something to cut from");
            if (kind == "intersection" && kids.empty())
                throw Error(o.loc, "an intersection needs at least one shape");
            if (kids.empty()) return -1;
            if (kids.size() == 1 && kind != "difference") return kids[0];
            pathtrace::ShapeNode n;
            n.kind = kind == "merge" ? K::Merge : kind == "intersection" ? K::Intersection
                   : kind == "difference" ? K::Difference : K::Union;
            n.first = static_cast<int>(m_shapes->childIndex.size());
            n.count = static_cast<int>(kids.size());
            m_shapes->childIndex.insert(m_shapes->childIndex.end(), kids.begin(), kids.end());
            m_shapes->nodes.push_back(n);
            return static_cast<int>(m_shapes->nodes.size() - 1);
        }

        pathtrace::ShapeNode n;
        n.material = mat;
        glm::dmat4 canon(1.0);   // canonical primitive -> the object's space
        if (o.kind == "sphere") {
            n.kind = K::Sphere;
            const double r = positive(o, "radius", 1.0);
            canon = glm::translate(glm::dmat4(1.0), vec(o, "center", glm::dvec3(0.0))) *
                    glm::scale(glm::dmat4(1.0), glm::dvec3(r));
        } else if (o.kind == "box") {
            n.kind = K::Box;
            glm::dvec3 lo, hi;
            if (o.find("min") || o.find("max")) {
                if (o.find("size")) throw Error(o.find("size")->loc, "a box has either min/max or size, not both");
                lo = vec(o, "min", glm::dvec3(-0.5));
                hi = vec(o, "max", glm::dvec3(0.5));
            } else {
                const glm::dvec3 s = vec(o, "size", glm::dvec3(1.0)), c = vec(o, "center", glm::dvec3(0.0));
                lo = c - s * 0.5;
                hi = c + s * 0.5;
            }
            const glm::dvec3 a = glm::min(lo, hi), b = glm::max(lo, hi);
            const glm::dvec3 half = (b - a) * 0.5;
            if (half.x <= 0.0 || half.y <= 0.0 || half.z <= 0.0)
                throw Error(o.loc, "this box has no volume (a side of length zero)");
            canon = glm::translate(glm::dmat4(1.0), (a + b) * 0.5) * glm::scale(glm::dmat4(1.0), half);
        } else if (o.kind == "plane") {
            n.kind = K::Plane;
            const glm::dvec3 nn = vec(o, "normal", glm::dvec3(0, 1, 0));
            if (glm::length(nn) < 1e-12) throw Error(o.loc, "a plane needs a normal that is not zero");
            const glm::dvec3 un = glm::normalize(nn);
            double off = num(o, "offset", 0.0);
            if (o.find("point")) off = glm::dot(vec(o, "point", glm::dvec3(0.0)), un);
            canon = glm::translate(glm::dmat4(1.0), un * off) * alignY(un);
        } else if (o.kind == "disc") {
            n.kind = K::Disc;
            const double r = positive(o, "radius", 1.0);
            n.p0 = static_cast<float>(std::max(0.0, num(o, "hole", 0.0)) / r);
            canon = glm::translate(glm::dmat4(1.0), vec(o, "center", glm::dvec3(0.0))) *
                    alignY(vec(o, "normal", glm::dvec3(0, 1, 0))) *
                    glm::scale(glm::dmat4(1.0), glm::dvec3(r, 1.0, r));
        } else if (o.kind == "cylinder" || o.kind == "cone") {
            glm::dvec3 a = vec(o, "from", glm::dvec3(0.0)), b = vec(o, "to", glm::dvec3(0, 1, 0));
            double r0 = num(o, "radius", o.kind == "cone" ? 1.0 : 0.5);
            double r1 = o.kind == "cone" ? num(o, "top_radius", 0.0) : r0;
            if (r0 < 0.0 || r1 < 0.0) throw Error(o.loc, "a radius cannot be negative");
            // The canonical cone has its wide end at the base.
            if (r0 < r1) { std::swap(a, b); std::swap(r0, r1); }
            if (!(r0 > 0.0)) throw Error(o.loc, "a " + o.kind + " needs a radius greater than zero");
            const glm::dvec3 axis = b - a;
            const double len = glm::length(axis);
            if (len < 1e-12) throw Error(o.loc, "a " + o.kind + " needs its two ends apart");
            n.kind = o.kind == "cone" ? K::Cone : K::Cylinder;
            n.p0 = static_cast<float>(r1 / r0);
            n.open = flag(o, "open", false);
            canon = glm::translate(glm::dmat4(1.0), a) * alignY(axis) *
                    glm::scale(glm::dmat4(1.0), glm::dvec3(r0, len, r0));
        } else if (o.kind == "torus") {
            n.kind = K::Torus;
            n.p0 = static_cast<float>(positive(o, "major", 1.0));
            n.p1 = static_cast<float>(positive(o, "minor", 0.25));
        } else if (o.kind == "isosurface") {
            isosurface(o, n);
        } else {
            throw Error(o.loc, "a " + o.kind + " is not a shape");
        }

        const glm::dmat4 localToWorld = world * canon;
        if (std::fabs(glm::determinant(glm::dmat3(localToWorld))) < 1e-30)
            throw Error(o.loc, "this " + o.kind + " has been scaled to nothing");
        const glm::dmat4 toLocal = glm::inverse(localToWorld);
        n.toLocal = glm::mat4(toLocal);
        n.normalToWorld = glm::mat3(glm::transpose(glm::dmat3(toLocal)));
        n.toPattern = glm::mat4(glm::inverse(patternToWorld));
        m_shapes->nodes.push_back(n);
        return static_cast<int>(m_shapes->nodes.size() - 1);
    }

    void isosurface(const Obj& o, pathtrace::ShapeNode& n) {
        n.kind = pathtrace::ShapeNode::Kind::Iso;
        const Prop* f = o.find("function");
        if (!f) throw Error(o.loc, "an isosurface needs a function: fn(p) => ...");
        if (f->value.t != T::Func) throw Error(f->loc, "function: should be a function of the point, fn(p) => ...");
        n.func = compile(m_in, f->value, f->loc);
        if (n.func->resultWidth != 1)
            throw Error(f->loc, "an isosurface's function must return a number, not a vector");
        glm::dvec3 lo(-1.0), hi(1.0);
        if (const Prop* b = o.find("bound")) {
            if (!b->value.isNumeric()) throw Error(b->loc, "bound: a radius or the half-size [x, y, z]");
            const glm::dvec3 h = glm::abs(b->value.vec3());
            lo = -h;
            hi = h;
        }
        if (o.find("min")) lo = vec(o, "min", lo);
        if (o.find("max")) hi = vec(o, "max", hi);
        if (!(hi.x > lo.x && hi.y > lo.y && hi.z > lo.z))
            throw Error(o.loc, "an isosurface's bounds must have volume");
        n.boxLo = glm::vec3(lo);
        n.boxHi = glm::vec3(hi);
        n.p0 = static_cast<float>(num(o, "threshold", 0.0));
        const double size = glm::length(hi - lo);
        n.p2 = static_cast<float>(num(o, "accuracy", std::max(1e-5, size * 2e-4)));
        n.open = flag(o, "open", false);
        if (o.find("max_gradient")) {
            n.p1 = static_cast<float>(positive(o, "max_gradient", 1.0));
        } else {
            // Not given: measured, over a few thousand points in its box. What
            // has to be right is the steepness on the way to the surface: too
            // small a value steps over thin parts and leaves holes. The very
            // steepest point of the whole box is a poor guide to that -- a
            // field built with atan2 or a twist has a singular axis far from
            // any surface, and its gradient there would make every ray crawl.
            // So: the steepest point near the surface, or (with a margin) what
            // all but the steepest twentieth of the box reaches, whichever is
            // more.
            const pathtrace::Func& F = *n.func;
            const float h = static_cast<float>(size * 1e-3);
            const float band = static_cast<float>(size * 0.05);
            float nearMost = 0.0f;
            std::vector<float> all;
            all.reserve(4096);
            for (int i = 0; i < 4096; ++i) {
                const glm::vec3 u(pathtrace::fn::hash(glm::vec3(i, 1, 0)),
                                  pathtrace::fn::hash(glm::vec3(i, 2, 0)),
                                  pathtrace::fn::hash(glm::vec3(i, 3, 0)));
                const glm::vec3 p = n.boxLo + u * (n.boxHi - n.boxLo);
                const glm::vec3 g((F.scalar(p + glm::vec3(h, 0, 0)) - F.scalar(p - glm::vec3(h, 0, 0))),
                                  (F.scalar(p + glm::vec3(0, h, 0)) - F.scalar(p - glm::vec3(0, h, 0))),
                                  (F.scalar(p + glm::vec3(0, 0, h)) - F.scalar(p - glm::vec3(0, 0, h))));
                const float l = glm::length(g) / (2.0f * h);
                if (!(l == l) || l > 1e12f) continue;
                all.push_back(l);
                if (std::fabs(F.scalar(p) - n.p0) < band) nearMost = std::max(nearMost, l);
            }
            float most = 0.0f;
            if (!all.empty()) {
                std::sort(all.begin(), all.end());
                most = all[all.size() * 95 / 100] * 1.5f;
            }
            most = std::max(most, nearMost * 1.25f);
            n.p1 = std::max(0.05f, most);
            char line[160];
            std::snprintf(line, sizeof line, "%s: isosurface max_gradient measured as %.3g "
                          "(give max_gradient: to set it)", m_in.where(o.loc).c_str(), n.p1);
            m_in.log.push_back(line);
        }
    }

    // --- Materials ---------------------------------------------------------
    // A material value: a material object, or a colour (a plain plastic of
    // that colour), or a pattern function (the same, coloured by it).
    int material(const Value& v, const Loc& loc, glm::dmat4& own) {
        pathtrace::Scene& sc = *m_out.scene;
        if (v.t == T::Vec || v.t == T::Num || v.t == T::Func) {
            pathtrace::Material m;
            m.roughness = 0.45f;
            if (v.t == T::Func) m.colorFn = compile(m_in, v, loc);
            else m.albedo = glm::vec3(v.vec3());
            sc.materials.push_back(m);
            return static_cast<int>(sc.materials.size() - 1);
        }
        if (v.t != T::Obj || v.objRef().kind != "material")
            throw Error(loc, "material: should be a material, a colour or a pattern, not " + m_in.describe(v));
        const Obj& o = v.objRef();
        own = o.xf;
        auto it = m_materials.find(&o);
        if (it != m_materials.end()) return it->second;

        pathtrace::Material m;
        m.roughness = 0.45f;
        // A channel that is a number or a colour, or a function per ray.
        const auto channel = [&](const char* name, std::shared_ptr<const pathtrace::Func>& fn) -> const Prop* {
            const Prop* p = o.find(name);
            if (p && p->value.t == T::Func) {
                fn = compile(m_in, p->value, p->loc);
                return nullptr;
            }
            return p;
        };
        // A colour slot holding something else, said with what it holds: a
        // material there is the likely one (light { color: gold }).
        const auto notColour = [&](const Prop& p) {
            const std::string got = p.value.t == T::Obj ? "a " + p.value.objRef().kind
                                                        : std::string("a ") + typeName(p.value.t);
            return Error(p.loc, p.name + ": should be a colour or a pattern, not " + got);
        };
        if (const Prop* p = channel("color", m.colorFn)) {
            if (!p->value.isNumeric()) throw notColour(*p);
            m.albedo = glm::vec3(p->value.vec3());
        }
        const auto colourFn = [&](const char* name, const std::shared_ptr<const pathtrace::Func>& f) {
            if (f && f->resultWidth == 2)
                throw Error(o.find(name)->loc, std::string(name) +
                                                   ": the function returns two components; a colour has three");
        };
        colourFn("color", m.colorFn);
        if (channel("roughness", m.roughnessFn)) m.roughness = static_cast<float>(num(o, "roughness", 0.45));
        if (channel("metallic", m.reflectivityFn)) m.reflectivity = static_cast<float>(num(o, "metallic", 0.0));
        if (const Prop* p = channel("emission", m.emissionFn)) {
            if (!p->value.isNumeric()) throw notColour(*p);
            m.emission = glm::vec3(p->value.vec3());
        }
        colourFn("emission", m.emissionFn);
        if (m.emissionFn && !o.find("strength")) m.emissionStrength = 1.0f;
        if (o.find("emission")) m.emissionStrength = static_cast<float>(num(o, "strength", 1.0));
        m.glass = flag(o, "glass", false);
        m.ior = static_cast<float>(num(o, "ior", 1.5));
        if (o.find("glass") == nullptr && o.find("ior")) m.glass = true;
        if (o.find("absorption")) m.absorption = glm::vec3(vec(o, "absorption", glm::dvec3(0.0)));
        if (o.find("medium_color")) m.mediumColor = glm::vec3(vec(o, "medium_color", glm::dvec3(0.0)));
        m.translucency = static_cast<float>(std::clamp(num(o, "translucency", 0.0), 0.0, 1.0));
        m.opacity = static_cast<float>(std::clamp(num(o, "opacity", 1.0), 0.0, 1.0));
        if (m.opacity < 1.0f) m.alphaMode = 2;
        if (const Prop* b = o.find("bump")) {
            if (b->value.t != T::Func) throw Error(b->loc, "bump: should be a height function, fn(p) => ...");
            m.bumpFn = compile(m_in, b->value, b->loc);
            if (m.bumpFn->resultWidth != 1) throw Error(b->loc, "bump: the function must return a number (a height)");
            m.bumpStrength = static_cast<float>(num(o, "bump_strength", 1.0));
        }
        m.castsShadow = flag(o, "shadow", true);
        m.additive = flag(o, "additive", false);
        if (m.glass && m.colorFn)
            m_in.log.push_back(m_in.where(o.loc) + ": a glass colour pattern tints what is seen through "
                                                   "it, but its shadow uses the plain colour");
        sc.materials.push_back(m);
        const int idx = static_cast<int>(sc.materials.size() - 1);
        m_materials[&o] = idx;
        return idx;
    }

    // --- Meshes --------------------------------------------------------------
    void mesh(const Obj& o, const glm::dmat4& world, int mat) {
        int m = mat;
        glm::dmat4 pw(1.0);
        materialOf(o, world, m, pw);
        const Prop* vp = o.find("vertices");
        const Prop* fp = o.find("faces");
        if (!vp || vp->value.t != T::List) throw Error(o.loc, "a mesh needs vertices: [[x, y, z], ...]");
        if (!fp || fp->value.t != T::List) throw Error(o.loc, "a mesh needs faces: [[0, 1, 2], ...]");
        const List& vs = vp->value.listRef();
        std::vector<glm::vec3> pos(vs.size());
        for (std::size_t i = 0; i < vs.size(); ++i) {
            if (vs[i].t != T::Vec || vs[i].n != 3) throw Error(vp->loc, "vertex " + std::to_string(i) + " is not [x, y, z]");
            pos[i] = glm::vec3(vs[i].vec3());
        }
        std::vector<glm::vec3> nrm;
        if (const Prop* np = o.find("normals")) {
            if (np->value.t != T::List || np->value.listRef().size() != vs.size())
                throw Error(np->loc, "normals: one per vertex");
            for (const Value& n : np->value.listRef()) nrm.push_back(glm::normalize(glm::vec3(needVec(n, np->loc))));
        }
        std::vector<glm::vec2> uvs;
        if (const Prop* up = o.find("uvs")) {
            if (up->value.t != T::List || up->value.listRef().size() != vs.size())
                throw Error(up->loc, "uvs: one per vertex");
            for (const Value& u : up->value.listRef()) {
                if (u.t != T::Vec) throw Error(up->loc, "uvs: [u, v] per vertex");
                uvs.emplace_back(static_cast<float>(u.v[0]), static_cast<float>(u.v[1]));
            }
        }
        // Triangles from the faces (a polygon as a fan), indices checked.
        std::vector<glm::ivec3> tris;
        for (const Value& f : fp->value.listRef()) {
            std::vector<int> idx;
            if (f.t == T::Vec) for (int k = 0; k < f.n; ++k) idx.push_back(static_cast<int>(f.v[k]));
            else if (f.t == T::List) for (const Value& e : f.listRef()) idx.push_back(static_cast<int>(e.v[0]));
            else throw Error(fp->loc, "a face is a list of vertex indices");
            if (idx.size() < 3) throw Error(fp->loc, "a face needs at least three corners");
            for (int k : idx)
                if (k < 0 || k >= static_cast<int>(pos.size()))
                    throw Error(fp->loc, "a face uses vertex " + std::to_string(k) + ", and there are " +
                                             std::to_string(pos.size()));
            for (std::size_t k = 1; k + 1 < idx.size(); ++k) tris.emplace_back(idx[0], idx[k], idx[k + 1]);
        }
        if (nrm.empty() && flag(o, "smooth", false)) {
            // Smooth: each vertex's normal the area-weighted average of its faces'.
            nrm.assign(pos.size(), glm::vec3(0.0f));
            for (const glm::ivec3& t : tris) {
                const glm::vec3 fn = glm::cross(pos[t.y] - pos[t.x], pos[t.z] - pos[t.x]);
                nrm[t.x] += fn; nrm[t.y] += fn; nrm[t.z] += fn;
            }
            for (glm::vec3& n : nrm) { const float l = glm::length(n); n = l > 0 ? n / l : glm::vec3(0, 1, 0); }
        }
        pathtrace::Mesh mesh;
        mesh.triangles.reserve(tris.size());
        for (const glm::ivec3& t : tris) {
            pathtrace::Triangle tr;
            tr.p0 = pos[t.x]; tr.p1 = pos[t.y]; tr.p2 = pos[t.z];
            const glm::vec3 fn = glm::cross(tr.p1 - tr.p0, tr.p2 - tr.p0);
            const float fl = glm::length(fn);
            const glm::vec3 flat = fl > 0 ? fn / fl : glm::vec3(0, 1, 0);
            tr.n0 = nrm.empty() ? flat : nrm[t.x];
            tr.n1 = nrm.empty() ? flat : nrm[t.y];
            tr.n2 = nrm.empty() ? flat : nrm[t.z];
            if (!uvs.empty()) { tr.uv0 = uvs[t.x]; tr.uv1 = uvs[t.y]; tr.uv2 = uvs[t.z]; }
            tr.material = m;
            mesh.triangles.push_back(tr);
        }
        pathtrace::Scene& sc = *m_out.scene;
        sc.meshes.push_back(std::move(mesh));
        pathtrace::Instance in;
        in.transform = glm::mat4(world);
        in.mesh = static_cast<int>(sc.meshes.size() - 1);
        sc.instances.push_back(in);
    }

    glm::dvec3 needVec(const Value& v, const Loc& loc) const {
        if (v.t != T::Vec || v.n < 3) throw Error(loc, "expected [x, y, z], found " + m_in.describe(v));
        return v.vec3();
    }

    // --- Lights ------------------------------------------------------------
    void light(const Obj& o, const glm::dmat4& world) {
        pathtrace::Scene& sc = *m_out.scene;
        pathtrace::Lamp l;
        const glm::dvec3 p = glm::dvec3(world * glm::dvec4(vec(o, "position", glm::dvec3(0, 3, 0)), 1.0));
        l.position = glm::vec3(p);
        const glm::dvec3 colour = vec(o, "color", glm::dvec3(1.0));
        const double intensity = num(o, "intensity", o.kind == "spot" ? 20.0 : 10.0);
        // Intensity is what falls on a surface one metre away facing the
        // lamp, in the same units as the sun's: 1 at 1 m, a quarter at 2 m.
        l.color = linear(colour) * static_cast<float>(intensity);
        l.radius = static_cast<float>(std::max(0.0, num(o, "radius", 0.05)));
        l.range = static_cast<float>(num(o, "range", 1e5));
        l.inverseSquare = true;
        if (o.kind == "spot") {
            glm::dvec3 dir;
            if (o.find("direction")) dir = vec(o, "direction", glm::dvec3(0, -1, 0));
            else dir = vec(o, "look_at", glm::dvec3(0.0)) - vec(o, "position", glm::dvec3(0, 3, 0));
            dir = glm::dvec3(world * glm::dvec4(dir, 0.0));
            if (glm::length(dir) < 1e-12) throw Error(o.loc, "a spot needs a direction (or a look_at away from it)");
            l.direction = glm::vec3(glm::normalize(dir));
            const double outer = std::clamp(num(o, "angle", 30.0), 0.5, 89.0);
            const double inner = std::clamp(num(o, "inner", outer * 0.7), 0.0, outer);
            l.cosOuter = static_cast<float>(std::cos(glm::radians(outer)));
            l.cosInner = static_cast<float>(std::cos(glm::radians(inner)));
        }
        sc.lamps.push_back(l);
        if (flag(o, "visible", false) && l.radius > 0.0f) {
            // The bulb, for the eye only: an additive sphere bright enough to
            // be the lamp it stands for, which blocks and lights nothing (the
            // lamp itself does the lighting). Seen through, so its two
            // crossings share the brightness.
            pathtrace::Material bulb;
            bulb.albedo = glm::vec3(0.0f);
            bulb.emission = glm::vec3(colour);
            bulb.emissionStrength = static_cast<float>(intensity / (kPiD * l.radius * l.radius) * 0.5);
            bulb.additive = true;
            bulb.castsShadow = false;
            sc.materials.push_back(bulb);
            pathtrace::ShapeNode n;
            n.kind = pathtrace::ShapeNode::Kind::Sphere;
            n.material = static_cast<int>(sc.materials.size() - 1);
            const glm::dmat4 lw = glm::translate(glm::dmat4(1.0), p) * glm::scale(glm::dmat4(1.0), glm::dvec3(l.radius));
            const glm::dmat4 inv = glm::inverse(lw);
            n.toLocal = glm::mat4(inv);
            n.normalToWorld = glm::mat3(glm::transpose(glm::dmat3(inv)));
            n.toPattern = n.toLocal;
            m_shapes->nodes.push_back(n);
            m_shapes->roots.push_back(static_cast<int>(m_shapes->nodes.size() - 1));
        }
    }

    void sun(const Obj& o, const glm::dmat4& world) {
        if (m_sun) throw Error(o.loc, "a second sun -- a scene has one (use lights for more)");
        m_sun = true;
        pathtrace::Sun& s = m_out.scene->sun;
        glm::dvec3 d = glm::dvec3(world * glm::dvec4(vec(o, "direction", glm::dvec3(0.5, 1.0, 0.35)), 0.0));
        if (glm::length(d) < 1e-12) throw Error(o.loc, "a sun needs a direction");
        s.direction = glm::vec3(glm::normalize(d));
        s.color = linear(vec(o, "color", glm::dvec3(1.0, 0.97, 0.92))) *
                  static_cast<float>(num(o, "intensity", 3.0));
        // The sun's disc, as an angle across: half a degree is the real one.
        s.angularRadiusDeg = static_cast<float>(std::max(0.0, num(o, "angle", 0.53)) * 0.5);
        s.enabled = true;
    }

    void sky(const Obj& o) {
        if (m_sky) throw Error(o.loc, "a second sky -- a scene has one");
        m_sky = true;
        pathtrace::Environment& env = m_out.scene->env;
        env.intensity = static_cast<float>(num(o, "intensity", 1.0));
        env.zenith  = linear(vec(o, "zenith", glm::dvec3(0.36, 0.52, 0.78)));
        env.horizon = linear(vec(o, "horizon", glm::dvec3(0.80, 0.86, 0.93)));
        env.ground  = linear(vec(o, "ground", glm::dvec3(0.30, 0.28, 0.26)));
        if (const Prop* h = o.find("hdri")) {
            if (h->value.t != T::Str) throw Error(h->loc, "hdri: the file name of a panorama (.hdr or .exr)");
            loadPanorama(h->value.string(), h->loc, env);
            const double rot = num(o, "rotate", 0.0);
            if (rot != 0.0) rotatePanorama(env.pixels, env.width, env.height, rot);
        } else if (flag(o, "procedural", false)) {
            // The engine's own sky gradient (skyair.glsl), for the scene's sun,
            // baked into a panorama so the tracer can aim at it.
            const glm::vec3 sunDir = m_out.scene->sun.direction;
            env.width = 256;
            env.height = 128;
            env.pixels.resize(static_cast<std::size_t>(env.width) * env.height * 3);
            for (int y = 0; y < env.height; ++y)
                for (int x = 0; x < env.width; ++x) {
                    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(env.height);
                    const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(env.width);
                    const float elev = 3.14159265f * (v - 0.5f), phi = 6.2831853f * (u - 0.5f);
                    const glm::vec3 d(std::cos(elev) * std::cos(phi), std::sin(elev), std::cos(elev) * std::sin(phi));
                    glm::vec3 c = pathtrace::skyGradient(d, sunDir);
                    if (d.y < 0.0f) c = glm::mix(c, env.ground, std::min(1.0f, -d.y * 4.0f));
                    const std::size_t i = (static_cast<std::size_t>(y) * env.width + x) * 3;
                    env.pixels[i] = c.r; env.pixels[i + 1] = c.g; env.pixels[i + 2] = c.b;
                }
        }
        if (o.find("background")) {
            // What the camera sees behind everything, as a flat colour; the
            // light still comes from the sky above.
            const glm::vec3 c = linear(vec(o, "background", glm::dvec3(1.0)));
            env.backdropWidth = 4;
            env.backdropHeight = 2;
            env.backdrop.assign(4 * 2 * 3, 0.0f);
            for (int i = 0; i < 8; ++i) { env.backdrop[i * 3] = c.r; env.backdrop[i * 3 + 1] = c.g; env.backdrop[i * 3 + 2] = c.b; }
        }
    }

    void loadPanorama(const std::string& path, const Loc& loc, pathtrace::Environment& env) {
        const std::filesystem::path p = m_in.resolve(path, loc);
        std::string ext = p.extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        int w = 0, h = 0;
        std::vector<float> rgb;
        if (ext == ".exr") {
            float* rgba = nullptr;
            const char* err = nullptr;
            if (LoadEXR(&rgba, &w, &h, p.string().c_str(), &err) != 0) {
                const std::string why = err ? err : "unreadable";
                if (err) FreeEXRErrorMessage(err);
                throw Error(loc, "cannot read the panorama '" + path + "': " + why);
            }
            rgb.resize(static_cast<std::size_t>(w) * h * 3);
            for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i)
                for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = rgba[i * 4 + c];
            std::free(rgba);
        } else {
            int n = 0;
            float* px = stbi_loadf(p.string().c_str(), &w, &h, &n, 3);
            if (!px) throw Error(loc, "cannot read the panorama '" + path + "'");
            rgb.assign(px, px + static_cast<std::size_t>(w) * h * 3);
            stbi_image_free(px);
        }
        // The tracer wants row 0 at the ground (as the engine's loader leaves
        // it); files store the sky first.
        env.width = w;
        env.height = h;
        env.pixels.resize(rgb.size());
        for (int y = 0; y < h; ++y)
            std::memcpy(&env.pixels[static_cast<std::size_t>(y) * w * 3],
                        &rgb[static_cast<std::size_t>(h - 1 - y) * w * 3], sizeof(float) * w * 3);
    }

    static void rotatePanorama(std::vector<float>& px, int w, int h, double deg) {
        const int shift = static_cast<int>(std::lround(deg / 360.0 * w)) % w;
        if (shift == 0) return;
        std::vector<float> row(static_cast<std::size_t>(w) * 3);
        for (int y = 0; y < h; ++y) {
            float* r = &px[static_cast<std::size_t>(y) * w * 3];
            for (int x = 0; x < w; ++x) {
                const int s = ((x - shift) % w + w) % w;
                for (int c = 0; c < 3; ++c) row[static_cast<std::size_t>(x) * 3 + c] = r[s * 3 + c];
            }
            std::memcpy(r, row.data(), row.size() * sizeof(float));
        }
    }

    void fog(const Obj& o) {
        pathtrace::FogDesc& f = m_out.scene->fog;
        f.density = static_cast<float>(std::max(0.0, num(o, "density", 0.02)));
        f.color = linear(vec(o, "color", glm::dvec3(0.75, 0.8, 0.86)));
        f.sunColor = linear(vec(o, "sun_color", glm::dvec3(1.0, 0.85, 0.65)));
        f.height = static_cast<float>(num(o, "height", 0.0));
        f.heightFalloff = static_cast<float>(std::max(0.0, num(o, "falloff", 0.0)));
        if (f.heightFalloff <= 0.0f) f.heightFalloff = 1e-6f;
    }

    void finishCamera() {
        pathtrace::CameraDesc& cam = m_out.scene->camera;
        glm::dvec3 pos(0.0, 1.5, 6.0), at(0.0), up(0, 1, 0);
        double fov = 45.0, aperture = 0.0, roll = 0.0;
        double focus = -1.0;
        bool autoFocus = false;
        glm::dvec3 dir(0.0);
        bool haveDir = false;
        const glm::dmat4 xf = m_cameraXf;
        if (m_cameraObj) {
            const Obj& o = *m_cameraObj;
            pos = vec(o, "position", pos);
            at = vec(o, "look_at", at);
            up = vec(o, "up", up);
            if (o.find("direction")) { dir = vec(o, "direction", dir); haveDir = true; }
            fov = num(o, "fov", fov);
            if (!(fov > 0.5 && fov < 179.0)) throw Error(o.find("fov")->loc, "fov: between 1 and 178 degrees");
            aperture = std::max(0.0, num(o, "aperture", 0.0));
            roll = num(o, "roll", 0.0);
            if (const Prop* f = o.find("focus")) {
                if (f->value.t == T::Str && f->value.string() == "auto") autoFocus = true;
                else if (f->value.t == T::Num) focus = f->value.v[0];
                else throw Error(f->loc, "focus: a distance in metres, or \"auto\"");
            }
        }
        pos = glm::dvec3(xf * glm::dvec4(pos, 1.0));
        glm::dvec3 fwd = haveDir ? glm::dvec3(xf * glm::dvec4(dir, 0.0))
                                 : glm::dvec3(xf * glm::dvec4(at, 1.0)) - pos;
        up = glm::dvec3(xf * glm::dvec4(up, 0.0));
        if (glm::length(fwd) < 1e-12)
            throw Error(m_cameraObj ? m_cameraObj->loc : Loc{}, "the camera looks at its own position");
        // Where it came from, for the window that moves it (Result::camera).
        Result::CameraSource& src = m_out.camera;
        if (m_cameraObj) {
            const Loc& l = m_cameraObj->loc;
            src.file = l.file >= 0 && l.file < static_cast<int>(m_in.files.size())
                     ? m_in.files[static_cast<std::size_t>(l.file)] : std::string();
            src.line = l.line;
            src.col = l.col;
        }
        src.xf = xf;
        src.fov = fov;
        src.focusFollows = focus < 0.0 && !autoFocus;
        // A camera aimed by direction turns around a point ahead of it: as far
        // as it focuses, or a few metres.
        src.target = haveDir ? pos + glm::normalize(fwd) * (focus > 0.0 ? focus : 5.0) : pos + fwd;
        if (focus < 0.0) focus = glm::length(fwd);
        fwd = glm::normalize(fwd);
        glm::dvec3 right = glm::cross(fwd, up);
        if (glm::length(right) < 1e-9) right = glm::cross(fwd, glm::dvec3(0, 0, 1));
        right = glm::normalize(right);
        glm::dvec3 trueUp = glm::cross(right, fwd);
        if (roll != 0.0) {
            const glm::dmat4 r = glm::rotate(glm::dmat4(1.0), glm::radians(roll), fwd);
            right  = glm::dvec3(r * glm::dvec4(right, 0.0));
            trueUp = glm::dvec3(r * glm::dvec4(trueUp, 0.0));
        }
        cam.position = glm::vec3(pos);
        cam.forward = glm::vec3(fwd);
        cam.right = glm::vec3(right);
        cam.up = glm::vec3(trueUp);
        cam.fovDegrees = static_cast<float>(fov);
        cam.apertureRadius = static_cast<float>(aperture);
        cam.focusDistance = static_cast<float>(focus);
        m_out.settings.autoFocus = autoFocus;
    }

    void render(const Obj& o) {
        if (m_render) throw Error(o.loc, "a second render { } -- a scene has one");
        m_render = true;
        pathtrace::Settings& s = m_out.settings;
        const auto count = [&](const char* name, int def, int lo, int hi) {
            const double v = num(o, name, def);
            if (v < lo || v > hi)
                throw Error(o.find(name)->loc, std::string(name) + ": between " + std::to_string(lo) +
                                                   " and " + std::to_string(hi));
            return static_cast<int>(v);
        };
        s.width = count("width", s.width, 8, 16384);
        s.height = count("height", s.height, 8, 16384);
        s.samples = count("samples", s.samples, 1, 1 << 20);
        s.maxBounces = count("bounces", s.maxBounces, 0, 64);
        s.batch = count("batch", s.batch, 1, 1024);
        s.seed = static_cast<unsigned>(count("seed", 1, 0, 1 << 30));
        s.threads = count("threads", 0, 0, 256);
        s.clampIndirect = static_cast<float>(num(o, "clamp", s.clampIndirect));
        pathtrace::Scene& sc = *m_out.scene;
        sc.exposure = static_cast<float>(num(o, "exposure", 1.0));
        const std::string tm = str(o, "tonemap", "aces");
        if (tm == "aces") sc.grade.curve = 0;
        else if (tm == "agx") sc.grade.curve = 1;
        else if (tm == "neutral") sc.grade.curve = 2;
        else throw Error(o.find("tonemap")->loc, "tonemap: \"aces\", \"agx\" or \"neutral\"");
        sc.grade.saturation = static_cast<float>(num(o, "saturation", 1.0));
        sc.grade.contrast = static_cast<float>(num(o, "contrast", 0.0));
        sc.grade.warmth = static_cast<float>(num(o, "warmth", 0.0));
        sc.grade.hueShift = static_cast<float>(num(o, "hue", 0.0));
        sc.grade.vignette = static_cast<float>(num(o, "vignette", 0.0));
        if (const Prop* p = o.find("output")) {
            if (p->value.t != T::Str) throw Error(p->loc, "output: a file name");
            m_out.output = m_in.dirOf(p->loc) / p->value.string();
        }
    }
};

} // namespace

void buildScene(Interp& in, Result& out) {
    Builder b(in, out);
    b.run();
}

std::string Diagnostic::format() const {
    std::string s = file.empty() ? std::string("<script>") : file;
    if (line > 0) s += ":" + std::to_string(line) + ":" + std::to_string(col);
    return s + ": " + message;
}

Result build(const std::string& source, const Options& options) {
    Result out;
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_ptr<Interp> in;
    const auto diag = [&](const Loc& loc, const std::string& msg) {
        Diagnostic d;
        d.line = loc.line;
        d.col = loc.col;
        d.message = msg;
        if (in && loc.file >= 0 && loc.file < static_cast<int>(in->files.size()))
            d.file = in->files[static_cast<std::size_t>(loc.file)];
        else
            d.file = options.file.string();
        out.errors.push_back(d);
    };
    try {
        in = std::make_unique<Interp>(options);
        in->runSource(source, options.file.empty() ? std::string("<script>") : options.file.string());
        buildScene(*in, out);
        out.ok = true;
    } catch (const Error& e) {
        diag(e.loc, e.what());
    } catch (const std::bad_alloc&) {
        diag(Loc{}, "out of memory");
    } catch (const std::exception& e) {
        diag(Loc{}, e.what());
    }
    if (in) out.log.insert(out.log.begin(), in->log.begin(), in->log.end());
    if (!out.ok) out.scene.reset();
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

Result buildFile(const std::filesystem::path& file, Options options) {
    options.file = file;
    std::ifstream f(file, std::ios::binary);
    if (!f) {
        Result r;
        Diagnostic d;
        d.file = file.string();
        d.message = "cannot open the file";
        r.errors.push_back(d);
        return r;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return build(ss.str(), options);
}

const Vocabulary& vocabulary() {
    static const Vocabulary v = [] {
        Vocabulary out;
        out.keywords = {"let", "fn", "if", "else", "for", "in", "while", "return", "break", "continue",
                        "import", "true", "false", "nil", "and", "or", "not"};
        out.objects = objectKinds();
        out.properties = propertyNames();
        Options o;
        Interp in(o);
        for (const auto& [name, val] : in.globals->vars)
            if ((val.t == T::Func || val.t == T::Native) &&
                std::find(out.objects.begin(), out.objects.end(), name) == out.objects.end())
                out.functions.push_back(name);
        std::sort(out.functions.begin(), out.functions.end());
        return out;
    }();
    return v;
}

} // namespace strahl
