#include "CitySystem.hpp"

#include <algorithm>
#include <chrono>

#include <nlohmann/json.hpp>

#include <fitzel/world/Terrain.hpp>

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
    ctx.modelBounds = modelBounds;

    for (std::size_t i = 0; i < towns.size(); ++i) {
        Built& b = m_built[i];
        if (!b.dirty) continue;
        b.dirty = false;
        b.town.clear();
        b.meshes.clear();
        if (!towns[i].enabled) continue;
        const auto t0 = std::chrono::steady_clock::now();
        const cityplan::Palettes pal = cityplan::ensurePalettes(materials, towns[i]);
        m_civic    = pal.civic;
        m_hasCivic = true;
        m_litWindows = pal.houseGlassLit;
        m_litWindows.insert(m_litWindows.end(), {pal.props.glowWhite, pal.props.glowWarm, pal.civic.neon});
        // Palettes may name textures by GUID (see civic::ensurePalette); load
        // the pixels the first time a material asks for them.
        if (loadTexture)
            for (MaterialDef& m : materials) {
                if (m.texId.valid() && !m.tex) m.tex = loadTexture(m.texId);
                if (m.normalTexId.valid() && !m.normalTex) m.normalTex = loadTexture(m.normalTexId);
            }
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
    ++m_revision;
}

void CitySystem::forEachDraw(const glm::vec3& eye,
                             const std::function<void(const fitzel::Mesh&, const fitzel::AssetId&,
                                                      bool castsShadow)>& draw) const {
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
            draw(b.meshes[i], batches[i].material, batches[i].castsShadow);
        }
    }
}

void CitySystem::forEachCollider(const std::function<void(const city::Piece&)>& fn) const {
    for (std::size_t t = 0; t < m_built.size() && t < towns.size(); ++t) {
        if (!towns[t].enabled) continue;
        for (const city::Piece& pc : m_built[t].town.district.colliders) fn(pc);
    }
}

void CitySystem::forEachModel(
    const glm::vec3& eye, const std::function<void(const std::string&, const glm::mat4&)>& fn) const {
    const float r2 = range * range;
    for (std::size_t t = 0; t < m_built.size() && t < towns.size(); ++t) {
        if (!towns[t].enabled) continue;
        for (const cityplan::ModelPlacement& m : m_built[t].town.models) {
            const glm::vec3 d = glm::vec3(m.transform[3]) - eye;
            if (glm::dot(d, d) > r2) continue;
            fn(m.model, m.transform);
        }
    }
}

void CitySystem::forEachSignalLamp(
    double t, const std::function<void(const fitzel::AssetId&, float)>& fn) const {
    if (!m_hasCivic) return;
    civic::SignalLamps lit[2];
    civic::signalPhase(t, lit[0], lit[1]);
    for (int a = 0; a < 2; ++a) {
        fn(m_civic.signalRed[a],   lit[a].red   ? civic::kSignalGlow : 0.0f);
        fn(m_civic.signalAmber[a], lit[a].amber ? civic::kSignalGlow : 0.0f);
        fn(m_civic.signalGreen[a], lit[a].green ? civic::kSignalGlow : 0.0f);
    }
}

float CitySystem::kerbReach() const {
    float reach = 0.0f;
    for (const cityplan::Rule& r : towns) {
        if (!r.enabled || !r.pavements) continue;
        const cityplan::Grid& g = r.built();
        reach = std::max(reach, 0.5f * std::max(g.streetWidth, g.avenueWidth) + r.sidewalk + 0.5f);
    }
    return reach;
}

std::vector<cityplan::Bare> CitySystem::bareGround() const {
    std::vector<cityplan::Bare> out;
    for (std::size_t t = 0; t < m_built.size() && t < towns.size(); ++t)
        if (towns[t].enabled)
            out.insert(out.end(), m_built[t].town.bare.begin(), m_built[t].town.bare.end());
    return out;
}

std::vector<glm::vec3> CitySystem::clearings() const {
    std::vector<glm::vec3> out;
    for (std::size_t t = 0; t < m_built.size() && t < towns.size(); ++t) {
        if (!towns[t].enabled) continue;
        for (const cityplan::Placed& p : m_built[t].town.placed)
            out.push_back({p.pos.x, p.pos.y, p.radius});
        for (const cityplan::Placed& p : m_built[t].town.furniture)
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
    retire(ids);
    return ids;
}

void CitySystem::retire(const std::vector<int>& roadIds) {
    if (roadIds.empty()) return;
    m_retired.insert(m_retired.end(), roadIds.begin(), roadIds.end());
    requestBuild();
}

bool CitySystem::releaseRetired(RoadSet& roads, fitzel::TerrainEditField& edit,
                                const std::function<float(std::int64_t)>& keep,
                                glm::vec2& outMin, glm::vec2& outMax) {
    bool any = false;
    std::vector<int> ids;
    ids.swap(m_retired);
    for (int id : ids) {
        if (roads.indexOfId(id) >= 0) continue;   // back in the scene: Build re-cuts it
        RoadSystem* r = roads.byId(id);
        glm::vec2 mn, mx;
        if (!r || !r->releaseCorridor(edit, keep, mn, mx)) continue;
        if (!any) { outMin = mn; outMax = mx; any = true; }
        else      { outMin = glm::min(outMin, mn); outMax = glm::max(outMax, mx); }
    }
    return any;
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
    // The streets being replaced set the look of the new ones -- a surface the
    // author gave them ("Same look on all roads") survives a re-lay. Found by id
    // before anything is added, so the index cannot shift under it.
    int lookId = -1;
    for (int k = 0; k < roads.count() && lookId < 0; ++k)
        if (roads.at(k).cityId == r.id) lookId = roads.idAt(k);
    const cityplan::Layout L = cityplan::layout(r, r.laid);
    int avenueN = 0, streetN = 0;
    for (const cityplan::Street& s : L.streets) {
        const std::vector<cityplan::StreetRun> runs = cityplan::streetRuns(r.laid, s, isWater);
        if (runs.size() > 1) out.breaks += static_cast<int>(runs.size()) - 1;
        // The street's own name, the one its signs carry. A street broken by
        // wide water keeps it on both halves -- it is still the one street.
        const std::string base = s.name.empty()
            ? r.name + (s.avenue ? " Avenue " : " Street ") +
                  std::to_string(s.avenue ? ++avenueN : ++streetN)
            : s.name;
        for (std::size_t k = 0; k < runs.size(); ++k) {
            const cityplan::StreetRun& run = runs[k];
            const std::string& name = base;
            const int idx = roads.add(name);
            RoadSystem& road = roads.at(idx);
            road.width     = run.width;
            road.edgeWidth = 0.0f;
            road.shoulder  = 2.5f;
            // The pavement lies on level ground at the road's own height; the
            // shoulder eases back to the terrain only behind it.
            road.bed       = towns[static_cast<std::size_t>(i)].pavements
                                 ? std::max(towns[static_cast<std::size_t>(i)].sidewalk, 0.0f) + 0.5f
                                 : 0.0f;
            RoadSystem::Shape sh;
            sh.points = run.pts;
            sh.lifts.assign(run.pts.size(), 0.0f);
            sh.banks.assign(run.pts.size(), 0.0f);
            for (const auto& [a, b] : run.bridges) sh.bridges.push_back({a, b});
            sh.label = name;
            road.setShape(sh);
            if (const RoadSystem* look = lookId >= 0 ? roads.byId(lookId) : nullptr)
                road.copyLookFrom(*look);
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
