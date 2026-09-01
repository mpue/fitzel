#include "RoadSet.hpp"

#include <utility>

#include <nlohmann/json.hpp>

#include "SandboxMath.hpp"   // kLineBreak

RoadSet::RoadSet(fitzel::Shader& lit, fitzel::AssetDatabase& assetDb,
                 fitzel::TerrainStreamer& streamer, std::string texDir)
    : m_lit(lit), m_assetDb(assetDb), m_streamer(streamer),
      m_texDir(std::move(texDir)) {
    // The scene always has a road to edit, even before the author draws one --
    // see the header comment. Named here rather than in add() because add() is
    // an authored gesture and this one is not.
    emplace().name = "Road";
    relist();
}

RoadSystem& RoadSet::emplace() {
    Slot s;
    s.road = std::make_unique<RoadSystem>(m_lit, m_assetDb, m_streamer, m_texDir);
    s.id   = m_nextId++;
    RoadSystem& r = *s.road;
    m_slots.push_back(std::move(s));
    if (onCreate) onCreate(r);
    return r;
}

void RoadSet::relist() {
    m_live.clear();
    for (const Slot& s : m_slots)
        if (s.alive) m_live.push_back(s.road.get());
    m_sel = clampIndex(m_sel);
}

int RoadSet::add(const std::string& name) {
    RoadSystem& r = emplace();
    r.name = name.empty() ? ("Road " + std::to_string(m_slots.back().id)) : name;
    relist();
    m_sel = static_cast<int>(m_live.size()) - 1;
    return m_sel;
}

bool RoadSet::remove(int i) {
    if (m_live.size() < 2) return false;          // there is always one road
    if (i < 0 || i >= static_cast<int>(m_live.size())) return false;
    RoadSystem* victim = m_live[i];
    for (Slot& s : m_slots)
        if (s.road.get() == victim) s.alive = false;
    relist();
    m_sel = clampIndex(i);                        // the next road down the list
    return true;
}

int RoadSet::idAt(int i) const {
    if (i < 0 || i >= static_cast<int>(m_live.size())) return -1;
    const RoadSystem* r = m_live[i];
    for (const Slot& s : m_slots)
        if (s.road.get() == r) return s.id;
    return -1;
}

int RoadSet::indexOfId(int id) const {
    for (int i = 0; i < static_cast<int>(m_live.size()); ++i)
        if (idAt(i) == id) return i;
    return -1;
}

void RoadSet::setAlive(int id, bool alive) {
    for (Slot& s : m_slots)
        if (s.id == id) s.alive = alive;
    relist();
}

bool RoadSet::buildAll(fitzel::TerrainEditField& edit, glm::vec2& outMin,
                       glm::vec2& outMax) {
    bool any = false;
    for (RoadSystem* r : m_live) {
        glm::vec2 mn, mx;
        if (!r->build(edit, mn, mx)) continue;
        if (!any) { outMin = mn; outMax = mx; any = true; }
        else      { outMin = glm::min(outMin, mn); outMax = glm::max(outMax, mx); }
    }
    return any;
}

void RoadSet::rebuildMeshes() {
    for (RoadSystem* r : m_live) r->rebuildMesh();
}

void RoadSet::rebuildSideObjects() {
    for (RoadSystem* r : m_live) r->rebuildSideObjects();
}

void RoadSet::refreshTextures(const std::string& projectDir) {
    // Every slot, dead ones included: a road that comes back through undo after
    // the project changed would otherwise show the old project's picker.
    for (const Slot& s : m_slots) s.road->refreshTextures(projectDir);
}

bool RoadSet::anyNeedsBuild() const {
    for (const RoadSystem* r : m_live)
        if (r->needsBuild) return true;
    return false;
}

void RoadSet::markNeedsBuild() {
    for (RoadSystem* r : m_live) r->needsBuild = true;
}

bool RoadSet::surfaceHeightAt(const glm::vec2& xz, float& outY, float maxY) const {
    bool  hit  = false;
    float best = 0.0f;
    for (const RoadSystem* r : m_live) {
        if (!r->enabled) continue;
        float y = 0.0f;
        if (!r->surfaceHeightAt(xz, r->surfaceHalf(), y, maxY)) continue;
        if (!hit || y > best) { best = y; hit = true; }
    }
    if (hit) outY = best;
    return hit;
}

std::vector<glm::vec2> RoadSet::centerlines() const {
    std::vector<glm::vec2> out;
    for (const RoadSystem* r : m_live) {
        const std::vector<glm::vec2>& cl = r->centerline();
        if (cl.size() < 2) continue;
        if (!out.empty()) out.push_back(kLineBreak);
        out.insert(out.end(), cl.begin(), cl.end());
    }
    return out;
}

float RoadSet::maxWidth() const {
    float w = 0.0f;
    for (const RoadSystem* r : m_live) w = glm::max(w, r->width);
    return w;
}

void RoadSet::save(nlohmann::json& j) const {
    nlohmann::json arr = nlohmann::json::array();
    for (const RoadSystem* r : m_live) {
        nlohmann::json one;
        r->save(one);
        arr.push_back(std::move(one));
    }
    j["roads"] = std::move(arr);
    // ...and the first road where a build that predates this class looks for the
    // only one it knows about. A scene made here still opens there, minus the
    // roads that version could not have held anyway.
    if (!m_live.empty()) m_live.front()->save(j["road"]);
}

RoadSystem& RoadSet::slotForLoad(int n) {
    // Reuse rather than recreate: a RoadSystem* handed to the undo history has to
    // stay valid, and a scene load is exactly when it is tempting to throw them
    // all away.
    if (n < static_cast<int>(m_slots.size())) {
        m_slots[n].alive = true;
        return *m_slots[n].road;
    }
    return emplace();
}

void RoadSet::load(const nlohmann::json& j) {
    int n = 0;
    if (j.contains("roads") && j["roads"].is_array()) {
        for (const nlohmann::json& one : j["roads"]) {
            if (!one.is_object()) continue;
            slotForLoad(n++).load(one);
        }
    } else if (j.contains("road") && j["road"].is_object()) {
        // A scene from before roads could be plural: one road, as it was saved.
        slotForLoad(n++).load(j["road"]);
    }
    if (n == 0) { clear(); return; }
    // Whatever the scene did not fill goes dead -- its RoadSystem stays put for
    // the history's sake, but it is not in this scene.
    for (int i = n; i < static_cast<int>(m_slots.size()); ++i) m_slots[i].alive = false;
    for (int i = 0; i < n; ++i) {
        m_slots[i].alive = true;
        // A road saved before roads had names loads as the road it is.
        if (m_slots[i].road->name.empty())
            m_slots[i].road->name = "Road " + std::to_string(i + 1);
    }
    relist();
    m_sel = 0;
}

void RoadSet::clear() {
    for (int i = 0; i < static_cast<int>(m_slots.size()); ++i)
        m_slots[i].alive = (i == 0);
    if (m_slots.empty()) emplace();
    RoadSystem& first = *m_slots.front().road;
    first.clearPoints();
    first.bridges.clear();
    first.tunnels.clear();
    first.loops.clear();
    first.sideLines.clear();
    first.biomes.clear();
    first.decals.clear();
    first.closed     = false;
    first.name       = "Road";
    first.needsBuild = false;
    first.rebuildSideObjects();
    first.rebuildCity();
    first.rebuildDecals();
    relist();
    m_sel = 0;
}
