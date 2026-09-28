#include "VehicleTool.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include <glm/glm.hpp>
#include <imgui.h>

#include "Command.hpp"
#include "Component.hpp"
#include "Document.hpp"
#include "EditorContext.hpp"
#include "SkidSystem.hpp"
#include "TrailSystem.hpp"
#include "PropertyMeta.hpp"
#include "SceneTypes.hpp"
#include "UiStyle.hpp"
#include "VehicleRig.hpp"

namespace vehicleui {

namespace {

const char* slotName(int i) {
    static const char* names[4] = {"FL", "FR", "RL", "RR"};
    return names[i];
}

} // namespace

std::string autoSetup(Document& doc, int rootId) {
    Entity* root = doc.find(rootId);
    if (!root) return "No entity selected.";

    auto* vc = root->components.get<VehicleComponent>();
    const bool fresh = (vc == nullptr);
    if (fresh) {
        root->components.items.push_back(std::make_unique<VehicleComponent>());
        vc = root->components.get<VehicleComponent>();
    }

    // The four wheels among the direct children (see vehiclerig::guessWheels).
    int ids[4];
    vehiclerig::guessWheels(doc.entities(), rootId, vc->forward, ids);
    const float s = (vc->forward == 1) ? -1.0f : 1.0f;
    const Entity* slot[4];
    for (int i = 0; i < 4; ++i) slot[i] = ids[i] >= 0 ? doc.find(ids[i]) : nullptr;

    int found = 0;
    float radius = 0.0f, width = 0.0f, track = 0.0f, wy = 0.0f;
    float fzSum = 0.0f, rzSum = 0.0f;
    int   fzN = 0, rzN = 0;
    for (int i = 0; i < 4; ++i) {
        vc->wheelId[i] = slot[i] ? slot[i]->id : -1;
        if (!slot[i]) continue;
        ++found;
        const glm::vec3 lc = slot[i]->localCenter;
        radius += 0.5f * (slot[i]->half.y + slot[i]->half.z);
        width  += 2.0f * slot[i]->half.x;
        track  += std::abs(lc.x);
        wy     += lc.y;
        const float fz = lc.z * s;
        if (i < 2) { fzSum += fz; ++fzN; } else { rzSum += fz; ++rzN; }
    }

    char msg[256];
    if (found > 0) {
        vc->wheelRadius = glm::clamp(radius / found, 0.05f, 3.0f);
        vc->wheelWidth  = glm::clamp(width / found, 0.05f, 2.0f);
        vc->halfTrack   = glm::max(track / found, 0.1f);
        vc->wheelY      = wy / found;
        // A missing axle pair mirrors the found one (better than a stale value).
        vc->frontZ = fzN ? fzSum / fzN : (rzN ? -rzSum / rzN : vc->frontZ);
        vc->rearZ  = rzN ? rzSum / rzN : (fzN ? -fzSum / fzN : vc->rearZ);
        std::string names;
        for (int i = 0; i < 4; ++i)
            if (slot[i]) names += std::string(names.empty() ? "" : ", ") +
                                  slotName(i) + " '" + slot[i]->name + "'";
        std::snprintf(msg, sizeof(msg),
                      "%d wheel%s: %s. Radius %.2f m, track %.2f m.",
                      found, found == 1 ? "" : "s", names.c_str(),
                      vc->wheelRadius, vc->halfTrack * 2.0f);
    } else {
        // No wheel children (a flat single-mesh model): the whole model rides
        // the chassis; guess a plausible geometry from the root AABB.
        vc->wheelRadius = glm::clamp(root->half.y * 0.4f, 0.15f, 1.2f);
        vc->wheelWidth  = glm::clamp(vc->wheelRadius * 0.7f, 0.05f, 2.0f);
        vc->halfTrack   = glm::max(root->half.x * 0.8f, 0.1f);
        vc->frontZ      = root->half.z * 0.62f;
        vc->rearZ       = -vc->frontZ;
        vc->wheelY      = -root->half.y + vc->wheelRadius;
        std::snprintf(msg, sizeof(msg),
                      "No wheel children found -- geometry guessed from the "
                      "bounding box (the whole model rides the chassis).");
    }
    vc->chassisHalf = glm::max(root->half, glm::vec3(0.05f));
    if (fresh) {
        // Mass from the body volume (~150 kg/m^3 lands near real cars); tuned
        // values survive a re-run since only a fresh component gets this.
        const glm::vec3 sz = 2.0f * vc->chassisHalf;
        vc->mass = glm::clamp(150.0f * sz.x * sz.y * sz.z, 200.0f, 20000.0f);
    }
    return msg;
}

void inspector(VehicleComponent& vc, Entity& root, Document& doc) {
    bool hndHeader = false, boatHeader = false;
    for (const Property& pr : vc.props()) {
        if (!hndHeader && pr.key == "comLower") { // handling group starts here
            ui::sectionText("Handling");
            hndHeader = true;
        }
        if (!boatHeader && pr.key == "boatFloat") { // boat group
            ui::sectionText("Boat");
            boatHeader = true;
        }
        drawProperty(pr, &vc);
    }

    ui::sectionText("Wheels");
    static const char* labels[4] = {"Front left", "Front right",
                                    "Rear left", "Rear right"};
    for (int i = 0; i < 4; ++i) {
        const Entity* cur = doc.find(vc.wheelId[i]);
        const std::string curLabel = cur ? cur->name : "(none)";
        if (ImGui::BeginCombo(labels[i], curLabel.c_str())) {
            if (ImGui::Selectable("(none)", vc.wheelId[i] < 0)) vc.wheelId[i] = -1;
            for (const Entity& ce : doc.entities()) {
                if (ce.parent != root.id) continue;
                const std::string item = ce.name + "##" + std::to_string(ce.id);
                if (ImGui::Selectable(item.c_str(), vc.wheelId[i] == ce.id))
                    vc.wheelId[i] = ce.id;
            }
            ImGui::EndCombo();
        }
    }

    // Wheel orientation: a wheel modelled facing some other way than its spin
    // expects (rim inward, axle along the car) is put right here, in quarter
    // turns about the car's axes -- buttons, not a drag, and best while
    // driving: the change shows at once and is kept when Play stops.
    ui::sectionText("Wheel orientation");
    ImGui::TextDisabled("Turn a wheel that looks wrong while driving.\n"
                        "Works in Play; kept when you stop.");
    static const char* shortLabels[4] = {"FL", "FR", "RL", "RR"};
    const float bw = ImGui::GetFontSize() * 4.2f;
    for (int i = 0; i < 4; ++i) {
        ImGui::PushID(1000 + i);
        glm::vec3& t = vc.wheelTurn[i];
        auto quarter = [](float& a, float by) {
            a = std::fmod(a + by + 360.0f, 360.0f);
            if (a > 180.0f) a -= 360.0f;
        };
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s", shortLabels[i]);
        ImGui::SameLine(ImGui::GetFontSize() * 2.2f);
        if (ImGui::Button("Flip", ImVec2(bw, 0.0f))) quarter(t.y, 180.0f);
        ImGui::SetItemTooltip("Half a turn about the vertical: the rim to the other side.");
        ImGui::SameLine();
        if (ImGui::Button("Up 90", ImVec2(bw, 0.0f))) quarter(t.y, 90.0f);
        ImGui::SetItemTooltip("A quarter turn about the vertical.");
        ImGui::SameLine();
        if (ImGui::Button("Along 90", ImVec2(bw, 0.0f))) quarter(t.z, 90.0f);
        ImGui::SetItemTooltip("A quarter turn about the car's length (the wheel lies down).");
        ImGui::SameLine();
        if (ImGui::Button("Axle 90", ImVec2(bw, 0.0f))) quarter(t.x, 90.0f);
        ImGui::SetItemTooltip("A quarter turn about the axle (the tread pattern, the valve).");
        ImGui::SameLine();
        ImGui::BeginDisabled(t == glm::vec3(0.0f));
        if (ImGui::Button("Reset", ImVec2(bw, 0.0f))) t = glm::vec3(0.0f);
        ImGui::EndDisabled();
        if (t != glm::vec3(0.0f)) {
            ImGui::SameLine();
            ImGui::TextDisabled("%.0f / %.0f / %.0f", t.x, t.y, t.z);
        }
        ImGui::PopID();
    }
    if (ImGui::Button("Copy front left to all")) {
        const glm::vec3 fl = vc.wheelTurn[0];
        for (int i = 1; i < 4; ++i) vc.wheelTurn[i] = fl;
    }

    static std::string lastDetect; // report of the last re-detect run
    if (ImGui::Button("Detect wheels & geometry"))
        lastDetect = autoSetup(doc, root.id);
    if (!lastDetect.empty()) ImGui::TextWrapped("%s", lastDetect.c_str());
    ImGui::TextDisabled("Press V in the viewport to drive.");
}

int panelSection(Document& doc, int selectedId,
                 const std::function<std::string(int)>& makeDrivable) {
    static std::string lastMsg; // report of the last Make-drivable run
    int pick = -1;

    ui::sectionText("Scene vehicles");
    int n = 0;
    for (const Entity& e : doc.entities()) {
        if (!e.components.get<VehicleComponent>()) continue;
        ++n;
        ImGui::PushID(e.id);
        if (ImGui::Selectable(e.name.c_str(), e.id == selectedId)) pick = e.id;
        ImGui::PopID();
    }
    if (n == 0)
        ImGui::TextDisabled("No drivable models yet.");
    else
        ImGui::TextDisabled("V drives the vehicle nearest to the camera.");

    const Entity* sel = doc.find(selectedId);
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button("Make selected entity drivable") && sel)
        lastMsg = makeDrivable(selectedId);
    ImGui::EndDisabled();
    if (!sel) ImGui::TextDisabled("Select a model in the scene first.");
    if (!lastMsg.empty()) ImGui::TextWrapped("%s", lastMsg.c_str());
    return pick;
}


void window(EditorContext& ed, const Window& w) {
    if (!w.show) return;
    if (ImGui::Begin("Vehicle", &w.show)) {
        if (ImGui::Checkbox("Drive mode (V)", &w.driveMode) && w.setDriving)
            w.setDriving(w.driveMode);
        if (w.driveMode)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                               "W/S drive, A/D steer, Space brake, Esc exit");
        else
            ImGui::TextDisabled("Press V or tick above to drive");

        // Per-scene Play options (saved with the scene / exported game).
        ui::sectionText("Play start");
        // WHAT Play starts as lives in File > Game Settings ("Start as"). It is
        // a statement about the game rather than about this panel, there are
        // five answers rather than one checkbox here and another in the Glider
        // panel, and two checkboxes could disagree. The pointer stays because
        // this is where people look for it.
        ImGui::TextDisabled("Start mode: File > Game Settings");
        ImGui::Checkbox("Show crosshair", &w.showCrosshair);

        ui::sectionText("Skid marks");
        ImGui::Checkbox("Enable skid marks", &w.skids.enabled);
        ImGui::BeginDisabled(!w.skids.enabled);
        ImGui::SliderFloat("Slip threshold", &w.skids.slipThresh, 0.1f, 1.5f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How much a wheel must slip (lock/spin/drift)\n"
                              "before it leaves a mark (lower = more marks).");
        ImGui::SliderFloat("Mark width", &w.skids.markHalfW, 0.05f, 0.6f, "%.2f m");
        ImGui::SliderFloat("Darkness", &w.skids.opacity, 0.1f, 1.0f, "%.2f");
        ImGui::EndDisabled();

        ui::sectionText("Contrails");
        ImGui::Checkbox("Enable contrails", &w.trails.enabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Vapour trails streaming behind the racers\n"
                              "(the driven craft and every opponent) in Play.");
        ImGui::BeginDisabled(!w.trails.enabled);
        ImGui::SliderFloat("Trail length", &w.trails.life, 0.3f, 5.0f, "%.1f s");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How long each puff lingers before it fades\n"
                              "out -- longer = a longer streak.");
        ImGui::SliderFloat("Trail width", &w.trails.width, 0.05f, 1.5f, "%.2f m");
        ImGui::SliderFloat("Trail opacity", &w.trails.opacity, 0.05f, 1.0f, "%.2f");
        ImGui::SliderFloat("Trail glow", &w.trails.glow, 0.0f, 6.0f, "%.1f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Self-illumination: the streak glows on its\n"
                              "own instead of being lit (and dimmed) by the sun.");
        ImGui::SliderFloat("Trail spacing", &w.trails.minStep, 0.2f, 3.0f, "%.1f m");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Distance between recorded points. Smaller = a\n"
                              "smoother ribbon (more geometry).");
        ImGui::ColorEdit3("Trail colour", &w.trails.color.x);
        ImGui::EndDisabled();

        ui::sectionText("Missiles");
        if (w.weaponSettings) w.weaponSettings();

        // Scene vehicles: hook a model into the vehicle system with one click.
        // The auto-setup edit goes through the undo history.
        auto makeDrivable = [&](int rootId) -> std::string {
            Entity* e = ed.document.find(rootId);
            if (!e) return std::string();
            const Entity before = *e;
            std::string rep = autoSetup(ed.document, rootId);
            if (Entity* after = ed.document.find(rootId)) {
                auto cmd = std::make_unique<ModifyEntityCmd>(before, *after);
                if (!cmd->trivial()) ed.history.pushApplied(std::move(cmd));
            }
            return rep;
        };
        const int selId = ed.sel.valid() ? ed.entities[ed.sel.index()].id : -1;
        const int pick = panelSection(ed.document, selId, makeDrivable);
        if (pick >= 0) ed.sel.select(pick);

        ui::sectionText("Setup gizmo");
        ImGui::Checkbox("Edit setup in viewport", &w.gizmoEdit);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Drag the axles, track, wheels, collision box and centre\n"
                "of mass directly in the viewport.\n\n"
                "The shape is ALWAYS drawn for the selected vehicle -- this\n"
                "hands the handles the left mouse button, so the transform\n"
                "gizmo pauses while it is on.\n\n"
                "Arrow keys nudge the selected handle (Shift = bigger steps).");
        ImGui::TextDisabled("Select a vehicle to see its setup drawn.");

        ui::sectionText("Test car");
        ImGui::Checkbox("Show vehicle", &w.showTestCar);
        if (ImGui::Button("Place at camera") && w.placeTestCar) w.placeTestCar();
        if (w.testCarPlaced) ImGui::Text("Speed: %.0f km/h", std::abs(w.testCarSpeed) * 3.6f);
        else                 ImGui::TextDisabled("Vehicle not placed yet");
    }
    ImGui::End();
}

} // namespace vehicleui
