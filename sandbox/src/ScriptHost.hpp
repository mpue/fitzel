#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

// The host bridge exposed to Lua scripts as the global `game` table. The sandbox
// fills these callbacks and fields in before ticking scripts each frame; the
// ScriptSystem's C functions call through them. Entity creation/removal is
// deferred by the host (the tick loop is iterating the entity list), so
// game.spawn returns the new id immediately but the entity appears next frame.
//
// Most callbacks are wired in ScriptBridge.cpp (assets, models, materials,
// entity queries, world helpers); the ones that need main()'s own state (input,
// physics bodies, audio voices, camera) are wired in main.cpp. Every callback is
// optional -- an unset one makes the matching Lua function a harmless no-op that
// returns nil/false.

// A request to create an entity at runtime (see game.spawn in Lua).
struct ScriptSpawn {
    int         type    = 3;          // EntityType (3 = Sphere)
    glm::vec3   pos{0.0f};
    glm::vec3   half{0.5f};           // half extents
    glm::vec3   rot{0.0f};            // Euler degrees
    glm::vec3   color{0.8f};
    glm::vec3   vel{0.0f};            // initial linear velocity (dynamic bodies)
    float       mass    = 1.0f;
    int         physics = 2;          // 0 none, 1 static, 2 dynamic
    std::string name;
    std::string script;               // Lua file under scripts/ ("" = none)
    // Asset-driven spawning. `model` is a Model asset (file name or GUID): set
    // it and the spawn becomes a Model entity sized from the model's AABB,
    // scaled by `scale`. `material` is a library material (name or GUID) applied
    // to a solid. `parent` attaches the new entity to an existing one (-1 root).
    std::string model;
    float       scale   = 1.0f;
    std::string material;
    int         parent  = -1;
};

// One row of the asset database as scripts see it (see game.assets).
struct ScriptAssetInfo {
    std::string id;       // 32-char GUID
    std::string name;     // file name with extension
    std::string path;     // '/'-separated path relative to its source root
    std::string type;     // "Texture" | "Model" | "Sound" | "Material"
    std::string source;   // "Engine" | "Project"
};

// An imported model in the ModelLibrary (see game.modelInfo).
struct ScriptModelInfo {
    std::string name;
    std::string path;
    glm::vec3   boundsMin{0.0f};
    glm::vec3   boundsMax{0.0f};
    int         meshes   = 0;
    bool        animated = false;
};

// A library material as scripts see it (see game.materialInfo).
struct ScriptMaterialInfo {
    std::string id;                   // material GUID
    std::string name;
    glm::vec3   albedo{0.72f};
    float       reflectivity = 0.0f;
    float       roughness    = 0.2f;
    float       opacity      = 1.0f;
    bool        glass        = false;
    int         alphaMode    = 0;     // 0 opaque, 1 cutout, 2 blend
    float       alphaCutoff  = 0.5f;
    glm::vec3   emission{0.0f};
    float       emissionStrength = 1.0f;
    std::string texture;              // texture asset GUID ("" = none)
    std::string normalMap;
    std::string emissionMap;
    bool        fromModel    = false; // owned by a model import
};

// The fields a script wants to change on a material (game.createMaterial /
// game.setMaterialProps). Only the engaged ones are written, so a script can
// tweak one value without restating the rest. The three map slots take an asset
// name or GUID; an empty string clears the slot.
struct ScriptMaterialEdit {
    std::optional<std::string> name;
    std::optional<glm::vec3>   albedo;
    std::optional<float>       reflectivity;
    std::optional<float>       roughness;
    std::optional<float>       opacity;
    std::optional<bool>        glass;
    std::optional<int>         alphaMode;
    std::optional<float>       alphaCutoff;
    std::optional<glm::vec3>   emission;
    std::optional<float>       emissionStrength;
    std::optional<std::string> texture;
    std::optional<std::string> normalMap;
    std::optional<std::string> emissionMap;
};

// What a script can change on a Light component (game.setLight).
struct ScriptLightEdit {
    std::optional<glm::vec3> color;
    std::optional<float>     intensity;
    std::optional<float>     range;
    std::optional<int>       type;       // 0 point, 1 spot
    std::optional<float>     spotAngle;
    std::optional<float>     spotBlend;
};

// A scene entity as scripts see it (see game.entityInfo).
struct ScriptEntityInfo {
    int         id     = 0;
    int         type   = 0;       // EntityType
    int         parent = -1;
    std::string name;
    std::string script;           // Script component's file ("" = none)
    std::string material;         // Material component's GUID ("" = none)
    std::string model;            // Model component's source path ("" = none)
    bool        active           = true;
    bool        activeInHierarchy = true;
    bool        hasPhysics = false;
    bool        dynamic    = false;
};

struct ScriptHost {
    // --- Input ----------------------------------------------------------------
    // `key` is a GLFW key code (see game.KEY_* constants); `button` is
    // 0=left, 1=right, 2=middle. *Pressed variants are true only on the frame
    // the key/button goes down.
    std::function<bool(int)> keyDown;
    std::function<bool(int)> keyPressed;
    std::function<bool(int)> mouseDown;
    std::function<bool(int)> mousePressed;

    // --- Camera ---------------------------------------------------------------
    // Player camera, refreshed each frame (Play mode).
    glm::vec3 camPos{0.0f};
    glm::vec3 camDir{0.0f, 0.0f, -1.0f};
    glm::vec2 screen{0.0f};       // viewport size in pixels

    std::function<void(glm::vec3)> setCamPos;
    std::function<void(glm::vec3)> setCamDir;   // direction is normalised by the host
    std::function<void(float)>     setCamFov;
    // Render from the Camera component on this entity (-1 = the player view).
    std::function<void(int)>       setActiveCamera;

    // --- Entities: lifetime ---------------------------------------------------
    // spawn returns the new id immediately (creation deferred to the end of the
    // frame). getPos fills outPos and returns false for unknown ids.
    std::function<int(const ScriptSpawn&)>          spawn;
    // Instantiate a prefab by name at a world position (yaw in degrees). Returns
    // the new root entity's id, or 0 if no prefab of that name exists. Deferred
    // like spawn -- the whole subtree appears next frame. See game.spawnPrefab.
    std::function<int(const std::string& name, glm::vec3 pos, float yawDeg)> spawnPrefab;
    std::function<void(int)>                        destroy;
    std::function<bool(int, glm::vec3&)>            getPos;
    std::function<void(int, glm::vec3)>            setPos;

    // --- Entities: query & transform ------------------------------------------
    std::function<std::vector<int>()>               allEntities;
    std::function<int(const std::string&)>          findEntity;   // -1 if absent
    std::function<std::vector<int>(const std::string&)> findEntities;
    std::function<bool(int, ScriptEntityInfo&)>     entityInfo;
    std::function<bool(int, glm::vec3&)>            getRot;
    std::function<void(int, glm::vec3)>             setRot;
    std::function<bool(int, glm::vec3&)>            getScale;     // half extents
    std::function<void(int, glm::vec3)>             setScale;
    std::function<void(int, const std::string&)>    setName;
    std::function<void(int, bool)>                  setActive;
    std::function<void(int, int)>                   setParent;    // -1 = detach
    std::function<std::vector<int>(int)>            children;

    // --- Physics on a dynamic body (by entity id). No-ops on unknown ids. ------
    std::function<void(int, glm::vec3)> setVelocity;
    std::function<void(int, glm::vec3)> applyImpulse;
    std::function<bool(int, glm::vec3&)> getVelocity;
    std::function<void(int, glm::vec3)> setAngularVelocity;

    // --- Assets ---------------------------------------------------------------
    // List every asset of `type` ("" = all types). findAsset resolves a file name
    // (or a relative path, or a GUID) to a GUID, optionally restricted to a type;
    // it returns "" when nothing matches.
    std::function<std::vector<ScriptAssetInfo>(const std::string& type)> assetList;
    std::function<std::string(const std::string& name, const std::string& type)> findAsset;
    std::function<bool(const std::string& id, ScriptAssetInfo&)> assetInfo;
    std::function<std::string(const std::string& id)> assetPath; // absolute path
    std::function<void()> refreshAssets;                         // rescan from disk

    // --- Models ---------------------------------------------------------------
    // Import a Model asset (name or GUID) into the model library and return its
    // runtime id (-1 on failure). Already-imported models are reused.
    std::function<int(const std::string& asset)> loadModel;
    std::function<bool(int, ScriptModelInfo&)>   modelInfo;

    // --- Materials ------------------------------------------------------------
    std::function<std::vector<ScriptMaterialInfo>()>            materialList;
    std::function<std::string(const std::string& name)>         findMaterial; // "" if absent
    std::function<bool(const std::string& id, ScriptMaterialInfo&)> materialInfo;
    std::function<std::string(const ScriptMaterialEdit&)>       createMaterial;
    std::function<bool(const std::string& id, const ScriptMaterialEdit&)> setMaterialProps;
    // Assign a library material to an entity / read back the assigned GUID.
    std::function<bool(int, const std::string& id)> setEntityMaterial;
    std::function<std::string(int)>                 entityMaterial;
    // Give this entity its own material tinted to `rgb` (creating one on first
    // use, reused afterwards), so recolouring one object never affects another.
    std::function<bool(int, glm::vec3)>             setEntityColor;

    // --- Lights ---------------------------------------------------------------
    std::function<bool(int, const ScriptLightEdit&)> setLight;

    // --- World ----------------------------------------------------------------
    std::function<float(float, float)> terrainHeight;
    // Ray vs. the entity pick boxes. Returns the hit entity id (-1 = miss) and
    // fills the world-space hit point and distance.
    std::function<int(glm::vec3 origin, glm::vec3 dir, float maxDist,
                      glm::vec3& outHit, float& outDist)> raycast;
    // Load another scene of the open project by name (deferred to frame end).
    std::function<void(const std::string&)> loadScene;
    // Print a line to the console / editor log.
    std::function<void(const std::string&)> log;

    // --- Audio ----------------------------------------------------------------
    // Play a one-shot sound file from the project's sounds/ folder.
    std::function<void(const std::string&)> playSound;

    // Start / stop an entity's AudioSource component by id (game.playAudio /
    // game.stopAudio). No-ops on ids without an AudioSource.
    std::function<void(int)> playAudio;
    std::function<void(int)> stopAudio;

    // --- Shared game state ----------------------------------------------------
    // Lua script environments are per-entity (isolated), so shared state like the
    // score lives here and is reached via game.addScore / game.getScore /
    // game.setHud.
    int         score = 0;
    std::string hud;
};
