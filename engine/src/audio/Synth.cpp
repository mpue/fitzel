#include "fitzel/audio/Synth.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "fitzel/asset/Vfs.hpp"

namespace fitzel::synth {

namespace {

struct TypeInfo {
    ModuleType  type;
    const char* name;
    int         inputs;
};

// One table, read both ways. A name that is not in it loads as Const, which is
// a module that makes a number and no noise -- an unknown type should leave the
// rest of the patch working rather than take the whole sound down with it.
const TypeInfo kTypes[] = {
    {ModuleType::Osc,    "osc",    2},
    {ModuleType::Lfo,    "lfo",    1},
    {ModuleType::Note,   "note",   1},
    {ModuleType::Filter, "filter", 2},
    {ModuleType::Adsr,   "adsr",   1},
    {ModuleType::Gain,   "gain",   2},
    {ModuleType::Mix,    "mix",    4},
    {ModuleType::Delay,  "delay",  2},
    {ModuleType::Drive,  "drive",  2},
    {ModuleType::Chorus, "chorus", 1},
    {ModuleType::Reverb, "reverb", 1},
    {ModuleType::Map,    "map",    1},
    {ModuleType::Param,  "param",  0},
    {ModuleType::Const,  "const",  0},
    {ModuleType::Out,    "out",    1},
};

// What an input reads when nothing is wired into it. An amplitude that defaults
// to zero would make every fresh oscillator silent and look like a broken
// module; one that defaults to 1 plays, which is what dropping an oscillator
// into a patch is asking for.
float defaultInput(ModuleType t, int port) {
    if (t == ModuleType::Osc && port == 1) return 1.0f;   // amplitude
    if (t == ModuleType::Gain && port == 1) return 1.0f;  // gain multiplier
    return 0.0f;
}

} // namespace

const char* typeName(ModuleType t) {
    for (const TypeInfo& i : kTypes)
        if (i.type == t) return i.name;
    return "const";
}

ModuleType typeFromName(const std::string& s) {
    for (const TypeInfo& i : kTypes)
        if (s == i.name) return i.type;
    return ModuleType::Const;
}

int inputCount(ModuleType t) {
    for (const TypeInfo& i : kTypes)
        if (i.type == t) return i.inputs;
    return 0;
}

// =============================================================================
// ModuleDef / Patch
// =============================================================================

float ModuleDef::param(const std::string& key, float fallback) const {
    for (const auto& p : params)
        if (p.first == key) return p.second;
    return fallback;
}

void ModuleDef::setParam(const std::string& key, float value) {
    for (auto& p : params)
        if (p.first == key) { p.second = value; return; }
    params.emplace_back(key, value);
}

int Patch::nextId() const {
    int n = 1;
    for (const ModuleDef& m : modules) n = std::max(n, m.id + 1);
    return n;
}

const ModuleDef* Patch::find(int id) const {
    for (const ModuleDef& m : modules)
        if (m.id == id) return &m;
    return nullptr;
}

ModuleDef* Patch::find(int id) {
    for (ModuleDef& m : modules)
        if (m.id == id) return &m;
    return nullptr;
}

bool Patch::fromJson(const std::string& text, Patch& out, std::string* error) {
    out = Patch{};
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        if (error) *error = "not a patch (JSON did not parse)";
        return false;
    }
    out.name = j.value("name", std::string{});
    for (const auto& jm : j.value("modules", nlohmann::json::array())) {
        ModuleDef m;
        m.id   = jm.value("id", 0);
        m.type = typeFromName(jm.value("type", std::string("const")));
        m.name = jm.value("name", std::string{});
        m.mode = jm.value("mode", std::string{});
        m.x    = jm.value("x", 0.0f);
        m.y    = jm.value("y", 0.0f);
        if (jm.contains("params") && jm["params"].is_object())
            for (auto it = jm["params"].begin(); it != jm["params"].end(); ++it)
                if (it.value().is_number())
                    m.params.emplace_back(it.key(), it.value().get<float>());
        out.modules.push_back(std::move(m));
    }
    for (const auto& jw : j.value("wires", nlohmann::json::array())) {
        Wire w;
        w.from = jw.value("from", 0);
        w.to   = jw.value("to", 0);
        w.port = jw.value("port", 0);
        out.wires.push_back(w);
    }
    for (const auto& ji : j.value("inputs", nlohmann::json::array())) {
        PatchInput in;
        in.name  = ji.value("name", std::string{});
        in.min   = ji.value("min", 0.0f);
        in.max   = ji.value("max", 1.0f);
        in.value = ji.value("value", 0.0f);
        if (!in.name.empty()) out.inputs.push_back(std::move(in));
    }
    return true;
}

std::string Patch::toJson() const {
    nlohmann::json j;
    j["name"] = name;
    for (const ModuleDef& m : modules) {
        nlohmann::json jm;
        jm["id"]   = m.id;
        jm["type"] = typeName(m.type);
        if (!m.name.empty()) jm["name"] = m.name;
        if (!m.mode.empty()) jm["mode"] = m.mode;
        jm["x"] = m.x;
        jm["y"] = m.y;
        nlohmann::json jp = nlohmann::json::object();
        for (const auto& p : m.params) jp[p.first] = p.second;
        jm["params"] = jp;
        j["modules"].push_back(jm);
    }
    for (const Wire& w : wires)
        j["wires"].push_back({{"from", w.from}, {"to", w.to}, {"port", w.port}});
    for (const PatchInput& in : inputs)
        j["inputs"].push_back({{"name", in.name},
                               {"min", in.min},
                               {"max", in.max},
                               {"value", in.value}});
    if (!j.contains("modules")) j["modules"] = nlohmann::json::array();
    if (!j.contains("wires"))   j["wires"]   = nlohmann::json::array();
    if (!j.contains("inputs"))  j["inputs"]  = nlohmann::json::array();
    return j.dump(2);
}

bool Patch::load(const std::string& path, Patch& out, std::string* error) {
    const std::string text = vfs::readText(path);
    if (text.empty()) {
        if (error) *error = "cannot read " + path;
        return false;
    }
    return fromJson(text, out, error);
}

bool Patch::save(const std::string& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const std::string text = toJson();
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(f);
}

// =============================================================================
// Graph
// =============================================================================

struct Graph::Impl {
    // One node per module, in processing order. Everything it needs while it
    // runs is here: no lookups into the patch, no strings, no allocation.
    struct Node {
        ModuleType type = ModuleType::Const;
        int        id   = 0;
        // Where each input reads from: the index of another node's buffer, or
        // -1 for "nothing wired", in which case `fallback` stands in.
        int   src[4]      = {-1, -1, -1, -1};
        float fallback[4] = {0, 0, 0, 0};

        // The settings, flattened out of the params list. Read by the audio
        // thread; new values arrive through Impl::pending (see updateParams).
        float p[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        int   mode = 0;
        int   inputIndex = -1;   // Param: which dial it reads

        // The running state, one of which is used.
        Osc       osc;
        Adsr      adsr;
        Biquad    filter;
        DelayLine delay;
        Drive     drive;
        Chorus    chorus;
        Reverb    reverb;
        bool      gateOn = false;
    };

    double             sampleRate = 48000.0;
    int                maxBlock   = 512;
    std::vector<Node>  nodes;                 // already in processing order
    std::vector<std::vector<float>> bufs;     // one output buffer per node
    int                outNode = -1;
    float              outGain = 1.0f;

    // The dials. Written from any thread, read once per block.
    std::vector<std::string>        inputNames;
    std::vector<std::atomic<float>> inputValues;
    std::vector<float>              smoothed;     // what the graph actually uses
    std::vector<float>              smoothCoef;   // per input, from its `smooth`

    std::vector<int>  order;      // module ids, for the editor
    std::vector<char> reached;    // per node: does it feed the Out?
    int               lastBlock = 0;   // frames in the buffers, for the scope

    // Settings on their way in from another thread: eight per node, and a flag
    // per node saying there is something to pick up. Kept beside the nodes
    // rather than inside them because an atomic member would make a Node
    // unmovable, and the node list is built by value.
    std::vector<std::atomic<float>> pending;
    std::vector<std::atomic<int>>   pendingDirty;

    // Hand a node's settings to the objects that actually use them. Called
    // after compile and whenever new values arrive.
    void applyParams(Node& nd) {
        switch (nd.type) {
        case ModuleType::Osc:
            nd.osc.setFine(nd.p[1]);
            nd.osc.setPulseWidth(nd.p[2]);
            break;
        case ModuleType::Lfo:
            nd.osc.setWave(static_cast<Osc::Wave>(nd.mode));
            nd.osc.setFrequency(nd.p[0]);
            break;
        case ModuleType::Filter:
            nd.filter.set(nd.p[0], nd.p[1]);
            break;
        case ModuleType::Drive:
            nd.drive.setShape(static_cast<Drive::Shape>(nd.mode));
            nd.drive.set(nd.p[0], nd.p[1]);
            break;
        case ModuleType::Chorus:
            nd.chorus.set(nd.p[0], nd.p[1], nd.p[2]);
            break;
        case ModuleType::Reverb:
            nd.reverb.set(nd.p[0], nd.p[1], nd.p[2]);
            break;
        case ModuleType::Adsr:
            nd.adsr.setAttack(nd.p[0]);
            nd.adsr.setDecay(nd.p[1]);
            nd.adsr.setSustain(nd.p[2]);
            nd.adsr.setRelease(nd.p[3]);
            break;
        default:
            break;   // the rest read p[] straight out of the loop
        }
    }
};

Graph::Graph() : m_impl(std::make_unique<Impl>()) {}
Graph::~Graph() = default;
Graph::Graph(Graph&&) noexcept = default;
Graph& Graph::operator=(Graph&&) noexcept = default;

bool Graph::ready() const { return m_impl && m_impl->outNode >= 0; }

const std::vector<int>& Graph::order() const { return m_impl->order; }

bool Graph::reaches(int moduleId) const {
    for (std::size_t i = 0; i < m_impl->nodes.size(); ++i)
        if (m_impl->nodes[i].id == moduleId)
            return i < m_impl->reached.size() && m_impl->reached[i] != 0;
    return false;
}

const std::vector<std::string>& Graph::inputNames() const { return m_impl->inputNames; }

int Graph::inputIndex(const std::string& name) const {
    for (int i = 0; i < static_cast<int>(m_impl->inputNames.size()); ++i)
        if (m_impl->inputNames[static_cast<std::size_t>(i)] == name) return i;
    return -1;
}

void Graph::setInput(int index, float value) {
    if (index < 0 || index >= static_cast<int>(m_impl->inputValues.size())) return;
    m_impl->inputValues[static_cast<std::size_t>(index)].store(value,
                                                               std::memory_order_relaxed);
}

void Graph::setInput(const std::string& name, float value) {
    setInput(inputIndex(name), value);
}

float Graph::input(int index) const {
    if (index < 0 || index >= static_cast<int>(m_impl->inputValues.size())) return 0.0f;
    return m_impl->inputValues[static_cast<std::size_t>(index)].load(
        std::memory_order_relaxed);
}

namespace {

int oscMode(const std::string& s) {
    if (s == "saw")   return 1;
    if (s == "pulse") return 2;
    if (s == "noise") return 3;
    return 0;   // sine
}

int filterMode(const std::string& s) {
    if (s == "hp") return 1;
    if (s == "bp") return 2;
    return 0;   // lp
}

int driveMode(const std::string& s) {
    if (s == "hard") return 1;
    if (s == "atan") return 2;
    if (s == "fold") return 3;
    return 0;   // soft
}

} // namespace

bool Graph::compile(const Patch& patch, double sampleRate, int maxBlock,
                    std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        m_impl = std::make_unique<Impl>();   // unusable rather than half-built
        return false;
    };

    Impl impl;
    impl.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    impl.maxBlock   = std::max(1, maxBlock);

    // --- the dials ---------------------------------------------------------
    impl.inputNames.reserve(patch.inputs.size());
    impl.inputValues = std::vector<std::atomic<float>>(patch.inputs.size());
    impl.smoothed.resize(patch.inputs.size(), 0.0f);
    impl.smoothCoef.resize(patch.inputs.size(), 0.0f);
    for (std::size_t i = 0; i < patch.inputs.size(); ++i) {
        impl.inputNames.push_back(patch.inputs[i].name);
        impl.inputValues[i].store(patch.inputs[i].value, std::memory_order_relaxed);
        impl.smoothed[i] = patch.inputs[i].value;
    }

    // --- order the modules -------------------------------------------------
    // Kahn's algorithm over the wires. A patch is a signal flow and has to be a
    // DAG: a module that reads something computed later in the same block would
    // be reading the previous block, and "it sounds different depending on the
    // order the editor happened to add things" is the kind of bug nobody ever
    // tracks down. A feedback loop is the Delay module's job.
    const int n = static_cast<int>(patch.modules.size());
    if (n == 0) return fail("the patch has no modules");

    std::unordered_map<int, int> indexOf;       // module id -> slot
    for (int i = 0; i < n; ++i) {
        if (indexOf.count(patch.modules[static_cast<std::size_t>(i)].id))
            return fail("two modules share id " +
                        std::to_string(patch.modules[static_cast<std::size_t>(i)].id));
        indexOf[patch.modules[static_cast<std::size_t>(i)].id] = i;
    }

    std::vector<std::vector<int>> feeds(static_cast<std::size_t>(n));  // i -> nodes it feeds
    std::vector<int>              indeg(static_cast<std::size_t>(n), 0);
    struct Link { int from, to, port; };
    std::vector<Link> links;
    for (const Wire& w : patch.wires) {
        auto f = indexOf.find(w.from);
        auto t = indexOf.find(w.to);
        if (f == indexOf.end() || t == indexOf.end()) continue;   // a stale wire
        const ModuleDef& dst = patch.modules[static_cast<std::size_t>(t->second)];
        if (w.port < 0 || w.port >= inputCount(dst.type)) continue;
        feeds[static_cast<std::size_t>(f->second)].push_back(t->second);
        ++indeg[static_cast<std::size_t>(t->second)];
        links.push_back({f->second, t->second, w.port});
    }

    std::vector<int> sorted;
    sorted.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        if (indeg[static_cast<std::size_t>(i)] == 0) sorted.push_back(i);
    for (std::size_t k = 0; k < sorted.size(); ++k)
        for (int to : feeds[static_cast<std::size_t>(sorted[k])])
            if (--indeg[static_cast<std::size_t>(to)] == 0) sorted.push_back(to);
    if (static_cast<int>(sorted.size()) != n) {
        // Name one module still in the loop: "there is a cycle" sends you
        // hunting, "module 7 is in a loop" points at it.
        for (int i = 0; i < n; ++i)
            if (indeg[static_cast<std::size_t>(i)] > 0)
                return fail("module " +
                            std::to_string(patch.modules[static_cast<std::size_t>(i)].id) +
                            " is part of a loop -- use a Delay to feed a signal back");
        return fail("the patch has a loop");
    }

    // --- build the nodes ---------------------------------------------------
    std::vector<int> slotToNode(static_cast<std::size_t>(n), -1);
    impl.nodes.resize(static_cast<std::size_t>(n));
    impl.bufs.assign(static_cast<std::size_t>(n),
                     std::vector<float>(static_cast<std::size_t>(impl.maxBlock), 0.0f));
    for (int k = 0; k < n; ++k) slotToNode[static_cast<std::size_t>(sorted[k])] = k;

    for (int k = 0; k < n; ++k) {
        const ModuleDef& m = patch.modules[static_cast<std::size_t>(sorted[k])];
        Impl::Node& nd = impl.nodes[static_cast<std::size_t>(k)];
        nd.type = m.type;
        nd.id   = m.id;
        for (int p = 0; p < 4; ++p) nd.fallback[p] = defaultInput(m.type, p);

        switch (m.type) {
        case ModuleType::Osc:
            nd.mode = oscMode(m.mode);
            nd.p[0] = m.param("freq", 220.0f);
            nd.p[1] = m.param("fine", 0.0f);
            nd.p[2] = m.param("pulseWidth", 0.5f);
            nd.p[3] = m.param("level", 1.0f);
            nd.osc.prepare(impl.sampleRate);
            nd.osc.setWave(static_cast<Osc::Wave>(nd.mode));
            nd.osc.setFine(nd.p[1]);
            nd.osc.setPulseWidth(nd.p[2]);
            break;
        case ModuleType::Lfo:
            nd.mode = oscMode(m.mode);
            nd.p[0] = m.param("rate", 2.0f);
            nd.p[1] = m.param("depth", 1.0f);
            nd.p[2] = m.param("offset", 0.0f);
            nd.osc.prepare(impl.sampleRate);
            nd.osc.setWave(static_cast<Osc::Wave>(nd.mode));
            nd.osc.setFrequency(nd.p[0]);
            break;
        case ModuleType::Note:
            nd.p[0] = m.param("transpose", 0.0f);
            nd.p[1] = m.param("fine", 0.0f);
            break;
        case ModuleType::Drive:
            nd.mode = driveMode(m.mode);
            nd.p[0] = m.param("drive", 2.0f);
            nd.p[1] = m.param("mix", 1.0f);
            nd.drive.setShape(static_cast<Drive::Shape>(nd.mode));
            nd.drive.set(nd.p[0], nd.p[1]);
            break;
        case ModuleType::Chorus:
            nd.p[0] = m.param("rate", 0.6f);
            nd.p[1] = m.param("depth", 0.002f);
            nd.p[2] = m.param("mix", 0.5f);
            nd.chorus.prepare(impl.sampleRate);
            nd.chorus.set(nd.p[0], nd.p[1], nd.p[2]);
            break;
        case ModuleType::Reverb:
            nd.p[0] = m.param("size", 0.5f);
            nd.p[1] = m.param("damp", 0.4f);
            nd.p[2] = m.param("mix", 0.3f);
            nd.reverb.prepare(impl.sampleRate);
            nd.reverb.set(nd.p[0], nd.p[1], nd.p[2]);
            break;
        case ModuleType::Filter:
            nd.mode = filterMode(m.mode);
            nd.p[0] = m.param("cutoff", 1000.0f);
            nd.p[1] = m.param("q", 0.707f);
            nd.filter.prepare(impl.sampleRate);
            nd.filter.setMode(static_cast<Biquad::Mode>(nd.mode));
            nd.filter.set(nd.p[0], nd.p[1]);
            break;
        case ModuleType::Adsr:
            nd.p[0] = m.param("attack", 0.01f);
            nd.p[1] = m.param("decay", 0.1f);
            nd.p[2] = m.param("sustain", 0.7f);
            nd.p[3] = m.param("release", 0.2f);
            nd.adsr.prepare(impl.sampleRate);
            nd.adsr.setAttack(nd.p[0]);
            nd.adsr.setDecay(nd.p[1]);
            nd.adsr.setSustain(nd.p[2]);
            nd.adsr.setRelease(nd.p[3]);
            break;
        case ModuleType::Gain:
            nd.p[0] = m.param("gain", 1.0f);
            break;
        case ModuleType::Mix:
            nd.p[0] = m.param("gain", 1.0f);
            nd.p[1] = m.param("level1", 1.0f);
            nd.p[2] = m.param("level2", 1.0f);
            nd.p[3] = m.param("level3", 1.0f);
            nd.p[4] = m.param("level4", 1.0f);
            break;
        case ModuleType::Delay:
            nd.p[0] = m.param("time", 0.25f);
            nd.p[1] = m.param("feedback", 0.3f);
            nd.p[2] = m.param("mix", 0.5f);
            // Sized for the patch's own time plus room to modulate it. A delay
            // that is swept longer than its buffer reads the oldest sample it
            // has, which is a flat repeat rather than a crash.
            nd.delay.prepare(impl.sampleRate, std::max(0.05f, nd.p[0] * 2.0f + 0.1f));
            break;
        case ModuleType::Map:
            nd.p[0] = m.param("inMin", 0.0f);
            nd.p[1] = m.param("inMax", 1.0f);
            nd.p[2] = m.param("outMin", 0.0f);
            nd.p[3] = m.param("outMax", 1.0f);
            nd.p[4] = m.param("curve", 1.0f);
            break;
        case ModuleType::Param: {
            nd.inputIndex = -1;
            for (int i = 0; i < static_cast<int>(impl.inputNames.size()); ++i)
                if (impl.inputNames[static_cast<std::size_t>(i)] == m.mode) nd.inputIndex = i;
            const float smooth = m.param("smooth", 0.02f);
            if (nd.inputIndex >= 0) {
                // One-pole smoothing per BLOCK, so a dial the game jerks does
                // not step the signal and click. Zero seconds means "follow it
                // exactly", which a gate needs.
                const float blocks = smooth * static_cast<float>(impl.sampleRate) /
                                     static_cast<float>(impl.maxBlock);
                impl.smoothCoef[static_cast<std::size_t>(nd.inputIndex)] =
                    blocks > 0.001f ? std::exp(-1.0f / blocks) : 0.0f;
            }
            break;
        }
        case ModuleType::Const:
            nd.p[0] = m.param("value", 0.0f);
            break;
        case ModuleType::Out:
            nd.p[0] = m.param("gain", 1.0f);
            if (impl.outNode >= 0) return fail("the patch has more than one Out");
            impl.outNode = k;
            impl.outGain = nd.p[0];
            break;
        }
    }
    if (impl.outNode < 0) return fail("the patch has no Out module");

    for (const Link& l : links) {
        Impl::Node& dst = impl.nodes[static_cast<std::size_t>(slotToNode[static_cast<std::size_t>(l.to)])];
        dst.src[l.port] = slotToNode[static_cast<std::size_t>(l.from)];
    }

    // Which modules actually reach the Out. Not an error -- a patch under
    // construction is full of them -- but the editor should be able to grey
    // them out, because "why can I not hear it" is nearly always this.
    impl.reached.assign(static_cast<std::size_t>(n), 0);
    impl.reached[static_cast<std::size_t>(impl.outNode)] = 1;
    for (int k = impl.outNode; k >= 0; --k) {
        if (!impl.reached[static_cast<std::size_t>(k)]) continue;
        for (int p = 0; p < 4; ++p) {
            const int s = impl.nodes[static_cast<std::size_t>(k)].src[p];
            if (s >= 0) impl.reached[static_cast<std::size_t>(s)] = 1;
        }
    }

    impl.order.clear();
    for (const Impl::Node& nd : impl.nodes) impl.order.push_back(nd.id);

    impl.pending      = std::vector<std::atomic<float>>(impl.nodes.size() * 8);
    impl.pendingDirty = std::vector<std::atomic<int>>(impl.nodes.size());
    for (std::size_t k = 0; k < impl.nodes.size(); ++k) {
        for (int i = 0; i < 8; ++i)
            impl.pending[k * 8 + static_cast<std::size_t>(i)].store(
                impl.nodes[k].p[i], std::memory_order_relaxed);
        impl.pendingDirty[k].store(0, std::memory_order_relaxed);
    }

    *m_impl = std::move(impl);
    return true;
}

bool Graph::updateParams(const Patch& patch) {
    Impl& im = *m_impl;
    if (im.nodes.empty() || patch.modules.size() != im.nodes.size()) return false;

    for (const ModuleDef& m : patch.modules) {
        // Find the node by id. The shape has to match exactly -- same ids, same
        // types -- or this would be writing one module's settings into another.
        std::size_t k = im.nodes.size();
        for (std::size_t i = 0; i < im.nodes.size(); ++i)
            if (im.nodes[i].id == m.id) { k = i; break; }
        if (k == im.nodes.size() || im.nodes[k].type != m.type) return false;

        float p[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        switch (m.type) {
        case ModuleType::Osc:
            p[0] = m.param("freq", 220.0f);  p[1] = m.param("fine", 0.0f);
            p[2] = m.param("pulseWidth", 0.5f); p[3] = m.param("level", 1.0f);
            break;
        case ModuleType::Filter:
            p[0] = m.param("cutoff", 1000.0f); p[1] = m.param("q", 0.707f);
            break;
        case ModuleType::Adsr:
            p[0] = m.param("attack", 0.01f);  p[1] = m.param("decay", 0.1f);
            p[2] = m.param("sustain", 0.7f);  p[3] = m.param("release", 0.2f);
            break;
        case ModuleType::Gain:
            p[0] = m.param("gain", 1.0f);
            break;
        case ModuleType::Mix:
            p[0] = m.param("gain", 1.0f);
            p[1] = m.param("level1", 1.0f);
            p[2] = m.param("level2", 1.0f);
            p[3] = m.param("level3", 1.0f);
            p[4] = m.param("level4", 1.0f);
            break;
        case ModuleType::Lfo:
            p[0] = m.param("rate", 2.0f);
            p[1] = m.param("depth", 1.0f);
            p[2] = m.param("offset", 0.0f);
            break;
        case ModuleType::Note:
            p[0] = m.param("transpose", 0.0f);
            p[1] = m.param("fine", 0.0f);
            break;
        case ModuleType::Drive:
            p[0] = m.param("drive", 2.0f);
            p[1] = m.param("mix", 1.0f);
            break;
        case ModuleType::Chorus:
            p[0] = m.param("rate", 0.6f);
            p[1] = m.param("depth", 0.002f);
            p[2] = m.param("mix", 0.5f);
            break;
        case ModuleType::Reverb:
            p[0] = m.param("size", 0.5f);
            p[1] = m.param("damp", 0.4f);
            p[2] = m.param("mix", 0.3f);
            break;
        case ModuleType::Delay:
            // The time changes freely; the buffer it reads from does not grow,
            // so a time swept past what compile() sized simply stops getting
            // longer instead of reading off the end.
            p[0] = m.param("time", 0.25f);    p[1] = m.param("feedback", 0.3f);
            p[2] = m.param("mix", 0.5f);
            break;
        case ModuleType::Map:
            p[0] = m.param("inMin", 0.0f);    p[1] = m.param("inMax", 1.0f);
            p[2] = m.param("outMin", 0.0f);   p[3] = m.param("outMax", 1.0f);
            p[4] = m.param("curve", 1.0f);
            break;
        case ModuleType::Const:
            p[0] = m.param("value", 0.0f);
            break;
        case ModuleType::Out:
            p[0] = m.param("gain", 1.0f);
            break;
        case ModuleType::Param:
            break;   // its only setting is smoothing, which compile() works out
        }
        for (int i = 0; i < 8; ++i)
            im.pending[k * 8 + static_cast<std::size_t>(i)].store(
                p[i], std::memory_order_relaxed);
        // Released last: the flag is what the audio thread reads first, so the
        // eight values have to be in place before it is set.
        im.pendingDirty[k].store(1, std::memory_order_release);
    }
    return true;
}

const float* Graph::buffer(int moduleId, int& frames) const {
    frames = 0;
    if (!m_impl) return nullptr;
    for (std::size_t k = 0; k < m_impl->nodes.size(); ++k)
        if (m_impl->nodes[k].id == moduleId) {
            frames = m_impl->lastBlock;
            return m_impl->bufs[k].data();
        }
    return nullptr;
}

void Graph::reset() {
    if (!m_impl) return;
    for (Impl::Node& nd : m_impl->nodes) {
        nd.osc.reset();
        nd.adsr.reset();
        nd.filter.reset();
        nd.delay.reset();
        nd.chorus.reset();
        nd.reverb.reset();
        nd.gateOn = false;
    }
    for (std::vector<float>& b : m_impl->bufs) std::fill(b.begin(), b.end(), 0.0f);
    for (std::size_t i = 0; i < m_impl->smoothed.size(); ++i)
        m_impl->smoothed[i] = m_impl->inputValues[i].load(std::memory_order_relaxed);
}

void Graph::process(float* out, int frames) {
    Impl& im = *m_impl;
    if (im.outNode < 0 || frames <= 0) {
        if (out) std::fill(out, out + std::max(0, frames), 0.0f);
        return;
    }

    // The dials, read once and eased towards their new value. Per block rather
    // than per sample: a block is ~10 ms, which is below what a moving parameter
    // is heard as, and it keeps the inner loops free of atomics.
    for (std::size_t i = 0; i < im.smoothed.size(); ++i) {
        const float target = im.inputValues[i].load(std::memory_order_relaxed);
        const float c      = im.smoothCoef[i];
        im.smoothed[i]     = c > 0.0f ? target + (im.smoothed[i] - target) * c : target;
    }

    int done = 0;
    while (done < frames) {
        const int block = std::min(im.maxBlock, frames - done);

        for (std::size_t k = 0; k < im.nodes.size(); ++k) {
            Impl::Node& nd  = im.nodes[k];
            float*      dst = im.bufs[k].data();
            // New settings from the editor, picked up once per block.
            if (k < im.pendingDirty.size() &&
                im.pendingDirty[k].exchange(0, std::memory_order_acquire) != 0) {
                for (int i = 0; i < 8; ++i)
                    nd.p[i] = im.pending[k * 8 + static_cast<std::size_t>(i)].load(
                        std::memory_order_relaxed);
                im.applyParams(nd);
            }
            // An input's samples: the buffer it is wired to, or its fallback.
            auto in = [&](int port, int i) -> float {
                const int s = nd.src[port];
                return s >= 0 ? im.bufs[static_cast<std::size_t>(s)][static_cast<std::size_t>(i)]
                              : nd.fallback[port];
            };

            switch (nd.type) {
            case ModuleType::Osc:
                for (int i = 0; i < block; ++i) {
                    nd.osc.setFrequency(static_cast<double>(nd.p[0]) + in(0, i));
                    dst[i] = nd.osc.process() * nd.p[3] * in(1, i);
                }
                break;
            case ModuleType::Filter: {
                // The cutoff moves per block, not per sample: recomputing five
                // coefficients 48000 times a second to chase a modulator costs
                // more than the sweep is worth, and the step is inaudible.
                nd.filter.set(nd.p[0] + in(1, 0), nd.p[1]);
                for (int i = 0; i < block; ++i) dst[i] = nd.filter.process(in(0, i));
                break;
            }
            case ModuleType::Adsr:
                for (int i = 0; i < block; ++i) {
                    const bool want = in(0, i) > 0.5f;
                    if (want != nd.gateOn) {
                        nd.adsr.gate(want);
                        nd.gateOn = want;
                    }
                    dst[i] = nd.adsr.process();
                }
                break;
            case ModuleType::Gain:
                for (int i = 0; i < block; ++i) dst[i] = in(0, i) * nd.p[0] * in(1, i);
                break;
            case ModuleType::Mix:
                for (int i = 0; i < block; ++i)
                    dst[i] = (in(0, i) * nd.p[1] + in(1, i) * nd.p[2] +
                              in(2, i) * nd.p[3] + in(3, i) * nd.p[4]) * nd.p[0];
                break;
            case ModuleType::Lfo:
                for (int i = 0; i < block; ++i) {
                    nd.osc.setFrequency(static_cast<double>(nd.p[0]) + in(0, i));
                    dst[i] = nd.p[2] + nd.osc.process() * nd.p[1];
                }
                break;
            case ModuleType::Note:
                // MIDI note to hertz. A patch says "note 60" and gets middle C,
                // which is the one conversion every playable patch needs and
                // the one thing Map (which is linear) cannot do.
                for (int i = 0; i < block; ++i)
                    dst[i] = 440.0f * std::pow(2.0f, (in(0, i) + nd.p[0] - 69.0f) / 12.0f) +
                             nd.p[1];
                break;
            case ModuleType::Drive:
                for (int i = 0; i < block; ++i) {
                    nd.drive.set(nd.p[0] + in(1, i), nd.p[1]);
                    dst[i] = nd.drive.process(in(0, i));
                }
                break;
            case ModuleType::Chorus:
                for (int i = 0; i < block; ++i) dst[i] = nd.chorus.process(in(0, i));
                break;
            case ModuleType::Reverb:
                for (int i = 0; i < block; ++i) dst[i] = nd.reverb.process(in(0, i));
                break;
            case ModuleType::Delay:
                for (int i = 0; i < block; ++i) {
                    const float dry = in(0, i);
                    const float wet = nd.delay.read(nd.p[0] + in(1, i));
                    nd.delay.write(dry + wet * nd.p[1]);
                    dst[i] = dry * (1.0f - nd.p[2]) + wet * nd.p[2];
                }
                break;
            case ModuleType::Map: {
                const float span = (nd.p[1] - nd.p[0]);
                for (int i = 0; i < block; ++i) {
                    float t = span != 0.0f ? (in(0, i) - nd.p[0]) / span : 0.0f;
                    t = std::clamp(t, 0.0f, 1.0f);
                    if (nd.p[4] != 1.0f && t > 0.0f) t = std::pow(t, nd.p[4]);
                    dst[i] = nd.p[2] + (nd.p[3] - nd.p[2]) * t;
                }
                break;
            }
            case ModuleType::Param: {
                const float v = nd.inputIndex >= 0
                                    ? im.smoothed[static_cast<std::size_t>(nd.inputIndex)]
                                    : 0.0f;
                std::fill(dst, dst + block, v);
                break;
            }
            case ModuleType::Const:
                std::fill(dst, dst + block, nd.p[0]);
                break;
            case ModuleType::Out:
                for (int i = 0; i < block; ++i) dst[i] = in(0, i) * nd.p[0];
                break;
            }
        }

        im.lastBlock = block;
        const float* src = im.bufs[static_cast<std::size_t>(im.outNode)].data();
        for (int i = 0; i < block; ++i) {
            const float v = src[i];
            // The one place the patch is not trusted: a filter whipped into
            // self-oscillation or a delay fed back above unity will run away,
            // and what comes out of a game's speakers must not.
            out[done + i] = std::isfinite(v) ? std::clamp(v, -1.0f, 1.0f) : 0.0f;
        }
        done += block;
    }
}

} // namespace fitzel::synth
