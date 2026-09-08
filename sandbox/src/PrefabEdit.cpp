#include "PrefabEdit.hpp"

#include <algorithm>
#include <cmath>

#ifndef FITZEL_PLAYER
#include <imgui.h>
#include <imgui_internal.h>  // BeginViewportSideBar
#include "UiStyle.hpp"
#endif

#include "PrefabSystem.hpp"

namespace prefabedit {

namespace {

// Everything on the stage that is NOT part of the edited subtree. Scaffolding
// the author added to work by, which the file will not carry.
int strays(const Session& s, const std::vector<Entity>& entities) {
    if (s.rootId < 0) return 0;
    // Walk down from the root: anything not reachable from it is a stray.
    std::vector<int> keep{s.rootId};
    bool grew = true;
    while (grew) {
        grew = false;
        for (const Entity& e : entities)
            if (e.parent >= 0 &&
                std::find(keep.begin(), keep.end(), e.parent) != keep.end() &&
                std::find(keep.begin(), keep.end(), e.id) == keep.end()) {
                keep.push_back(e.id);
                grew = true;
            }
    }
    return static_cast<int>(entities.size()) - static_cast<int>(keep.size());
}

} // namespace

bool open(Session& s, projectio::Context& ctx, const std::string& path,
          std::vector<Entity>& entities, CommandStack& history, int& entityCounter,
          const glm::vec3& groundAt, std::string& status) {
    if (s.active) {
        status = "Already editing a prefab -- close that one first.";
        return false;
    }
    auto p = prefab::load(ctx, path);
    if (!p || p->entities.empty()) {
        status = "Failed to open prefab: " + path;
        return false;
    }
    // Built BEFORE anything is stashed, so a failure above leaves the editor
    // exactly as it was rather than holding a scene nobody can get back to.
    std::vector<Entity> stage =
        prefab::instantiate(*p, entityCounter, groundAt, 0.0f);
    if (stage.empty()) {
        status = "Prefab is empty: " + p->name;
        return false;
    }

    s.name   = p->name;
    s.path   = p->path.empty() ? path : p->path;
    s.guid   = p->guid;
    s.rootId = stage.front().id;   // instantiate emits the root first
    s.scene  = std::move(entities);
    s.history = std::move(history);
    history  = CommandStack{};
    entities = std::move(stage);
    s.active = true;
    status   = "Editing prefab: " + s.name;
    return true;
}

bool saveBack(Session& s, projectio::Context& ctx,
              const std::vector<Entity>& entities, const std::string& dir,
              std::string& status) {
    if (!s.active) return false;
    if (dir.empty()) { status = "No project open -- nowhere to save."; return false; }
    auto p = prefab::fromSubtree(entities, s.rootId, s.name);
    if (!p) {
        // The root was deleted on the stage. Saying so beats writing an empty
        // prefab over a good one.
        status = "The prefab's root object is gone -- nothing to save.";
        return false;
    }
    // Its own identity, kept: the GUID is what every instance in every scene
    // points at, and a save that minted a fresh one would quietly orphan all of
    // them and leave a second file beside the first.
    p->guid = s.guid;
    p->path = s.path;
    if (!prefab::save(ctx, *p, dir)) {
        status = "Failed to write prefab: " + s.name;
        return false;
    }
    s.guid = p->guid;
    s.path = p->path;
    const int extra = strays(s, entities);
    status = "Saved prefab: " + s.name;
    if (extra > 0)
        status += " (" + std::to_string(extra) + " object" +
                  (extra == 1 ? "" : "s") + " outside it not saved)";
    return true;
}

void close(Session& s, std::vector<Entity>& entities, CommandStack& history) {
    if (!s.active) return;
    entities = std::move(s.scene);
    history  = std::move(s.history);
    s = Session{};
}

bool frame(const Session& s, const std::vector<Entity>& entities, float fovDeg,
           glm::vec3& eye, glm::vec3& lookAt) {
    if (!s.active || entities.empty()) return false;
    // The whole stage's bounds, not just the root's: a prefab is usually a root
    // with the shape hanging off it, and framing the root alone would put the eye
    // inside the thing being edited.
    glm::vec3 lo(1.0e9f), hi(-1.0e9f);
    for (const Entity& e : entities) {
        lo = glm::min(lo, e.center - e.half);
        hi = glm::max(hi, e.center + e.half);
    }
    if (lo.x > hi.x) return false;
    lookAt = (lo + hi) * 0.5f;
    const float radius = glm::max(glm::length(hi - lo) * 0.5f, 0.5f);
    const float fov    = glm::radians(glm::max(fovDeg, 1.0f));
    const float dist   = radius / glm::max(std::tan(fov * 0.5f), 0.05f) * 1.6f;
    // Three-quarter view, slightly above: the angle a modelled object is
    // recognisable from, and the one every showroom shot opens on.
    const glm::vec3 dir = glm::normalize(glm::vec3(0.55f, 0.38f, 0.74f));
    eye = lookAt + dir * dist;
    return true;
}

#ifndef FITZEL_PLAYER
Action banner(const Session& s, const std::vector<Entity>& entities) {
    if (!s.active) return Action::None;
    Action act = Action::None;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float h = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;
    // Amber, and across the whole width: while this is up the document is not the
    // scene, and every save door in the editor is shut. That is not a state to
    // discover from a greyed-out menu item.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.32f, 0.24f, 0.05f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    const bool barOpen = ImGui::BeginViewportSideBar(
        "##PrefabEditBar", vp, ImGuiDir_Up, h,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration);
    if (barOpen) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.45f, 1.0f), "Editing prefab:");
        ImGui::SameLine();
        ImGui::Text("%s", s.name.c_str());
        ImGui::SameLine();
        const int extra = strays(s, entities);
        if (extra > 0) {
            ImGui::TextDisabled("(+%d object%s outside it -- not saved)",
                                extra, extra == 1 ? "" : "s");
            ImGui::SameLine();
        }
        // Right-aligned buttons, so the row reads name-first and the thing that
        // ends the session is where a dialog would put it.
        const float bw = 110.0f;
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - (bw * 3.0f + sp * 3.0f));
        if (ImGui::Button("Save", ImVec2(bw, 0.0f)))       act = Action::Save;
        ImGui::SameLine();
        if (ImGui::Button("Save & Close", ImVec2(bw, 0.0f))) act = Action::SaveClose;
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(bw, 0.0f)))    act = Action::Discard;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Leave the prefab as it was on disk and go back to "
                              "the scene.\nEverything edited here is dropped.");
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    return act;
}
#endif

} // namespace prefabedit
