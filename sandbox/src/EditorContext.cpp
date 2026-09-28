#include "EditorContext.hpp"

#include <memory>

#include "Component.hpp"
#include "EditMesh.hpp"
#include "SceneGraph.hpp"

std::vector<Entity> EditorContext::snapshot(const std::vector<int>& ids) const {
    std::vector<Entity> out;
    out.reserve(ids.size());
    for (int id : ids) if (const Entity* e = document.find(id)) out.push_back(*e);
    return out;
}

void EditorContext::commitEdit(std::vector<Entity> before, const std::vector<int>& ids,
                               const char* label) {
    auto cmd = std::make_unique<ModifyEntitiesCmd>(std::move(before), snapshot(ids), label);
    if (!cmd->trivial()) history.pushApplied(std::move(cmd));
}

void ViewportFrame::mouseRay(glm::vec3& origin, glm::vec3& dir) const {
    const glm::mat4 inv = glm::inverse(viewProj);
    glm::vec4 pn = inv * glm::vec4(mouseNdc, -1.0f, 1.0f); pn /= pn.w;
    glm::vec4 pf = inv * glm::vec4(mouseNdc,  1.0f, 1.0f); pf /= pf.w;
    origin = glm::vec3(pn);
    dir    = glm::normalize(glm::vec3(pf) - glm::vec3(pn));
}

bool ViewportFrame::toScreen(const glm::vec3& p, ImVec2& out) const {
    const glm::vec4 c = viewProj * glm::vec4(p, 1.0f);
    if (c.w <= 1e-4f) return false;
    const glm::vec3 n = glm::vec3(c) / c.w;
    out = ImVec2(origin.x + (n.x * 0.5f + 0.5f) * w,
                 origin.y + (1.0f - (n.y * 0.5f + 0.5f)) * h);
    return true;
}

glm::mat4 meshModelOf(const Entity& e, const MeshComponent& mc) {
    return scenegraph::compose(e.center, e.rotation, editmesh::fitScale(mc.mesh, e.half));
}

std::vector<glm::vec3> meshFaceWorld(const Entity& e, const MeshComponent& mc, int face) {
    std::vector<glm::vec3> out;
    if (!mc.mesh.validFace(face)) return out;
    const glm::mat4 m = meshModelOf(e, mc);
    out.reserve(mc.mesh.faces[face].size());
    for (int i : mc.mesh.faces[face])
        out.push_back(glm::vec3(m * glm::vec4(mc.mesh.verts[i], 1.0f)));
    return out;
}
