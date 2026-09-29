#include "Modifiers.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "EditMeshModifiers.hpp"
#include "PropertyMeta.hpp"   // writeProps / readProps

namespace modifiers {

std::vector<TypeInfo>& registryRef() {
    static std::vector<TypeInfo> r;
    return r;
}
void registerType(TypeInfo info) { registryRef().push_back(std::move(info)); }
const std::vector<TypeInfo>& registry() { return registryRef(); }

std::unique_ptr<Modifier> make(const std::string& typeId) {
    for (const TypeInfo& t : registry())
        if (t.typeId == typeId) return t.make();
    return nullptr;
}

} // namespace modifiers

// --- The component --------------------------------------------------------------

ModifierStackComponent& ModifierStackComponent::operator=(const ModifierStackComponent& o) {
    if (this == &o) return *this;
    stack.clear();
    stack.reserve(o.stack.size());
    for (const auto& m : o.stack) stack.push_back(m->clone());
    smooth      = o.smooth;
    smoothAngle = o.smoothAngle;
    return *this;
}

// {"smooth": .., "smoothAngle": .., "stack": [{"kind": "subsurf", "enabled":
// true, <its settings>}, ...]}. "kind" and not "type": the scene writer puts
// the COMPONENT's type id under "type" in the same object.
void ModifierStackComponent::save(nlohmann::json& j) const {
    j["smooth"]      = smooth;
    j["smoothAngle"] = smoothAngle;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : stack) {
        nlohmann::json e;
        e["kind"]    = m->typeId();
        e["enabled"] = m->enabled;
        writeProps(e, m->props(), m.get());
        arr.push_back(std::move(e));
    }
    j["stack"] = std::move(arr);
}

void ModifierStackComponent::load(const nlohmann::json& j) {
    smooth      = j.value("smooth", smooth);
    smoothAngle = j.value("smoothAngle", smoothAngle);
    stack.clear();
    const auto it = j.find("stack");
    if (it == j.end() || !it->is_array()) return;
    for (const nlohmann::json& e : *it) {
        const std::string kind = e.value("kind", std::string());
        std::unique_ptr<modifiers::Modifier> m = modifiers::make(kind);
        if (!m) {
            // A kind this build does not know (a newer editor wrote it): said,
            // and left out -- the rest of the stack still runs.
            std::fprintf(stderr, "[Fitzel] unknown modifier '%s' dropped\n", kind.c_str());
            continue;
        }
        m->enabled = e.value("enabled", true);
        readProps(e, m->props(), m.get());
        stack.push_back(std::move(m));
    }
}

// --- Evaluation -----------------------------------------------------------------

namespace modifiers {

EditMesh evaluate(const EditMesh& base, const ModifierStackComponent& ms, int count) {
    EditMesh m = base;
    const int n = count < 0 ? static_cast<int>(ms.stack.size())
                            : std::min(count, static_cast<int>(ms.stack.size()));
    for (int i = 0; i < n; ++i)
        if (ms.stack[static_cast<std::size_t>(i)]->enabled)
            ms.stack[static_cast<std::size_t>(i)]->apply(m);
    return m;
}

namespace {

struct Entry {
    std::uint64_t base  = 0;   // the mesh revision it was made from
    std::size_t   stack = 0;   // ...and a hash of the stack's settings
    std::uint64_t revision = 0;
    EditMesh      mesh;
};
std::unordered_map<int, Entry>& cache() {
    static std::unordered_map<int, Entry> c;
    return c;
}

// Everything that decides the result, in one number: the stack as the file
// would keep it. A handful of fields per modifier -- far cheaper than the
// evaluation it saves, and it cannot forget a field the way a hand-written
// comparison would the day a kind gains one.
std::size_t stackHash(const ModifierStackComponent& ms) {
    nlohmann::json j;
    ms.save(j);
    return std::hash<std::string>{}(j.dump());
}

} // namespace

Shown shown(int key, const MeshComponent& mc, const ModifierStackComponent* ms) {
    // No stack, or one that neither changes the mesh nor shades it: the mesh
    // itself, and its own revision -- the path every other object takes.
    bool active = false;
    if (ms) {
        active = ms->smooth;
        for (const auto& m : ms->stack) active = active || m->enabled;
    }
    if (!active) return {&mc.mesh, mc.revision};

    Entry& e = cache()[key];
    const std::size_t h = stackHash(*ms);
    if (e.revision == 0 || e.base != mc.revision || e.stack != h) {
        e.mesh = evaluate(mc.mesh, *ms);
        // In the space of the mesh it came from: fitScale reads this box, so
        // copies and shells that reach beyond it are not squeezed back into it.
        e.mesh.hasFrame = true;
        mc.mesh.bounds(e.mesh.frameMin, e.mesh.frameMax);
        e.mesh.smoothAngle = ms->smooth ? std::max(ms->smoothAngle, 0.1f) : 0.0f;
        e.base     = mc.revision;
        e.stack    = h;
        e.revision = editmesh::nextRevision();
    }
    return {&e.mesh, e.revision};
}

void clearCache() { cache().clear(); }

} // namespace modifiers

// --- The kinds --------------------------------------------------------------------
// One class each: its settings as members, the same settings as Property rows
// (the card's fields and the file's keys), and apply(). The geometry itself is
// in EditMeshModifiers.cpp.

namespace {

template <class T, class V>
Property prop(const char* label, const char* key, PropKind kind, V T::*member) {
    Property p;
    p.label = label;
    p.key   = key;
    p.kind  = kind;
    p.field = [member](void* o) -> void* { return &(static_cast<T*>(o)->*member); };
    return p;
}

class SubdivisionModifier : public modifiers::ModifierOf<SubdivisionModifier> {
public:
    int levels = 2;
    int mode   = 0;   // 0 Catmull-Clark, 1 Simple

    const char* typeId() const override { return "subsurf"; }
    const char* displayName() const override { return "Subdivision Surface"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            std::vector<Property> v;
            Property lv = prop("Levels", "levels", PropKind::Int, &SubdivisionModifier::levels);
            lv.slider = true; lv.min = 0.0f; lv.max = 5.0f;
            v.push_back(std::move(lv));
            Property md = prop("Mode", "mode", PropKind::EnumInt, &SubdivisionModifier::mode);
            md.enumLabels = {"Catmull-Clark", "Simple"};
            v.push_back(std::move(md));
            return v;
        }();
        return p;
    }
    void apply(EditMesh& m) const override {
        editmesh::subdivideSurface(m, std::clamp(levels, 0, 5), mode == 0);
    }
};

class DecimateModifier : public modifiers::ModifierOf<DecimateModifier> {
public:
    float ratio = 0.5f;

    const char* typeId() const override { return "decimate"; }
    const char* displayName() const override { return "Decimate"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            std::vector<Property> v;
            Property r = prop("Ratio", "ratio", PropKind::Float, &DecimateModifier::ratio);
            r.slider = true; r.min = 0.01f; r.max = 1.0f; r.fmt = "%.2f";
            v.push_back(std::move(r));
            return v;
        }();
        return p;
    }
    void apply(EditMesh& m) const override { editmesh::decimate(m, std::clamp(ratio, 0.01f, 1.0f)); }
};

class WireframeModifier : public modifiers::ModifierOf<WireframeModifier> {
public:
    float thickness = 0.04f;
    float offset    = 0.0f;
    bool  replace   = true;

    const char* typeId() const override { return "wireframe"; }
    const char* displayName() const override { return "Wireframe"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            std::vector<Property> v;
            Property t = prop("Thickness", "thickness", PropKind::Float, &WireframeModifier::thickness);
            t.min = 0.001f; t.max = 10.0f; t.speed = 0.002f; t.fmt = "%.3f";
            v.push_back(std::move(t));
            Property o = prop("Offset", "offset", PropKind::Float, &WireframeModifier::offset);
            o.slider = true; o.min = -1.0f; o.max = 1.0f; o.fmt = "%.2f";
            v.push_back(std::move(o));
            v.push_back(prop("Replace original", "replace", PropKind::Bool, &WireframeModifier::replace));
            return v;
        }();
        return p;
    }
    void apply(EditMesh& m) const override {
        editmesh::wireframe(m, std::max(thickness, 0.0f), offset, replace);
    }
};

// Register the kinds, and the component that carries them. A new kind is a
// class above and one line here.
struct RegisterModifiers {
    template <class T>
    static void add(const char* tip) {
        const T t;
        modifiers::registerType({t.typeId(), t.displayName(), tip,
                                 [] { return std::unique_ptr<modifiers::Modifier>(std::make_unique<T>()); }});
    }
    RegisterModifiers() {
        add<SubdivisionModifier>("Smooth the mesh: every face split into quads,\n"
                                 "the surface pulled towards the curve its cage describes.");
        add<DecimateModifier>("Fewer faces: collapse the mesh to a share of its triangles,\n"
                              "keeping its shape as far as it goes.");
        add<WireframeModifier>("Turn every edge into a solid strut -- a lattice, a cage,\n"
                               "a frame -- of the thickness you give it.");
        components::registerType({"modifiers", "Modifiers",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<ModifierStackComponent>()); }});
    }
} g_registerModifiers;

} // namespace
