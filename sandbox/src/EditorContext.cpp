#include "EditorContext.hpp"

#include <memory>

#include <glm/gtc/quaternion.hpp>

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

glm::mat4 meshModelOf(const Entity& e, const MeshComponent& mc) {
    return scenegraph::compose(e.center, e.rotation, editmesh::fitScale(mc.mesh, e.half));
}

void normalizeMeshEntity(const std::vector<Entity>& entities, Entity& e, MeshComponent& mc,
                         const glm::vec3& scale) {
    const glm::vec3 shift = editmesh::recenter(mc.mesh);
    glm::vec3 mn, mx;
    mc.mesh.bounds(mn, mx);
    e.half = glm::max((mx - mn) * 0.5f * scale, glm::vec3(1e-3f));
    if (glm::dot(shift, shift) > 0.0f) {
        const glm::quat q  = glm::quat(glm::radians(e.rotation));
        const glm::mat4 pw = scenegraph::parentWorld(entities, e);
        scenegraph::setWorld(e, e.center + q * (shift * scale), e.rotation,
                             e.parent >= 0 ? &pw : nullptr);
    }
    mc.touch();
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
