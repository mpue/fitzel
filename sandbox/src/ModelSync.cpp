#include "ModelSync.hpp"

#include "Component.hpp"
#include "ModelLibrary.hpp"
#include "SceneTypes.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

namespace modelsync {

namespace {

bool differs(const glm::vec3& a, const glm::vec3& b, float eps) {
    return glm::any(glm::greaterThan(glm::abs(a - b), glm::vec3(eps)));
}

// The value most of `v` agree on, to a centimetre: the offset between file
// and scene that the parts nobody touched share.
glm::vec3 consensus(const std::vector<glm::vec3>& v) {
    if (v.empty()) return glm::vec3(0.0f);
    std::map<std::tuple<long, long, long>, std::pair<int, glm::vec3>> votes;
    for (const glm::vec3& p : v) {
        auto& slot = votes[{std::lround(p.x * 100.0f), std::lround(p.y * 100.0f),
                            std::lround(p.z * 100.0f)}];
        ++slot.first;
        slot.second = p;
    }
    const auto best = std::max_element(votes.begin(), votes.end(),
        [](const auto& a, const auto& b) { return a.second.first < b.second.first; });
    return best->second.second;
}

} // namespace

Report syncAll(std::vector<Entity>& entities, int& entityCounter, ModelLibrary& models,
               fitzel::AssetDatabase& assetDb, std::vector<MaterialDef>& materials) {
    Report rep;

    // The groups: parts of the same file under the same parent.
    std::map<std::pair<std::string, int>, std::vector<int>> groups;   // -> entity ids
    for (const Entity& e : entities) {
        const auto* mc = e.components.get<ModelComponent>();
        if (!mc || mc->modelPath.empty()) continue;
        if (mc->nodeIndex < 0 && mc->nodeName.empty()) continue;   // a whole model
        groups[{mc->modelPath, e.parent}].push_back(e.id);
    }

    std::set<int> toRemove;
    std::vector<Entity> toAdd;
    std::set<int> parents;   // ids that are some entity's parent
    for (const Entity& e : entities)
        if (e.parent >= 0) parents.insert(e.parent);
    const auto byId = [&entities](int id) -> Entity* {
        for (Entity& e : entities)
            if (e.id == id) return &e;
        return nullptr;
    };

    for (auto& [key, ids] : groups) {
        const std::string& path = key.first;
        const std::vector<fitzel::ModelNode>& ns = models.nodes(path, true);
        if (ns.empty()) continue;   // file missing or unreadable: leave the scene alone
        ++rep.groups;

        std::unordered_map<std::string, std::vector<int>> byName;
        for (int i = 0; i < static_cast<int>(ns.size()); ++i) byName[ns[i].name].push_back(i);
        std::vector<char> referenced(ns.size(), 0);
        // File centre minus scene centre, per part, after the match: what a
        // part new to the file is placed by.
        std::vector<glm::vec3> offsets;

        for (int id : ids) {
            Entity* e = byId(id);
            auto* mc = e->components.get<ModelComponent>();
            const bool hadIndex = mc->nodeIndex >= 0 &&
                                  mc->nodeIndex < static_cast<int>(ns.size());
            // Older scenes know the part only by position: take its name from
            // the file where the entity is called the same, else from the
            // position it had.
            if (mc->nodeName.empty()) {
                if (hadIndex && ns[mc->nodeIndex].name == e->name)
                    mc->nodeName = e->name;
                else if (byName.count(e->name))
                    mc->nodeName = e->name;
                else if (hadIndex)
                    mc->nodeName = ns[mc->nodeIndex].name;
            }
            int idx = -1;
            if (auto it = byName.find(mc->nodeName); it != byName.end()) {
                // A name the file uses twice is told apart by position.
                idx = it->second.front();
                for (int c : it->second)
                    if (c == mc->nodeIndex) idx = c;
            }
            if (idx < 0) {
                // Gone from the file. A plain part goes with it; one that
                // carries more than its mesh stays, drawing nothing.
                const bool plain = e->components.items.size() == 1 && !parents.count(e->id);
                if (plain) {
                    toRemove.insert(e->id);
                    rep.removed.push_back(e->name);
                } else {
                    mc->modelId = -1;
                    rep.kept.push_back(e->name);
                }
                continue;
            }
            referenced[idx] = 1;
            if (idx != mc->nodeIndex) ++rep.relinked;

            const int modelId = models.importNode(path, idx, true, assetDb, materials);
            const LoadedModel* lm = models.byId(modelId);
            const glm::vec3 newCenter = ns[idx].center;
            const glm::vec3 newHalf = lm ? 0.5f * lm->size() : mc->fileHalf;

            if (mc->fileRef) {
                // Only what changed in the file since the last reading moves
                // the part; where the editor put it stays the base.
                if (differs(newCenter, mc->fileCenter, 1e-4f)) {
                    e->localCenter += newCenter - mc->fileCenter;
                    ++rep.moved;
                }
                if (differs(newHalf, mc->fileHalf, 1e-4f)) {
                    for (int a = 0; a < 3; ++a) {
                        if (mc->fileHalf[a] > 1e-6f) e->half[a] *= newHalf[a] / mc->fileHalf[a];
                        else e->half[a] = newHalf[a];
                    }
                    ++rep.resized;
                }
            } else {
                ++rep.baselined;   // the first reading: the scene as it is is the base
            }
            mc->nodeIndex  = idx;
            mc->modelId    = modelId;
            mc->fileRef    = true;
            mc->fileCenter = newCenter;
            mc->fileHalf   = newHalf;
            offsets.push_back(newCenter - e->localCenter);
        }

        // Objects new to the file join the group where the file puts them.
        const glm::vec3 offset = consensus(offsets);
        for (int i = 0; i < static_cast<int>(ns.size()); ++i) {
            if (referenced[i]) continue;
            const int modelId = models.importNode(path, i, true, assetDb, materials);
            const LoadedModel* lm = models.byId(modelId);
            if (!lm) continue;
            Entity ch;
            ch.type   = EntityType::Model;
            ch.name   = ns[i].name.empty() ? ("part " + std::to_string(i)) : ns[i].name;
            ch.parent = key.second;
            auto mc = std::make_unique<ModelComponent>();
            mc->modelId    = modelId;
            mc->modelPath  = path;
            mc->nodeIndex  = i;
            mc->nodeName   = ns[i].name;
            mc->scale      = 1.0f;
            mc->fileRef    = true;
            mc->fileCenter = ns[i].center;
            mc->fileHalf   = 0.5f * lm->size();
            ch.components.items.push_back(std::move(mc));
            ch.half        = 0.5f * lm->size();
            ch.localCenter = ns[i].center - offset;
            ch.center      = ch.localCenter;   // world values follow at the next resolve
            ch.id          = entityCounter++;
            rep.added.push_back(ch.name);
            toAdd.push_back(std::move(ch));
        }
    }

    if (!toRemove.empty())
        entities.erase(std::remove_if(entities.begin(), entities.end(),
                                      [&](const Entity& e) { return toRemove.count(e.id) > 0; }),
                       entities.end());
    for (Entity& e : toAdd) entities.push_back(std::move(e));
    return rep;
}

std::string describe(const Report& r) {
    if (!r.changedScene() && r.kept.empty()) return {};
    std::string s = "Models matched to their files:";
    if (r.moved)            s += " " + std::to_string(r.moved) + " moved,";
    if (r.resized)          s += " " + std::to_string(r.resized) + " resized,";
    if (!r.added.empty())   s += " " + std::to_string(r.added.size()) + " added,";
    if (!r.removed.empty()) s += " " + std::to_string(r.removed.size()) + " removed,";
    if (!r.kept.empty())    s += " " + std::to_string(r.kept.size()) +
                                 " gone from the file but kept (they carry components),";
    if (r.relinked)         s += " " + std::to_string(r.relinked) + " found at a new position,";
    s.pop_back();
    s += ". Save the scene to keep it.";
    return s;
}

} // namespace modelsync
