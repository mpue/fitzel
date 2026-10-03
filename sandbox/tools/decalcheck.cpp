// decalcheck -- decals cut from the surfaces under them (Decals.hpp).
//
// The cutting is arithmetic, so it is measured: a decal on a flat floor covers
// exactly its footprint, with U and V running 0..1 across it; one over an edge
// takes only the part that faces it; the lift is the lift; on a box object, on
// a slope of terrain, round a corner it is cut where the box ends. The
// receivers are gathered from real entities through the same code the editor
// uses -- a modelled box, a dynamic body that must be left out, another decal.
// And the bullet hole is a hole: opaque and dark in the middle, clear at the
// rim. No GL.
//
//   build/release/bin/decalcheck.exe

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "../src/Component.hpp"
#include "../src/Decals.hpp"
#include "../src/EditMesh.hpp"
#include "../src/ModelLibrary.hpp"
#include "../src/SceneGraph.hpp"

namespace {

int failures = 0;
void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(), detail.empty() ? "" : "  -- ",
                detail.c_str());
    if (!ok) ++failures;
}

float area(const fitzel::MeshData& m) {
    float a = 0.0f;
    for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const glm::vec3& p = m.vertices[m.indices[i]].position;
        const glm::vec3& q = m.vertices[m.indices[i + 1]].position;
        const glm::vec3& r = m.vertices[m.indices[i + 2]].position;
        a += 0.5f * glm::length(glm::cross(q - p, r - p));
    }
    return a;
}

// A floor of two triangles, `size` square at height `y`, facing up.
std::vector<decals::Tri> floor(float size, float y) {
    const float h = size * 0.5f;
    const glm::vec3 a(-h, y, -h), b(-h, y, h), c(h, y, h), d(h, y, -h);
    return {{a, b, c}, {a, c, d}};
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // --- The cut --------------------------------------------------------------------------
    std::printf("Cutting\n");
    {
        // A 2 x 2 m decal, 1 m deep, standing on a 10 m floor.
        const glm::mat4 box = scenegraph::compose(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(2.0f, 1.0f, 2.0f));
        const fitzel::MeshData m = decals::project(box, floor(10.0f, 0.0f), 0.01f, 75.0f);
        check(std::abs(area(m) - 4.0f) < 1e-3f, "on a floor it covers exactly its footprint", std::to_string(area(m)));
        float uLo = 1e9f, uHi = -1e9f, vLo = 1e9f, vHi = -1e9f;
        bool lifted = true, up = true;
        for (const fitzel::Vertex& v : m.vertices) {
            uLo = std::min(uLo, v.uv.x); uHi = std::max(uHi, v.uv.x);
            vLo = std::min(vLo, v.uv.y); vHi = std::max(vHi, v.uv.y);
            lifted = lifted && std::abs(v.position.y - 0.01f) < 1e-5f;
            up = up && v.normal.y > 0.999f;
        }
        check(std::abs(uLo) < 1e-4f && std::abs(uHi - 1.0f) < 1e-4f && std::abs(vLo) < 1e-4f && std::abs(vHi - 1.0f) < 1e-4f,
              "U and V run 0..1 across it");
        check(lifted && up, "lifted a centimetre off the floor, facing up with it");
        // V along -Z: the corner at -Z (the image's top) has V = 1.
        bool vUp = false;
        for (const fitzel::Vertex& v : m.vertices)
            if (v.position.z < -0.999f && std::abs(v.uv.y - 1.0f) < 1e-4f) vUp = true;
        check(vUp, "the image stands upright seen from above, -Z at the top");

        // Facing away, and too steep: a ceiling and a wall receive nothing.
        std::vector<decals::Tri> ceiling = floor(10.0f, 0.2f);
        for (auto& t : ceiling) std::swap(t.b, t.c);
        const decals::Tri wall{{1.0f, -0.5f, -0.5f}, {1.0f, 0.5f, -0.5f}, {1.0f, 0.5f, 0.5f}};
        check(decals::project(box, ceiling, 0.01f, 75.0f).indices.empty() &&
                  decals::project(box, {wall}, 0.01f, 75.0f).indices.empty(),
              "a surface facing away, or a wall square to it, takes nothing");
        // A ramp at 30 degrees (rising towards -Z) is taken -- in a box tall
        // enough to hold it -- and its cut is the footprint over cos 30.
        const float tan30 = std::tan(glm::radians(30.0f)), c30 = std::cos(glm::radians(30.0f));
        const glm::vec3 a(-5.0f, 5.0f * tan30, -5.0f), b(-5.0f, -5.0f * tan30, 5.0f),
                        c(5.0f, -5.0f * tan30, 5.0f), d(5.0f, 5.0f * tan30, -5.0f);
        const std::vector<decals::Tri> ramp = {{a, b, c}, {a, c, d}};
        const glm::mat4 tall = scenegraph::compose(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(2.0f, 3.0f, 2.0f));
        const fitzel::MeshData r = decals::project(tall, ramp, 0.0f, 75.0f);
        check(std::abs(area(r) - 4.0f / c30) < 2e-3f, "a 30-degree slope is covered, the image stretched over it",
              std::to_string(area(r)));
        check(decals::project(tall, ramp, 0.0f, 20.0f).indices.empty(), "...and refused when the limit is 20 degrees");
    }
    {
        // Over the edge of a 1 m box: only the top inside the decal is covered.
        const glm::mat4 box = scenegraph::compose(glm::vec3(0.5f, 1.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f, 0.6f, 1.0f));
        std::vector<decals::Tri> top = floor(1.0f, 1.0f);   // the top of a box from -0.5..0.5
        const fitzel::MeshData m = decals::project(box, top, 0.0f, 75.0f);
        check(std::abs(area(m) - 0.5f) < 1e-3f, "over an edge: cut where the surface ends", std::to_string(area(m)));
    }

    // --- Receivers ----------------------------------------------------------------------------
    std::printf("Receivers\n");
    {
        ModelLibrary models;
        std::vector<Entity> ents;
        auto boxEntity = [&](int id, glm::vec3 c, glm::vec3 half) {
            Entity e;
            e.id = id;
            e.type = EntityType::Box;
            e.center = e.localCenter = c;
            e.half = half;
            auto mc = std::make_unique<MeshComponent>();
            mc->mesh = EditMesh::box(half);
            e.components.items.push_back(std::move(mc));
            return e;
        };
        ents.push_back(boxEntity(1, glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.5f)));       // a crate
        Entity loose = boxEntity(2, glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.5f));       // the same place, dynamic
        loose.components.items.push_back(std::make_unique<PhysicsComponent>());
        ents.push_back(std::move(loose));
        Entity other = boxEntity(3, glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.5f));       // another decal
        other.components.items.push_back(std::make_unique<DecalComponent>());
        ents.push_back(std::move(other));
        Entity far = boxEntity(4, glm::vec3(50.0f, 0.5f, 0.0f), glm::vec3(0.5f));        // nowhere near
        ents.push_back(std::move(far));
        // A plain cylinder primitive, never made editable, standing beside the crate.
        Entity post;
        post.id = 5;
        post.type = EntityType::Cylinder;
        post.center = post.localCenter = glm::vec3(3.0f, 1.0f, 0.0f);
        post.half = glm::vec3(0.3f, 1.0f, 0.3f);
        ents.push_back(std::move(post));

        const glm::mat4 box = scenegraph::compose(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f), glm::vec3(0.6f, 0.4f, 0.6f));
        decals::Receivers who;
        who.terrain = false;
        std::vector<decals::Tri> tris;
        decals::gather(ents, models, {}, box, who, tris);
        const fitzel::MeshData m = decals::project(box, tris, 0.0f, 75.0f);
        check(std::abs(area(m) - 0.36f) < 1e-3f,
              "on a crate's lid, through the gather -- the dynamic body, the other decal and the far one left out",
              std::to_string(area(m)) + " m2 from " + std::to_string(tris.size()) + " triangles");

        {
            // ...and a decal on the post's round top: a primitive receives too.
            const glm::mat4 top = scenegraph::compose(glm::vec3(3.0f, 2.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f, 0.4f, 1.0f));
            std::vector<decals::Tri> pt;
            decals::gather(ents, models, {}, top, who, pt);
            const float got = area(decals::project(top, pt, 0.0f, 75.0f));
            const float disc = 20.0f * 0.5f * 0.3f * 0.3f * std::sin(2.0f * 3.14159265f / 20.0f);   // its 20-gon
            check(std::abs(got - disc) < 2e-3f, "on a plain primitive (a cylinder never made editable), its top",
                  std::to_string(got) + " (want " + std::to_string(disc) + ")");
        }
        const std::uint64_t s0 = decals::signature(ents, models, box, who);
        ents[0].center.x += 0.1f;
        const std::uint64_t s1 = decals::signature(ents, models, box, who);
        ents[0].center.x -= 0.1f;
        ents[3].center.x += 5.0f;
        const std::uint64_t s2 = decals::signature(ents, models, box, who);
        check(s0 != s1 && s0 == s2, "the signature changes when a receiver moves, not when something far away does");

        // The terrain: a 20-degree slope under a 3 m decal.
        const float k = std::tan(glm::radians(20.0f));
        const decals::HeightFn slope = [k](float, float z) { return k * z; };
        decals::Receivers ground;
        ground.objects = false;
        const glm::mat4 gbox = scenegraph::compose(glm::vec3(10.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(3.0f, 3.0f, 3.0f));
        std::vector<decals::Tri> gt;
        decals::gather(ents, models, slope, gbox, ground, gt);
        const fitzel::MeshData g = decals::project(gbox, gt, 0.0f, 75.0f);
        const float want = 9.0f / std::cos(glm::radians(20.0f));
        check(std::abs(area(g) - want) < 0.02f, "on the terrain, following its slope",
              std::to_string(area(g)) + " (want " + std::to_string(want) + ")");
    }

    // --- A thrown decal's box -------------------------------------------------------------------
    std::printf("Thrown\n");
    {
        const glm::vec3 n = glm::normalize(glm::vec3(1.0f, 0.0f, 0.0f));
        const glm::mat4 b = decals::boxAt(glm::vec3(2.0f, 1.0f, 0.0f), n, 0.2f, 37.0f);
        const glm::vec3 y = glm::normalize(glm::vec3(b[1]));
        check(glm::dot(y, n) > 0.9999f && std::abs(glm::length(glm::vec3(b[0])) - 0.2f) < 1e-5f &&
                  std::abs(glm::length(glm::vec3(b[1])) - 0.1f) < 1e-5f && std::abs(glm::dot(glm::vec3(b[0]), n)) < 1e-5f,
              "a hole's box sits on the hit, its depth along the normal, its face across the surface");
        // On a wall facing +X: a hole of 0.2 covers 0.04 m2.
        const decals::Tri wa{{2.0f, 0.0f, 1.0f}, {2.0f, 2.0f, -1.0f}, {2.0f, 2.0f, 1.0f}};
        const decals::Tri wb{{2.0f, 0.0f, 1.0f}, {2.0f, 0.0f, -1.0f}, {2.0f, 2.0f, -1.0f}};
        const fitzel::MeshData m = decals::project(b, {wa, wb}, 0.004f, 60.0f);
        check(std::abs(area(m) - 0.04f) < 1e-4f, "...and on the wall it hit, a hole its own size", std::to_string(area(m)));
    }
    {
        const std::vector<unsigned char> px = decals::bulletHolePixels(64);
        auto a = [&](int x, int y) { return px[(static_cast<std::size_t>(y) * 64 + x) * 4 + 3]; };
        auto lum = [&](int x, int y) { return px[(static_cast<std::size_t>(y) * 64 + x) * 4]; };
        check(px.size() == 64u * 64u * 4u && a(32, 32) == 255 && lum(32, 32) < 40 && a(0, 0) == 0 && a(63, 32) == 0,
              "the engine's bullet hole: opaque and dark in the middle, clear at the rim");
    }

    std::printf("\ndecalcheck: %s (%d failed)\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}
