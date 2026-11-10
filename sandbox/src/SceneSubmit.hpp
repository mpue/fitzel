#pragma once

#include <deque>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

// Complete, not forward-declared: Scratch holds vectors of them and clears
// them inline, which instantiates the vector.
#include <fitzel/graphics/Material.hpp>

#include "Document.hpp"
#include "EditMesh.hpp"     // EditMeshCache
#include "ModelLibrary.hpp"
#include "SceneTypes.hpp"   // MaterialDef, Entity
#include "Wounds.hpp"

struct GlassDropsView;

namespace fitzel {
class Mesh;
class Renderer;
class Shader;
}

// Handing the scene's entities to the renderer: one GPU material per library
// asset, then every visible entity as the geometry it actually is -- an imported
// model's primitives, a modelled mesh, or one of the boxes-and-friends.
//
// This lived in main() until it was the only copy. That is fine as long as the
// editor is the only thing that draws a scene, and stops being fine the moment
// anything else needs to: an offline render, a check that a change looks the way
// it should, a thumbnail. A second implementation would be "close enough" on the
// day it was written and quietly different a month later -- and the difference
// would be invisible in exactly the way the harnesses here exist to catch. So it
// is one function, and the editor is one of its callers.
//
// Nothing about it is editor-specific: no ImGui, no selection, no gizmo. The one
// thing it will not decide for itself is how a transform becomes a matrix (see
// ComposeModel).
namespace scenesubmit {

// The model matrix for a placed entity, injected rather than reimplemented.
// The editor composes it through ImGuizmo, and this pass has to produce EXACTLY
// that matrix -- a second implementation agreeing to within a rounding error is
// a harness whose picture differs from the screen's, which is the one thing a
// harness must never be. Captureless, so a plain function pointer carries it.
using ComposeModel = glm::mat4 (*)(const glm::vec3& translation,
                                   const glm::vec3& rotationDeg,
                                   const glm::vec3& scale);

// How an object comes apart (game.dissolve), beside how far it has.
struct DissolveStyle {
    glm::vec3 glow{0.0f};   // the colour its edge glows in
    float     cell = 0.0f;  // > 0: in cubes this many metres wide (lit.frag uDissolveCell)
    float     band = 0.0f;  // in cubes: how far ahead of the cut they show (0 = the shader's own)
    glm::vec4 front{0.0f};  // xyz, and w: the share grows by w per metre from there
    // Gone everywhere / whole everywhere at `amount`: a front moves the share
    // by place, so 1 is only gone all over when nothing near is held back.
    bool goneAt(float amount) const { return amount >= 0.999f && front.w >= 0.0f; }
    bool wholeAt(float amount) const { return amount <= 0.0f && front.w <= 0.0f; }
};

// What the pass needs from the frame around it. All borrowed; nothing here owns
// anything, and the caller keeps it all alive across the render.
struct Context {
    const std::vector<Entity>&      entities;
    const std::vector<MaterialDef>& materials;
    const Document&                 document;    // materialIndex() by GUID
    ModelLibrary&                   models;      // imported models, by id
    EditMeshCache&                  meshCache;   // GPU copies of modelled meshes
    fitzel::Shader&                 lit;         // the shader the materials wear
    fitzel::Renderer&               renderer;

    // The primitives an entity can be when it carries no geometry of its own.
    const fitzel::Mesh& box;
    const fitzel::Mesh& ramp;
    const fitzel::Mesh& cylinder;
    const fitzel::Mesh& sphere;
    const fitzel::Mesh& plane;

    ComposeModel composeModel = nullptr;

    float roadWetness = 0.0f;  // the weather, as the materials want it
    bool  playMode    = false; // hides the authoring aids (markers, player starts)

    // The cut of placed decal `id` (Decals.hpp), world space, or null. Unset: a
    // decal object draws nothing.
    std::function<const fitzel::Mesh*(int)> decalMesh = nullptr;
    // Glass shot out in Play (Shatter.hpp): what is left of model object `id`'s
    // primitive `prim` (null: all of it, a mesh with nothing in it: none), and
    // whether object `id` broke as a whole. Unset: nothing is broken.
    std::function<const fitzel::Mesh*(int id, std::size_t prim)> leftOf = nullptr;
    std::function<bool(int id)> vanished = nullptr;
    // A figure posed in meshes of its own (SkinCopies.hpp): its primitive `prim`,
    // or null for the model's shared meshes. Unset: every figure draws those.
    std::function<const fitzel::Mesh*(int id, std::size_t prim)> skinnedOf = nullptr;
    // How wet water falling on object `id` has made it (ParticleSystem::wetness),
    // on top of its Wet component's own wetness. Unset: only the component counts.
    std::function<float(int id)> wetnessOf = nullptr;
    // The water on object `id`'s glass (GlassDrops.hpp), or null. Unset: none.
    std::function<const GlassDropsView*(int id)> glassDropsOf = nullptr;
    // How far object `id` has come apart (game.dissolve): 0 whole .. 1 gone, and
    // how (DissolveStyle). Unset: everything is whole.
    std::function<float(int id, DissolveStyle& style)> dissolveOf = nullptr;
    // The holes in figure `id` (Wounds.hpp), or null. Unset: none.
    std::function<const wounds::List*(int id)> woundsOf = nullptr;
    // The glowing cube a light is marked with in the editor. Off while the
    // viewport shows the objects' icons (overlay::icons): the icon says the same
    // as a picture, and a lamp-sized box on every light only hides what it lights.
    bool lightMarkers = true;
};

// The materials the render queue points INTO, and therefore the reason this is
// not a local inside submit(). Renderer::submit stores a reference, and the
// queue is replayed several times per frame -- shadow cascades, six probe faces,
// the water mirror, the real pass. These have to outlive all of them, so the
// caller owns them and lets them die at the end of its frame.
struct Scratch {
    std::vector<fitzel::Material> gpuMats;   // one per library material
    // Per DRAWN PIECE of a painted mesh (its own slots). A deque, not a vector:
    // a mesh dressed face by face contributes one of these per material it wears,
    // so there is no count to reserve up front -- and a vector that outgrew its
    // reservation would move every material already handed to the renderer.
    std::deque<fitzel::Material>  paintMats;
    std::vector<fitzel::Material> lightMats; // per light marker (its own glow)
    std::deque<fitzel::Material>  enemyMats; // per Enemy box (its own colour)

    void clear() { gpuMats.clear(); paintMats.clear(); lightMats.clear(); enemyMats.clear(); }
};

// Build the materials and submit every visible entity. Call between
// Renderer::begin() and the render passes.
void submit(const Context& c, Scratch& scratch);

} // namespace scenesubmit
