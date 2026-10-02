// The collider check: does a modelled mesh collide as the shape that is drawn?
//
// A converted primitive keeps its type, so without a collider of its own a box
// pulled into an L would still collide as its bounding box, and a ramp bent out
// of shape as the ideal wedge. addEntityBody (PhysicsShapes.cpp) gives a static
// mesh its own triangles and a dynamic one the hull of its corners; this drops
// balls on them and reads where they come to rest. The L is the telling case: a
// ball over the notch lands IN it only if the collider is the real mesh -- a
// convex hull fills the notch and holds the ball half a metre up.
//
// Imported models get the same treatment: a static one collides as its own
// triangles -- one mesh shared by every copy, placed, turned and stretched the
// way it is drawn -- a dynamic one as its hull. A trough stands in for the L,
// and a doorway for the hall a figure has to get into. Physics on a model
// group (a structured import's root) makes its parts solid, minus the ones
// that are only leaves and the ones with Physics of their own.
//
// The second half walks figures (PhysicsWorld::addFigure) over a small course:
// flat terrain, a wall, a bridge deck, a ramp built as a mesh and a kerb. What
// a game's hero needs from the world -- stand on the deck rather than the
// ground under it, stop at the wall, step up the kerb, fall off the end.
//
// Console program, no GL, no window, no assets.
//   build/release/bin/collidecheck.exe
// Exits non-zero if any check fails.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/physics/Physics.hpp>

#include "../src/Component.hpp"
#include "../src/MeshQuery.hpp"
#include "../src/PhysicsShapes.hpp"

namespace {

int failures = 0;
int checks   = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

// An L in XY, 1 m deep: a 2 x 1 slab with a 1 x 1 tower on its left half.
// Built from convex faces, as every EditMesh face is.
EditMesh lShape() {
    EditMesh m;
    const glm::vec2 P[7] = {{-1, -1}, {1, -1}, {1, 0}, {0, 0}, {0, 1}, {-1, 1}, {-1, 0}};
    for (float z : {-0.5f, 0.5f})
        for (const glm::vec2& p : P) m.verts.push_back({p.x, p.y, z});
    const int B = 7;   // back ring offset
    m.faces.push_back({0, 6, 3, 2, 1});            // front, slab (seen from -Z)
    m.faces.push_back({6, 5, 4, 3});               // front, tower
    m.faces.push_back({B + 0, B + 1, B + 2, B + 3, B + 6});
    m.faces.push_back({B + 6, B + 3, B + 4, B + 5});
    for (int i = 0; i < 7; ++i) {
        const int j = (i + 1) % 7;
        m.faces.push_back({i, j, B + j, B + i});
    }
    m.syncPaint();
    return m;
}

Entity meshEntity(EditMesh mesh, glm::vec3 center, glm::vec3 half, EntityType t) {
    Entity e;
    e.type   = t;
    e.center = e.localCenter = center;
    e.half   = half;
    e.id     = 1;
    auto mc  = std::make_unique<MeshComponent>();
    mc->mesh = std::move(mesh);
    e.components.items.push_back(std::move(mc));
    return e;
}

// A ball of radius 0.2 dropped from `from` onto `e` (static), stepped for 3 s.
// Returns the ball's resting centre height, NaN if nothing could be built.
float dropBall(const Entity& e, glm::vec3 from) {
    fitzel::PhysicsWorld w;
    if (!addEntityBody(w, e, 0.0f, nullptr)) return NAN;
    const fitzel::PhysicsBodyId ball = w.addSphere(0.2f, from, 1.0f);
    for (int i = 0; i < 180; ++i) w.step(1.0f / 60.0f);
    glm::vec3 p; glm::quat q;
    w.getTransform(ball, p, q);
    return p.y;
}

// The same, into a world the caller has filled.
float dropBallIn(fitzel::PhysicsWorld& w, glm::vec3 from) {
    const fitzel::PhysicsBodyId ball = w.addSphere(0.2f, from, 1.0f);
    for (int i = 0; i < 180; ++i) w.step(1.0f / 60.0f);
    glm::vec3 p; glm::quat q;
    w.getTransform(ball, p, q);
    w.removeBody(ball);
    return p.y;
}

// An imported model as ModelLibrary leaves it, minus the GPU meshes: boxes as
// a triangle list with every corner repeated per face (what a flat-shaded
// export looks like), positions in model space.
LoadedModel boxModel(int id, const std::vector<std::pair<glm::vec3, glm::vec3>>& boxes) {
    LoadedModel lm;
    lm.id = id;
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const auto& [a, b] : boxes) {
        const glm::vec3 c[8] = {{a.x, a.y, a.z}, {b.x, a.y, a.z}, {b.x, b.y, a.z}, {a.x, b.y, a.z},
                                {a.x, a.y, b.z}, {b.x, a.y, b.z}, {b.x, b.y, b.z}, {a.x, b.y, b.z}};
        const int f[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                             {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}};
        for (const auto& q : f)
            for (int t : {0, 1, 2, 0, 2, 3}) lm.meshTris.push_back(c[q[t]]);
        lo = glm::min(lo, a);
        hi = glm::max(hi, b);
    }
    lm.hullPoints = lm.meshTris;
    lm.boundsMin = lo;
    lm.boundsMax = hi;
    return lm;
}

// A trough, 2 x 1 x 1: a floor 0.2 thick and a wall 0.4 thick at each end. Its convex
// hull is a solid block -- a ball dropped in the middle lands on the floor
// (centre at 0.4) only if the collider is the triangles, at 1.2 on the hull.
LoadedModel trough(int id) {
    return boxModel(id, {{{-1, 0, -0.5f}, {1, 0.2f, 0.5f}},
                         {{-1, 0.2f, -0.5f}, {-0.6f, 1, 0.5f}},
                         {{0.6f, 0.2f, -0.5f}, {1, 1, 0.5f}}});
}

Entity modelEntity(const LoadedModel& lm, int id, glm::vec3 center, glm::vec3 half) {
    Entity e;
    e.type   = EntityType::Model;
    e.id     = id;
    e.center = e.localCenter = center;
    e.half   = half;
    auto mc     = std::make_unique<ModelComponent>();
    mc->modelId = lm.id;
    e.components.items.push_back(std::move(mc));
    return e;
}

// A world for figures to walk in: flat ground (a heightfield, as the terrain
// is), a wall, a bridge deck 3 m up, a ramp built as a triangle mesh (as a
// road is) and a kerb.
struct Course {
    fitzel::PhysicsWorld w;
    fitzel::PhysicsBodyId deck = 0;
    Course() {
        std::vector<float> flat(32 * 32, 0.0f);
        w.addHeightField(flat.data(), 32, {-32.0f, 0.0f, -32.0f}, 2.0f);
        w.addBox({3.0f, 1.5f, 0.2f}, {-10.0f, 1.5f, 0.0f}, glm::quat(1, 0, 0, 0), 0.0f);
        deck = w.addBox({1.5f, 0.25f, 8.0f}, {10.0f, 2.75f, 0.0f}, glm::quat(1, 0, 0, 0), 0.0f);
        const glm::vec3 rv[4] = {{18, 0, 0}, {22, 0, 0}, {22, 1.5f, 10}, {18, 1.5f, 10}};
        const std::uint32_t ri[6] = {0, 2, 1, 0, 3, 2};
        w.addMesh(rv, 4, ri, 6);
        w.addBox({2.0f, 0.075f, 2.0f}, {-20.0f, 0.075f, 10.0f}, glm::quat(1, 0, 0, 0), 0.0f);
    }
};

// Walk a figure at `vel` for `seconds` at 60 Hz; `each` sees every step.
template <class F>
fitzel::PhysicsWorld::FigureStep walk(fitzel::PhysicsWorld& w, int fig, glm::vec3 vel,
                                      float seconds, F each) {
    fitzel::PhysicsWorld::FigureStep s;
    for (int i = 0; i < static_cast<int>(seconds * 60.0f); ++i) {
        w.moveFigure(fig, vel, 1.0f / 60.0f, s);
        each(s);
    }
    return s;
}
fitzel::PhysicsWorld::FigureStep walk(fitzel::PhysicsWorld& w, int fig, glm::vec3 vel,
                                      float seconds) {
    return walk(w, fig, vel, seconds, [](const fitzel::PhysicsWorld::FigureStep&) {});
}

} // namespace

int main() {
    std::printf("static L (bottom at y = 0, notch floor at 1, tower top at 2)\n");
    const Entity L = meshEntity(lShape(), {0, 1, 0}, {1, 1, 0.5f}, EntityType::Box);
    const float notch = dropBall(L, {0.5f, 5.0f, 0.0f});
    const float tower = dropBall(L, {-0.5f, 5.0f, 0.0f});
    std::printf("       ball over notch rests at %.3f, over tower at %.3f\n", notch, tower);
    check(std::fabs(notch - 1.2f) < 0.05f, "a ball over the notch lands in it (not on a hull)");
    check(std::fabs(tower - 2.2f) < 0.05f, "a ball over the tower lands on its top");

    std::printf("static mesh follows the entity's transform and scale\n");
    {
        // The same L drawn twice as big (half doubled) and moved: the mesh is
        // stretched to the half-extents exactly as the renderer stretches it.
        const Entity big = meshEntity(lShape(), {10, 2, 0}, {2, 2, 1}, EntityType::Box);
        const float y = dropBall(big, {11.0f, 9.0f, 0.0f});
        std::printf("       ball over the doubled notch rests at %.3f\n", y);
        check(std::fabs(y - 2.2f) < 0.05f, "scaled and moved, the notch floor is where it is drawn");
    }

    std::printf("a converted primitive keeps its shape as a collider\n");
    {
        // No ray query in PhysicsWorld, and a ball does not rest on a slope --
        // so let it roll: a ramp rises along +Z, and a ball dropped on its middle
        // runs off the LOW end. Through a missing collider it would fall
        // straight down and keep z = 0.
        fitzel::PhysicsWorld w;
        const Entity ramp = meshEntity(EditMesh::ramp({1, 0.5f, 1}), {0, 0.5f, 0},
                                       {1, 0.5f, 1}, EntityType::Ramp);
        check(addEntityBody(w, ramp, 0.0f, nullptr) != 0, "a made-editable ramp gets a body");
        const fitzel::PhysicsBodyId ball = w.addSphere(0.2f, {0.0f, 2.0f, 0.0f}, 1.0f);
        for (int i = 0; i < 120; ++i) w.step(1.0f / 60.0f);
        glm::vec3 p; glm::quat q;
        w.getTransform(ball, p, q);
        std::printf("       after 2 s the ball is at z = %.3f\n", p.z);
        check(p.z < -0.8f, "and a ball rolls down it towards the low end");
    }

    std::printf("flat plane\n");
    {
        const Entity pl = meshEntity(EditMesh::plane({2, 0.01f, 2}), {0, 0, 0},
                                     {2, 0.01f, 2}, EntityType::Plane);
        const float y = dropBall(pl, {0.5f, 3.0f, 0.5f});
        std::printf("       ball on the static plane rests at %.3f\n", y);
        check(std::fabs(y - 0.2f) < 0.05f, "a static editable plane is a floor");
        fitzel::PhysicsWorld w;
        check(addEntityBody(w, pl, 1.0f, nullptr) != 0,
              "a dynamic one (no hull possible) falls back to its box");
    }

    std::printf("dynamic mesh body\n");
    {
        fitzel::PhysicsWorld w;
        w.addBox({20, 0.5f, 20}, {0, -0.5f, 0}, glm::quat(1, 0, 0, 0), 0.0f);
        const Entity L2 = meshEntity(lShape(), {0, 3, 0}, {1, 1, 0.5f}, EntityType::Box);
        const fitzel::PhysicsBodyId id = addEntityBody(w, L2, 1.0f, nullptr);
        check(id != 0, "a dynamic L gets a hull");
        for (int i = 0; i < 240; ++i) w.step(1.0f / 60.0f);
        glm::vec3 p; glm::quat q;
        w.getTransform(id, p, q);
        std::printf("       it came to rest at y = %.3f\n", p.y);
        check(p.y > 0.4f && p.y < 1.2f, "it falls and rests on the floor, not in it");
    }

    std::printf("imported models: static ones collide as their triangles\n");
    {
        const LoadedModel U = trough(1);
        const Entity e = modelEntity(U, 1, {0, 0.5f, 0}, {1, 0.5f, 0.5f});
        fitzel::PhysicsWorld w;
        check(addEntityBody(w, e, 0.0f, &U) != 0, "a static model gets a body");
        const float in = dropBallIn(w, {0.0f, 3.0f, 0.0f});
        const float wall = dropBallIn(w, {0.8f, 3.0f, 0.0f});
        std::printf("       ball in the trough rests at %.3f, on its wall at %.3f\n", in, wall);
        check(std::fabs(in - 0.4f) < 0.05f, "a ball dropped into the trough lands on its floor (not on a hull)");
        check(std::fabs(wall - 1.2f) < 0.05f, "one over a wall lands on the wall");

        // The welding. A crate shoved across the floor slides over the diagonal
        // that splits every quad into two triangles. With the exporter's
        // corners left unshared, Jolt takes that diagonal for a real edge: the
        // crate is kicked sideways (measured: 0.38 m off its line at 8 m/s) and
        // tips. Welded, the diagonal is known to lie inside a flat face.
        fitzel::PhysicsWorld r;
        const LoadedModel slab = boxModel(2, {{{-6, 0, -1}, {6, 0.2f, 1}}});
        const Entity s = modelEntity(slab, 2, {0, 0.1f, 0}, {6, 0.1f, 1});
        addEntityBody(r, s, 0.0f, &slab);
        const fitzel::PhysicsBodyId crate =
            r.addBox({0.3f, 0.2f, 0.3f}, {-5.0f, 0.401f, -0.5f}, glm::quat(1, 0, 0, 0), 1.0f);
        r.setLinearVelocity(crate, {8.0f, 0.0f, 0.0f});
        float drift = 0.0f, tip = 0.0f;
        glm::vec3 p; glm::quat q;
        for (int i = 0; i < 90; ++i) {
            r.step(1.0f / 60.0f);
            r.getTransform(crate, p, q);
            drift = std::max(drift, std::fabs(p.z + 0.5f));
            tip   = std::max(tip, std::max(p.y - 0.4f, std::fabs(q.x) + std::fabs(q.z)));
        }
        std::printf("       crate slid to x %.2f: off its line %.3f m, tipped/lifted %.4f\n",
                    p.x, drift, tip);
        check(p.x > 2.0f && drift < 0.02f && tip < 0.005f,
              "a crate slides straight over a model's floor (no edges inside its faces)");
    }

    std::printf("a static model is placed the way it is drawn\n");
    {
        // Turned a quarter round Y and stretched to twice its length: local +X
        // (along which the walls stand) now runs along -Z, the walls at |z| 1.2..2.
        const LoadedModel U = trough(3);
        Entity e = modelEntity(U, 3, {5, 0.5f, 0}, {2, 0.5f, 0.5f});
        e.rotation = {0.0f, 90.0f, 0.0f};
        fitzel::PhysicsWorld w;
        addEntityBody(w, e, 0.0f, &U);
        const float mid  = dropBallIn(w, {5.0f, 3.0f, 0.0f});
        const float near = dropBallIn(w, {5.0f, 3.0f, 0.8f});
        const float wall = dropBallIn(w, {5.0f, 3.0f, -1.6f});
        const float out  = dropBallIn(w, {5.8f, 3.0f, 0.0f});
        std::printf("       floor %.3f, floor near the end %.3f, wall %.3f, beside %.3f\n",
                    mid, near, wall, out);
        check(std::fabs(mid - 0.4f) < 0.05f && std::fabs(near - 0.4f) < 0.05f &&
                  std::fabs(wall - 1.2f) < 0.05f,
              "turned and stretched, its floor and walls are where they are drawn");
        check(out < -1.0f, "and beside it, where nothing is drawn, a ball falls past");

        // A second copy shares the first one's mesh and still stands where IT is.
        Entity f = modelEntity(U, 4, {-5, 0.5f, 0}, {1, 0.5f, 0.5f});
        check(addEntityBody(w, f, 0.0f, &U) != 0 &&
                  std::fabs(dropBallIn(w, {-5.0f, 3.0f, 0.0f}) - 0.4f) < 0.05f,
              "a second copy of the model collides at its own place");
    }

    std::printf("a dynamic model is still its hull\n");
    {
        fitzel::PhysicsWorld w;
        w.addBox({20, 0.5f, 20}, {0, -0.5f, 0}, glm::quat(1, 0, 0, 0), 0.0f);
        const LoadedModel U = trough(5);
        const Entity e = modelEntity(U, 5, {0, 0.5f, 0}, {1, 0.5f, 0.5f});
        check(addEntityBody(w, e, 50.0f, &U) != 0, "a dynamic model gets a body");
        const float y = dropBallIn(w, {0.0f, 3.0f, 0.0f});
        std::printf("       ball over the dynamic trough rests at %.3f\n", y);
        check(y > 1.0f, "a ball over it lands on the hull's top -- Jolt cannot move triangles");
    }

    std::printf("Physics on a model group (an imported building)\n");
    {
        // A root with no model of its own, as a structured import makes it, and
        // three parts: a trough, a part that is only leaves (no triangles that
        // collide, a hull that would), and a trough with Physics of its own.
        const LoadedModel U = trough(6);
        LoadedModel leaves = boxModel(7, {{{-1, 0, -1}, {1, 2, 1}}});
        leaves.meshTris.clear();
        std::vector<Entity> ents;
        Entity root;
        root.type = EntityType::Model;
        root.id = 10;
        root.center = root.localCenter = {0, 1, 20};
        root.half = {6, 1, 1};
        root.components.items.push_back(std::make_unique<PhysicsComponent>());
        static_cast<PhysicsComponent*>(root.components.items.back().get())->dynamic = false;
        ents.push_back(std::move(root));
        Entity a = modelEntity(U, 11, {0, 0.5f, 20}, {1, 0.5f, 0.5f});
        a.parent = 10;
        Entity b = modelEntity(leaves, 12, {4, 1, 20}, {1, 1, 1});
        b.parent = 10;
        Entity c = modelEntity(U, 13, {-4, 0.5f, 20}, {1, 0.5f, 0.5f});
        c.parent = 10;
        c.components.items.push_back(std::make_unique<PhysicsComponent>());
        ents.push_back(std::move(a));
        ents.push_back(std::move(b));
        ents.push_back(std::move(c));
        check(isModelGroup(ents[0]) && !isModelGroup(ents[1]), "the root is recognised as a model group");
        fitzel::PhysicsWorld w;
        const int made = addGroupBodies(w, ents, ents[0], [&](const Entity& part) -> const LoadedModel* {
            return part.id == 12 ? &leaves : &U;
        });
        const float part   = dropBallIn(w, {0.0f, 3.0f, 20.0f});
        const float leafy  = dropBallIn(w, {4.0f, 3.0f, 20.0f});
        const float own    = dropBallIn(w, {-4.0f, 3.0f, 20.0f});
        std::printf("       %d bod(ies); ball in the part %.3f, over the leaves %.3f, over the own-physics part %.3f\n",
                    made, part, leafy, own);
        check(made == 1 && std::fabs(part - 0.4f) < 0.05f,
              "its part collides as its own triangles");
        check(leafy < -1.0f, "a part that is only leaves is left out (no hull around it)");
        check(own < -1.0f, "a part with its own Physics is left to it");
    }

    std::printf("a figure walks through a doorway in a static model\n");
    {
        // Two pillars and a lintel, 1.2 m wide and 2.2 m high: the hull of it
        // is a solid wall 4 m wide.
        const LoadedModel gate = boxModel(8, {{{-2, 0, -0.2f}, {-0.6f, 3, 0.2f}},
                                              {{0.6f, 0, -0.2f}, {2, 3, 0.2f}},
                                              {{-0.6f, 2.2f, -0.2f}, {0.6f, 3, 0.2f}}});
        fitzel::PhysicsWorld w;
        w.addBox({20, 0.5f, 20}, {0, -0.5f, 0}, glm::quat(1, 0, 0, 0), 0.0f);
        addEntityBody(w, modelEntity(gate, 20, {0, 1.5f, 0}, {2, 1.5f, 0.2f}), 0.0f, &gate);
        const glm::vec3 north(0.0f, 0.0f, 1.5f);
        const int through = w.addFigure(0.3f, 0.6f, {0.0f, 0.0f, -3.0f});
        const auto st = walk(w, through, north, 4.0f);
        const int pillar = w.addFigure(0.3f, 0.6f, {1.3f, 0.0f, -3.0f});
        const auto sp = walk(w, pillar, north, 4.0f);
        std::printf("       through the door: z %.2f; into the pillar: z %.2f\n", st.foot.z, sp.foot.z);
        check(st.foot.z > 2.0f, "through the doorway it walks on");
        check(sp.foot.z < -0.4f && sp.foot.z > -1.0f, "into a pillar it stops in front of it");
    }

    // The ground and wall tests outside the physics world (editor walk, glider
    // hover) ask meshquery instead of the half-extents.
    std::printf("mesh queries (editor walk, glider ground)\n");
    {
        float y = 0.0f;
        check(meshquery::surfaceBelow(L, lShape(), 0.5f, 0.0f, 10.0f, y) &&
                  std::fabs(y - 1.0f) < 1e-3f,
              "the ground over the notch is the notch floor");
        check(meshquery::surfaceBelow(L, lShape(), -0.5f, 0.0f, 10.0f, y) &&
                  std::fabs(y - 2.0f) < 1e-3f,
              "the ground over the tower is its top");
        check(!meshquery::surfaceBelow(L, lShape(), -0.5f, 0.0f, 1.5f, y),
              "the underside of the tower is no floor, even below a low ceiling");
        check(!meshquery::surfaceBelow(L, lShape(), 1.5f, 0.0f, 10.0f, y),
              "beside the mesh there is nothing");
        check(meshquery::blocks(L, lShape(), -0.5f, 0.0f, 0.5f, 1.8f),
              "a body standing inside the tower is blocked");
        check(!meshquery::blocks(L, lShape(), 0.5f, 0.0f, 1.1f, 1.9f),
              "a body in the notch, above its floor, is free");
        check(meshquery::blocks(L, lShape(), 0.5f, 0.0f, 0.5f, 0.9f),
              "a body inside the slab is blocked");

        // Turned a quarter round Y: local +X (the notch) now points along -Z.
        // The box tests ignored rotation; this one must not.
        Entity turned = meshEntity(lShape(), {0, 1, 0}, {1, 1, 0.5f}, EntityType::Box);
        turned.rotation = {0.0f, 90.0f, 0.0f};
        check(meshquery::surfaceBelow(turned, lShape(), 0.0f, -0.5f, 10.0f, y) &&
                  std::fabs(y - 1.0f) < 1e-3f &&
                  meshquery::surfaceBelow(turned, lShape(), 0.0f, 0.5f, 10.0f, y) &&
                  std::fabs(y - 2.0f) < 1e-3f,
              "a turned mesh is asked where it is drawn");

        const Entity ramp = meshEntity(EditMesh::ramp({1, 0.5f, 1}), {0, 0.5f, 0},
                                       {1, 0.5f, 1}, EntityType::Ramp);
        const EditMesh rm = EditMesh::ramp({1, 0.5f, 1});
        check(meshquery::surfaceBelow(ramp, rm, 0.0f, 0.0f, 10.0f, y) &&
                  std::fabs(y - 0.5f) < 1e-3f,
              "half way up a made-editable ramp, the ground is half its height");
    }

    std::printf("figures: capsules a game walks over the world\n");
    {
        Course c;
        fitzel::PhysicsWorld& w = c.w;
        const glm::vec3 north(0.0f, 0.0f, 1.5f);   // 1.5 m/s along +Z

        const int a = w.addFigure(0.3f, 0.6f, {-20.0f, 0.0f, -20.0f});
        const auto s = walk(w, a, north, 2.0f);
        std::printf("       on flat ground: foot (%.2f, %.3f, %.2f)\n", s.foot.x, s.foot.y, s.foot.z);
        check(s.onGround && std::fabs(s.foot.y) < 0.05f && std::fabs(s.foot.z + 17.0f) < 0.2f,
              "on flat ground a figure walks 3 m in 2 s and stays on it");
        check(s.onHeightField, "...and knows that ground is the terrain");

        const int b = w.addFigure(0.3f, 0.6f, {-10.0f, 0.0f, -3.0f});
        const auto sw = walk(w, b, north, 3.0f);
        std::printf("       at the wall: z %.2f (wall face at -0.2)\n", sw.foot.z);
        check(sw.foot.z < -0.4f && sw.foot.z > -1.0f, "a wall stops it, right in front of the wall");

        const int d = w.addFigure(0.3f, 0.6f, {10.0f, 3.05f, -6.0f});
        float lowest = 1e9f, highest = -1e9f;
        bool alwaysOff = true;
        const auto sd = walk(w, d, north, 3.0f, [&](const fitzel::PhysicsWorld::FigureStep& st) {
            lowest = std::min(lowest, st.foot.y);
            highest = std::max(highest, st.foot.y);
            if (st.onHeightField) alwaysOff = false;
        });
        std::printf("       on the deck: foot y %.3f .. %.3f\n", lowest, highest);
        check(sd.onGround && lowest > 2.95f && highest < 3.05f,
              "on a bridge deck it walks along the top, not the ground under it");
        check(alwaysOff, "...and does not take the deck for terrain");
        const auto off = walk(w, d, north, 8.0f);   // to z = 10.5, the deck ends at 8
        std::printf("       past the deck's end: foot (%.2f, %.3f, %.2f)\n", off.foot.x, off.foot.y, off.foot.z);
        check(off.foot.z > 8.5f && std::fabs(off.foot.y) < 0.05f && off.onHeightField,
              "walking off the end, it falls to the ground below");

        const int r = w.addFigure(0.3f, 0.6f, {20.0f, 0.0f, -1.0f});
        float worst = 0.0f;
        int onRamp = 0;
        walk(w, r, north, 6.0f, [&](const fitzel::PhysicsWorld::FigureStep& st) {
            if (st.foot.z > 2.0f && st.foot.z < 8.0f) {
                worst = std::max(worst, std::fabs(st.foot.y - 0.15f * st.foot.z));
                if (st.onGround && !st.onHeightField) ++onRamp;
            }
        });
        std::printf("       up the ramp: worst height error %.3f m\n", worst);
        check(worst < 0.1f && onRamp > 100, "up a ramp built as a mesh (a road) it keeps to the surface");

        const int k = w.addFigure(0.3f, 0.6f, {-20.0f, 0.0f, 6.0f});
        const auto sk = walk(w, k, north, 3.0f);
        std::printf("       over the kerb: foot (%.2f, %.3f, %.2f)\n", sk.foot.x, sk.foot.y, sk.foot.z);
        check(sk.foot.z > 9.5f && std::fabs(sk.foot.y - 0.15f) < 0.03f,
              "it steps up a 15 cm kerb instead of stopping at it");

        glm::vec3 hit, n;
        fitzel::PhysicsBodyId body = 0;
        check(w.castRay({10.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f, hit, n, body) &&
                  std::fabs(hit.y - 3.0f) < 1e-3f && n.y > 0.99f && body == c.deck,
              "a ray straight down finds the deck's top, facing up");
        check(!w.castRay({0.0f, 10.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 20.0f, hit, n, body),
              "...and one pointing at the sky finds nothing");

        w.removeFigure(a);
        fitzel::PhysicsWorld::FigureStep gone;
        check(!w.hasFigure(a) && !w.moveFigure(a, north, 1.0f / 60.0f, gone) && w.hasFigure(b),
              "a removed figure is gone, the others stay");
    }

    std::printf("\n%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
