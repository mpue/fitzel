#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

class SynthSystem;
class MusicSystem;

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

// One 2D drawing call a script queued for this frame's HUD (game.hudRect and
// friends). Coordinates are on a virtual canvas 1080 units high and as wide as
// the view's aspect makes it (game.hudSize), so a layout written once holds at
// any window size and in the editor's inset viewport alike. The host scales and
// offsets them onto the rendered view when it draws the HUD.
struct ScriptHudCmd {
    enum class Kind : unsigned char { Rect, Gradient, Frame, Line, Circle, Ring, Tri, Text,
                                      Image };
    Kind        kind = Kind::Rect;
    float       a[6] = {};         // Rect/Frame: x, y, w, h, rounding | Gradient:
                                   // x, y, w, h | Line: x1, y1, x2, y2 | Circle/Ring:
                                   // cx, cy, r | Tri: three corners | Text: x, y |
                                   // Image: x, y, w, h
    float       size    = 0.0f;    // Frame/Line/Ring: thickness | Text: height
    float       align   = 0.0f;    // Text: 0 left .. 0.5 centred .. 1 right
    bool        bold    = false;   // Text: the UI's semibold face
    unsigned    col     = 0;       // RGBA8 (ImGui's IM_COL32 layout) | Image: the tint
    unsigned    col2    = 0;       // Gradient: the bottom colour
    unsigned    tex     = 0;       // Image: the GL texture (kept alive by the host)
    float       uv[4]   = {0.0f, 0.0f, 1.0f, 1.0f}; // Image: u0, v0, u1, v1 (v down)
    std::string text;
};

// A picture as the script HUD holds it (game.hudImage / game.imageSize): the GL
// texture, its size in pixels as loaded (long side at most 1024), and the box its
// visible -- not fully transparent -- part fills, in 0..1 with v running down the
// picture, so an icon rendered with a margin can be fitted to a slot.
struct ScriptImage {
    unsigned  tex    = 0;
    int       width  = 0;
    int       height = 0;
    glm::vec4 content{0.0f, 0.0f, 1.0f, 1.0f}; // u0, v0, u1, v1
};

// What game.castRay met: where, the surface normal there, the object it belongs
// to (-1 = the world itself: terrain, a road, a bridge) and how far along.
struct ScriptRayHit {
    glm::vec3 pos{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    int       id   = -1;
    float     dist = 0.0f;
};

// A Collectible component as scripts see it (game.collectible). `item` is the
// name to show -- the component's own, or the object's when it has none.
struct ScriptCollectible {
    float       points    = 0.0f;
    float       radius    = 0.0f;
    std::string sound;
    bool        inventory = false;  // picked up by a script, not on contact
    std::string item;
    std::string icon;               // Texture asset (file name), "" = none
    std::string category;           // "misc", "weapon", "ammo", "health", "key", "document"
    std::string description;
    int         count     = 1;
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

// One object's LOCAL transform, set by a script in a batch (game.setLocals):
// relative to its parent, which for a part of a unit -- a turret on a hull, a
// leg on a torso -- is exactly the number the script has. `hasRot` false keeps
// the rotation it has.
struct ScriptLocal {
    int       id = 0;
    glm::vec3 pos{0.0f};
    glm::vec3 rot{0.0f};
    bool      hasRot = false;
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
    // The pointer, for a game played with the mouse: where it is in the HUD's
    // 1080-high canvas (origin top left; false when it is not over the view),
    // and the world ray from the eye through it.
    std::function<bool(glm::vec2&)>             mousePos;
    // How far the mouse wheel turned this frame (notches; + = away from you).
    std::function<float()>                      mouseWheel;
    std::function<bool(glm::vec3&, glm::vec3&)> mouseRay;
    // Free the pointer during Play (the walking player holds it), or hand it back.
    std::function<void(bool)>                   showCursor;

    // --- Camera ---------------------------------------------------------------
    // Player camera, refreshed each frame (Play mode).
    glm::vec3 camPos{0.0f};
    glm::vec3 camDir{0.0f, 0.0f, -1.0f};
    glm::vec2 screen{0.0f};       // viewport size in pixels

    std::function<void(glm::vec3)> setCamPos;
    std::function<void(glm::vec3)> setCamDir;   // direction is normalised by the host
    std::function<void(float)>     setCamFov;
    // Depth of field for the script's view: sharp up to nearM, fully blurred
    // beyond farM. farM <= 0 hands the focus back to the view's own settings.
    std::function<void(float nearM, float farM)> setFocus;
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
    // Copy an object -- its components and its children -- under the same
    // parent, as it stands. Deferred like spawn; returns the copy's id (0 for an
    // unknown id). `name` renames the copy ("" keeps the original's).
    std::function<int(int, const std::string& name)> clone;
    std::function<bool(int, glm::vec3&)>            getPos;
    std::function<void(int, glm::vec3)>            setPos;
    // Many objects' local transforms at once (game.setLocal / game.setLocals):
    // one call a frame for a whole army instead of one per part, and one id
    // lookup table for the batch instead of a scan per object.
    std::function<void(const std::vector<ScriptLocal>&)> setLocals;

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
    // The pickups (game.collectibles / game.collectible): every active object
    // with a Collectible component, and one component's data.
    std::function<std::vector<int>()>               collectibles;
    std::function<bool(int, ScriptCollectible&)>    collectible;

    // --- Bones of an animated figure (BoneAttach.hpp) ---------------------------
    // Where a bone of a skinned model is, as the figure was last drawn: world
    // position and rotation (scene Euler degrees). False for an unknown object or
    // bone, or a figure that has not been posed yet.
    std::function<bool(int, const std::string& bone, glm::vec3& pos, glm::vec3& rotDeg)> boneWorld;
    std::function<std::vector<std::string>(int)>    boneNames;
    // Hang `child` on a bone of `figure`: from then on it follows the bone every
    // frame, after the pose and before the picture. `pos`/`rotDeg` (bone space,
    // metres / degrees) place it on the bone; null keeps it where it is now,
    // relative to the bone. `blend` seconds: travel there from where it is now.
    // False for an unknown object or bone.
    std::function<bool(int child, int figure, const std::string& bone,
                       const glm::vec3* pos, const glm::vec3* rotDeg, float blend)> attach;
    std::function<void(int child)>                  detach;
    // A point given in an object's own frame -- metres from its centre, turned
    // with it, not scaled -- in the world (the muzzle of a pistol in a hand).
    std::function<bool(int, glm::vec3 local, glm::vec3& world)> toWorld;

    // --- Animation state machines (AnimGraph.hpp) -------------------------
    // Driving an object's graph from a script: this is the point of the graph
    // having parameters at all. A trigger rings once and is cleared by the
    // transition that reads it, so a script may fire it on any frame without
    // knowing when the machine next steps.
    std::function<void(int, const std::string&)>          animTrigger;
    std::function<void(int, const std::string&, bool)>    animSetBool;
    std::function<void(int, const std::string&, float)>   animSetNumber;
    // The name of the state the object is in ("" when it has no graph running),
    // so a script can wait for a door to have finished opening.
    std::function<std::string(int)>                       animState;
    std::function<std::vector<int>(int)>            children;

    // --- Physics on a dynamic body (by entity id). No-ops on unknown ids. ------
    std::function<void(int, glm::vec3)> setVelocity;
    // An impulse on object `id` -- struck at `at` (world), when given: what turns
    // a hanging thing (Swing.hpp) as well as pushing it.
    std::function<void(int, glm::vec3, const glm::vec3* at)> applyImpulse;
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
    // Water at a world XZ: a river's surface or the sea/lake level when the
    // ground is below it. False where it is dry.
    std::function<bool(float, float, float&)> waterAt;
    // The procedural forest: trees in a rectangle (x, y, z, scale each), and a
    // disc the forest stops growing in (a building went up there).
    std::function<void(glm::vec2 lo, glm::vec2 hi, std::vector<glm::vec4>& out)> trees;
    std::function<void(float x, float z, float r)> clearTrees;
    // Ray vs. the entity pick boxes. Returns the hit entity id (-1 = miss) and
    // fills the world-space hit point and distance.
    std::function<int(glm::vec3 origin, glm::vec3 dir, float maxDist,
                      glm::vec3& outHit, float& outDist)> raycast;
    // A figure walked through the physics world (game.moveCharacter): the first
    // call for an object makes its capsule, standing on whatever is under the
    // object; each call walks it at the horizontal velocity `vel` (x, z in m/s)
    // for `dt` and writes where its feet are now (world space), whether it
    // stands on something, and whether that something is the terrain. False when
    // there is no physics world (not playing) or no such object.
    std::function<bool(int id, glm::vec2 vel, float dt, glm::vec3& foot,
                       bool& onGround, bool& onTerrain)> moveCharacter;
    // Drop an object's capsule again (a figure getting into a car, say).
    std::function<void(int id)> removeCharacter;
    // Scene vehicles from a script: a figure getting into a car and out again.
    // spawnVehicle puts the object's Vehicle into the physics world without
    // driving it (parked, handbrake on) -- true when it is there now; there is
    // one physics car per world, so asking for a second one is false.
    // driveVehicle does the same and hands the player's controls to it (what V
    // does with the nearest one); leaveVehicle takes them back and leaves the
    // car braking where it is. drivenVehicle: the object being driven (-1 none)
    // with its speed (m/s, + forward), steering (-1 left .. 1 right) and
    // throttle (-1 .. 1), for a steering wheel and pedals that move.
    struct DrivenVehicle {
        int   id       = -1;
        float speed    = 0.0f;
        float steer    = 0.0f;
        float throttle = 0.0f;
    };
    std::function<bool(int id)>      spawnVehicle;
    std::function<bool(int id)>      driveVehicle;
    std::function<void()>            leaveVehicle;
    std::function<DrivenVehicle()>   drivenVehicle;
    // A ray through the physics world and the terrain as it is DRAWN (the
    // terrain's own collider is coarse): the first thing it meets within
    // `maxDist`. False on a miss.
    std::function<bool(glm::vec3 origin, glm::vec3 dir, float maxDist,
                       ScriptRayHit& out)> castRay;
    // Frame the orbit camera for this frame (CameraSystem::frameOrbit).
    std::function<void(float weight, float dist, float side, float up, float fov)> orbitFrame;
    // A hand of figure `id` (0 left, 1 right) -- or a foot (2 left, 3 right) --
    // to a point in the world, this frame, `weight` of the way from where the
    // animation has it (game.reach; LimbIK.hpp).
    std::function<void(int id, int side, glm::vec3 target, float weight)> reach;
    // An image where something hit (game.decal): at `pos` on a surface with
    // normal `normal`, `size` across, in library material `material` ("" = the
    // engine's bullet hole), turned `spin` degrees. False when nothing that
    // stands still is there (Decals.hpp).
    std::function<bool(glm::vec3 pos, glm::vec3 normal, float size, const std::string& material,
                       float spin)> decal;
    // Glass broken where a shot struck it (game.shatter; Shatter.hpp): object
    // `id` (-1: whatever is there, as castRay reports the world) at `pos`, the
    // shot going `dir`, `strength` 1 for a bullet. False when there is no glass.
    std::function<bool(int id, glm::vec3 pos, glm::vec3 dir, float strength)> shatter;
    // Replay an object's Particle burst where it stands now (an impact, a flash).
    std::function<void(int id)> emit;
    // What a thing let go of at `from` comes to rest on, straight down within
    // `maxDist`: a road, a bridge deck, a floor -- or the terrain as it is
    // DRAWN (never below it). False when nothing is there.
    std::function<bool(glm::vec3 from, float maxDist, float& outY)> groundHeight;
    // Load another scene of the open project by name (deferred to frame end).
    std::function<void(const std::string&)> loadScene;
    // Play again from how the scene stood when Play began (deferred): the
    // overlay's Restart. Unsaved editor edits survive it, the file is not read.
    std::function<void()> restart;
    // Whose saves game.saveData / game.loadData keep (SaveData.hpp): the game
    // being played, set by the host when Play starts. Empty = "default".
    std::string saveGame;
    // Where `require` finds a script's modules: the folder its scripts live in
    // (the project's scripts/, or the bundled one). Set when Play starts.
    std::string scriptsDir;
    // Print a line to the console / editor log.
    std::function<void(const std::string&)> log;

    // --- Audio ----------------------------------------------------------------
    // Play a one-shot sound file from the project's sounds/ folder.
    std::function<void(const std::string&)> playSound;
    // game.sound: a one-shot at a volume and pitch, and -- with `pos` -- heard
    // from a place in the world, full within nearM, silent past farM.
    std::function<void(const std::string&, float volume, float pitch, const glm::vec3* pos,
                       float nearM, float farM)> playSoundEx;

    // Start / stop an entity's AudioSource component by id (game.playAudio /
    // game.stopAudio). No-ops on ids without an AudioSource.
    std::function<void(int)> playAudio;
    std::function<void(int)> stopAudio;
    // The Synth components' players, for the `synth` table (synth.noteOn,
    // synth.playMidi, ...). Null = every synth.* call is a no-op returning false.
    SynthSystem* synths = nullptr;
    // The game's song with its clock and filter, for the `music` table
    // (music.play, music.time, music.analyze, ...). Null = every call a no-op.
    MusicSystem* music = nullptr;

    // --- Shared game state ----------------------------------------------------
    // Lua script environments are per-entity (isolated), so shared state like the
    // score lives here and is reached via game.addScore / game.getScore /
    // game.setHud.
    int         score = 0;
    std::string hud;

    // --- Script-drawn HUD -----------------------------------------------------
    // Cleared by the host before the scripts tick each frame, drawn after the
    // frame is rendered, so what a script queued is exactly one frame's HUD.
    std::vector<ScriptHudCmd> hudCmds;
    // game.setCrosshair: a game that draws its own HUD has no use for the
    // editor's aiming cross. Reset to shown whenever Play starts.
    bool crosshair = true;
    // game.rest: nothing in this game moves on its own this frame, so the next
    // one may wait for input -- at most `restFps` frames a second (0 = run
    // free). Asked for anew every frame; the main loop takes it and clears it,
    // so a script that stops asking (or stops running) is back at full rate.
    float restFps = 0.0f;
    // Width/height in HUD units of a line of text at `size` (see ScriptHudCmd).
    std::function<glm::vec2(const std::string&, float size, bool bold)> measureText;
    // A Texture asset (file name or GUID) loaded for the HUD (see ScriptImage).
    std::function<bool(const std::string&, ScriptImage&)> hudImage;
    // Where a world point lands on the HUD canvas; false when it is behind the eye.
    std::function<bool(glm::vec3, glm::vec2&)> worldToHud;
};
