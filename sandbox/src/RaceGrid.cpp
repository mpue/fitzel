#include "RaceGrid.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include <glm/glm.hpp>

#ifndef FITZEL_PLAYER
#include <imgui.h>
#include "UiStyle.hpp"
#endif

#include "Component.hpp"
#include "RoadSystem.hpp"
#include "SandboxMath.hpp"   // sceneHeading()
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

Road makeRoad(const Track& track) {
    Road r;
    if (!track.line) return r;
    r.cl = track.line;
    r.cy = track.y;
    r.closed = track.closed;
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

// Stand `e` on an authored grid marker: the marker's spot is the GROUND, so the
// craft goes its own ride height above it, facing the marker's nose.
//
// The heading comes from `sceneHeading`, never from the marker's rotation.y. A
// decomposed Euler triple expresses the same facing in several ways -- two of the
// circuits here are authored as [-180, y, -180], where y is not the heading at
// all -- and reading the middle number off one aims the whole field into the
// scenery. It is the same trap the chase camera and the showroom's grid slot both
// fell into.
void placeOn(Entity& e, const Entity& mark, float rideHeight, int forward) {
    const glm::vec3 p(mark.center.x, mark.center.y + rideHeight, mark.center.z);
    float yawDeg = glm::degrees(sceneHeading(mark.rotation));
    if (forward == 1) yawDeg -= 180.0f;            // model nose along -Z
    e.center   = p;
    e.rotation = glm::vec3(0.0f, yawDeg, 0.0f);
    if (e.parent < 0) { e.localCenter = p; e.localRotation = e.rotation; }
}

// How far off the road's centreline a point is (metres, + = the road's own
// right). An opponent standing on a marker needs this as its base lane, or it
// spends its first seconds sliding to the middle of the road from wherever the
// author stood it. 0 when there is no road to measure against, which is also the
// right answer: no road, no lane.
float laneOf(const Road& r, const glm::vec2& p) {
    if (!r.valid()) return 0.0f;
    glm::vec2 pos, dir; float y = 0.0f;
    r.at(r.project(p), pos, dir, y);
    return glm::dot(p - pos, glm::vec2(dir.y, -dir.x));
}

// The scene's grid markers, pole first. Ties keep scene order, so a grid left
// entirely at slot 0 lines up in the order it was placed -- stable_sort, not
// sort, for exactly that.
//
// Returned as IDS rather than pointers because populate() spawns craft into the
// same vector, and a vector that grows moves everything it holds. Every
// Entity* in this file is short-lived for that reason.
std::vector<int> markerIds(const std::vector<Entity>& entities) {
    std::vector<const Entity*> m;
    for (const Entity& e : entities)
        if (e.activeInHierarchy && e.components.get<GridPositionComponent>())
            m.push_back(&e);
    std::stable_sort(m.begin(), m.end(), [](const Entity* a, const Entity* b) {
        return a->components.get<GridPositionComponent>()->slot <
               b->components.get<GridPositionComponent>()->slot;
    });
    std::vector<int> ids;
    ids.reserve(m.size());
    for (const Entity* e : m) ids.push_back(e->id);
    return ids;
}

Entity* byId(std::vector<Entity>& entities, int id) {
    for (Entity& e : entities)
        if (e.id == id) return &e;
    return nullptr;
}

// Where player two stands on an authored grid: beside player one -- the next
// marker up the grid, or the one before when player one drew the back of it.
//
// Two people at one machine start together or the pane showing the slower one is
// a picture of an empty track. Written once and shared, because populate() has to
// know which marker will be taken before it decides what to build there, and a
// rule that lived in both places would eventually disagree with itself and stand
// a rival where player two was about to appear.
int besideMark(int playerMark, int nMarks) {
    if (playerMark < 0 || nMarks <= 1) return -1;
    return (playerMark + 1 < nMarks) ? playerMark + 1 : playerMark - 1;
}

} // namespace

int populate(std::vector<Entity>& entities, int rivalsWanted, int playerCraftId,
             const std::function<int(const std::string&, const glm::vec3&,
                                     float)>& spawn, int playerCraftId2) {
    if (!spawn) return 0;
    const std::vector<int> marks = markerIds(entities);
    if (marks.empty()) return 0;

    // Rivals the scene already holds. They are the field's first members: a
    // circuit that parks two craft on its grid and is asked for five wants three
    // more, not five more. The player's own craft is not one of them even when it
    // carries an Opponent component, which is how a two-craft track is authored.
    int have = 0;
    for (const Entity& e : entities) {
        const auto* op = e.components.get<OpponentComponent>();
        if (op && op->entered && e.id != playerCraftId && e.id != playerCraftId2)
            ++have;
    }
    // How many the grid can seat besides the people playing it.
    const int seats = static_cast<int>(marks.size()) -
                      (playerCraftId >= 0 ? 1 : 0) - (playerCraftId2 >= 0 ? 1 : 0);
    const int want  = (rivalsWanted < 0) ? seats : glm::min(rivalsWanted, seats);
    int toBuild = glm::max(0, want - have);

    // WHERE THE PLAYER STANDS, decided here rather than in lineUp, because the
    // marker the player takes is the one marker that must NOT build a rival --
    // and a slot drawn twice would be two different answers.
    //
    // The draw is written back onto the marker (`player`), so the line-up that
    // follows reads it as a reservation and puts the player exactly there. That
    // write is runtime state on a scene Play has already snapshotted; Stop puts
    // the authored grid back untouched.
    int playerMark = -1;
    if (playerCraftId >= 0) {
        for (std::size_t i = 0; i < marks.size(); ++i)
            if (const Entity* e = byId(entities, marks[i]))
                if (e->components.get<GridPositionComponent>()->player) {
                    playerMark = static_cast<int>(i); break;
                }
        if (playerMark < 0) {
            // Only from the markers this race actually fills: drawing slot seven
            // for a field of three would start the player alone at the back of a
            // field that is not there.
            const int used = glm::clamp(1 + have + toBuild, 1,
                                        static_cast<int>(marks.size()));
            std::mt19937 rng{std::random_device{}()};
            playerMark = std::uniform_int_distribution<int>(0, used - 1)(rng);
            if (Entity* e = byId(entities, marks[playerMark]))
                e->components.get<GridPositionComponent>()->player = true;
        }
    }

    // ...and player two's, which lineUp will hand it. Worked out here so nothing
    // is built on the marker it is about to stand on.
    const int player2Mark = (playerCraftId2 >= 0)
        ? besideMark(playerMark, static_cast<int>(marks.size())) : -1;

    int built = 0;
    for (std::size_t i = 0; i < marks.size() && toBuild > 0; ++i) {
        if (static_cast<int>(i) == playerMark ||
            static_cast<int>(i) == player2Mark) continue;
        std::string prefab;
        glm::vec3   pos(0.0f);
        float       yawDeg = 0.0f;
        {
            Entity* m = byId(entities, marks[i]);
            if (!m) continue;
            auto* gp = m->components.get<GridPositionComponent>();
            // Already built one for this race (a grid lined up twice in one
            // session), or nothing to build: leave it alone. A marker with no
            // prefab is a parking space for a craft the scene already holds.
            if (gp->prefab.empty() || (gp->spawnedId && byId(entities, gp->spawnedId)))
                continue;
            // ...and the slot the author reserved for the player NEVER builds a
            // rival, whether or not a player craft is known yet. Its prefab means
            // something else entirely there: it is the craft the player flies
            // when the scene holds none (see ensurePlayerCraft). Without this the
            // editor's own preview -- which has no player craft to name -- would
            // stand a rival on the player's slot.
            if (gp->player) continue;
            prefab = gp->prefab;
            pos    = m->center;
            yawDeg = glm::degrees(sceneHeading(m->rotation));
        }
        // From here on nothing above may be trusted: the callback appends to
        // `entities`, and a growing vector moves every Entity it holds.
        const int id = spawn(prefab, pos, yawDeg);
        if (id <= 0) continue;
        if (Entity* m = byId(entities, marks[i]))
            m->components.get<GridPositionComponent>()->spawnedId = id;
        if (Entity* craft = byId(entities, id)) {
            // What makes it a rival rather than scenery. A craft prefab built for
            // the player has no Opponent component; giving it one here is what
            // lets the same prefab be flown and be raced against, which is the
            // whole point of a grid that builds its own field.
            auto* op = craft->components.get<OpponentComponent>();
            if (!op) {
                craft->components.items.push_back(
                    std::make_unique<OpponentComponent>());
                op = craft->components.get<OpponentComponent>();
            }
            op->entered = true;
            craft->active = true;
        }
        --toBuild;
        ++built;
    }
    return built;
}

int playerMarker(const std::vector<Entity>& entities) {
    // In slot order, so a scene with two markers ticked (an authoring mistake,
    // not a mode) answers the same way every time: the one nearest pole.
    for (int id : markerIds(entities))
        for (const Entity& e : entities)
            if (e.id == id &&
                e.components.get<GridPositionComponent>()->player)
                return id;
    return -1;
}

std::string playerMarkerPrefab(const std::vector<Entity>& entities) {
    const int id = playerMarker(entities);
    if (id < 0) return {};
    for (const Entity& e : entities)
        if (e.id == id)
            return e.components.get<GridPositionComponent>()->prefab;
    return {};
}

bool isRace(const std::vector<Entity>& entities) {
    const FinishLineComponent* fl = findLine(entities);
    return fl && fl->mode == Mode::Race;
}

int lineUp(std::vector<Entity>& entities, const RoadSystem& road,
           int playerCraftId, bool applyParticipation, int playerCraftId2) {
    return lineUp(entities,
                  Track{&road.centerline(), &road.centerlineY(), road.closed},
                  playerCraftId, applyParticipation, playerCraftId2);
}

int lineUp(std::vector<Entity>& entities, const Track& track,
           int playerCraftId, bool applyParticipation, int playerCraftId2) {
    glm::vec3 linePos(0.0f);
    const FinishLineComponent* fl = findLine(entities, &linePos);
    if (!fl) return 0;
    const Road r = makeRoad(track);
    // A missing road is only fatal when the grid has to be MEASURED along one.
    // A scene with authored markers already knows where every craft stands, and
    // refusing to line it up because nobody has pressed Build Road would be a
    // grid that works only on circuits -- which is half of what the markers are
    // for (see GridPositionComponent).
    const bool haveMarks =
        std::any_of(entities.begin(), entities.end(), [](const Entity& e) {
            return e.activeInHierarchy && e.components.get<GridPositionComponent>();
        });
    if (!r.valid() && !haveMarks) return 0;

    const float lineDist =
        r.valid() ? r.project(glm::vec2(linePos.x, linePos.z)) : 0.0f;
    const bool  race     = fl->mode == Mode::Race;

    // Who is in it. Scene order, so the grid is reproducible and the author can
    // tell which craft will end up where.
    std::vector<Entity*> field;
    for (Entity& e : entities) {
        auto* op = e.components.get<OpponentComponent>();
        if (!op) continue;
        // Player two's craft is flown, not driven by the AI: out of the field,
        // and never deactivated by a tick meant for opponents. It is placed
        // below, beside player one.
        if (playerCraftId2 >= 0 && e.id == playerCraftId2) {
            if (applyParticipation) e.active = true;
            continue;
        }
        const bool in = race && op->entered;
        if (applyParticipation) e.active = in;
        if (in) field.push_back(&e);
    }

    auto findCraft = [&](int id) -> Entity* {
        if (id < 0) return nullptr;
        for (Entity& e : entities)
            if (e.id == id) return &e;
        return nullptr;
    };
    Entity* player  = findCraft(playerCraftId);
    Entity* player2 = findCraft(playerCraftId2);
    if (player2 == player) player2 = nullptr;   // one craft cannot seat both

    // Pole or the back. Starting last is the arcade default -- a race you begin
    // at the front is a time trial with scenery -- but a scene can say otherwise.
    const int total = static_cast<int>(field.size()) + (player ? 1 : 0) +
                      (player2 ? 1 : 0);
    const int playerSlot = (!player)          ? -1
                         : fl->playerPole     ? 0
                                              : total - 1;
    // Beside player one: the next slot up the grid from theirs, which is the
    // other half of the same row (slots run in pairs, see slotOf). Clamped into
    // the grid so a one-craft field cannot send it to slot -1.
    const int player2Slot = (!player2) ? -1
                          : (!player)  ? 0
                          : glm::clamp(fl->playerPole ? playerSlot + 1
                                                      : playerSlot - 1,
                                       0, std::max(total - 1, 0));

    // --- The authored grid ---------------------------------------------------
    // Markers beat the measured rows. A scene that carries GridPosition empties
    // is a scene whose author has said where the field stands, and three numbers
    // on the start/finish line have no business overruling that.
    const std::vector<int> markIds = markerIds(entities);
    std::vector<const Entity*> marks;
    marks.reserve(markIds.size());
    for (int id : markIds)
        for (const Entity& e : entities)
            if (e.id == id) { marks.push_back(&e); break; }
    const int nMarks = static_cast<int>(marks.size());

    // WHERE THE PLAYER STARTS on an authored grid.
    //
    // A ticked marker wins: someone answered the question by hand. Otherwise the
    // slot is DRAWN, from the markers this field actually fills -- which is the
    // point of the whole feature. A grid whose player is always on pole or always
    // at the back is learned in two races and is the same race after that; drawn,
    // the first corner is a different problem every time, and the rivals ahead
    // and behind are different craft.
    //
    // Drawn per line-up, so a restart re-draws. That is deliberate: "again" after
    // a bad start should be a new start, not the same one.
    int playerMark = -1, player2Mark = -1;
    if (nMarks > 0 && player) {
        for (int i = 0; i < nMarks; ++i)
            if (marks[i]->components.get<GridPositionComponent>()->player) {
                playerMark = i; break;
            }
        if (playerMark < 0) {
            // Only from the markers in USE: with three craft on an eight-slot
            // grid, drawing slot seven would start the player alone at the back
            // of a field that is not there.
            const int used = glm::clamp(total, 1, nMarks);
            std::mt19937 rng{std::random_device{}()};
            playerMark = std::uniform_int_distribution<int>(0, used - 1)(rng);
        }
    }
    // Beside player one -- the same rule populate() built the grid around, so the
    // marker it kept free is the marker player two lands on (see besideMark).
    if (nMarks > 0 && player2) player2Mark = besideMark(playerMark, nMarks);

    // How far behind the line the LAST marker sits, so a field too big for the
    // authored grid queues up behind it in ordinary rows rather than landing on
    // top of it. Measured along the road like everything else here.
    float marksBack = fl->gridBack;
    if (r.valid())
        for (const Entity* m : marks) {
            float back = lineDist - r.project(glm::vec2(m->center.x, m->center.z));
            if (r.closed) { back = std::fmod(back, r.total);
                            if (back < 0.0f) back += r.total; }
            marksBack = std::max(marksBack, back);
        }

    int placed = 0;
    int mark = 0;   // next free authored marker
    int slot = 0;   // next measured slot (used when there are no markers)
    int over = 0;   // next overflow row, behind the authored grid
    auto nextSlot = [&]() {
        while (slot == playerSlot || slot == player2Slot) ++slot;
        return slot++;
    };
    auto nextMark = [&]() {
        while (mark == playerMark || mark == player2Mark) ++mark;
        return mark++;
    };
    // Stand one craft on the grid, wherever the grid happens to be. Returns the
    // lane it ended up in (metres off the centreline), which the AI needs so it
    // pulls away along the line it is standing on instead of diving for the
    // middle of the road on its first tick.
    auto standOn = [&](Entity& e, float rideHeight, int forward) -> float {
        if (nMarks > 0) {
            const int m = nextMark();
            if (m < nMarks) {
                placeOn(e, *marks[m], rideHeight, forward);
                return laneOf(r, glm::vec2(e.center.x, e.center.z));
            }
            // More craft than markers. Ordinary rows, behind the last marker --
            // and only if there is a road to measure them along; without one
            // there is nowhere to invent a slot, so the craft keeps the place
            // its author parked it in.
            if (!r.valid()) return 0.0f;
            const int row  = over / 2;
            const int side = (over % 2 == 0) ? -1 : 1;
            ++over;
            const float back = marksBack + static_cast<float>(row + 1) * fl->gridRow;
            const float lane = static_cast<float>(side) * fl->gridLane;
            place(e, r, lineDist - back, lane, rideHeight, forward);
            return lane;
        }
        float back = 0.0f, lane = 0.0f;
        slotOf(*fl, nextSlot(), back, lane);
        place(e, r, lineDist - back, lane, rideHeight, forward);
        return lane;
    };

    for (Entity* e : field) {
        auto* op = e->components.get<OpponentComponent>();
        const float lane = standOn(*e, op->rideHeight, op->forward);
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

    auto placePlayer = [&](Entity* e, int s, int m) {
        if (!e) return;
        const auto* gc = e->components.get<GliderComponent>();
        const float ride = gc ? gc->rideHeight : 1.6f;
        const int   fwd  = gc ? gc->forward : 0;
        if (nMarks > 0) {
            if (m < 0 || m >= nMarks) return;
            placeOn(*e, *marks[m], ride, fwd);
        } else {
            if (s < 0) return;
            float back = 0.0f, lane = 0.0f;
            slotOf(*fl, s, back, lane);
            place(*e, r, lineDist - back, lane, ride, fwd);
        }
        ++placed;
    };
    placePlayer(player,  playerSlot,  playerMark);
    placePlayer(player2, player2Slot, player2Mark);
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
    // Where the field will actually stand: the markers, when the scene has any.
    // Said here rather than left to be found out at the first start, because the
    // three numbers above go quiet the moment one marker exists -- and a setting
    // that silently stops doing anything is worse than one that is not there.
    int marks = 0, pinned = 0, building = 0;
    for (const Entity& e : entities)
        if (const auto* gp = e.components.get<GridPositionComponent>()) {
            ++marks;
            if (gp->player) ++pinned;
            if (!gp->prefab.empty()) ++building;
        }
    const int entrants = entered + 1;   // the field, plus the player
    if (marks > 0) {
        ui::hint("%d Grid Position marker%s in this scene -- the rows above are\n"
                 "not used. Your own slot is %s.",
                 marks, marks == 1 ? "" : "s",
                 pinned > 0 ? "the marker that reserves it"
                            : "drawn at random from the ones in use");
        if (building > 0)
            ui::hint("%d of them build their own rival, so the field is whatever\n"
                     "the start screen asks for -- up to %d of them.",
                     building, glm::max(marks - 1, 0));
        // The two ways a grid ends up with nobody on it. Both look identical at
        // the start line (an empty grid), and neither is this panel's fault, so
        // both are said here rather than found out at the lights.
        if (building == 0 && entered == 0)
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f),
                               "Nothing to line up: no rivals in the scene, and no\n"
                               "marker builds one. Give a marker a rival prefab.");
        else if (marks < entrants)
            ImGui::TextDisabled("%d craft, %d markers -- the rest queue up behind"
                                " in rows", entrants, marks);
    }
    // Markers need no road under them; measured rows do.
    ImGui::BeginDisabled(!road.built() && marks == 0);
    if (ImGui::Button("Line up the grid", ImVec2(-1.0f, 0.0f))) {
        // Preview only: it moves craft, it does not deactivate the ones sitting
        // out. A look at the grid that quietly switched half the scene off -- and
        // then got saved -- would be a trap.
        lineUp(entities, road, -1, /*applyParticipation=*/false);
        changed = true;
    }
    ImGui::EndDisabled();
    if (!road.built() && marks == 0)
        ImGui::TextDisabled("Build the road first, or place Grid Position markers");
    else
        ui::hint("Starting a race lines the field up by itself; this is just to\n"
                 "see it. The player is placed too, but only at Play, when it is\n"
                 "known which craft is being flown.");
    return changed;
}
#endif

} // namespace racegrid
