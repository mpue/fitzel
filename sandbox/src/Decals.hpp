#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/asset/AssetId.hpp>
#include <fitzel/graphics/Mesh.hpp>

#include "SceneTypes.hpp"   // Entity, MaterialDef

class ModelLibrary;
class Document;
namespace fitzel {
class Material;
class Renderer;
class Texture;
}

// --- Decals: an image laid on whatever is there --------------------------------
// A poster on a wall, a crack in the floor, moss on a rock, a scorch mark, a
// bullet hole: an image projected onto the surfaces in a box, whatever they
// are -- a modelled object, an imported model, the terrain -- and drawn in an
// ordinary library material, so it is lit, shadowed and wet like everything
// else, in the editor, in Play, in the exported game and in the path tracer.
//
// The road's decals (RoadDecal.hpp) are lofted from the road's own profile; these
// are CUT from the receivers' own triangles instead: every triangle in the box
// that faces the projection is clipped to the box, lifted a centimetre off its
// surface and given the UV of where it lies in the box. Real geometry rather
// than a screen-space trick, which is what this forward renderer wants -- no
// depth prepass, no G-buffer -- and what makes the decal exact on a curved or
// creased surface.
//
// Two kinds share the cutting:
//   - Placed: an object with a Decal component (and a Material). Its box is the
//     decal: move, turn and scale it like any object; it projects down its own
//     -Y. The cut is re-made when the box or anything in it moved.
//   - Thrown: game.decal from a script -- a bullet hole where a shot landed, a
//     splat where something burst. Not objects; a ring of the last 256, gone
//     when Play stops. Only what stands still receives one (a loose physics body
//     would carry its hole off into the air).
namespace decals {

struct Tri {
    glm::vec3 a, b, c;   // world space, counter-clockwise from the outside
};

// The terrain as it is drawn: its height at (x, z). Empty: no terrain.
using HeightFn = std::function<float(float x, float z)>;

// Cut `tris` to the decal box. `boxToWorld` maps the unit cube [-0.5, 0.5]^3 into
// the world; the image lies on its X/Z face and is projected down its -Y. A
// triangle facing away from the projection, or tilted from it by more than
// `maxAngleDeg`, receives nothing. What comes back is in world space, lifted
// `lift` metres off each surface, U along the box's X, V along its -Z (so the
// image stands upright seen from +Y with -Z up), the receivers' own normals.
fitzel::MeshData project(const glm::mat4& boxToWorld, const std::vector<Tri>& tris,
                         float lift, float maxAngleDeg);

// What may receive a decal in the box: modelled objects (as their modifier stack
// shows them), imported static models (their collision triangles) and the
// terrain. Never `skip`, another decal, a hidden object or a dynamic physics body.
struct Receivers {
    bool objects = true;
    bool terrain = true;
    int  skip    = -1;
};
void gather(const std::vector<Entity>& entities, ModelLibrary& models, const HeightFn& terrain,
            const glm::mat4& boxToWorld, const Receivers& who, std::vector<Tri>& out);
// A number that changes when anything that decides the cut changes: the box and
// the objects in it (where they are, how big, which mesh revision). The terrain
// is not in it -- a decal on ground that is sculpted afterwards is moved once to
// follow.
std::uint64_t signature(const std::vector<Entity>& entities, ModelLibrary& models,
                        const glm::mat4& boxToWorld, const Receivers& who);

// The box of a thrown decal: centred on `pos`, its +Y along the surface normal,
// `size` across and half that deep, the image turned `spinDeg` about the normal.
glm::mat4 boxAt(const glm::vec3& pos, const glm::vec3& normal, float size, float spinDeg);

// The engine's own bullet hole (game.decal with no material): a dark hole in a
// ring of soot, RGBA, `size` square. And the same as a texture (GL).
std::vector<unsigned char> bulletHolePixels(int size);
std::shared_ptr<fitzel::Texture> bulletHoleTexture();

class System {
public:
    System();
    ~System();
    System(const System&)            = delete;
    System& operator=(const System&) = delete;

    // Once a frame, before the scene is submitted: every placed decal's cut,
    // made again where its signature changed.
    void update(const std::vector<Entity>& entities, ModelLibrary& models, const HeightFn& terrain);
    // The cut of placed decal `id` (world space), or null when it covers nothing.
    const fitzel::Mesh* meshFor(int id) const;

    // A thrown decal in `material`. False when nothing that stands still is there.
    bool spawn(const fitzel::AssetId& material, const glm::vec3& pos, const glm::vec3& normal,
               float size, float spinDeg, const std::vector<Entity>& entities, ModelLibrary& models,
               const HeightFn& terrain);
    // The thrown ones into the render queue, in the frame's library materials.
    void submitThrown(fitzel::Renderer& renderer, const std::vector<fitzel::Material>& gpuMats,
                      const std::vector<MaterialDef>& materials, const Document& document) const;
    void clearThrown();
    std::size_t thrownCount() const { return m_thrown.size(); }

    static constexpr std::size_t kMaxThrown = 256;

private:
    struct Placed {
        std::uint64_t sig = 0;
        bool          has = false;
        bool          seen = false;
        fitzel::Mesh  mesh;
    };
    struct Thrown {
        fitzel::AssetId material;
        fitzel::Mesh    mesh;
    };
    std::unordered_map<int, Placed> m_placed;
    std::vector<Thrown> m_thrown;
    std::size_t         m_next = 0;   // the oldest, replaced when the ring is full
};

} // namespace decals
