#include "CitySystem.hpp"

#include <algorithm>
#include <chrono>

#include <nlohmann/json.hpp>

#include "RoadSet.hpp"

int CitySystem::add(cityplan::Rule r) {
    r.id = m_nextId++;
    towns.push_back(std::move(r));
    sync();
    m_built.back().dirty = true;
    return count() - 1;
}

void CitySystem::erase(int i) {
    if (i < 0 || i >= count()) return;
    towns.erase(towns.begin() + i);
    m_built.erase(m_built.begin() + i);
}

void CitySystem::sync() {
    if (m_built.size() != towns.size()) {
        m_built.resize(towns.size());
        for (Built& b : m_built) b.dirty = true;
    }
}

void CitySystem::markDirty() {
    sync();
    for (Built& b : m_built) b.dirty = true;
}

void CitySystem::markDirty(int i) {
    sync();
    if (i >= 0 && i < count()) m_built[static_cast<std::size_t>(i)].dirty = true;
}

void CitySystem::update(std::vector<MaterialDef>& materials) {
    sync();
    bool any = false;
    for (const Built& b : m_built) any |= b.dirty;
    if (!any) return;

    // The roads are asked once for every town that re-derives this frame.
    cityplan::Context ctx;
    ctx.groundAt = groundAt;
    ctx.isWater  = isWater;
    if (roadLines) ctx.roads = roadLines();

    for (std::size_t i = 0; i < towns.size(); ++i) {
        Built& b = m_built[i];
        if (!b.dirty) continue;
        b.dirty = false;
        b.town.clear();
        b.meshes.clear();
        if (!towns[i].enabled) continue;
        const auto t0 = std::chrono::steady_clock::now();
        const cityplan::Palettes pal = cityplan::ensurePalettes(materials, towns[i]);
        b.town = cityplan::derive(towns[i], pal, ctx);
        // Upload and drop the CPU copy, as the roadside city does: a town's
        // vertices are tens of megabytes nothing reads again.
        b.meshes.reserve(b.town.district.batches.size());
        for (city::Batch& batch : b.town.district.batches) {
            b.meshes.push_back(fitzel::Mesh::create(batch.data));
            batch.data = fitzel::MeshData{};
        }
        b.ms = std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - t0).count();
    }
}

void CitySystem::forEachDraw(const glm::vec3& eye,
                             const std::function<void(const fitzel::Mesh&,
                                                      const fitzel::AssetId&)>& draw) const {
    const float r2 = range * range;
    for (std::size_t t = 0; t < m_built.size() && t < towns.size(); ++t) {
        if (!towns[t].enabled) continue;
        const Built& b = m_built[t];
        const auto& batches = b.town.district.batches;
        for (std::size_t i = 0; i < batches.size() && i < b.meshes.size(); ++i) {
            // Nearest point of the chunk's box, so a big chunk is not dropped
            // because its centre happens to be far.
            const glm::vec3 d = glm::max(glm::max(batches[i].lo - eye, eye - batches[i].hi),
                                         glm::vec3(0.0f));
            if (glm::dot(d, d) > r2) continue;
            draw(b.meshes[i], batches[i].material);
        }
    }
}

void CitySystem::forEachCollider(const std::function<void(const city::Piece&)>& fn) const {
    for (std::size_t t = 0; t < m_built.size() && t < towns.size(); ++t) {
        if (!towns[t].enabled) continue;
        for (const city::Piece& pc : m_built[t].town.district.colliders) fn(pc);
    }
}

std::vector<glm::vec3> CitySystem::clearings() const {
    std::vector<glm::vec3> out;
    for (std::size_t t = 0; t < m_built.size() && t < towns.size(); ++t) {
        if (!towns[t].enabled) continue;
        for (const cityplan::Placed& p : m_built[t].town.placed)
            out.push_back({p.pos.x, p.pos.y, p.radius});
    }
    return out;
}

// --- Streets ---------------------------------------------------------------------

int CitySystem::streetCount(int townId, const RoadSet& roads) const {
    int n = 0;
    for (const RoadSystem* r : roads)
        if (r->cityId == townId) ++n;
    return n;
}

std::vector<int> CitySystem::removeStreets(int townId, RoadSet& roads,
                                           std::vector<int>* added) {
    std::vector<int> ids;
    for (int i = 0; i < roads.count(); ++i)
        if (roads.at(i).cityId == townId) ids.push_back(roads.idAt(i));
    if (ids.empty()) return ids;
    // There is always one road (see RoadSet): if the town's streets are all
    // there is, leave an empty one behind for the editor to draw into.
    if (roads.count() - static_cast<int>(ids.size()) < 1) {
        const int idx = roads.add();
        if (added) added->push_back(roads.idAt(idx));
    }
    for (int id : ids) roads.setAlive(id, false);
    return ids;
}

CitySystem::Laid CitySystem::layStreets(int i, RoadSet& roads) {
    Laid out;
    if (i < 0 || i >= count()) return out;
    // Laying is what makes the draft real: from here on the buildings follow it.
    cityplan::Rule& r = towns[static_cast<std::size_t>(i)];
    r.laid    = r.grid;
    r.hasLaid = true;
    const int keepSel = roads.selected();

    // New streets first, THEN the old ones out: removeStreets would otherwise
    // have to add an empty placeholder road whenever the town is all there is.
    std::vector<int> fresh;
    const cityplan::Layout L = cityplan::layout(r, r.laid);
    int avenueN = 0, streetN = 0;
    for (const cityplan::Street& s : L.streets) {
        const std::vector<cityplan::StreetRun> runs = cityplan::streetRuns(r.laid, s, isWater);
        if (runs.size() > 1) out.breaks += static_cast<int>(runs.size()) - 1;
        const std::string base = r.name + (s.avenue ? " \xC2\xB7 Avenue " : " \xC2\xB7 Street ") +
                                 std::to_string(s.avenue ? ++avenueN : ++streetN);
        for (std::size_t k = 0; k < runs.size(); ++k) {
            const cityplan::StreetRun& run = runs[k];
            const std::string name = runs.size() > 1
                ? base + static_cast<char>('a' + static_cast<int>(k)) : base;
            const int idx = roads.add(name);
            RoadSystem& road = roads.at(idx);
            road.width     = run.width;
            road.edgeWidth = 0.0f;
            road.shoulder  = 2.5f;
            RoadSystem::Shape sh;
            sh.points = run.pts;
            sh.lifts.assign(run.pts.size(), 0.0f);
            sh.banks.assign(run.pts.size(), 0.0f);
            for (const auto& [a, b] : run.bridges) sh.bridges.push_back({a, b});
            sh.label = name;
            road.setShape(sh);
            fresh.push_back(roads.idAt(idx));
            out.bridges += static_cast<int>(run.bridges.size());
        }
        ++out.streets;
    }
    // Tag after laying, so the removal below does not take the new ones too.
    const std::vector<int> old = removeStreets(r.id, roads, &out.added);
    for (int id : fresh) {
        const int idx = roads.indexOfId(id);
        if (idx >= 0) roads.at(idx).cityId = r.id;
    }
    out.removed = old;
    out.added.insert(out.added.end(), fresh.begin(), fresh.end());
    roads.select(keepSel);
    markDirty(i);
    requestBuild();
    return out;
}

// --- Undo ------------------------------------------------------------------------

void CitySystem::restore(const Snapshot& s) {
    towns    = s.towns;
    m_nextId = s.nextId;
    m_built.clear();
    sync();
}

// --- Persistence -------------------------------------------------------------------

void CitySystem::save(nlohmann::json& j) const {
    nlohmann::json arr = nlohmann::json::array();
    for (const cityplan::Rule& r : towns) {
        nlohmann::json one;
        cityplan::save(one, r);
        arr.push_back(std::move(one));
    }
    j = {{"towns", std::move(arr)}, {"nextId", m_nextId}, {"range", range}};
}

void CitySystem::load(const nlohmann::json& j) {
    clear();
    if (j.contains("towns") && j["towns"].is_array())
        for (const nlohmann::json& one : j["towns"]) {
            if (!one.is_object()) continue;
            cityplan::Rule r;
            cityplan::load(one, r);
            towns.push_back(std::move(r));
        }
    m_nextId = j.value("nextId", 1);
    for (const cityplan::Rule& r : towns) m_nextId = std::max(m_nextId, r.id + 1);
    range = j.value("range", range);
    sync();
}

void CitySystem::clear() {
    towns.clear();
    m_built.clear();
    m_nextId = 1;
}
