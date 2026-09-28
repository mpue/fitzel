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

bool ViewportFrame::project(const glm::vec3& p, ImVec2& out) const {
    const glm::vec4 c = viewProj * glm::vec4(p, 1.0f);
    if (c.w <= 1e-4f) return false;
    const glm::vec3 n = glm::vec3(c) / c.w;
    if (n.z > 1.0f) return false;
    out = ImVec2(origin.x + (n.x * 0.5f + 0.5f) * w,
                 origin.y + (1.0f - (n.y * 0.5f + 0.5f)) * h);
    return true;
}

void ViewportFrame::wireBox(const glm::mat4& model, const glm::vec3& lo, const glm::vec3& hi,
                            ImU32 col, float thick) const {
    ImVec2 sp[8];
    bool   ok[8];
    for (int c = 0; c < 8; ++c) {
        const glm::vec3 p((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z);
        ok[c] = project(glm::vec3(model * glm::vec4(p, 1.0f)), sp[c]);
    }
    static const int kEdges[12][2] = {
        {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7},
        {0,4},{1,5},{2,6},{3,7}};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const auto& e : kEdges)
        if (ok[e[0]] && ok[e[1]]) dl->AddLine(sp[e[0]], sp[e[1]], col, thick);
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
