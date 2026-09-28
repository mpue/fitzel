#include "ViewportHud.hpp"

#include <cstdio>

#include <glm/glm.hpp>

#include "Command.hpp"
#include "GameSettings.hpp"

namespace viewhud {

void cameraPreview(ImVec2 vmin, ImVec2 vmax, ImTextureID texture, int texW, int texH,
                   const std::string& name) {
    const float pw = glm::clamp((vmax.x - vmin.x) / 6.0f, 160.0f, 420.0f);
    const float ph = pw * static_cast<float>(texH) / static_cast<float>(texW);
    const float pad = 12.0f;
    const ImVec2 p1(vmax.x - pad, vmax.y - pad);
    const ImVec2 p0(p1.x - pw, p1.y - ph);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p0.x - 3.0f, p0.y - 3.0f), ImVec2(p1.x + 3.0f, p1.y + 3.0f),
                      IM_COL32(0, 0, 0, 170), 3.0f);
    // GL textures are bottom-up: flip V, like the viewport image.
    dl->AddImage(texture, p0, p1, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
    dl->AddRect(p0, p1, IM_COL32(255, 225, 140, 200), 0.0f, 0, 1.5f);
    const std::string cap = name.empty() ? std::string("Camera") : name;
    dl->AddText(ImVec2(p0.x + 6.0f, p0.y - ImGui::GetTextLineHeight() - 4.0f),
                IM_COL32(255, 225, 140, 230), cap.c_str());
}

// In the viewport rather than in the Game Settings dialog because it is not a
// setting about the game -- it is about this next Play, in this scene, now --
// and because a shortcut you have to go and find is one you stop taking. It
// reads back what it is doing at all times: an override left on for a week must
// not be a mystery.
bool playAsPicker(ImVec2 vmin, ImVec2 vmax, int& sceneStartMode, CommandStack& history) {
    const float cw = 230.0f;
    ImGui::SetCursorScreenPos(ImVec2(vmax.x - cw - 12.0f, vmin.y + 10.0f));
    ImGui::SetNextItemWidth(cw);
    const std::string label =
        sceneStartMode < 0
            ? std::string("This scene: the game's own start")
            : std::string("This scene: ") +
                  game::startModeName(static_cast<game::StartMode>(sceneStartMode));
    // Amber while it is forcing something, so the corner reads as "this is not
    // how the game opens" at a glance.
    const bool forcing = sceneStartMode >= 0;
    if (forcing) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.32f, 0.24f, 0.05f, 0.95f));
    const int wasMode = sceneStartMode;
    if (ImGui::BeginCombo("##playas", label.c_str())) {
        if (ImGui::Selectable("The game's own start", sceneStartMode < 0)) sceneStartMode = -1;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("What game.json says -- and a showroom\n"
                              "scene opens its start screen.");
        ImGui::Separator();
        for (int m = 0; m <= static_cast<int>(game::StartMode::Multishot); ++m)
            if (ImGui::Selectable(game::startModeName(static_cast<game::StartMode>(m)),
                                  sceneStartMode == m))
                sceneStartMode = m;
        ImGui::EndCombo();
    }
    // It is scene data, so changing it is an edit: say so, or the autosave sits
    // on its hands and the scene closes without it. touch() rather than a
    // command -- there is no object to undo (see CommandStack::touch).
    if (sceneStartMode != wasMode) history.touch();
    if (forcing) ImGui::PopStyleColor();
    const bool onIt = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (ImGui::IsItemHovered() && forcing)
        ImGui::SetTooltip("Saved with the scene: it opens this way\n"
                          "wherever it is reached from, in the\n"
                          "editor and in the shipped game, and no\n"
                          "start screen comes first.");
    return onIt;
}

void traceStatus(ImVec2 vmin, const std::string& status) {
    if (status.empty()) return;
    ImGui::GetWindowDrawList()->AddText(ImVec2(vmin.x + 10.0f, vmin.y + 8.0f),
                                        IM_COL32(255, 225, 140, 230), status.c_str());
}

// Without it the free camera has simply gone, and getting it back means knowing
// that some camera in the hierarchy has it -- a trap, and exactly the kind this
// editor is meant not to set.
bool exitCamera(ImVec2 vmin, const std::string& cameraName, int& activeCam) {
    char lbl[160];
    std::snprintf(lbl, sizeof lbl, "Exit camera: %s", cameraName.c_str());
    ImGui::SetCursorScreenPos(ImVec2(vmin.x + 12.0f, vmin.y + 30.0f));
    if (ImGui::Button(lbl)) activeCam = -1;
    return ImGui::IsItemHovered();
}

// Blender puts this in the same corner, and for the same reason: front and back
// look identical until something moves, so a view you cannot name is one you
// have to test by nudging the camera -- which is exactly what the standard
// views are for avoiding. The lens goes in the same place: an orthographic
// picture of a landscape is easy to mistake for a flat one.
void viewLabel(ImVec2 vmin, const char* standardView, bool ortho) {
    char vl[48] = "";
    if (standardView && ortho) std::snprintf(vl, sizeof vl, "%s (Ortho)", standardView);
    else if (standardView)     std::snprintf(vl, sizeof vl, "%s", standardView);
    else if (ortho)            std::snprintf(vl, sizeof vl, "Orthographic");
    if (!vl[0]) return;
    ImDrawList* vdl = ImGui::GetWindowDrawList();
    const ImVec2 at(vmin.x + 12.0f, vmin.y + 10.0f);
    vdl->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, 160), vl);
    vdl->AddText(at, IM_COL32(235, 240, 250, 225), vl);
}

} // namespace viewhud
