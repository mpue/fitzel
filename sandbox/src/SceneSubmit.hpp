#pragma once

#include <deque>
#include <vector>

#include <glm/glm.hpp>

// Complete, not forward-declared: Scratch holds vectors of them and clears
// them inline, which instantiates the vector.
#include <fitzel/graphics/Material.hpp>

#include "Document.hpp"
#include "EditMesh.hpp"     // EditMeshCache
#include "ModelLibrary.hpp"
#include "SceneTypes.hpp"   // MaterialDef, Entity

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

    void clear() { gpuMats.clear(); paintMats.clear(); lightMats.clear(); }
};

// Build the materials and submit every visible entity. Call between
// Renderer::begin() and the render passes.
void submit(const Context& c, Scratch& scratch);

} // namespace scenesubmit
