#include "SceneGraph.hpp"

#include <algorithm>
#include <functional>
#include <unordered_set>

#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>      // ImGuizmo.h leans on ImGui's types; must come first
#include <ImGuizmo.h>

namespace scenegraph {

glm::mat4 compose(const glm::vec3& t, const glm::vec3& rotDeg, const glm::vec3& s) {
    const float tt[3] = {t.x, t.y, t.z};
    const float rr[3] = {rotDeg.x, rotDeg.y, rotDeg.z};
    const float ss[3] = {s.x, s.y, s.z};
    float m[16];
    ImGuizmo::RecomposeMatrixFromComponents(tt, rr, ss, m);
    return glm::make_mat4(m);
}

void decompose(const glm::mat4& m, glm::vec3& t, glm::vec3& rotDeg, glm::vec3& s) {
    float tt[3], rr[3], ss[3];
    ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(m), tt, rr, ss);
    t      = glm::vec3(tt[0], tt[1], tt[2]);
    rotDeg = glm::vec3(rr[0], rr[1], rr[2]);
    s      = glm::vec3(ss[0], ss[1], ss[2]);
}

void resolve(std::vector<Entity>& entities) {
    auto find = [&](int id) -> Entity* {
        for (Entity& e : entities)
            if (e.id == id) return &e;
        return nullptr;
    };

    // Depth first through the parent chain, with a visited set doing two jobs:
    // it keeps a parent from being resolved once per child, and it is what stops
    // a cycle -- a scene file can always claim a loop, and this walk must end
    // whatever it is handed.
    std::unordered_set<int> done;
    std::function<void(Entity&)> one = [&](Entity& e) {
        if (!done.insert(e.id).second) return;
        Entity* p = (e.parent >= 0) ? find(e.parent) : nullptr;
        if (p) one(*p);
        // Effective visibility: off if this object OR any ancestor is off.
        e.activeInHierarchy = e.active && (!p || p->activeInHierarchy);
        if (!p) {
            e.center   = e.localCenter;
            e.rotation = e.localRotation;
        } else {
            const glm::mat4 w = compose(p->center, p->rotation, glm::vec3(1.0f)) *
                                compose(e.localCenter, e.localRotation, glm::vec3(1.0f));
            glm::vec3 scale;
            decompose(w, e.center, e.rotation, scale);
        }
    };
    for (Entity& e : entities) one(e);
}

glm::mat4 worldOf(const Entity& e) {
    return compose(e.center, e.rotation, glm::vec3(1.0f));
}

glm::mat4 parentWorld(const std::vector<Entity>& entities, const Entity& e) {
    if (e.parent < 0) return glm::mat4(1.0f);
    for (const Entity& p : entities)
        if (p.id == e.parent) return worldOf(p);
    return glm::mat4(1.0f);
}

void setWorld(Entity& e, const glm::vec3& worldPos, const glm::vec3& worldRotDeg,
              const glm::mat4* parentWorld) {
    e.center = worldPos; e.rotation = worldRotDeg;
    if (!parentWorld) { e.localCenter = worldPos; e.localRotation = worldRotDeg; return; }
    const glm::mat4 lm =
        glm::inverse(*parentWorld) * compose(worldPos, worldRotDeg, glm::vec3(1.0f));
    glm::vec3 scale;
    decompose(lm, e.localCenter, e.localRotation, scale);
}

std::vector<int> subtree(const std::vector<Entity>& entities, int rootId) {
    std::vector<int> ids{rootId};
    for (bool grew = true; grew; ) {
        grew = false;
        for (const Entity& e : entities) {
            const bool have     = std::find(ids.begin(), ids.end(), e.id) != ids.end();
            const bool parentIn = std::find(ids.begin(), ids.end(), e.parent) != ids.end();
            if (!have && parentIn) { ids.push_back(e.id); grew = true; }
        }
    }
    return ids;
}

} // namespace scenegraph
