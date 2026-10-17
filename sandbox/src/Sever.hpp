#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <fitzel/asset/AssetId.hpp>
#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/physics/Physics.hpp>

#include "SceneTypes.hpp"   // Entity, MaterialDef, LoadedModel

class ModelLibrary;
class Document;
namespace fitzel {
class Material;
class Renderer;
}
namespace boneattach { class Attachments; }

// --- Limbs that come off ------------------------------------------------------------
// A chainsaw through an arm, a leg, a neck, a waist (game.sever): what hangs on that
// bone leaves the figure and falls as a piece of its own.
//
// The piece is the figure's own skin, exactly as it was last drawn: every triangle
// that is carried mostly by the bone or a bone under it, posed on the CPU with the
// palette the skinning pass stored (BoneAttach), in the figure's materials. Where it
// was joined to the rest, its open edge is closed with a cap in the material the
// script names (flesh). It becomes a dynamic body in the physics world -- a convex
// hull round it -- so it drops, tumbles, rolls off steps, comes to rest on whatever
// is under it, and the player can kick it about. On the figure, the bone with all
// under it is scaled to nothing (as game.boneScale 0 would), which pinches the skin
// shut at the joint.
//
// Shots pass through the pieces (castRay asks owns() and looks past them): they
// are scenery, not cover. Play-time state, like the physics world it lives in:
// cleared when Play starts and stops. The oldest pieces go when there are too many.
namespace sever {

struct Options {
    fitzel::AssetId cap;          // material of the cut face; invalid: none
    glm::vec3       vel{0.0f};    // m/s, added to how the bone was moving (not known: 0)
    glm::vec3       spin{0.0f};   // rad/s, world axes
};

// What came off: where the cut is (the middle of the open edge) and which way the
// cut face of the piece looks (from the piece towards where it was joined).
struct Cut {
    glm::vec3 at{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    int       triangles = 0;
    int       piece = 0;     // its handle, for piece()
};

// Where a piece's cut face is now, which way it looks, and whether the piece
// has come to rest.
struct PieceState {
    glm::vec3 at{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    bool      resting = false;
};

class System {
public:
    System();
    ~System();
    System(const System&)            = delete;
    System& operator=(const System&) = delete;

    // Cut what hangs on `bone` of `figure` off into a falling piece. False for an
    // unknown figure or bone, a figure not posed yet, or a bone that carries no skin
    // (nothing to cut). The caller hides the bone on the figure.
    bool cut(const std::vector<Entity>& entities, ModelLibrary& models,
             const boneattach::Attachments& poses, fitzel::PhysicsWorld& physics, int figure,
             const std::string& bone, const Options& opts, Cut& out);

    // Is this physics body one of the pieces?
    bool owns(fitzel::PhysicsBodyId body) const;
    // The piece with handle `piece` (from Cut) as it lies now -- so a script can
    // let it bleed from its cut face. False once it is gone (swept up, fell out
    // of the world, Play ended).
    bool piece(int piece, const fitzel::PhysicsWorld* physics, PieceState& out) const;

    // Once a frame in Play, after the physics step: the pieces follow their bodies.
    void update(const fitzel::PhysicsWorld* physics);
    void submit(fitzel::Renderer& renderer, const std::vector<fitzel::Material>& gpuMats,
                const std::vector<MaterialDef>& materials, const Document& document) const;
    // Everything forgotten. Without a world (it was just thrown away) the bodies
    // went with it; with one, they are taken out of it.
    void clear(fitzel::PhysicsWorld* physics = nullptr);
    std::size_t count() const { return m_pieces.size(); }

    // Pieces kept at most: the oldest go first.
    static constexpr std::size_t kMaxPieces = 40;

private:
    struct Part {
        fitzel::AssetId material;
        fitzel::Mesh    mesh;      // around the body's origin
    };
    struct Piece {
        fitzel::PhysicsBodyId body = 0;
        int                   handle = 0;
        std::vector<Part>     parts;
        glm::mat4             model{1.0f};
        glm::vec3             cutAt{0.0f};                 // around the body's origin
        glm::vec3             cutN{0.0f, 1.0f, 0.0f};      // as it was cut
    };
    std::vector<std::unique_ptr<Piece>> m_pieces;
    int m_nextHandle = 1;
};

} // namespace sever
