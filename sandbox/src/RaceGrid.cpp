#include "RaceGrid.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include <glm/glm.hpp>

#ifndef FITZEL_PLAYER
#include <imgui.h>
#include "UiStyle.hpp"
#endif

#include "Component.hpp"
#include "RoadSystem.hpp"
#include "SceneTypes.hpp"

namespace racegrid {

namespace {

// Arc length of the point on the road nearest `p`, and the road's frame there.
// The grid is measured ALONG the road rather than in a straight line back from
// the line, so it follows the track onto a curved approach instead of marching
// off into the scenery.
struct Road {
    const std::vector<glm::vec2>* cl = nullptr;
    const std::vector<float>*     cy = nullptr;
    std::vector<float>            s;
    float total = 0.0f;
    bool  closed = false;

    bool valid() const { return cl && cl->size() >= 2 && total > 1.0f; }

    float project(const glm::vec2& p) const {
        float best = 0.0f, bestD2 = 1e30f;
        for (std::size_t i = 0; i + 1 < cl->size(); ++i) {
            const glm::vec2 a = (*cl)[i], b = (*cl)[i + 1];
            const glm::vec2 ab = b - a;
            const float L2 = glm::dot(ab, ab);
            const float t = (L2 > 1e-8f) ? glm::clamp(glm::dot(p - a, ab) / L2, 0.0f, 1.0f)
                                         : 0.0f;
            const glm::vec2 q = a + ab * t;
            const float d2 = glm::dot(p - q, p - q);
            if (d2 < bestD2) { bestD2 = d2; best = s[i] + t * glm::length(ab); }
        }
        return best;
    }

    void at(float q, glm::vec2& pos, glm::vec2& dir, float& y) const {
        if (closed) { q = std::fmod(q, total); if (q < 0.0f) q += total; }
        else        q = glm::clamp(q, 0.0f, total);
        std::size_t i = static_cast<std::size_t>(
            std::lower_bound(s.begin(), s.end(), q) - s.begin());
        if (i == 0) i = 1;
        if (i >= cl->size()) i = cl->size() - 1;
        const float seg = std::max(s[i] - s[i - 1], 1e-5f);
        const float t   = glm::clamp((q - s[i - 1]) / seg, 0.0f, 1.0f);
        pos = (*cl)[i - 1] + ((*cl)[i] - (*cl)[i - 1]) * t;
        const glm::vec2 d = (*cl)[i] - (*cl)[i - 1];
        const float l = glm::length(d);
        dir = (l > 1e-5f) ? d / l : glm::vec2(0.0f, 1.0f);
        y = (cy && cy->size() == cl->size())
                ? glm::mix((*cy)[i - 1], (*cy)[i], t) : 0.0f;
    }
};

Road makeRoad(const RoadSystem& road) {
    Road r;
    r.cl = &road.centerline();
    r.cy = &road.centerlineY();
    r.closed = road.closed;
    if (r.cl->size() < 2) return r;
    r.s.assign(r.cl->size(), 0.0f);
    for (std::size_t i = 1; i < r.cl->size(); ++i)
        r.s[i] = r.s[i - 1] + glm::length((*r.cl)[i] - (*r.cl)[i - 1]);
    r.total = r.s.back();
    return r;
}

const FinishLineComponent* findLine(const std::vector<Entity>& entities,
                                    glm::vec3* outPos = nullptr) {
    for (const Entity& e : entities) {
        if (!e.activeInHierarchy) continue;
        if (const auto* fl = e.components.get<FinishLineComponent>()) {
            if (outPos) *outPos = e.center;
            return fl;
        }
    }
    return nullptr;
}

// Slot k, counting from pole: rows of two, alternating sides. Row 0 is
// `gridBack` behind the line, so nobody starts across it.
void slotOf(const FinishLineComponent& fl, int k, float& backMetres, float& lane) {
    const int row  = k / 2;
    const int side = (k % 2 == 0) ? -1 : 1;
    backMetres = fl.gridBack + static_cast<float>(row) * fl.gridRow;
    lane       = static_cast<float>(side) * fl.gridLane;
}

// Stand `e` on the road at `dist` metres before the line, `lane` metres across,
// facing along the track. Craft parented to something else are left alone rather
// than teleported by a local transform that means something different.
void place(Entity& e, const Road& r, float dist, float lane, float rideHeight,
           int forward) {
    glm::vec2 pos, dir; float y = 0.0f;
    r.at(dist, pos, dir, y);
    const glm::vec2 side(dir.y, -dir.x);           // the road's own right, as loft uses
    const glm::vec3 p(pos.x + side.x * lane, y + rideHeight, pos.y + side.y * lane);
    float yawDeg = glm::degrees(std::atan2(dir.x, dir.y));
    if (forward == 1) yawDeg -= 180.0f;            // model nose along -Z
    e.center = p;
    e.rotation = glm::vec3(0.0f, yawDeg, 0.0f);
    if (e.parent < 0) { e.localCenter = p; e.localRotation = e.rotation; }
}

} // namespace

bool isRace(const std::vector<Entity>& entities) {
    const FinishLineComponent* fl = findLine(entities);
    return fl && fl->mode == Mode::Race;
}

int lineUp(std::vector<Entity>& entities, const RoadSystem& road,
           int playerCraftId, bool applyParticipation) {
    glm::vec3 linePos(0.0f);
    const FinishLineComponent* fl = findLine(entities, &linePos);
    if (!fl) return 0;
    const Road r = makeRoad(road);
    if (!r.valid()) return 0;

    const float lineDist = r.project(glm::vec2(linePos.x, linePos.z));
    const bool  race     = fl->mode == Mode::Race;

    // Who is in it. Scene order, so the grid is reproducible and the author can
    // tell which craft will end up where.
    std::vector<Entity*> field;
    for (Entity& e : entities) {
        auto* op = e.components.get<OpponentComponent>();
        if (!op) continue;
        const bool in = race && op->entered;
        if (applyParticipation) e.active = in;
        if (in) field.push_back(&e);
    }

    Entity* player = nullptr;
    if (playerCraftId >= 0)
        for (Entity& e : entities)
            if (e.id == playerCraftId) { player = &e; break; }

    // Pole or the back. Starting last is the arcade default -- a race you begin
    // at the front is a time trial with scenery -- but a scene can say otherwise.
    const int total = static_cast<int>(field.size()) + (player ? 1 : 0);
    const int playerSlot = (!player)          ? -1
                         : fl->playerPole     ? 0
                                              : total - 1;

    int placed = 0;
    int slot = 0;
    auto nextSlot = [&]() {
        while (slot == playerSlot) ++slot;
        return slot++;
    };

    for (Entity* e : field) {
        auto* op = e->components.get<OpponentComponent>();
        float back = 0.0f, lane = 0.0f;
        slotOf(*fl, nextSlot(), back, lane);
        place(*e, r, lineDist - back, lane, op->rideHeight, op->forward);
        // The opponent seeds its race distance from where it stands, so the two
        // must not disagree: clear the manual stagger and hand it the lane it was
        // actually put in.
        op->startDistance = 0.0f;
        op->laneOffset    = lane;
        op->laneCur       = lane;
        op->laneSeeded    = true;
        op->started       = false;   // re-seed dist from the new spot
        op->curSpeed      = 0.0f;
        ++placed;
    }

    if (player) {
        float back = 0.0f, lane = 0.0f;
        slotOf(*fl, playerSlot, back, lane);
        const auto* gc = player->components.get<GliderComponent>();
        place(*player, r, lineDist - back, lane, gc ? gc->rideHeight : 1.6f,
              gc ? gc->forward : 0);
        ++placed;
    }
    return placed;
}

#ifndef FITZEL_PLAYER
bool inspector(FinishLineComponent& fl, std::vector<Entity>& entities,
               const RoadSystem& road) {
    // The caller has already drawn the component's own properties (it hides the
    // three sound fields to swap in pickers), so this adds only what metadata
    // cannot express: a list of other entities, and a button.
    bool changed = false;

    // The roster. Listing the scene's opponents HERE, next to the mode, is the
    // point: "which of these fly against me" is one question, and answering it by
    // hunting each craft down in the hierarchy and finding a tick box on it is
    // the same answer given badly.
    ui::sectionText("Field");
    if (fl.mode == Mode::TimeTrial) {
        ui::hint("Time trial: the player runs alone. Every opponent sits this\n"
                 "one out -- they stay in the scene, they just do not start.");
    }
    int entered = 0, total = 0;
    ImGui::BeginDisabled(fl.mode == Mode::TimeTrial);
    for (Entity& e : entities) {
        auto* op = e.components.get<OpponentComponent>();
        if (!op) continue;
        ++total;
        if (op->entered) ++entered;
        ImGui::PushID(e.id);
        if (ImGui::Checkbox(e.name.empty() ? "(unnamed)" : e.name.c_str(),
                            &op->entered))
            changed = true;
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    if (total == 0)
        ImGui::TextDisabled("No opponents in this scene");
    else if (fl.mode == Mode::Race)
        ui::hint("%d of %d entered -- a field of %d with the player.",
                 entered, total, entered + 1);

    ui::sectionText("Grid");
    ImGui::BeginDisabled(!road.built());
    if (ImGui::Button("Line up the grid", ImVec2(-1.0f, 0.0f))) {
        // Preview only: it moves craft, it does not deactivate the ones sitting
        // out. A look at the grid that quietly switched half the scene off -- and
        // then got saved -- would be a trap.
        lineUp(entities, road, -1, /*applyParticipation=*/false);
        changed = true;
    }
    ImGui::EndDisabled();
    if (!road.built())
        ImGui::TextDisabled("Build the road first -- the grid is measured along it");
    else
        ui::hint("Starting a race lines the field up by itself; this is just to\n"
                 "see it. The player is placed too, but only at Play, when it is\n"
                 "known which craft is being flown.");
    return changed;
}
#endif

} // namespace racegrid
