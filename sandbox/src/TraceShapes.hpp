#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include "TraceFunc.hpp"

// Exact geometry for the path tracer: spheres that are spheres, boxes, planes
// that go on for ever, cylinders, cones, tori, discs, isosurfaces -- and the
// constructive solid geometry that combines them.
//
// WHY, NEXT TO A TRIANGLE TRACER THAT ALREADY HANDLES ANYTHING. The harvested
// game scene is triangles because the game is, and that stays as it was. But a
// scene WRITTEN for the tracer (Strahl.hpp) is a different kind of thing: a
// chrome sphere tessellated finely enough that its reflection does not show
// facets is a hundred thousand triangles, an infinite floor is not a triangle
// at all, and a difference of two solids -- a box with a sphere cut out of it --
// has no triangle form short of a mesh boolean, which is a project of its own
// and still facets every curved cut. A ray against the solid itself has none of
// those problems: the intersection is exact, the normal is exact, and CSG is a
// question of which crossings along the ray lie inside which operands.
//
// EVERY SHAPE IS A CANONICAL PRIMITIVE PLUS A TRANSFORM. A unit sphere at the
// origin, the box [-1, 1]^3, a cylinder of radius 1 from y = 0 to 1, a cone
// with base radius 1 at y = 0 and `p0` at y = 1, a torus around the y axis, the
// plane y = 0 (solid below), a disc in that plane. Whatever the script said --
// a centre and a radius, two end points, a rotation -- is folded into
// `toLocal` when the scene is built, so the intersection code only ever meets
// the canonical forms. The ray is carried into local space with its direction
// NOT renormalised, so a distance along it is the world distance and hits from
// different shapes, and from the triangles, compare directly.
//
// CSG BY WALKING THE CROSSINGS. A difference's surface is the first operand's
// surface where it is outside every other operand, plus the others' surfaces
// where they are inside the first (facing the other way). So the nearest
// candidate crossing of any operand is taken, tested against the rule with the
// operands' inside() tests, and the walk resumes just past it when it fails.
// Constant memory, no crossing lists, and nesting composes by recursion.
//
// No GL. Plain arithmetic, testable headless (tools/strahlcheck.cpp).
namespace pathtrace {

struct ShapeNode {
    enum class Kind : std::uint8_t {
        Sphere, Box, Plane, Disc, Cylinder, Cone, Torus, Iso,
        Union, Merge, Intersection, Difference,
    };
    Kind kind = Kind::Sphere;

    // World -> the canonical primitive's space, and its normals back out.
    glm::mat4 toLocal{1.0f};
    glm::mat3 normalToWorld{1.0f};
    // World -> the space the material's functions are evaluated in: the space
    // of the object the material was GIVEN to, before that object's own
    // transforms. A marble block keeps its veins when it is moved, and a
    // difference cut from it shows the marble inside rather than a pattern
    // stretched over the canonical unit box.
    glm::mat4 toPattern{1.0f};

    // Per kind:
    //   Cone      p0 = top radius (base is 1)
    //   Torus     p0 = major radius, p1 = minor radius
    //   Disc      p0 = hole radius (0 = none)
    //   Iso       p0 = threshold, p1 = max gradient, p2 = accuracy
    float p0 = 0.0f, p1 = 0.0f, p2 = 0.0f;
    bool  open = false;   // cylinder / cone without caps; isosurface without its box
    // Isosurface: the field (local space) and the box it is confined to.
    std::shared_ptr<const Func> func;
    glm::vec3 boxLo{-1.0f}, boxHi{1.0f};

    int material = 0;     // index into Scene::materials (leaves only)
    // CSG operands: childIndex[first .. first + count).
    int first = 0, count = 0;

    // Filled by Shapes::finalize(): the world box, or unbounded (a plane, or
    // anything an unbounded operand makes infinite).
    glm::vec3 lo{0.0f}, hi{0.0f};
    bool bounded = true;

    bool isLeaf() const { return kind < Kind::Union; }
};

// Where a ray met a shape: the leaf whose surface it was, with the normal
// already in world space and facing OUT of the solid that surface bounds (so a
// face a difference cut is turned to face into the hole).
struct ShapeHit {
    float     t = 0.0f;
    glm::vec3 n{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};
    int       leaf = -1;
};

struct Shapes {
    std::vector<ShapeNode> nodes;
    std::vector<int>       childIndex;
    std::vector<int>       roots;    // the shapes in the scene; operands are not roots

    bool empty() const { return roots.empty(); }
    // Bounds for every node, operands first. Call once after building.
    void finalize();

    // The nearest crossing of `node` along o + t*d with tMin < t < tMax.
    bool closest(int node, const glm::vec3& o, const glm::vec3& d, float tMin, float tMax,
                 ShapeHit& hit) const;
    // Whether a world point is inside the solid.
    bool inside(int node, const glm::vec3& p) const;
};

} // namespace pathtrace
