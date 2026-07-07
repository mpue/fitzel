#pragma once

#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/asset/AssetId.hpp>
#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/Texture.hpp>
#include <fitzel/world/Model.hpp>

#include "Component.hpp"

// Editor scene data types, shared across the sandbox translation units.

// A scene entity: a placed, selectable object. Box/Ramp/Cylinder/Sphere are
// solid, walkable geometry; Model is an imported glTF/GLB; Light is a point
// light; Sun is the (singleton) directional light driving the whole sky. Empty
// is a transform-only node (no geometry) used to group and parent other objects,
// exactly like an empty GameObject in Unity.
// New types are appended last so the enum value (== serialized index, palette
// combo index) of the existing ones is unaffected.
enum class EntityType { Box, Ramp, Cylinder, Sphere, Light, Sun, Model, Empty };

inline const char* entityTypeName(EntityType t) {
    switch (t) {
        case EntityType::Box:      return "Box";
        case EntityType::Ramp:     return "Ramp";
        case EntityType::Cylinder: return "Cylinder";
        case EntityType::Sphere:   return "Sphere";
        case EntityType::Light:    return "Light";
        case EntityType::Sun:      return "Sun";
        case EntityType::Model:    return "Model";
        case EntityType::Empty:    return "Empty";
    }
    return "Entity";
}

struct Entity {
    EntityType  type = EntityType::Box;
    glm::vec3   center;
    glm::vec3   half{1.0f};                  // half-extents (Ramp rises along +Z)
    glm::vec3   rotation{0.0f};              // Euler angles in degrees (gizmo)
    // Capabilities live in components (see Component.hpp): light -> Light/Sun,
    // physics -> Physics, material -> Material, imported model -> ModelComponent.
    std::string name;
    int         id     = 0;   // stable unique id (survives deletion/reordering)
    int         parent = -1;  // parent's id, or -1 for a root object
    ComponentList components; // optional attached capabilities (deep-copied)

    // Scene-graph transform: localCenter/localRotation are the SOURCE OF TRUTH
    // (relative to the parent); center/rotation above are the derived WORLD values
    // that every consumer reads, filled by resolveHierarchy() each frame. The
    // inspector/scripts edit local; the gizmo/physics edit world (converted back).
    glm::vec3   localCenter{0.0f};
    glm::vec3   localRotation{0.0f};
};

// A reusable surface material asset, saved as a `.fmat` file in the project's
// materials/ folder. Solids reference one by its `assetId` (GUID); several meshes
// can share a material, and editing it updates them all. Drives the lit shader's
// albedo / reflection parameters.
struct MaterialDef {
    fitzel::AssetId assetId;                  // stable GUID (from its .fmat, or
                                              // generated for model-embedded mats)
    std::string name;
    glm::vec3   albedo{0.72f, 0.72f, 0.74f};
    float       reflectivity = 0.0f;         // 0 matte .. 1 mirror (env probe)
    float       roughness    = 0.2f;         // reflection blur (0 sharp)
    float       opacity      = 1.0f;         // 1 opaque .. 0 invisible (alpha blend)
    bool        glass        = false;        // Fresnel alpha: clear centre, reflective rim
    // Optional base-colour texture (shared so MaterialDef stays copyable). When
    // set, the surface samples it (uColorMode 2) instead of the flat albedo.
    std::shared_ptr<fitzel::Texture> tex;
    // Asset GUID of `tex` when it comes from a file-backed texture asset (so the
    // reference survives save/load). Invalid for model-embedded textures, which
    // are recreated on import and therefore not serialized.
    fitzel::AssetId texId;
    // Optional tangent-space normal map (same conventions as `tex`).
    std::shared_ptr<fitzel::Texture> normalTex;
    fitzel::AssetId normalTexId;
    bool        fromModel = false;           // created by a model import
};

// An imported glTF/GLB, uploaded to the GPU: one Mesh + Material per material
// group, sharing a base-colour texture. Model entities reference one by id and
// render it through the normal Renderer (so it gets shadows/lighting/probe).
// Move-only (owns GPU resources); stored behind unique_ptr for stable addresses
// (Materials hold pointers into `texs`). `boundsMin/Max` are the local AABB.
struct LoadedModel {
    int                       id = 0;
    fitzel::AssetId           assetId;       // GUID of the source file (for save/load)
    std::string               name;
    std::string               path;
    std::vector<fitzel::Mesh> meshes;
    std::vector<fitzel::AssetId> primMaterialId; // library MaterialDef GUID per mesh
    std::vector<glm::vec3>    hullPoints;     // raw vertex positions (physics hull)
    glm::vec3                 boundsMin{0.0f};
    glm::vec3                 boundsMax{0.0f};
    // Skinned models keep their CPU data (skeleton + clips + bind verts) so an
    // AnimationComponent can CPU-skin the meshes each frame (see main.cpp).
    bool                              animated = false;
    std::shared_ptr<fitzel::ModelData> animData;

    glm::vec3 center() const { return 0.5f * (boundsMin + boundsMax); }
    glm::vec3 size()   const { return boundsMax - boundsMin; }
};
