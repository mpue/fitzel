#pragma once

#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/asset/AssetId.hpp>
#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/Texture.hpp>

// Editor scene data types, shared across the sandbox translation units.

// A scene entity: a placed, selectable object. Box/Ramp/Cylinder/Sphere are
// solid, walkable geometry; Model is an imported glTF/GLB; Light is a point
// light; Sun is the (singleton) directional light driving the whole sky.
// Model is appended last so the palette combo (index == enum value) is unaffected.
enum class EntityType { Box, Ramp, Cylinder, Sphere, Light, Sun, Model };

inline const char* entityTypeName(EntityType t) {
    switch (t) {
        case EntityType::Box:      return "Box";
        case EntityType::Ramp:     return "Ramp";
        case EntityType::Cylinder: return "Cylinder";
        case EntityType::Sphere:   return "Sphere";
        case EntityType::Light:    return "Light";
        case EntityType::Sun:      return "Sun";
        case EntityType::Model:    return "Model";
    }
    return "Entity";
}

struct Entity {
    EntityType  type = EntityType::Box;
    glm::vec3   center;
    glm::vec3   half{1.0f};                  // half-extents (Ramp rises along +Z)
    glm::vec3   rotation{0.0f};              // Euler angles in degrees (gizmo)
    glm::vec3   color{0.62f, 0.62f, 0.64f};  // albedo / light colour (tint for Sun)
    float       intensity = 8.0f;            // Light/Sun only
    float       range      = 12.0f;          // Light only: falloff + shadow far plane
    bool        castShadows = false;         // Light only: opt-in cube shadows
    float       shadowBias  = 0.003f;        // Light only: normalized cube-shadow bias
    int         materialId = 0;              // solids: assigned MaterialDef id
    int         modelId   = -1;              // Model only: LoadedModel id
    float       scale     = 1.0f;            // Model only: uniform scale
    std::string modelPath;                   // Model only: source file (for reload)
    std::string script;                      // Lua file under scripts/ ("" = none)
    std::string name;
    int         id     = 0;   // stable unique id (survives deletion/reordering)
    int         parent = -1;  // parent's id, or -1 for a root object
};

// A reusable surface material asset. Solids reference one by `id`; several
// meshes can share a material, and editing it updates them all. Drives the lit
// shader's albedo / reflection parameters.
struct MaterialDef {
    int         id = 0;                      // stable unique id
    std::string name;
    glm::vec3   albedo{0.72f, 0.72f, 0.74f};
    float       reflectivity = 0.0f;         // 0 matte .. 1 mirror (env probe)
    float       roughness    = 0.2f;         // reflection blur (0 sharp)
    // Optional base-colour texture (shared so MaterialDef stays copyable). When
    // set, the surface samples it (uColorMode 2) instead of the flat albedo.
    std::shared_ptr<fitzel::Texture> tex;
    // Asset GUID of `tex` when it comes from a file-backed texture asset (so the
    // reference survives save/load). Invalid for model-embedded textures, which
    // are recreated on import and therefore not serialized.
    fitzel::AssetId texId;
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
    std::vector<int>          primMaterialId; // library MaterialDef id per mesh
    glm::vec3                 boundsMin{0.0f};
    glm::vec3                 boundsMax{0.0f};

    glm::vec3 center() const { return 0.5f * (boundsMin + boundsMax); }
    glm::vec3 size()   const { return boundsMax - boundsMin; }
};
