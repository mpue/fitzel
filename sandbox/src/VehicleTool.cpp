#include "VehicleTool.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include <glm/glm.hpp>
#include <imgui.h>

#include "Component.hpp"
#include "Document.hpp"
#include "PropertyMeta.hpp"
#include "SceneTypes.hpp"

namespace vehicleui {

namespace {

// True when an entity name reads like a wheel. Substring match for the long
// words; "rad" (German) only as a whole token so "gradient"/"radio" don't bite.
// "steer"/"lenk" are excluded so a steering wheel never lands in a wheel slot.
bool nameLooksLikeWheel(const std::string& name) {
    std::string n;
    n.reserve(name.size());
    for (char c : name) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n.find("steer") != std::string::npos || n.find("lenk") != std::string::npos)
        return false;
    for (const char* w : {"wheel", "tire", "tyre", "reifen", "felge"})
        if (n.find(w) != std::string::npos) return true;
    for (std::size_t at = n.find("rad"); at != std::string::npos; at = n.find("rad", at + 1)) {
        const bool leftOk  = at == 0 || !std::isalpha(static_cast<unsigned char>(n[at - 1]));
        const bool rightOk = at + 3 >= n.size() ||
                             !std::isalpha(static_cast<unsigned char>(n[at + 3]));
        if (leftOk && rightOk) return true;
    }
    return false;
}

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

    // Wheel candidates among the direct children: name matches first, else
    // disc-shaped AABBs (round in YZ -- the axle runs along X on a car).
    struct Cand { const Entity* e; bool byName; };
    std::vector<Cand> cands;
    int named = 0;
    for (const Entity& e : doc.entities()) {
        if (e.parent != rootId) continue;
        const bool byName = nameLooksLikeWheel(e.name);
        const float r = std::max(e.half.y, e.half.z);
        const bool disc = r > 1e-3f &&
                          std::abs(e.half.y - e.half.z) <= 0.3f * r &&
                          e.half.x <= 0.75f * r;
        if (byName || disc) { cands.push_back({&e, byName}); named += byName ? 1 : 0; }
    }
    if (named >= 4) // enough named wheels -> ignore the shape guesses
        cands.erase(std::remove_if(cands.begin(), cands.end(),
                                   [](const Cand& c) { return !c.byName; }),
                    cands.end());

    // Assign candidates to the FL/FR/RL/RR quadrants (in the chassis frame,
    // which flips Z when the model's nose points -Z); the outermost candidate
    // wins each corner, so mirrors/hubcaps near the centre lose out.
    const float s = (vc->forward == 1) ? -1.0f : 1.0f;
    const Entity* slot[4] = {nullptr, nullptr, nullptr, nullptr};
    float score[4] = {0, 0, 0, 0};
    for (const Cand& c : cands) {
        const glm::vec3 lc = c.e->localCenter;
        const float fz = lc.z * s;
        if (std::abs(lc.x) < 0.05f) continue; // centred (steering wheel, spare)
        const int   q  = (fz > 0.0f ? 0 : 2) + (lc.x > 0.0f ? 1 : 0);
        const float sc = std::abs(lc.x) + std::abs(fz);
        if (!slot[q] || sc > score[q]) { slot[q] = c.e; score[q] = sc; }
    }

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
    bool hndHeader = false, camHeader = false;
    for (const Property& pr : vc.props()) {
        if (!hndHeader && pr.key == "comLower") { // handling group starts here
            ImGui::SeparatorText("Handling");
            hndHeader = true;
        }
        if (!camHeader && pr.key.rfind("cam", 0) == 0) { // cam props sort last
            ImGui::SeparatorText("Follow camera");
            camHeader = true;
        }
        drawProperty(pr, &vc);
    }

    ImGui::SeparatorText("Wheels");
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

    ImGui::SeparatorText("Scene vehicles");
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

} // namespace vehicleui
