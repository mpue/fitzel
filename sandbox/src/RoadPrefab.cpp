#include "RoadPrefab.hpp"

#include <utility>

#include <glm/gtc/constants.hpp>

#include "Component.hpp"
#include "PrefabSystem.hpp"
#include "SceneTypes.hpp"

namespace roadprefab {

roadside::Line asLine(const Settings& s) {
    roadside::Line L;
    L.enabled  = true;
    L.kind     = s.onRoad ? roadside::Kind::OnRoad : roadside::Kind::Post;
    L.model    = "prefab";   // only has to be non-empty, see the header
    L.onRoad   = s.onRoad;
    L.side     = s.side;
    L.offset   = s.offset;
    L.spacing  = s.spacing;
    L.yaw      = s.yaw;
    L.scale    = s.scale;
    L.lift     = s.lift;
    L.sink     = 0.0f;       // a prefab decides its own footing; don't bury it
    L.faceRoad = false;      // no one-sided profile to turn toward the road
    L.knockable = false;     // the prefab's own Physics components decide that
    return L;
}

void scaleInstance(std::vector<Entity>& inst, float scale) {
    if (scale == 1.0f || scale <= 0.0f) return;
    for (Entity& e : inst) {
        // The root is the anchor: its position is where the stamp put it, so only
        // its size scales. A child's offset from its parent scales with it, or
        // the subtree would grow while staying pinned to the root.
        // (localCenter is the source of truth; the world `center` is refilled by
        // resolveHierarchy on the next frame, so it is not touched here.)
        if (e.parent >= 0) e.localCenter *= scale;
        e.half *= scale;
        for (auto& c : e.components.items)
            if (auto* mc = dynamic_cast<ModelComponent*>(c.get())) mc->scale *= scale;
    }
}

std::vector<Entity> stamp(const prefab::Prefab& p,
                          const std::vector<roadside::Instance>& at,
                          int& entityCounter) {
    std::vector<Entity> out;
    if (p.entities.empty() || at.empty()) return out;
    out.reserve(p.entities.size() * at.size());
    for (const roadside::Instance& in : at) {
        // The instance already carries the line's extra yaw and its height
        // offset; instantiate() takes degrees.
        std::vector<Entity> one =
            prefab::instantiate(p, entityCounter, in.pos, glm::degrees(in.yaw));
        scaleInstance(one, in.scale);
        for (Entity& e : one) out.push_back(std::move(e));
    }
    return out;
}

} // namespace roadprefab
