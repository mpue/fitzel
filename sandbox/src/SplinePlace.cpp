#include "SplinePlace.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

#include "PrefabSystem.hpp"
#include "RoadPrefab.hpp"     // scaleInstance
#include "SceneTypes.hpp"
#include "SplineGen.hpp"
#include "SplineSystem.hpp"

namespace splineplace {
namespace {

// A plain copy of `t` with fresh ids, rooted at `pos` and turned by `yawDeg` --
// prefab::instantiate without the prefab tag, so whatever links the template
// carried (or didn't) come along unchanged.
std::vector<Entity> copyOf(const prefab::Prefab& t, int& entityCounter,
                           const glm::vec3& pos, float yawDeg) {
    std::unordered_map<int, int> remap;
    remap.reserve(t.entities.size());
    for (const Entity& src : t.entities) remap[src.id] = entityCounter++;

    std::vector<Entity> out;
    out.reserve(t.entities.size());
    for (const Entity& src : t.entities) {
        Entity e = src;                  // deep-clones the component list
        e.id = remap[src.id];
        if (src.parent < 0) {
            e.parent        = -1;
            e.localCenter   = e.center = pos;
            e.localRotation.y += yawDeg;
            e.rotation      = e.localRotation;
        } else {
            const auto it = remap.find(src.parent);
            e.parent = it != remap.end() ? it->second : -1;
        }
        out.push_back(std::move(e));
    }
    return out;
}

} // namespace

float spacingOn(const SplineSystem& sp, int path, const Settings& s) {
    const float want = std::max(s.spacing, 0.1f);
    if (path < 0 || path >= static_cast<int>(sp.paths.size()) || !sp.paths[path].closed ||
        path >= static_cast<int>(sp.runs().size()))
        return want;
    const float len = sp.runs()[path].geo.length;
    if (len <= 0.0f) return want;
    const float n = std::max(1.0f, std::round(len / want));
    return len / n;
}

std::vector<Spot> spots(const SplineSystem& sp, int path, const Settings& s,
                        int maxCount) {
    std::vector<Spot> out;
    if (path < 0 || path >= static_cast<int>(sp.paths.size()) || maxCount <= 0)
        return out;
    // On a loop, "First at" only turns the pattern round the ring: the copies
    // still go all the way round, they just start somewhere else.
    const bool  closed  = sp.paths[path].closed;
    const float spacing = spacingOn(sp, path, s);
    const float start   = closed ? std::fmod(std::max(s.start, 0.0f), spacing) : s.start;
    const std::vector<splinegen::Station> st =
        splinegen::stations(sp.line(path), closed, spacing, start, maxCount);
    // "Both sides" only means something off the centreline.
    const int sides = (s.bothSides && s.side != 0.0f) ? 2 : 1;
    out.reserve(st.size() * sides);
    for (const splinegen::Station& q : st) {
        for (int k = 0; k < sides; ++k) {
            const float off = (k == 0) ? s.side : -s.side;
            Spot o;
            o.pos = q.p + q.right * off;
            // Off the line, stand on the ground THERE -- a lamp three metres to
            // the side of a path on a slope would otherwise hang in the air or
            // stand in the hill. The path's own lift above its ground is kept.
            if (off != 0.0f && sp.groundAt)
                o.pos.y += sp.groundAt(o.pos.x, o.pos.z) - sp.groundAt(q.p.x, q.p.z);
            o.pos.y += s.height;
            o.yawDeg = (s.align ? glm::degrees(q.yaw) : 0.0f) + s.turn;
            // The mirrored copy faces the other way, so a pair of lamps both
            // lean over the path rather than one over it and one away from it.
            if (k == 1 && s.align) o.yawDeg += 180.0f;
            out.push_back(o);
            if (static_cast<int>(out.size()) >= maxCount) return out;
        }
    }
    return out;
}

prefab::Prefab fromScene(const std::vector<Entity>& scene, int rootId) {
    prefab::Prefab t;   // guid left invalid: not a prefab file
    auto find = [&](int id) -> const Entity* {
        for (const Entity& e : scene)
            if (e.id == id) return &e;
        return nullptr;
    };
    const Entity* root = find(rootId);
    if (!root) return t;
    t.name = root->name;

    // Breadth-first, so every parent precedes its children -- the order
    // AddEntitiesCmd needs and prefab::instantiate assumes.
    std::vector<int> ids{rootId};
    for (std::size_t k = 0; k < ids.size(); ++k)
        for (const Entity& e : scene)
            if (e.parent == ids[k]) ids.push_back(e.id);
    std::unordered_map<int, int> local;
    for (int id : ids) local.emplace(id, static_cast<int>(local.size()));

    for (int id : ids) {
        const Entity* src = find(id);
        if (!src) continue;
        Entity e = *src;
        e.id = local[id];
        if (id == rootId) {
            // The root's parent is left behind, so what it had as a WORLD
            // transform becomes its own.
            e.parent        = -1;
            e.localCenter   = src->center;
            e.localRotation = src->rotation;
        } else {
            e.parent = local[src->parent];
        }
        t.entities.push_back(std::move(e));
    }
    return t;
}

std::vector<Entity> stamp(const prefab::Prefab& t, const std::vector<Spot>& at,
                          float scale, int& entityCounter) {
    std::vector<Entity> out;
    if (t.entities.empty() || at.empty()) return out;
    out.reserve(t.entities.size() * at.size());
    for (const Spot& s : at) {
        std::vector<Entity> one = t.guid.valid()
            ? prefab::instantiate(t, entityCounter, s.pos, s.yawDeg)
            : copyOf(t, entityCounter, s.pos, s.yawDeg);
        roadprefab::scaleInstance(one, scale);
        for (Entity& e : one) out.push_back(std::move(e));
    }
    return out;
}

} // namespace splineplace
