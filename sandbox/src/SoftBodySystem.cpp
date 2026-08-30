#include "SoftBodySystem.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include "Component.hpp"
#include "EditMesh.hpp"

namespace {

// The Softness slider, in the units Jolt actually solves in. Compliance is the
// INVERSE of stiffness: 0 is as rigid as the solver gets, and about a thousandth
// is properly slack. Squared on the way in so the low end of the slider -- where
// the interesting range is, between "firm rubber" and "wet dough" -- gets most of
// the travel instead of being crushed into the first tenth of it.
float complianceOf(float softness) {
    const float s = glm::clamp(softness, 0.0f, 1.0f);
    return s * s * 2.0e-3f;
}

// The particles and triangles a soft body starts from, before Jolt turns them
// into constraints. `pinned` is empty when nothing is nailed down.
struct Surface {
    std::vector<glm::vec3>     verts;
    std::vector<std::uint32_t> tris;
    std::vector<std::uint8_t>  pinned;
};

void addTri(Surface& s, int a, int b, int c) {
    s.tris.push_back(static_cast<std::uint32_t>(a));
    s.tris.push_back(static_cast<std::uint32_t>(b));
    s.tris.push_back(static_cast<std::uint32_t>(c));
}

// Wind every triangle so its normal points away from the body's centre. Pressure
// pushes along the face normal, so a shell wound inwards does not inflate -- it
// implodes. Cheaper to fix here, once, than to get every generator's winding
// right by inspection.
void orientOutward(Surface& s) {
    for (std::size_t i = 0; i + 2 < s.tris.size(); i += 3) {
        const glm::vec3& a = s.verts[s.tris[i]];
        const glm::vec3& b = s.verts[s.tris[i + 1]];
        const glm::vec3& c = s.verts[s.tris[i + 2]];
        if (glm::dot(glm::cross(b - a, c - a), a + b + c) < 0.0f)
            std::swap(s.tris[i + 1], s.tris[i + 2]);
    }
}

// A hollow ellipsoid filling the entity's box: the balloon. Poles plus rings of
// latitude, which is the cheapest sphere there is and perfectly good for
// something whose whole point is that it does not stay a sphere.
Surface ballSurface(glm::vec3 half, int res) {
    const int stacks = std::clamp(res, 2, 10) + 1; // bands from pole to pole
    const int slices = stacks * 2;                 // wedges around the equator
    Surface s;
    s.verts.push_back(glm::vec3(0.0f, half.y, 0.0f)); // north pole: index 0
    for (int i = 1; i < stacks; ++i) {
        const float th = glm::pi<float>() * float(i) / float(stacks);
        for (int j = 0; j < slices; ++j) {
            const float ph = glm::two_pi<float>() * float(j) / float(slices);
            s.verts.push_back(glm::vec3(std::sin(th) * std::cos(ph) * half.x,
                                        std::cos(th) * half.y,
                                        std::sin(th) * std::sin(ph) * half.z));
        }
    }
    s.verts.push_back(glm::vec3(0.0f, -half.y, 0.0f)); // south pole: last
    const int south = static_cast<int>(s.verts.size()) - 1;
    auto ring = [slices](int i, int j) { return 1 + (i - 1) * slices + (j % slices); };

    for (int j = 0; j < slices; ++j) addTri(s, 0, ring(1, j + 1), ring(1, j));
    for (int i = 1; i < stacks - 1; ++i)
        for (int j = 0; j < slices; ++j) {
            const int a = ring(i, j), b = ring(i, j + 1);
            const int c = ring(i + 1, j + 1), d = ring(i + 1, j);
            addTri(s, a, b, c);
            addTri(s, a, c, d);
        }
    for (int j = 0; j < slices; ++j)
        addTri(s, south, ring(stacks - 1, j), ring(stacks - 1, j + 1));
    orientOutward(s);
    return s;
}

// A flat sheet across the box's X/Z, at its centre height. The one kind with no
// volume to hold it up: with nothing pinned it simply falls, which is a fine
// thing to drop over a car and a poor thing to hang a flag from.
Surface clothSurface(glm::vec3 half, int res, int pinning) {
    const int n = std::clamp(res, 2, 16) + 1; // vertices per side
    Surface s;
    s.verts.reserve(static_cast<std::size_t>(n) * n);
    s.pinned.assign(static_cast<std::size_t>(n) * n, 0);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const float u = float(x) / float(n - 1), v = float(z) / float(n - 1);
            s.verts.push_back(glm::vec3(glm::mix(-half.x, half.x, u), 0.0f,
                                        glm::mix(-half.z, half.z, v)));
            const bool corner = (x == 0 || x == n - 1) && (z == 0 || z == n - 1);
            if ((pinning == 1 && corner) || (pinning == 2 && z == 0))
                s.pinned[static_cast<std::size_t>(z) * n + x] = 1;
        }
    for (int z = 0; z < n - 1; ++z)
        for (int x = 0; x < n - 1; ++x) {
            const int a = z * n + x, b = z * n + x + 1;
            const int c = (z + 1) * n + x + 1, d = (z + 1) * n + x;
            addTri(s, a, b, c);
            addTri(s, a, c, d);
        }
    if (pinning == 0) s.pinned.clear();
    return s;
}

// The entity's own modelled mesh as a soft shell, scaled the way the renderer
// scales it (mesh bounds -> the entity's half-extents), so what starts wobbling
// is exactly the shape that was standing there a frame ago.
Surface meshSurface(const EditMesh& m, glm::vec3 half) {
    glm::vec3 mn, mx;
    m.bounds(mn, mx);
    const glm::vec3 size  = glm::max(mx - mn, glm::vec3(1.0e-4f));
    const glm::vec3 scale = (half * 2.0f) / size;
    const glm::vec3 mid   = 0.5f * (mn + mx);
    Surface s;
    s.verts.reserve(m.verts.size());
    for (const glm::vec3& v : m.verts) s.verts.push_back((v - mid) * scale);
    for (const std::vector<int>& f : m.faces)
        for (std::size_t k = 2; k < f.size(); ++k) // fan: n-gons become triangles
            addTri(s, f[0], f[k - 1], f[k]);
    return s;
}

} // namespace

void SoftBodySystem::spawn(std::vector<Entity>& entities,
                           fitzel::PhysicsWorld& world) {
    m_bodies.clear();
    for (Entity& e : entities) {
        const auto* sc = e.components.get<SoftBodyComponent>();
        if (!sc || !e.activeInHierarchy) continue;
        if (e.type == EntityType::Light || e.type == EntityType::Sun) continue;

        fitzel::PhysicsWorld::SoftBodyDesc desc;
        desc.mass            = std::max(sc->mass, 0.1f);
        desc.compliance      = complianceOf(sc->softness);
        desc.shearCompliance = desc.compliance;
        desc.damping         = glm::clamp(sc->damping, 0.0f, 1.0f);
        const glm::quat q    = glm::quat(glm::radians(e.rotation));
        const glm::vec3 half = glm::max(e.half, glm::vec3(0.05f));

        fitzel::PhysicsBodyId id = 0;
        bool twoSided    = false;
        if (sc->kind == SoftBodyComponent::Jelly) {
            // The lattice is the one shape Jolt has to build itself: its particles
            // go THROUGH the box, and there is no surface to hand over.
            id = world.addSoftBox(half, sc->resolution, e.center, q, desc);
        } else {
            Surface s;
            if (sc->kind == SoftBodyComponent::Cloth) {
                s = clothSurface(half, sc->resolution, sc->pinning);
                twoSided = true; // a sheet has no inside to cull away
                // Cloth barely STRETCHES but folds easily, so its diagonals have
                // to give where its edges do not. Tie the two together and a
                // hanging sheet comes out as a stiff panel: it is the shear that
                // makes the difference between a flag and a table top.
                desc.shearCompliance = desc.compliance * 4.0f + 2.0e-4f;
            } else if (sc->kind == SoftBodyComponent::FromMesh) {
                const auto* mc = e.components.get<MeshComponent>();
                if (!mc) continue; // nothing modelled here to make soft
                s = meshSurface(mc->mesh, half);
                desc.pressure = std::max(sc->pressure, 0.0f);
            } else {
                s = ballSurface(half, sc->resolution);
                desc.pressure = std::max(sc->pressure, 0.0f);
            }
            if (s.verts.empty() || s.tris.empty()) continue;
            id = world.addSoftMesh(s.verts.data(), static_cast<int>(s.verts.size()),
                                   s.tris.data(), static_cast<int>(s.tris.size()),
                                   s.pinned.empty() ? nullptr : s.pinned.data(),
                                   e.center, q, desc);
        }
        if (!id) continue;

        const int faces = world.softFaceCount(id);
        const int count = world.softVertexCount(id);
        if (faces <= 0 || count <= 0) continue;
        Body b;
        b.id = id;
        b.tris.resize(static_cast<std::size_t>(faces) * 3);
        world.getSoftFaces(id, b.tris.data(), faces * 3);
        b.verts.resize(static_cast<std::size_t>(count));

        // The entity wears the simulated surface as a mesh for the length of the
        // run. Its TOPOLOGY is fixed -- only the corners move -- so the faces are
        // written once here and sync() touches nothing but the vertices.
        MeshComponent* mc = e.components.get<MeshComponent>();
        if (!mc) {
            e.components.items.push_back(std::make_unique<MeshComponent>());
            mc = e.components.get<MeshComponent>();
        }
        mc->mesh.verts.assign(static_cast<std::size_t>(count), glm::vec3(0.0f));
        mc->mesh.paint.clear(); // these are not the corners anything was painted on
        mc->mesh.faces.clear();
        mc->mesh.faces.reserve(static_cast<std::size_t>(faces) * (twoSided ? 2 : 1));
        for (int f = 0; f < faces; ++f) {
            const int a = static_cast<int>(b.tris[3 * f]);
            const int c = static_cast<int>(b.tris[3 * f + 1]);
            const int d = static_cast<int>(b.tris[3 * f + 2]);
            mc->mesh.faces.push_back({a, c, d});
            // Cloth is one triangle thick: without the mirrored copy it vanishes
            // the moment it drapes and you end up looking at its back.
            if (twoSided) mc->mesh.faces.push_back({d, c, a});
        }
        m_bodies[e.id] = std::move(b);
    }
}

void SoftBodySystem::remove(int entityId, fitzel::PhysicsWorld& world) {
    auto it = m_bodies.find(entityId);
    if (it == m_bodies.end()) return;
    world.removeBody(it->second.id);
    m_bodies.erase(it);
}

void SoftBodySystem::sync(std::vector<Entity>& entities,
                          fitzel::PhysicsWorld& world, const SetWorld& setWorld) {
    if (m_bodies.empty()) return;
    for (Entity& e : entities) {
        auto it = m_bodies.find(e.id);
        if (it == m_bodies.end()) continue;
        Body& b = it->second;
        MeshComponent* mc = e.components.get<MeshComponent>();
        if (!mc || b.verts.empty()) continue;

        glm::vec3 center(0.0f);
        if (!world.getSoftVertices(b.id, b.verts.data(),
                                   static_cast<int>(b.verts.size()), center))
            continue;
        mc->mesh.verts = b.verts; // positions relative to the body's centre of mass
        mc->touch();

        // The mesh path fits a mesh to the entity's half-extents. Keeping those
        // equal to the simulated bounds makes that fit exactly 1:1 -- otherwise
        // every squash would be scaled straight back out again on its way to the
        // screen -- and it keeps the pick box honest about the shape inside it.
        glm::vec3 mn, mx;
        mc->mesh.bounds(mn, mx);
        e.half = glm::max(mx - mn, glm::vec3(1.0e-4f)) * 0.5f;
        // No rotation of its own: the body's rotation is already in the vertices.
        setWorld(e, center, glm::vec3(0.0f));
    }
}
