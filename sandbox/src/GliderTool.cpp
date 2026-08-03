#include "GliderTool.hpp"

#include <cstdio>
#include <memory>

#include <glm/glm.hpp>
#include <imgui.h>

#include "Component.hpp"
#include "Document.hpp"
#include "PropertyMeta.hpp"
#include "SceneTypes.hpp"
#include "UiStyle.hpp"

namespace gliderui {

std::string autoSetup(Document& doc, int rootId) {
    Entity* root = doc.find(rootId);
    if (!root) return "No entity selected.";

    auto* gc = root->components.get<GliderComponent>();
    const bool fresh = (gc == nullptr);
    if (fresh) {
        root->components.items.push_back(std::make_unique<GliderComponent>());
        gc = root->components.get<GliderComponent>();
    }

    // Seed the hover height so the craft floats just clear of its own body, and
    // scale the camera stand-off to the craft size. Tuned values survive a re-run.
    if (fresh) {
        gc->rideHeight   = glm::clamp(root->half.y * 1.4f, 0.4f, 6.0f);
        const float span = glm::max(root->half.x, root->half.z);
        gc->camDistance  = glm::clamp(span * 5.0f, 4.0f, 24.0f);
        gc->camHeight    = glm::clamp(span * 2.0f, 1.5f, 10.0f);
        gc->camLookHeight = glm::clamp(root->half.y, 0.5f, 4.0f);
    }

    char msg[192];
    std::snprintf(msg, sizeof(msg),
                  "Glider ready on '%s'. Ride height %.2f m. Press G to fly.",
                  root->name.c_str(), gc->rideHeight);
    return msg;
}

void inspector(GliderComponent& gc, Entity& /*root*/, Document& /*doc*/) {
    bool hoverHdr = false, attHdr = false, camHdr = false;
    for (const Property& pr : gc.props()) {
        if (!hoverHdr && pr.key == "rideHeight") { ui::sectionText("Hover");    hoverHdr = true; }
        if (!attHdr   && pr.key == "bankAngle")  { ui::sectionText("Attitude"); attHdr   = true; }
        if (!camHdr   && pr.key.rfind("cam", 0) == 0) { ui::sectionText("Follow camera"); camHdr = true; }
        drawProperty(pr, &gc);
    }
    ImGui::TextDisabled("Press G in the viewport to fly.");
}

int panelSection(Document& doc, int selectedId,
                 const std::function<std::string(int)>& makeGlider) {
    static std::string lastMsg; // report of the last Make-glider run
    int pick = -1;

    ui::sectionText("Scene gliders");
    int n = 0;
    for (const Entity& e : doc.entities()) {
        if (!e.components.get<GliderComponent>()) continue;
        ++n;
        ImGui::PushID(e.id);
        if (ImGui::Selectable(e.name.c_str(), e.id == selectedId)) pick = e.id;
        ImGui::PopID();
    }
    if (n == 0)
        ImGui::TextDisabled("No gliders yet.");
    else
        ImGui::TextDisabled("G flies the glider nearest to the camera.");

    const Entity* sel = doc.find(selectedId);
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button("Make selected entity a glider") && sel)
        lastMsg = makeGlider(selectedId);
    ImGui::EndDisabled();
    if (!sel) ImGui::TextDisabled("Select a model in the scene first.");
    if (!lastMsg.empty()) ImGui::TextWrapped("%s", lastMsg.c_str());
    return pick;
}

} // namespace gliderui
