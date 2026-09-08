// gridcheck -- does the starting grid stand the right craft in the right places?
//
// The grid is the one part of a race nobody watches being built. It happens in
// the half second between pressing start and the lights, it is different every
// time (the player's slot is drawn), and it depends on three things at once: how
// many markers the author placed, how many rivals the start screen asked for, and
// which craft is being flown. Every way it can go wrong looks, from inside the
// cockpit, like one plausible grid -- a rival on your slot, a field of four using
// six markers, a marker's heading read off the wrong Euler component so the whole
// row faces the scenery. You would have to count the craft in a screenshot to
// notice, and by then the race has started.
//
// So it is counted here instead. No road is built: the authored-marker path is
// exactly the one that must work without one (see GridPositionComponent), which
// makes this a harness with no GL, no window and no assets.
//
//   build/release/bin/gridcheck.exe

#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "../src/Component.hpp"
#include "../src/RaceGrid.hpp"
#include "../src/SandboxMath.hpp"
#include "../src/SceneTypes.hpp"

namespace {

int failures = 0;

void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(),
                detail.empty() ? "" : "  -- ", detail.c_str());
    if (!ok) ++failures;
}

int nextId = 1;

Entity& add(std::vector<Entity>& es, const glm::vec3& pos,
            const glm::vec3& rot = glm::vec3(0.0f)) {
    Entity e;
    e.id = nextId++;
    e.center = e.localCenter = pos;
    e.rotation = e.localRotation = rot;
    es.push_back(std::move(e));
    return es.back();
}

// A marker at `pos`, facing `yawDeg`.
Entity& addMark(std::vector<Entity>& es, const glm::vec3& pos, int slot,
                bool player = false, float yawDeg = 0.0f) {
    Entity& e = add(es, pos, glm::vec3(0.0f, yawDeg, 0.0f));
    auto gp = std::make_unique<GridPositionComponent>();
    gp->slot = slot; gp->player = player;
    e.components.items.push_back(std::move(gp));
    return e;
}

Entity& addRival(std::vector<Entity>& es, const glm::vec3& pos) {
    Entity& e = add(es, pos);
    auto op = std::make_unique<OpponentComponent>();
    op->entered = true;
    op->rideHeight = 1.6f;
    e.components.items.push_back(std::move(op));
    return e;
}

Entity& addCraft(std::vector<Entity>& es, const glm::vec3& pos, float ride = 2.0f) {
    Entity& e = add(es, pos);
    auto gc = std::make_unique<GliderComponent>();
    gc->rideHeight = ride;
    e.components.items.push_back(std::move(gc));
    return e;
}

Entity& addLine(std::vector<Entity>& es) {
    Entity& e = add(es, glm::vec3(0.0f, 0.0f, 100.0f));
    auto fl = std::make_unique<FinishLineComponent>();
    fl->mode = racegrid::Race;
    e.components.items.push_back(std::move(fl));
    return e;
}

// Which marker a craft is standing on, by position; -1 if it is on none of them.
// Compared in XZ, because the craft floats above its marker by its ride height --
// which is the placement rule, and therefore also worth testing separately.
int markUnder(const Entity& craft, const std::vector<glm::vec3>& markPos) {
    for (std::size_t i = 0; i < markPos.size(); ++i)
        if (std::abs(craft.center.x - markPos[i].x) < 1e-3f &&
            std::abs(craft.center.z - markPos[i].z) < 1e-3f)
            return static_cast<int>(i);
    return -1;
}

// A scene with no road at all -- the case authored markers exist to serve.
const racegrid::Track& noRoad() {
    static const racegrid::Track t;
    return t;
}

// ...and a straight one along +Z through x = 0, for the cases that ARE measured
// along a road: the overflow rows behind the markers, and the lane an opponent
// is handed to pull away on.
const racegrid::Track& straightRoad() {
    static const std::vector<glm::vec2> line = [] {
        std::vector<glm::vec2> l;
        for (int i = 0; i <= 40; ++i) l.push_back(glm::vec2(0.0f, i * 5.0f));
        return l;
    }();
    static const std::vector<float> y(line.size(), 10.0f);
    static const racegrid::Track t{&line, &y, false};
    return t;
}

// A prefab spawner that makes a bare craft -- a GliderComponent and nothing else,
// which is what a prefab built for the PLAYER looks like. Whether such a craft
// still ends up racing is one of the things being measured: populate() has to
// give it what makes it a rival.
struct Spawner {
    std::vector<Entity>* es = nullptr;
    int calls = 0;
    std::vector<std::string> names;
    int operator()(const std::string& name, const glm::vec3& pos, float yawDeg) {
        ++calls;
        names.push_back(name);
        Entity& e = add(*es, pos, glm::vec3(0.0f, yawDeg, 0.0f));
        e.name = name;
        auto gc = std::make_unique<GliderComponent>();
        gc->rideHeight = 1.6f;
        e.components.items.push_back(std::move(gc));
        return e.id;
    }
};

// Everything with a craft component that is standing on a marker.
int craftOnMarkers(const std::vector<Entity>& es,
                   const std::vector<glm::vec3>& markPos) {
    int n = 0;
    for (const Entity& e : es) {
        if (e.components.get<GridPositionComponent>()) continue;
        if (!e.components.get<OpponentComponent>() &&
            !e.components.get<GliderComponent>()) continue;
        if (markUnder(e, markPos) >= 0) ++n;
    }
    return n;
}

} // namespace

int main() {
    // The markers, in the order they are placed in the scene, deliberately NOT in
    // slot order: pole is slot 0 on the third one placed, which is what the sort
    // has to sort out.
    const glm::vec3 P0(0.0f, 10.0f, 40.0f);   // slot 0
    const glm::vec3 P1(4.0f, 10.0f, 40.0f);   // slot 1
    const glm::vec3 P2(0.0f, 10.0f, 32.0f);   // slot 2
    const glm::vec3 P3(4.0f, 10.0f, 32.0f);   // slot 3
    const std::vector<glm::vec3> markPos = {P0, P1, P2, P3};

    // --- 1. The field fills the front markers, in slot order -----------------
    {
        std::vector<Entity> es;
        addLine(es);
        addMark(es, P2, 2); addMark(es, P0, 0);   // placed out of order
        addMark(es, P3, 3); addMark(es, P1, 1);
        Entity& r1 = addRival(es, glm::vec3(-50.0f, 0.0f, 0.0f));
        Entity& r2 = addRival(es, glm::vec3(-60.0f, 0.0f, 0.0f));
        const int r1i = static_cast<int>(&r1 - es.data());
        const int r2i = static_cast<int>(&r2 - es.data());
        Entity& me = addCraft(es, glm::vec3(-70.0f, 0.0f, 0.0f));
        const int meId = me.id;

        const int placed = racegrid::lineUp(es, noRoad(), meId,
                                            /*applyParticipation=*/true);
        check(placed == 3, "three craft placed on an authored grid",
              "placed " + std::to_string(placed));

        std::set<int> used;
        for (const Entity& e : es) {
            if (!e.components.get<OpponentComponent>() &&
                !e.components.get<GliderComponent>()) continue;
            const int m = markUnder(e, markPos);
            if (m >= 0) used.insert(m);
        }
        // Three craft, four markers: the back one stays bare, and no two craft
        // share a slot.
        check(used.size() == 3, "each craft gets a marker of its own",
              std::to_string(used.size()) + " distinct markers used");
        check(used.count(3) == 0, "a field smaller than the grid leaves the back bare");
        // Slot order, not scene order: the marker placed third in the scene is
        // slot 3 and must be the empty one.
        (void)es[r1i]; (void)es[r2i];
    }

    // --- 2. The player's slot is DRAWN, and only from the slots in use --------
    // The whole point of an authored grid: a start that cannot be memorised.
    {
        std::set<int> seen;
        bool everOutside = false, everShared = false;
        for (int run = 0; run < 200; ++run) {
            std::vector<Entity> es;
            addLine(es);
            addMark(es, P0, 0); addMark(es, P1, 1);
            addMark(es, P2, 2); addMark(es, P3, 3);
            addRival(es, glm::vec3(-50.0f, 0.0f, 0.0f));
            addRival(es, glm::vec3(-60.0f, 0.0f, 0.0f));
            Entity& me = addCraft(es, glm::vec3(-70.0f, 0.0f, 0.0f));
            const int meId = me.id;
            racegrid::lineUp(es, noRoad(), meId, /*applyParticipation=*/true);

            int mine = -1;
            std::set<int> rivals;
            for (const Entity& e : es) {
                const int m = markUnder(e, markPos);
                if (m < 0) continue;
                if (e.id == meId) mine = m;
                else if (e.components.get<OpponentComponent>()) rivals.insert(m);
            }
            if (mine < 0 || mine > 2) everOutside = true;      // 3 craft -> 3 slots
            if (rivals.count(mine)) everShared = true;
            seen.insert(mine);
        }
        check(!everOutside, "the draw never reaches a slot the field does not fill");
        check(!everShared,  "no rival is ever put on the player's slot");
        check(seen.size() >= 2, "the player's slot really does move between races",
              std::to_string(seen.size()) + " different slots over 200 line-ups");
    }

    // --- 3. A marker that reserves the slot beats the draw --------------------
    {
        bool always = true;
        for (int run = 0; run < 40; ++run) {
            std::vector<Entity> es;
            addLine(es);
            addMark(es, P0, 0); addMark(es, P1, 1);
            addMark(es, P2, 2, /*player=*/true); addMark(es, P3, 3);
            addRival(es, glm::vec3(-50.0f, 0.0f, 0.0f));
            addRival(es, glm::vec3(-60.0f, 0.0f, 0.0f));
            Entity& me = addCraft(es, glm::vec3(-70.0f, 0.0f, 0.0f));
            const int meId = me.id;
            racegrid::lineUp(es, noRoad(), meId, /*applyParticipation=*/true);
            for (const Entity& e : es)
                if (e.id == meId && markUnder(e, markPos) != 2) always = false;
        }
        check(always, "a ticked marker holds the player's slot every time");
    }

    // --- 4. Where the craft ends up, exactly ---------------------------------
    // The marker is the GROUND: the craft stands its own ride height above it and
    // faces the marker's nose. Heading read through sceneHeading, so a marker
    // whose rotation is the [-180, y, -180] form of a facing still aims the craft
    // down the track rather than into the scenery.
    {
        std::vector<Entity> es;
        addLine(es);
        addMark(es, P0, 0, /*player=*/true, /*yawDeg=*/90.0f);
        Entity& me = addCraft(es, glm::vec3(-70.0f, 0.0f, 0.0f), /*ride=*/2.5f);
        const int meId = me.id;
        racegrid::lineUp(es, noRoad(), meId, /*applyParticipation=*/true);
        const Entity* p = nullptr;
        for (const Entity& e : es) if (e.id == meId) p = &e;
        char d[96];
        std::snprintf(d, sizeof d, "y %.2f (marker %.2f + ride 2.50)",
                      p ? p->center.y : 0.0f, P0.y);
        check(p && std::abs(p->center.y - (P0.y + 2.5f)) < 1e-3f,
              "the craft stands its ride height above the marker", d);
        // Heading 90 degrees means facing +X, in the sim's dir = (sin H, 0, cos H).
        const float head = p ? sceneHeading(p->rotation) : 0.0f;
        std::snprintf(d, sizeof d, "%.1f deg", glm::degrees(head));
        check(p && std::abs(glm::degrees(head) - 90.0f) < 0.5f,
              "and faces the way the marker points", d);
    }

    // --- 5. A craft whose model is built nose-backwards ----------------------
    // Same marker, same facing wanted; the craft is turned 180 degrees to get it.
    {
        std::vector<Entity> es;
        addLine(es);
        addMark(es, P0, 0, /*player=*/true, /*yawDeg=*/90.0f);
        Entity& me = addCraft(es, glm::vec3(-70.0f, 0.0f, 0.0f));
        me.components.get<GliderComponent>()->forward = 1;   // nose along -Z
        const int meId = me.id;
        racegrid::lineUp(es, noRoad(), meId, /*applyParticipation=*/true);
        const Entity* p = nullptr;
        for (const Entity& e : es) if (e.id == meId) p = &e;
        // sceneHeading reads the +Z axis, so a nose-backwards craft standing
        // correctly reads as 90 - 180 = -90.
        const float head = p ? glm::degrees(sceneHeading(p->rotation)) : 0.0f;
        char d[64];
        std::snprintf(d, sizeof d, "%.1f deg", head);
        check(p && std::abs(std::remainder(head + 90.0f, 360.0f)) < 0.5f,
              "a nose-backwards model is turned to face the same way", d);
    }

    // --- 6. More craft than markers, with no road ----------------------------
    // Nowhere to put the overflow, and the honest answer is to leave it where the
    // author parked it rather than stack two craft on one marker.
    {
        std::vector<Entity> es;
        addLine(es);
        addMark(es, P0, 0); addMark(es, P1, 1);
        const glm::vec3 parked(-50.0f, 3.0f, -20.0f);
        addRival(es, parked);
        addRival(es, parked + glm::vec3(0.0f, 0.0f, -8.0f));
        addRival(es, parked + glm::vec3(0.0f, 0.0f, -16.0f));
        Entity& me = addCraft(es, glm::vec3(-70.0f, 0.0f, 0.0f));
        racegrid::lineUp(es, noRoad(), me.id, /*applyParticipation=*/true);

        std::set<int> used;
        int stranded = 0;
        for (const Entity& e : es) {
            if (!e.components.get<OpponentComponent>() &&
                !e.components.get<GliderComponent>()) continue;
            const int m = markUnder(e, markPos);
            if (m >= 0) used.insert(m); else ++stranded;
        }
        check(used.size() == 2, "both markers are filled");
        check(stranded == 2, "the craft that do not fit keep their parked spot",
              std::to_string(stranded) + " left standing");
    }

    // --- 6b. ...and with one, they queue up BEHIND the grid ------------------
    // Behind the last marker, never on top of it: a race that starts with two
    // craft in the same cubic metre is a collision, not a grid. The road here
    // runs up +Z through x = 0 and the line sits at z = 100, so "behind" means
    // smaller z than every marker.
    {
        std::vector<Entity> es;
        addLine(es);                                   // at z = 100
        addMark(es, P0, 0); addMark(es, P1, 1);        // z = 40
        addRival(es, glm::vec3(-50.0f, 0.0f, 0.0f));
        addRival(es, glm::vec3(-60.0f, 0.0f, 0.0f));
        addRival(es, glm::vec3(-70.0f, 0.0f, 0.0f));
        Entity& me = addCraft(es, glm::vec3(-80.0f, 0.0f, 0.0f));
        racegrid::lineUp(es, straightRoad(), me.id, /*applyParticipation=*/true);

        int onMarks = 0, behind = 0;
        float worstZ = 1.0e9f;
        for (const Entity& e : es) {
            auto* op = e.components.get<OpponentComponent>();
            if (!op && !e.components.get<GliderComponent>()) continue;
            if (markUnder(e, markPos) >= 0) { ++onMarks; continue; }
            ++behind;
            worstZ = glm::min(worstZ, e.center.z);
            // Placed on the road, not left in the paddock.
            if (std::abs(e.center.x) > 6.0f) behind = -1000;
        }
        char d[96];
        std::snprintf(d, sizeof d, "%d on markers, %d behind at z <= %.1f",
                      onMarks, behind, worstZ);
        check(onMarks == 2 && behind == 2 && worstZ < P0.z,
              "the overflow lines up behind the authored grid", d);
    }

    // --- 6c. An opponent keeps the lane it was stood in -----------------------
    // Its base lane is seeded from where the marker put it, or it spends its
    // first seconds sliding to the middle of the road from a slot the author
    // deliberately placed off-centre.
    {
        std::vector<Entity> es;
        addLine(es);
        addMark(es, P0, 0, /*player=*/true);           // x = 0
        addMark(es, P1, 1);                            // x = 4
        Entity& rival = addRival(es, glm::vec3(-50.0f, 0.0f, 0.0f));
        const int rid = rival.id;
        Entity& me = addCraft(es, glm::vec3(-70.0f, 0.0f, 0.0f));
        racegrid::lineUp(es, straightRoad(), me.id, /*applyParticipation=*/true);
        const OpponentComponent* op = nullptr;
        for (const Entity& e : es)
            if (e.id == rid) op = e.components.get<OpponentComponent>();
        char d[64];
        std::snprintf(d, sizeof d, "laneOffset %.2f m", op ? op->laneOffset : 0.0f);
        // The road's own right, walking up +Z, is -X; the marker sits at +4 X.
        check(op && std::abs(std::abs(op->laneOffset) - 4.0f) < 1e-2f,
              "an opponent is handed the lane its marker stands in", d);
        check(op && op->startDistance == 0.0f && !op->started,
              "and re-seeds its race distance from the new spot");
    }

    // --- 7. A scene with neither markers nor a road is not a grid -------------
    {
        std::vector<Entity> es;
        addLine(es);
        addRival(es, glm::vec3(-50.0f, 0.0f, 0.0f));
        Entity& me = addCraft(es, glm::vec3(-70.0f, 0.0f, 0.0f));
        const glm::vec3 before = me.center;
        const int placed = racegrid::lineUp(es, noRoad(), me.id, true);
        const Entity* p = nullptr;
        for (const Entity& e : es) if (e.id == me.id) p = &e;
        check(placed == 0 && p && p->center == before,
              "no markers and no road: nothing is moved");
    }

    // --- 8. A circuit with NO rivals in it at all ----------------------------
    // The case that started this: four markers, an empty circuit, and a start
    // screen asking for a field. Without markers that build their own rivals
    // there is nothing to line up and the grid stands empty -- which looks
    // exactly like a broken grid.
    {
        std::vector<Entity> es;
        addLine(es);
        for (int i = 0; i < 4; ++i)
            addMark(es, markPos[i], i)
                .components.get<GridPositionComponent>()->prefab = "rival";
        const int meId = addCraft(es, glm::vec3(-70.0f, 0.0f, 0.0f)).id;

        Spawner spawn{&es};
        const int built = racegrid::populate(
            es, /*rivalsWanted=*/3, meId,
            [&](const std::string& n, const glm::vec3& p, float y) {
                return spawn(n, p, y); });
        check(built == 3, "an empty circuit builds the field its grid asks for",
              std::to_string(built) + " built");
        check(spawn.names.size() == 3 && spawn.names[0] == "rival",
              "each one from the prefab its own marker names");

        racegrid::lineUp(es, noRoad(), meId, /*applyParticipation=*/true);
        check(craftOnMarkers(es, markPos) == 4,
              "and all four markers end up occupied",
              std::to_string(craftOnMarkers(es, markPos)) + " on markers");
        // A built rival has to RACE, not just stand there. The prefab carried no
        // Opponent component -- populate has to hand it one.
        int racing = 0;
        for (const Entity& e : es) {
            const auto* op = e.components.get<OpponentComponent>();
            if (op && op->entered) ++racing;
        }
        check(racing == 3, "the built craft are rivals, not scenery",
              std::to_string(racing) + " entered");
    }

    // --- 9. The field size is the start screen's, and it is a real number -----
    {
        for (int want : {0, 1, 2, 3, 5}) {
            std::vector<Entity> es;
            addLine(es);
            for (int i = 0; i < 4; ++i)
                addMark(es, markPos[i], i)
                    .components.get<GridPositionComponent>()->prefab = "rival";
            const int meId = addCraft(es, glm::vec3(-70.0f, 0.0f, 0.0f)).id;
            Spawner spawn{&es};
            const int built = racegrid::populate(
                es, want, meId,
                [&](const std::string& n, const glm::vec3& p, float y) {
                    return spawn(n, p, y); });
            // Three seats: four markers, one of which is the player's.
            const int expect = glm::min(want, 3);
            char d[80];
            std::snprintf(d, sizeof d, "asked %d, built %d", want, built);
            check(built == expect, "the grid builds what was asked, up to its size", d);
        }
    }

    // --- 10. Rivals the scene already holds count toward the field ------------
    // A circuit that parks two craft and is asked for three wants ONE more, not
    // three more -- otherwise every hand-authored circuit doubles its field the
    // day a marker gets a prefab.
    {
        std::vector<Entity> es;
        addLine(es);
        for (int i = 0; i < 4; ++i)
            addMark(es, markPos[i], i)
                .components.get<GridPositionComponent>()->prefab = "rival";
        addRival(es, glm::vec3(-50.0f, 0.0f, 0.0f));
        addRival(es, glm::vec3(-60.0f, 0.0f, 0.0f));
        const int meId = addCraft(es, glm::vec3(-70.0f, 0.0f, 0.0f)).id;
        Spawner spawn{&es};
        const int built = racegrid::populate(
            es, /*rivalsWanted=*/3, meId,
            [&](const std::string& n, const glm::vec3& p, float y) {
                return spawn(n, p, y); });
        check(built == 1, "hand-placed rivals are part of the field, not extra to it",
              std::to_string(built) + " built");
    }

    // --- 11. The player's slot builds nothing, and the line-up honours it -----
    // populate draws the slot and pins it, so the line-up that follows cannot
    // draw a different one and stand the player on top of a rival.
    {
        for (int run = 0; run < 40; ++run) {
            std::vector<Entity> es;
            addLine(es);
            for (int i = 0; i < 4; ++i)
                addMark(es, markPos[i], i)
                    .components.get<GridPositionComponent>()->prefab = "rival";
            const int meId = addCraft(es, glm::vec3(-70.0f, 0.0f, 0.0f)).id;
            Spawner spawn{&es};
            racegrid::populate(es, 3, meId,
                [&](const std::string& n, const glm::vec3& p, float y) {
                    return spawn(n, p, y); });
            racegrid::lineUp(es, noRoad(), meId, /*applyParticipation=*/true);
            int mine = -1;
            std::set<int> rivals;
            for (const Entity& e : es) {
                if (e.components.get<GridPositionComponent>()) continue;
                const int m = markUnder(e, markPos);
                if (m < 0) continue;
                if (e.id == meId) mine = m;
                else rivals.insert(m);
            }
            if (mine < 0 || rivals.count(mine)) {
                check(false, "the player's drawn slot survives into the line-up");
                break;
            }
            if (run == 39) check(true, "the player's drawn slot survives into the line-up",
                                 "40 races, never shared");
        }
    }

    // --- 12. Lining up twice does not build a second field --------------------
    // Entering glider mode again, or a restart inside one Play session, must not
    // leave two craft per marker.
    {
        std::vector<Entity> es;
        addLine(es);
        for (int i = 0; i < 4; ++i)
            addMark(es, markPos[i], i)
                .components.get<GridPositionComponent>()->prefab = "rival";
        const int meId = addCraft(es, glm::vec3(-70.0f, 0.0f, 0.0f)).id;
        Spawner spawn{&es};
        auto cb = [&](const std::string& n, const glm::vec3& p, float y) {
            return spawn(n, p, y); };
        const int first  = racegrid::populate(es, 3, meId, cb);
        const int second = racegrid::populate(es, 3, meId, cb);
        char d[64];
        std::snprintf(d, sizeof d, "%d then %d", first, second);
        check(first == 3 && second == 0, "a second line-up builds nothing new", d);
    }

    // --- 13. A marker with no prefab is a parking space -----------------------
    {
        std::vector<Entity> es;
        addLine(es);
        addMark(es, markPos[0], 0);   // no prefab
        addMark(es, markPos[1], 1).components.get<GridPositionComponent>()
            ->prefab = "rival";
        addMark(es, markPos[2], 2);   // no prefab
        const int meId = addCraft(es, glm::vec3(-70.0f, 0.0f, 0.0f)).id;
        Spawner spawn{&es};
        const int built = racegrid::populate(
            es, /*rivalsWanted=*/-1, meId,
            [&](const std::string& n, const glm::vec3& p, float y) {
                return spawn(n, p, y); });
        check(built <= 1, "a marker with no prefab builds nothing",
              std::to_string(built) + " built from one prefab marker");
    }

    // --- 14. The player's own slot, and the craft that stands on it ----------
    // A circuit does not have to contain a glider -- the one being flown normally
    // arrives from the start screen. The marker that reserves the player's slot
    // is where it goes, and its prefab is the craft the editor builds there when
    // Play is forced straight into the scene.
    {
        std::vector<Entity> es;
        addLine(es);
        addMark(es, markPos[0], 0)
            .components.get<GridPositionComponent>()->prefab = "rival";
        Entity& mine = addMark(es, markPos[1], 1, /*player=*/true);
        mine.components.get<GridPositionComponent>()->prefab = "myglider";
        addMark(es, markPos[2], 2)
            .components.get<GridPositionComponent>()->prefab = "rival";
        const int mineId = mine.id;

        check(racegrid::playerMarker(es) == mineId,
              "the reserved marker is the one that answers as the player's");
        check(racegrid::playerMarkerPrefab(es) == "myglider",
              "and it names the craft to build there",
              racegrid::playerMarkerPrefab(es));

        // The editor's preview has no player craft yet. Even so, nothing may be
        // built on the reserved slot -- that is the one a rival must never take.
        Spawner spawn{&es};
        const int built = racegrid::populate(
            es, /*rivalsWanted=*/-1, /*playerCraftId=*/-1,
            [&](const std::string& n, const glm::vec3& p, float y) {
                return spawn(n, p, y); });
        bool onMine = false;
        for (const std::string& n : spawn.names) onMine = onMine || n == "myglider";
        check(!onMine, "the player's prefab is never built as a rival");
        check(built == 2, "the other two markers build theirs",
              std::to_string(built) + " built");
    }

    // --- 15. A grid with no reserved marker ----------------------------------
    {
        std::vector<Entity> es;
        addLine(es);
        addMark(es, markPos[0], 0);
        addMark(es, markPos[1], 1);
        check(racegrid::playerMarker(es) < 0, "no marker reserved: none is claimed");
        check(racegrid::playerMarkerPrefab(es).empty(), "and there is no craft to name");
    }

    std::printf("\n%s\n", failures == 0 ? "gridcheck: all good"
                                        : "gridcheck: FAILURES above");
    return failures == 0 ? 0 : 1;
}
