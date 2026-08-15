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

    // Seed the hover height so the craft floats just clear of its own body.
    // Tuned values survive a re-run.
    //
    // Nothing about a camera here any more: a craft's view is a camera entity
    // parented to it, and this tool makes a CRAFT. Add a Camera object, drop it
    // on the craft in the hierarchy, set it to Follow -- where you park it is
    // where the view sits.
    if (fresh)
        gc->rideHeight = glm::clamp(root->half.y * 1.4f, 0.4f, 6.0f);

    char msg[192];
    std::snprintf(msg, sizeof(msg),
                  "Glider ready on '%s'. Ride height %.2f m. Add a Camera child "
                  "for the view, then press G to fly.",
                  root->name.c_str(), gc->rideHeight);
    return msg;
}

void inspector(GliderComponent& gc, Entity& /*root*/, Document& /*doc*/,
               const SoundPicker& soundPicker) {
    bool hoverHdr = false, attHdr = false, boostHdr = false;
    bool nrgHdr = false;
    for (const Property& pr : gc.props()) {
        if (!boostHdr && pr.key == "boostCapacity") {
            ui::sectionText("Boost (A / Left Shift)");
            boostHdr = true;
        }
        if (!nrgHdr && pr.key == "energyCapacity") {
            ui::sectionText("Energy (hull)");
            nrgHdr = true;
        }
        if (!hoverHdr && pr.key == "rideHeight") { ui::sectionText("Hover");    hoverHdr = true; }
        if (!attHdr   && pr.key == "bankAngle")  { ui::sectionText("Attitude"); attHdr   = true; }
        // The two SFX filenames are chosen from the project's sounds, not typed
        // -- same rule as every other sound field in the editor. Without a picker
        // to hand (no caller supplied one) they fall back to their text field.
        if (soundPicker && (pr.key == "soundHit" || pr.key == "soundWarn")) {
            soundPicker(pr.label.c_str(),
                        pr.key == "soundHit" ? gc.soundHit : gc.soundWarn);
            continue;
        }
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
