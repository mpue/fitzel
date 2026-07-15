#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <fitzel/graphics/Mesh.hpp> // Vertex (skinning output)

namespace fitzel {

// Per-vertex skin binding, parallel to a skinned primitive's de-indexed vertices:
// up to four skeleton joint indices with blend weights (summing to ~1).
struct VertexSkin {
    int   joints[4]  = {0, 0, 0, 0};
    float weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

// One skeleton joint. `parent` indexes the skeleton (-1 = a root). `inverseBind`
// maps a model-space vertex into this joint's bind-pose local space. rest* is the
// joint's local transform when no animation channel drives it. `baseParent` is
// the world transform of a root joint's non-joint ancestor (e.g. an armature),
// so the rest pose reproduces the bind pose exactly.
struct SkeletonJoint {
    int       parent = -1;
    glm::mat4 inverseBind{1.0f};
    glm::mat4 baseParent{1.0f};
    glm::vec3 restT{0.0f};
    glm::quat restR{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 restS{1.0f};
};

// A joint's animated tracks within a clip (an empty track = use the joint rest).
struct JointTrack {
    std::vector<float>     tTimes; std::vector<glm::vec3> tVals;
    std::vector<float>     rTimes; std::vector<glm::quat> rVals;
    std::vector<float>     sTimes; std::vector<glm::vec3> sVals;
};

// A named animation: one track per skeleton joint, sampled over [0, duration].
struct AnimationClip {
    std::string             name;
    float                   duration = 0.0f;
    std::vector<JointTrack> tracks; // sized to the skeleton's joint count
};

// One material group of a loaded model: a de-indexed triangle list interleaved
// as position(3) + normal(3) + uv(2) per vertex (8 floats), plus its decoded
// base-colour texture (RGBA, top-to-bottom). Node transforms are baked in for
// static meshes; for skinned meshes the vertices stay in model space and `skin`
// (one entry per vertex) drives the deformation.
struct ModelPrimitive {
    std::vector<float>         vertices;  // 8 floats per vertex
    std::vector<VertexSkin>    skin;      // per-vertex binding (empty = static)
    std::vector<std::uint8_t>  texPixels; // RGBA base colour, empty if untextured
    int   texWidth    = 0;
    int   texHeight   = 0;
    std::vector<std::uint8_t>  normalPixels; // RGBA tangent-space normal map (opt)
    int   normalWidth  = 0;
    int   normalHeight = 0;
    std::vector<std::uint8_t>  emissionPixels; // RGBA emission/_Illum map (opt)
    int   emissionWidth  = 0;
    int   emissionHeight = 0;
    bool  alphaCutout = false;            // material uses MASK/BLEND (foliage)
    float baseColor[4] = {0.8f, 0.8f, 0.8f, 1.0f}; // PBR base-colour factor (tint)
    std::string materialName;             // glTF material name (may be empty)

    int vertexCount() const { return static_cast<int>(vertices.size() / 8); }
};

// A loaded glTF/GLB model split into material primitives, optionally with a
// skeleton + animation clips (skinned characters).
struct ModelData {
    std::vector<ModelPrimitive> primitives;
    std::vector<SkeletonJoint>  skeleton;   // empty for static models
    std::vector<AnimationClip>  animations; // empty for static models
    float minY = 0.0f;
    float maxY = 0.0f;

    bool  empty()    const { return primitives.empty(); }
    bool  animated() const { return !skeleton.empty() && !animations.empty(); }
    float height()   const { return maxY - minY; }
};

// Joint palette (each = animated global-joint transform * inverseBind) for
// `clip` sampled at `timeSec` (looped over the clip duration). Empty if the
// model isn't animated / clip index is out of range. Feed to skinPrimitive.
std::vector<glm::mat4> sampleSkeleton(const ModelData& model, int clip, float timeSec);

// CPU-skin a primitive's bind vertices with `palette` into `out` (ready for
// Mesh::update). A static primitive (no skin) is copied through unchanged.
void skinPrimitive(const ModelPrimitive& prim, const std::vector<glm::mat4>& palette,
                   std::vector<Vertex>& out);

// Load a glTF / GLB file (with embedded textures) via cgltf + stb_image.
// Returns an empty ModelData on failure.
ModelData loadGltf(const std::string& path);

// Load a Collada (.dae) file via assimp: node transforms are baked into the
// vertices exactly like loadGltf, so it feeds the same static render path.
// (Skeleton/animation parsing is a later phase.) Empty ModelData on failure.
// `flipV` mirrors the V texture coordinate (FBX/DAE usually need it; see below).
ModelData loadCollada(const std::string& path, bool flipV = true);

// Load a rigged model via assimp (FBX and friends): as loadCollada, but also
// extracts the skeleton, per-vertex skin bindings and animation clips, so a
// character feeds the same skinning path as loadGltf -- sampleSkeleton and
// skinPrimitive don't care which format the rig came from.
//
// Unlike loadGltf (where the spec says a skinned mesh's node transform is ignored),
// vertices are baked through their node transform here and the palette deforms them
// from that world-space bind pose. A file with bones but no clips can only ever
// stand in bind pose, so it falls back to loadCollada's static output rather than
// paying for a skinning pass to do nothing. Empty ModelData on failure.
ModelData loadSkinnedModel(const std::string& path, bool flipV = true);

// One mesh-bearing node of a model imported with its structure preserved: the
// node's meshes (world transform baked, then recentred on their combined AABB so
// each sits at the origin) plus `center`, where that origin belongs in model
// space. Lets a caller spread a model across one editor entity per node.
struct ModelNode {
    std::string name;
    ModelData   data;         // meshes centred at the origin
    glm::vec3   center{0.0f}; // world position of that centre (model space)
};

// Load a model via assimp (FBX, DAE, ...), one ModelNode per mesh-bearing node,
// so the caller can create a separate entity per element (structure preserved,
// no animation). Empty on failure. `flipV` mirrors the V texture coordinate:
// most FBX/DAE author UVs bottom-left (glTF/our upload are top-left), so V needs
// flipping -- but it varies per exporter, hence the toggle.
std::vector<ModelNode> loadModelNodes(const std::string& path, bool flipV = true);

// One material's Unity-convention texture matches, for the import-preview UI.
// `albedo`/`normal` are absolute paths of the maps found next to the model
// ("<Material or Model>_Albedo" etc.), or empty when none matched.
struct UnityTexMatch {
    std::string material;
    std::string albedo;
    std::string normal;
    std::string emission; // matched _Illum/emission map, empty if none
};

// Inspect a model file WITHOUT importing: list its materials and the albedo /
// normal maps found for each by Unity naming convention (sibling Textures/
// folders). Drives the "Import Unity Asset" panel's preview; the same matching
// runs during the real import (see loadModelNodes), so a preview is faithful.
std::vector<UnityTexMatch> previewUnityTextures(const std::string& path);

// The image files found in the folders the matcher searches around `path` (bare
// filenames, sorted, de-duplicated). Diagnostic for the import panel: shows what
// textures are actually present so mismatched naming is visible at a glance.
std::vector<std::string> nearbyTextureFiles(const std::string& path);

} // namespace fitzel
