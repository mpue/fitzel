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
// Console program, no GL, no window, no assets.
//   build/release/bin/collidecheck.exe
// Exits non-zero if any check fails.

#include <cmath>
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

    std::printf("\n%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
