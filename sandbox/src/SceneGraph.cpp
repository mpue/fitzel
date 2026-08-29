#include "SceneGraph.hpp"

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

} // namespace scenegraph
