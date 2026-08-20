#include "SplineSystem.hpp"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

#include "SandboxMath.hpp"   // sampleSpline()

namespace {

const std::vector<glm::vec3> kNoLine;
const std::vector<int>       kNoSamples;

// Ramp the per-control-point heights out to every sample: linear between the
// points whose sample indices `ptSample` names. Deliberately not smoothed the way
// the road's profile is -- a wall is meant to be able to step, and a fence that
// eased its way up to a gatepost would miss it.
std::vector<float> liftRamp(const std::vector<float>& lifts,
                            const std::vector<int>& ptSample, std::size_t samples,
                            bool closed) {
    std::vector<float> out(samples, 0.0f);
    if (lifts.empty() || ptSample.size() < 2) return out;
    const std::size_t spans = ptSample.size() - 1;
    for (std::size_t k = 0; k < spans; ++k) {
        const int a = ptSample[k], b = ptSample[k + 1];
        if (a < 0 || b <= a || static_cast<std::size_t>(b) >= samples + 1) continue;
        // The closing entry is the loop's start point again (or the open line's
        // last point), so its value comes from control point 0 / the last one.
        const float la = lifts[std::min(k, lifts.size() - 1)];
        const float lb = (k + 1 < lifts.size()) ? lifts[k + 1]
                                                : (closed ? lifts[0] : lifts.back());
        for (int s = a; s <= b && static_cast<std::size_t>(s) < samples; ++s)
            out[s] = glm::mix(la, lb, static_cast<float>(s - a) / static_cast<float>(b - a));
    }
    return out;
}

void writeStyle(nlohmann::json& j, const splinegen::Style& s) {
    // Material overrides as GUID strings, and only when set -- an unset one must
    // read back as "use the shared palette slot", not as a broken reference.
    if (s.matA.valid()) j["matA"] = s.matA.toString();
    if (s.matB.valid()) j["matB"] = s.matB.toString();
    if (s.matC.valid()) j["matC"] = s.matC.toString();
    j["sink"]         = s.sink;
    j["lift"]         = s.lift;
    j["collide"]      = s.collide;
    j["palette"]      = s.palette;
    j["seed"]         = s.seed;
    j["postSpacing"]  = s.postSpacing;
    j["postWidth"]    = s.postWidth;
    j["postHeight"]   = s.postHeight;
    j["rails"]        = s.rails;
    j["railThick"]    = s.railThick;
    j["railTop"]      = s.railTop;
    j["railBottom"]   = s.railBottom;
    j["infill"]       = s.infill;
    j["infillTop"]    = s.infillTop;
    j["infillBottom"] = s.infillBottom;
    j["postJitter"]   = s.postJitter;
    j["postCap"]      = s.postCap;
    j["postCapOver"]  = s.postCapOver;
    j["picketEvery"]  = s.picketEvery;
    j["picketWidth"]  = s.picketWidth;
    j["picketDepth"]  = s.picketDepth;
    j["picketTop"]    = s.picketTop;
    j["picketBottom"] = s.picketBottom;
    j["wallHeight"]   = s.wallHeight;
    j["wallThick"]    = s.wallThick;
    j["wallTaper"]    = s.wallTaper;
    j["copingHeight"] = s.copingHeight;
    j["copingOver"]   = s.copingOver;
    j["pillarEvery"]  = s.pillarEvery;
    j["pillarWidth"]  = s.pillarWidth;
    j["pillarRise"]   = s.pillarRise;
    j["toeHeight"]    = s.toeHeight;
    j["toeOver"]      = s.toeOver;
    j["merlonEvery"]  = s.merlonEvery;
    j["merlonWidth"]  = s.merlonWidth;
    j["merlonRise"]   = s.merlonRise;
    j["merlonInset"]  = s.merlonInset;
    j["gauge"]        = s.gauge;
    j["ballastWidth"] = s.ballastWidth;
    j["ballastHeight"]= s.ballastHeight;
    j["ballastSlope"] = s.ballastSlope;
    j["sleeperSpacing"] = s.sleeperSpacing;
    j["sleeperLength"] = s.sleeperLength;
    j["sleeperWidth"] = s.sleeperWidth;
    j["sleeperHeight"] = s.sleeperHeight;
    j["railHeight"]   = s.railHeight;
    j["railWidth"]    = s.railWidth;
    j["texTile"]      = s.texTile;
    j["colorA"] = {s.colorA.x, s.colorA.y, s.colorA.z};
    j["colorB"] = {s.colorB.x, s.colorB.y, s.colorB.z};
    j["colorC"] = {s.colorC.x, s.colorC.y, s.colorC.z};
}

// Read with a default for every field, so a scene written by an older build (or
// by a newer one that has since gained a knob) loads instead of throwing.
void readStyle(const nlohmann::json& j, splinegen::Style& s) {
    auto col = [&](const char* key, glm::vec3& out) {
        if (j.contains(key) && j[key].is_array() && j[key].size() == 3)
            out = glm::vec3(j[key][0].get<float>(), j[key][1].get<float>(),
                            j[key][2].get<float>());
    };
    s.sink          = j.value("sink", s.sink);
    s.lift          = j.value("lift", s.lift);
    s.collide       = j.value("collide", s.collide);
    s.palette       = j.value("palette", s.palette);
    s.seed          = j.value("seed", s.seed);
    s.postSpacing   = j.value("postSpacing", s.postSpacing);
    s.postWidth     = j.value("postWidth", s.postWidth);
    s.postHeight    = j.value("postHeight", s.postHeight);
    s.rails         = j.value("rails", s.rails);
    s.railThick     = j.value("railThick", s.railThick);
    s.railTop       = j.value("railTop", s.railTop);
    s.railBottom    = j.value("railBottom", s.railBottom);
    s.infill        = j.value("infill", s.infill);
    s.infillTop     = j.value("infillTop", s.infillTop);
    s.infillBottom  = j.value("infillBottom", s.infillBottom);
    s.postJitter    = j.value("postJitter", s.postJitter);
    s.postCap       = j.value("postCap", s.postCap);
    s.postCapOver   = j.value("postCapOver", s.postCapOver);
    s.picketEvery   = j.value("picketEvery", s.picketEvery);
    s.picketWidth   = j.value("picketWidth", s.picketWidth);
    s.picketDepth   = j.value("picketDepth", s.picketDepth);
    s.picketTop     = j.value("picketTop", s.picketTop);
    s.picketBottom  = j.value("picketBottom", s.picketBottom);
    s.wallHeight    = j.value("wallHeight", s.wallHeight);
    s.wallThick     = j.value("wallThick", s.wallThick);
    s.wallTaper     = j.value("wallTaper", s.wallTaper);
    s.copingHeight  = j.value("copingHeight", s.copingHeight);
    s.copingOver    = j.value("copingOver", s.copingOver);
    s.pillarEvery   = j.value("pillarEvery", s.pillarEvery);
    s.pillarWidth   = j.value("pillarWidth", s.pillarWidth);
    s.pillarRise    = j.value("pillarRise", s.pillarRise);
    s.toeHeight     = j.value("toeHeight", s.toeHeight);
    s.toeOver       = j.value("toeOver", s.toeOver);
    s.merlonEvery   = j.value("merlonEvery", s.merlonEvery);
    s.merlonWidth   = j.value("merlonWidth", s.merlonWidth);
    s.merlonRise    = j.value("merlonRise", s.merlonRise);
    s.merlonInset   = j.value("merlonInset", s.merlonInset);
    auto mat = [&](const char* key, fitzel::AssetId& out) {
        if (j.contains(key) && j[key].is_string())
            out = fitzel::AssetId::fromString(j[key].get<std::string>());
    };
    mat("matA", s.matA);
    mat("matB", s.matB);
    mat("matC", s.matC);
    s.gauge         = j.value("gauge", s.gauge);
    s.ballastWidth  = j.value("ballastWidth", s.ballastWidth);
    s.ballastHeight = j.value("ballastHeight", s.ballastHeight);
    s.ballastSlope  = j.value("ballastSlope", s.ballastSlope);
    s.sleeperSpacing= j.value("sleeperSpacing", s.sleeperSpacing);
    s.sleeperLength = j.value("sleeperLength", s.sleeperLength);
    s.sleeperWidth  = j.value("sleeperWidth", s.sleeperWidth);
    s.sleeperHeight = j.value("sleeperHeight", s.sleeperHeight);
    s.railHeight    = j.value("railHeight", s.railHeight);
    s.railWidth     = j.value("railWidth", s.railWidth);
    s.texTile       = j.value("texTile", s.texTile);
    col("colorA", s.colorA);
    col("colorB", s.colorB);
    col("colorC", s.colorC);
}

} // namespace

int SplineSystem::addPath(splinegen::Preset pr, const std::string& name) {
    Path p;
    p.preset = pr;
    p.kind   = splinegen::presetKind(pr);
    p.style  = splinegen::preset(pr);
    p.name   = name.empty() ? splinegen::presetName(pr) : name;
    paths.push_back(std::move(p));
    m_built.resize(paths.size());
    m_runs.resize(paths.size());
    touch(static_cast<int>(paths.size()) - 1);
    return static_cast<int>(paths.size()) - 1;
}

void SplineSystem::applyPreset(int i, splinegen::Preset pr) {
    if (i < 0 || i >= static_cast<int>(paths.size())) return;
    Path& p = paths[i];
    // The material overrides survive: they are the author's choice of surface,
    // not part of the shape, and losing a picked brick texture every time you try
    // another wall would make the picker useless.
    const fitzel::AssetId a = p.style.matA, b = p.style.matB, c = p.style.matC;
    p.preset = pr;
    p.kind   = splinegen::presetKind(pr);
    p.style  = splinegen::preset(pr);
    p.style.matA = a; p.style.matB = b; p.style.matC = c;
    touch(i);
}

void SplineSystem::removePath(int i) {
    if (i < 0 || i >= static_cast<int>(paths.size())) return;
    paths.erase(paths.begin() + i);
    if (i < static_cast<int>(m_built.size())) m_built.erase(m_built.begin() + i);
    if (i < static_cast<int>(m_runs.size()))  m_runs.erase(m_runs.begin() + i);
}

void SplineSystem::insertPoint(int path, int at, glm::vec2 p, float lift) {
    if (path < 0 || path >= static_cast<int>(paths.size())) return;
    Path& q = paths[path];
    q.lifts.resize(q.points.size(), 0.0f);
    at = glm::clamp(at, 0, static_cast<int>(q.points.size()));
    q.points.insert(q.points.begin() + at, p);
    q.lifts.insert(q.lifts.begin() + at, lift);
    touch(path);
}

void SplineSystem::erasePoint(int path, int at) {
    if (path < 0 || path >= static_cast<int>(paths.size())) return;
    Path& q = paths[path];
    if (at < 0 || at >= static_cast<int>(q.points.size())) return;
    q.lifts.resize(q.points.size(), 0.0f);
    q.points.erase(q.points.begin() + at);
    q.lifts.erase(q.lifts.begin() + at);
    touch(path);
}

float SplineSystem::liftOf(int path, int i) const {
    if (path < 0 || path >= static_cast<int>(paths.size())) return 0.0f;
    const Path& q = paths[path];
    return (i >= 0 && i < static_cast<int>(q.lifts.size())) ? q.lifts[i] : 0.0f;
}

void SplineSystem::setLift(int path, int i, float lift) {
    if (path < 0 || path >= static_cast<int>(paths.size())) return;
    Path& q = paths[path];
    if (i < 0 || i >= static_cast<int>(q.points.size())) return;
    q.lifts.resize(q.points.size(), 0.0f);
    q.lifts[i] = lift;
    touch(path);
}

void SplineSystem::touch(int path) {
    m_built.resize(paths.size());
    m_runs.resize(paths.size());
    if (path < 0) {
        for (Built& b : m_built) b.dirty = true;
        return;
    }
    if (path < static_cast<int>(m_built.size())) m_built[path].dirty = true;
}

const std::vector<glm::vec3>& SplineSystem::line(int i) const {
    if (i < 0 || i >= static_cast<int>(m_built.size())) return kNoLine;
    return m_built[i].line;
}

const std::vector<int>& SplineSystem::pointSamples(int i) const {
    if (i < 0 || i >= static_cast<int>(m_built.size())) return kNoSamples;
    return m_built[i].ptSample;
}

void SplineSystem::update(std::vector<MaterialDef>& materials) {
    m_built.resize(paths.size());
    m_runs.resize(paths.size());
    for (int i = 0; i < static_cast<int>(paths.size()); ++i)
        if (m_built[i].dirty) rebuild(i, materials);
}

void SplineSystem::rebuild(int i, std::vector<MaterialDef>& materials) {
    Built& b = m_built[i];
    Run&   r = m_runs[i];
    b.dirty = false;
    b.line.clear();
    b.ptSample.clear();
    r.geo.clear();
    r.meshes.clear();

    const Path& p = paths[i];
    if (!p.enabled || p.points.size() < 2) return;

    // The path itself: the same centripetal spline the road runs on (see
    // sampleSpline), draped on the terrain and raised by the per-point lifts.
    const std::vector<glm::vec2> flat = sampleSpline(p.points, p.closed, &b.ptSample);
    if (flat.size() < 2) return;

    std::vector<float> lifts = p.lifts;
    lifts.resize(p.points.size(), 0.0f);
    const bool anyLift = std::any_of(lifts.begin(), lifts.end(),
                                     [](float v) { return v != 0.0f; });
    const std::vector<float> ramp =
        anyLift ? liftRamp(lifts, b.ptSample, flat.size(), p.closed)
                : std::vector<float>(flat.size(), 0.0f);

    b.line.reserve(flat.size());
    for (std::size_t s = 0; s < flat.size(); ++s) {
        const float y = (groundAt ? groundAt(flat[s].x, flat[s].y) : 0.0f) + ramp[s];
        b.line.emplace_back(flat[s].x, y, flat[s].y);
    }

    const splinegen::Palette pal =
        splinegen::ensurePalette(materials, p.kind, p.style);
    r.geo = splinegen::generate(p.kind, p.style, b.line, p.closed, pal);

    // Upload and release the CPU copy: keeping a second copy of a kilometre of
    // ballast costs tens of megabytes for nothing (the AABB and material stay).
    r.meshes.reserve(r.geo.batches.size());
    for (splinegen::Batch& batch : r.geo.batches) {
        r.meshes.push_back(fitzel::Mesh::create(batch.data));
        batch.data = fitzel::MeshData{};
    }
}

void SplineSystem::clear() {
    paths.clear();
    m_built.clear();
    m_runs.clear();
}

void SplineSystem::save(nlohmann::json& j) const {
    nlohmann::json arr = nlohmann::json::array();
    for (const Path& p : paths) {
        nlohmann::json e;
        e["name"]    = p.name;
        e["kind"]    = static_cast<int>(p.kind);
        e["preset"]  = static_cast<int>(p.preset);
        e["closed"]  = p.closed;
        e["enabled"] = p.enabled;
        nlohmann::json pts = nlohmann::json::array();
        for (const glm::vec2& q : p.points) pts.push_back({q.x, q.y});
        e["points"] = std::move(pts);
        nlohmann::json lifts = nlohmann::json::array();
        for (std::size_t k = 0; k < p.points.size(); ++k)
            lifts.push_back(k < p.lifts.size() ? p.lifts[k] : 0.0f);
        e["lifts"] = std::move(lifts);
        nlohmann::json st;
        writeStyle(st, p.style);
        e["style"] = std::move(st);
        arr.push_back(std::move(e));
    }
    j["paths"] = std::move(arr);
}

void SplineSystem::load(const nlohmann::json& j) {
    clear();
    if (!j.contains("paths") || !j["paths"].is_array()) return;
    for (const auto& e : j["paths"]) {
        Path p;
        p.name    = e.value("name", std::string("Fence"));
        const int k = e.value("kind", 0);
        p.kind    = static_cast<splinegen::Kind>(
            glm::clamp(k, 0, static_cast<int>(splinegen::Kind::Count) - 1));
        p.closed  = e.value("closed", false);
        p.enabled = e.value("enabled", true);
        const int pr = e.value("preset", -1);
        p.preset = (pr >= 0 && pr < static_cast<int>(splinegen::Preset::Count))
                 ? static_cast<splinegen::Preset>(pr)
                 : splinegen::Preset::PostRail;   // scenes written before presets
        // Defaults for anything the file doesn't carry: from the named preset when
        // the file names one, from the kind when it doesn't. A scene saved before
        // presets existed has a style of its own and must not have half of
        // someone else's grafted onto it -- but one that says "battlement" and
        // then gains a field should get the battlement's value for it, not the
        // plain wall's.
        p.style  = (pr >= 0) ? splinegen::preset(p.preset)
                             : splinegen::preset(p.kind);
        if (e.contains("style")) readStyle(e["style"], p.style);
        if (e.contains("points"))
            for (const auto& q : e["points"])
                if (q.is_array() && q.size() == 2)
                    p.points.emplace_back(q[0].get<float>(), q[1].get<float>());
        if (e.contains("lifts"))
            for (const auto& v : e["lifts"]) p.lifts.push_back(v.get<float>());
        p.lifts.resize(p.points.size(), 0.0f);
        paths.push_back(std::move(p));
    }
    m_built.assign(paths.size(), Built{});
    m_runs.resize(paths.size());
    touch();
}
