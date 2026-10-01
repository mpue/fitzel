#include "ProcGraph.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "PropertyMeta.hpp"   // writeProps / readProps

namespace proc {

// --- The registry -------------------------------------------------------------

namespace {
std::vector<TypeInfo>& registryRef() {
    static std::vector<TypeInfo> r;
    return r;
}
} // namespace

void registerType(TypeInfo info) { registryRef().push_back(std::move(info)); }
const std::vector<TypeInfo>& registry() { return registryRef(); }

const TypeInfo* typeInfo(const std::string& typeId) {
    for (const TypeInfo& t : registry())
        if (t.typeId == typeId) return &t;
    return nullptr;
}

std::unique_ptr<Node> make(const std::string& typeId) {
    const TypeInfo* t = typeInfo(typeId);
    return t ? t->make() : nullptr;
}

// --- The graph ----------------------------------------------------------------

Graph& Graph::operator=(const Graph& o) {
    if (this == &o) return *this;
    nodes.clear();
    nodes.reserve(o.nodes.size());
    for (const auto& n : o.nodes) {
        std::unique_ptr<Node> c = n->clone();
        // clone() copies the derived settings AND the base fields (it is a copy
        // construction of the whole object); said here because a kind that
        // forgot to derive from NodeOf would lose its wiring on every undo.
        nodes.push_back(std::move(c));
    }
    output = o.output;
    nextId = o.nextId;
    return *this;
}

Node* Graph::find(int id) {
    for (auto& n : nodes)
        if (n->id == id) return n.get();
    return nullptr;
}

const Node* Graph::find(int id) const {
    for (const auto& n : nodes)
        if (n->id == id) return n.get();
    return nullptr;
}

std::string Graph::uniqueName(const std::string& base) const {
    // "tube3" asks for another tube: the number is not part of what it is.
    std::string stem = base;
    while (!stem.empty() && stem.back() >= '0' && stem.back() <= '9') stem.pop_back();
    if (stem.empty()) stem = "node";
    for (int k = 1;; ++k) {
        const std::string nm = stem + std::to_string(k);
        bool taken = false;
        for (const auto& n : nodes) taken = taken || n->name == nm;
        if (!taken) return nm;
    }
}

Node& Graph::add(std::unique_ptr<Node> n) {
    n->id = nextId++;
    if (n->name.empty()) n->name = uniqueName(n->typeId());
    if (!n->variadic()) n->inputs.resize(static_cast<std::size_t>(n->inputSlots()), -1);
    nodes.push_back(std::move(n));
    return *nodes.back();
}

void Graph::remove(int id) {
    const Node* gone = find(id);
    if (!gone) return;
    const int through = gone->inputs.empty() ? -1 : gone->inputs.front();
    for (auto& n : nodes) {
        if (n->id == id) continue;
        for (std::size_t s = 0; s < n->inputs.size();) {
            if (n->inputs[s] != id) { ++s; continue; }
            if (n->variadic() && through < 0) {
                // A merge just loses that strand; a gap in its list would be a
                // slot nobody asked for.
                n->inputs.erase(n->inputs.begin() + static_cast<std::ptrdiff_t>(s));
                continue;
            }
            n->inputs[s] = through;
            ++s;
        }
    }
    if (output == id) output = through;
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                               [id](const std::unique_ptr<Node>& n) { return n->id == id; }),
                nodes.end());
}

std::vector<int> Graph::upstream(int id) const {
    std::vector<int> out;
    std::vector<int> todo{id};
    std::unordered_set<int> seen;
    while (!todo.empty()) {
        const int cur = todo.back();
        todo.pop_back();
        if (!seen.insert(cur).second) continue;
        const Node* n = find(cur);
        if (!n) continue;
        out.push_back(cur);
        for (int in : n->inputs)
            if (in >= 0) todo.push_back(in);
    }
    return out;
}

bool Graph::placedByHand() const {
    for (const auto& n : nodes)
        if (n->placed) return true;
    return false;
}

bool Graph::wouldCycle(int consumer, int source) const {
    if (source < 0) return false;
    if (source == consumer) return true;
    const std::vector<int> up = upstream(source);
    return std::find(up.begin(), up.end(), consumer) != up.end();
}

bool Graph::connect(int consumer, int slot, int source) {
    Node* n = find(consumer);
    if (!n || slot < 0) return false;
    if (source >= 0 && (!find(source) || wouldCycle(consumer, source))) return false;
    if (n->variadic()) {
        const std::size_t s = static_cast<std::size_t>(slot);
        if (s >= n->inputs.size()) {
            if (source >= 0) n->inputs.push_back(source);
        } else if (source < 0) {
            n->inputs.erase(n->inputs.begin() + static_cast<std::ptrdiff_t>(s));
        } else {
            n->inputs[s] = source;
        }
        return true;
    }
    if (slot >= n->inputSlots()) return false;
    n->inputs.resize(static_cast<std::size_t>(n->inputSlots()), -1);
    n->inputs[static_cast<std::size_t>(slot)] = source;
    return true;
}

// {"output": 7, "nextId": 12, "nodes": [{"id": 1, "kind": "tube", "name":
// "hub", "inputs": [], "bypass": false, <its settings>}, ...]}. "kind" for
// the same reason as the modifier stack: "type" is the component's.
void Graph::save(nlohmann::json& j) const {
    j["output"] = output;
    j["nextId"] = nextId;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& n : nodes) {
        nlohmann::json e;
        e["id"]     = n->id;
        e["kind"]   = n->typeId();
        e["name"]   = n->name;
        e["inputs"] = n->inputs;
        if (n->bypass) e["bypass"] = true;
        if (n->placed) e["pos"] = nlohmann::json::array({n->pos.x, n->pos.y});
        writeProps(e, n->props(), n.get());
        arr.push_back(std::move(e));
    }
    j["nodes"] = std::move(arr);
}

void Graph::load(const nlohmann::json& j) {
    nodes.clear();
    output = j.value("output", -1);
    nextId = j.value("nextId", 1);
    const auto it = j.find("nodes");
    if (it == j.end() || !it->is_array()) return;
    for (const nlohmann::json& e : *it) {
        const std::string kind = e.value("kind", std::string());
        std::unique_ptr<Node> n = make(kind);
        if (!n) {
            // A kind this build does not know: said, and left out. Whatever it
            // fed now reads an empty input, which the editor shows.
            std::fprintf(stderr, "[Fitzel] unknown procedural node '%s' dropped\n", kind.c_str());
            continue;
        }
        n->id     = e.value("id", 0);
        n->name   = e.value("name", std::string());
        n->bypass = e.value("bypass", false);
        if (const auto p = e.find("pos"); p != e.end() && p->is_array() && p->size() == 2) {
            n->pos    = glm::vec2((*p)[0].get<float>(), (*p)[1].get<float>());
            n->placed = true;
        }
        if (const auto in = e.find("inputs"); in != e.end() && in->is_array())
            for (const nlohmann::json& v : *in) n->inputs.push_back(v.is_number_integer() ? v.get<int>() : -1);
        if (!n->variadic()) n->inputs.resize(static_cast<std::size_t>(n->inputSlots()), -1);
        readProps(e, n->props(), n.get());
        nextId = std::max(nextId, n->id + 1);
        nodes.push_back(std::move(n));
    }
}

void mergeIntoOutput(Graph& g, int id) {
    if (!g.find(id) || g.output == id) return;
    if (g.output < 0 || !g.find(g.output)) { g.output = id; return; }
    Node* out = g.find(g.output);
    if (out->variadic()) {
        g.connect(out->id, static_cast<int>(out->inputs.size()), id);
        return;
    }
    std::unique_ptr<Node> merge = make("merge");
    if (!merge) return;
    const int was = g.output;
    const int m = g.add(std::move(merge)).id;
    g.connect(m, 0, was);
    g.connect(m, 1, id);
    g.output = m;
}

std::size_t hashOf(const Graph& g) {
    nlohmann::json j;
    g.save(j);
    for (nlohmann::json& n : j["nodes"]) n.erase("pos");
    return std::hash<std::string>{}(j.dump());
}

// --- Geometry ---------------------------------------------------------------------

void Geo::syncSel() {
    if (!hasSel) return;
    meshSel.resize(mesh.verts.size(), 0);
    for (Curve& c : curves) c.sel.resize(c.pts.size(), 0);
}

std::size_t Geo::pointCount() const {
    std::size_t n = mesh.verts.size();
    for (const Curve& c : curves) n += c.pts.size();
    return n;
}

std::size_t Geo::selectedCount() const {
    if (!hasSel) return 0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < mesh.verts.size() && i < meshSel.size(); ++i) n += meshSel[i] ? 1 : 0;
    for (const Curve& c : curves)
        for (std::size_t i = 0; i < c.pts.size() && i < c.sel.size(); ++i) n += c.sel[i] ? 1 : 0;
    return n;
}

std::vector<glm::vec3> cornerNormals(const EditMesh& m) {
    std::vector<glm::vec3> nrm(m.verts.size(), glm::vec3(0.0f));
    // Newell's normal is as long as twice the face's area: summing it weights
    // a big face over a sliver without a second pass.
    for (int f = 0; f < static_cast<int>(m.faces.size()); ++f) {
        if (!m.validFace(f)) continue;
        const glm::vec3 n = m.faceNormal(f) * std::max(m.faceArea(f), 1e-6f);
        for (int v : m.faces[static_cast<std::size_t>(f)])
            if (v >= 0 && v < static_cast<int>(nrm.size())) nrm[static_cast<std::size_t>(v)] += n;
    }
    for (glm::vec3& n : nrm)
        n = glm::dot(n, n) > 1e-12f ? glm::normalize(n) : glm::vec3(0.0f, 1.0f, 0.0f);
    return nrm;
}

std::vector<glm::vec3> parsePoints(const std::string& s) {
    std::vector<glm::vec3> out;
    std::size_t at = 0;
    while (at <= s.size()) {
        const std::size_t end = std::min(s.find(';', at), s.size());
        const std::string one = s.substr(at, end - at);
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (std::sscanf(one.c_str(), "%f %f %f", &x, &y, &z) == 3) out.emplace_back(x, y, z);
        at = end + 1;
    }
    return out;
}

std::string formatPoints(const std::vector<glm::vec3>& pts) {
    std::string s;
    char buf[96];
    for (const glm::vec3& p : pts) {
        std::snprintf(buf, sizeof buf, "%s%g %g %g", s.empty() ? "" : "; ", p.x, p.y, p.z);
        s += buf;
    }
    return s;
}

// --- Cooking ------------------------------------------------------------------

namespace {

// One cook of one graph: every node made at most once, whatever number of
// nodes downstream want it -- a spoke copied four times and merged twice is
// still one tube.
struct Cooker {
    const Graph& g;
    CookInfo*    info;
    // Node ids -> what they made. An unordered_map keeps references to its
    // values valid while it grows, which is what lets get() hand out one while
    // cooking the next.
    std::unordered_map<int, Geo> done;
    std::unordered_set<int>      busy;

    const Geo& empty() {
        static const Geo none;
        return none;
    }

    void fail(int id, const std::string& why) {
        if (info && !why.empty()) info->errors[id] = why;
    }

    const Geo& get(int id) {
        if (auto it = done.find(id); it != done.end()) return it->second;
        const Node* n = g.find(id);
        if (!n) return empty();
        if (!busy.insert(id).second) {
            // Only a hand-edited file gets here: connect() refuses loops.
            fail(id, "Part of a loop");
            return empty();
        }
        const std::size_t slots = n->variadic() ? n->inputs.size()
                                                : static_cast<std::size_t>(n->inputSlots());
        std::vector<const Geo*> in(slots, nullptr);
        for (std::size_t s = 0; s < slots && s < n->inputs.size(); ++s)
            if (n->inputs[s] >= 0 && g.find(n->inputs[s])) in[s] = &get(n->inputs[s]);

        Geo out;
        if (n->bypass) {
            if (!in.empty() && in[0]) out = *in[0];
        } else {
            fail(id, n->cook(in, out));
        }
        // Whatever a node did to the point counts, the selection stays one
        // entry per point: new points come in unselected.
        out.syncSel();
        if (info) {
            info->faces[id]   = out.mesh.faces.size();
            info->corners[id] = out.pointCount();
            info->curves[id]  = out.curves.size();
            if (out.hasSel) info->selected[id] = out.selectedCount();
            else            info->selected.erase(id);
            info->prefabs[id] = out.instances.size();
        }
        busy.erase(id);
        return done.emplace(id, std::move(out)).first->second;
    }
};

} // namespace

Geo cookGeo(const Graph& g, int id, CookInfo* info) {
    const auto t0 = std::chrono::steady_clock::now();
    Cooker c{g, info, {}, {}};
    Geo out = c.get(id);
    if (info)
        info->ms = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0).count();
    return out;
}

EditMesh cook(const Graph& g, int id, CookInfo* info) {
    return facesOf(cookGeo(g, id, info));
}

EditMesh facesOf(const Geo& geo) {
    EditMesh out = geo.mesh;
    // A corner no face uses -- left behind by a deleted face, or a point that
    // was only ever a point -- is nothing the object can show.
    {
        std::vector<char> used(out.verts.size(), 0);
        for (const auto& f : out.faces)
            for (int v : f)
                if (v >= 0 && v < static_cast<int>(used.size())) used[static_cast<std::size_t>(v)] = 1;
        if (std::find(used.begin(), used.end(), 0) != used.end()) {
            std::vector<int> remap(out.verts.size(), -1);
            EditMesh m;
            m.faces    = out.faces;
            m.faceMat  = out.faceMat;
            m.faceUV   = out.faceUV;
            for (std::size_t i = 0; i < out.verts.size(); ++i) {
                if (!used[i]) continue;
                remap[i] = static_cast<int>(m.verts.size());
                m.verts.push_back(out.verts[i]);
                if (!out.paint.empty()) m.paint.push_back(out.paintAt(static_cast<int>(i)));
            }
            for (auto& f : m.faces)
                for (int& v : f) v = remap[static_cast<std::size_t>(v)];
            out = std::move(m);
        }
    }
    // Arrays nobody filled stay empty: a generated mesh with a zero weight on
    // every corner would be saved -- and drawn -- as a painted one.
    if (!out.painted()) out.paint.clear();
    if (!out.dressed()) out.faceMat.clear();
    if (!out.unwrapped()) out.faceUV.clear();
    return out;
}

// --- Picking faces ------------------------------------------------------------

const std::vector<std::string>& faceSetLabels() {
    static const std::vector<std::string> labels = {
        "All faces", "Facing up (+Y)", "Facing down (-Y)", "Sides (level normal)",
        "Facing +X", "Facing -X", "Facing +Z", "Facing -Z",
        "Outside (away from Y axis)", "Inside (toward Y axis)",
        "Between selected points",
    };
    return labels;
}

namespace {

bool inSet(const Geo& g, int f, FaceSet set) {
    const EditMesh& m = g.mesh;
    if (set == FaceSet::All) return true;
    if (set == FaceSet::SelectedPoints) {
        for (int v : m.faces[static_cast<std::size_t>(f)])
            if (!g.meshPicked(v)) return false;
        return true;
    }
    const glm::vec3 n = m.faceNormal(f);
    // About 45 degrees either way: a face "facing up" on a gently domed cap
    // still counts, a wall does not.
    constexpr float kFacing = 0.7f;
    switch (set) {
        case FaceSet::Up:    return n.y >  kFacing;
        case FaceSet::Down:  return n.y < -kFacing;
        case FaceSet::Sides: return std::fabs(n.y) < 0.3f;
        case FaceSet::PosX:  return n.x >  kFacing;
        case FaceSet::NegX:  return n.x < -kFacing;
        case FaceSet::PosZ:  return n.z >  kFacing;
        case FaceSet::NegZ:  return n.z < -kFacing;
        case FaceSet::AwayFromAxis:
        case FaceSet::TowardAxis: {
            const glm::vec3 c = m.faceCenter(f);
            const glm::vec3 r(c.x, 0.0f, c.z);
            if (glm::dot(r, r) < 1e-8f) return false;
            const float d = glm::dot(glm::vec3(n.x, 0.0f, n.z), glm::normalize(r));
            return set == FaceSet::AwayFromAxis ? d > 0.5f : d < -0.5f;
        }
        default: return true;
    }
}

} // namespace

float random01(int index, int seed) {
    std::uint32_t h = static_cast<std::uint32_t>(index) * 0x9E3779B1u ^
                      (static_cast<std::uint32_t>(seed) + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 16; h *= 0x7FEB352Du;
    h ^= h >> 15; h *= 0x846CA68Bu;
    h ^= h >> 16;
    return static_cast<float>(h >> 8) / 16777216.0f;
}

std::vector<int> pickFaces(const Geo& g, FaceSet set, float share, int seed) {
    std::vector<int> out;
    const float keep = std::clamp(share, 0.0f, 1.0f);
    for (int f = 0; f < static_cast<int>(g.mesh.faces.size()); ++f) {
        if (g.mesh.faces[static_cast<std::size_t>(f)].size() < 3) continue;
        if (!inSet(g, f, set)) continue;
        if (keep < 1.0f && random01(f, seed) >= keep) continue;
        out.push_back(f);
    }
    return out;
}
} // namespace proc

// --- The component --------------------------------------------------------------

void ProcGraphComponent::save(nlohmann::json& j) const {
    graph.save(j);
    j["pivot"] = nlohmann::json::array({pivot.x, pivot.y, pivot.z});
}

void ProcGraphComponent::load(const nlohmann::json& j) {
    graph.load(j);
    pivot = glm::vec3(0.0f);
    if (const auto p = j.find("pivot"); p != j.end() && p->is_array() && p->size() == 3)
        pivot = glm::vec3((*p)[0].get<float>(), (*p)[1].get<float>(), (*p)[2].get<float>());
}

void ProcMadeComponent::save(nlohmann::json& j) const { j["sig"] = sig; }
void ProcMadeComponent::load(const nlohmann::json& j) { sig = j.value("sig", std::string()); }

namespace {
// Not in the Add Component menu: a graph on its own does nothing, and the
// Procedural window is where one is made -- together with the mesh it cooks.
// The tag on placed prefabs is the graph's to set, too.
struct RegisterProcGraph {
    RegisterProcGraph() {
        components::registerType({"procgraph", "Procedural",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<ProcGraphComponent>()); },
            false});
        components::registerType({"procmade", "Placed by a graph",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<ProcMadeComponent>()); },
            false});
    }
} g_registerProcGraph;
} // namespace