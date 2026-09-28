#include "ViewportFrame.hpp"

#include <algorithm>
#include <cmath>

#include <fitzel/scene/Camera.hpp>

ViewportFrame ViewportFrame::looking(const fitzel::Camera& cam, ImVec2 origin, float w, float h,
                                     ImVec2 mouse, bool hovered) {
    ViewportFrame f;
    f.viewProj    = cam.projectionMatrix(w / h) * cam.viewMatrix();
    f.origin      = origin;
    f.w           = w;
    f.h           = h;
    f.hovered     = hovered;
    f.mousePos    = mouse;
    f.mouseNdc    = glm::vec2((w > 0.0f ? (mouse.x - origin.x) / w : 0.5f) * 2.0f - 1.0f,
                              1.0f - (h > 0.0f ? (mouse.y - origin.y) / h : 0.5f) * 2.0f);
    f.cameraPos   = cam.position();
    f.cameraFront = cam.front();
    f.cameraFov   = cam.fov();
    f.orthoHalfH  = cam.orthographic() ? cam.orthoHalfHeight() : 0.0f;
    return f;
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

float ViewportFrame::metresPerPixel(const glm::vec3& at) const {
    const float dist = glm::length(at - cameraPos);
    return (orthoHalfH > 0.0f ? 2.0f * orthoHalfH
                              : 2.0f * dist * std::tan(glm::radians(cameraFov * 0.5f))) /
           std::max(1.0f, h);
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
