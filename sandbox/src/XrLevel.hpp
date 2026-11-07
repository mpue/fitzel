#pragma once

// A compiled X-Ray level (S.T.A.L.K.E.R.), read into plain world-space meshes:
// `level` (shaders, visuals, lights, glows) with its `level.geom` (the vertex
// and index buffers the visuals point into) and the object list in
// `level.spawn`. The format is xrLC's version 14, which Clear Sky, Call of
// Pripyat and the Enhanced Editions of all three games write.
//
// What is read of the visuals: every leaf -- a static mesh (Fvisual), a
// progressive one at its full detail, and a tree/MU instance with its own
// transform baked in. Hierarchies and LOD groups only gather leaves; the LOD
// billboards themselves are not geometry and are skipped.
//
// Everything comes out in fitzel's axes: X-Ray's left-handed frame with Z
// mirrored (x, y, -z), triangles wound counter-clockwise seen from the front.

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace xray {

class FileSystem;

struct LevelShader {
    std::string engine;                 // "default", "def_shaders\def_aref", "flora\leaf_wave"
    std::string texture;                // "mtl\mtl_bochka_01" (no extension)
    std::vector<std::string> more;      // what follows the comma: lightmaps, a second map
};

struct LevelVertex {
    glm::vec3 pos{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};                 // base map, glTF orientation (V down the image)
    glm::vec2 uvLight{0.0f};            // the lightmap's coordinates, where there is one
    float     hemi = 1.0f;              // xrLC's sky visibility at the vertex (0..1)
};

struct LevelMesh {
    enum class Kind { Static, Progressive, Tree };
    Kind kind = Kind::Static;
    int  shader = 0;
    std::vector<LevelVertex>   verts;
    std::vector<std::uint32_t> indices;   // triangle list into verts
    glm::vec3 lo{1e30f}, hi{-1e30f};
};

// D3DLIGHT9 as the level stores it, in fitzel's axes.
struct LevelLight {
    int       type = 1;                 // 1 point, 2 spot, 3 directional
    glm::vec3 color{1.0f};              // diffuse, linear, may exceed 1
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    float     range = 10.0f;
    float     falloff = 1.0f;
    float     attenuation[3] = {1.0f, 0.0f, 0.0f};
    float     theta = 0.0f, phi = 0.0f; // spot cone, inner and outer, radians
    std::uint32_t controller = 0;       // the light animation (flicker) it follows
};

struct LevelGlow {
    glm::vec3 position{0.0f};
    float     radius = 1.0f;
    int       shader = 0;
};

// One object of level.spawn: what it is, where, and the model it shows (for
// the classes that have one: props, lamps, stashes, creatures).
struct LevelSpawn {
    std::string section;                // "actor", "physic_object", "stalker", ...
    std::string name;
    std::string visual;                 // "physics\decor\ventilator_01"; "" for none
    glm::vec3   position{0.0f};
    glm::vec3   angle{0.0f};            // X-Ray's Euler angles (radians), as stored
    glm::vec3   gamePosition{0.0f};     // the position in X-Ray's own axes
    // The object's state as level.spawn has it, for readState: what follows
    // depends on the object's class, which only the game's configs say.
    std::uint16_t version = 0;
    std::vector<std::uint8_t> state;
};

// How an object's state is laid out, by the class of its section (the
// `class` key of its config section).
enum class StateLayout {
    Object,        // cse_alife_object, then a model for those that have one
    Human,         // a stalker: the trader part first, then the creature
    Trader,        // a trader: the creature, then the trader part
    Restrictor,    // cse_alife_object, a shape, a restrictor type
    LevelChanger,  // a restrictor, then where it leads
    Zone,          // a restrictor, then the anomaly's power
};

// A zone's shape, in the object's own frame (X-Ray's axes): a sphere, or a box
// that is the unit cube [-0.5, 0.5] through `axes` and `offset`.
struct SpawnShape {
    bool      box = false;
    glm::vec3 center{0.0f};
    float     radius = 0.0f;
    glm::mat3 axes{1.0f};               // columns: the box's i, j, k (full lengths)
    glm::vec3 offset{0.0f};
};

struct SpawnState {
    std::string   custom;               // the custom data: ltx ([logic], [smart_terrains], ...)
    std::uint32_t story = 0xffffffffu;
    std::string   visual;
    std::string   profile;              // a stalker's or trader's character profile
    std::vector<SpawnShape> shapes;
    int           restrictorType = 0;
    std::string   destLevel, destPoint; // a level changer: where to
    glm::vec3     destPosition{0.0f};   // ...in the other level's X-Ray axes
    glm::vec3     destDirection{0.0f};
    float         power = 0.0f;         // a zone's
};

bool readState(const LevelSpawn& spawn, StateLayout layout, SpawnState& out);

// A patrol path of level.game: the routes and look points the game's logic
// sends its people along. A point's name carries flags of the scripts
// ("wp01|a=guard|t=20000"); `flags` are the editor's bits.
struct PatrolPoint {
    std::string   name;
    std::uint32_t flags = 0;
    glm::vec3     position{0.0f};       // fitzel's axes
};
struct PatrolPath {
    std::string name;
    std::vector<PatrolPoint> points;
    struct Link { int from, to; float probability; };
    std::vector<Link> links;
};

// level.snd_static: a sound the level plays at a place, in a loop or now and
// then (play and pause times in ms, random between the two; an active window
// in game time, both 0 for always).
struct StaticSound {
    std::string name;                   // "ambient\\sparks1" (sounds/<name>.ogg)
    glm::vec3   position{0.0f};
    float       volume = 1.0f;
    float       frequency = 1.0f;
    std::uint32_t activeTime[2] = {0, 0};
    std::uint32_t playTime[2] = {0, 0};
    std::uint32_t pauseTime[2] = {0, 0};
};

// One textured piece of an OGF model (a prop, a lamp, a vehicle wreck), in
// the model's own space and X-Ray's axes -- an instance's transform still has
// to be applied before the mirror -- and its triangles as D3D winds them.
// Skinned models come in their bind pose.
struct ModelPart {
    std::string texture;                // "mtl\mtl_barrel_05"
    std::string shader;                 // "models\model", "models\window"
    std::vector<LevelVertex>   verts;
    std::vector<std::uint32_t> indices;
};

bool readModel(const FileSystem& fs, const std::string& path, std::vector<ModelPart>& out,
               std::string& error);

// X-Ray's object transform from a spawn's angles (Fmatrix::setXYZ: heading
// about Y, pitch about X, bank about Z), as rows i, j, k: a point p of the
// model lands at p.x*i + p.y*j + p.z*k + position, in X-Ray's axes.
glm::mat3 objectRotation(const glm::vec3& angle);

struct Level {
    std::string name;
    int version = 0;
    std::vector<LevelShader> shaders;
    std::vector<LevelMesh>   meshes;
    std::vector<LevelLight>  lights;
    std::vector<LevelGlow>   glows;
    std::vector<LevelSpawn>  spawns;
    std::vector<PatrolPath>  paths;
    std::vector<StaticSound> sounds;
    int skippedVisuals = 0;             // leaves that pointed outside the buffers
};

bool readLevel(const FileSystem& fs, const std::string& name, Level& out, std::string& error);

// Only the objects of a level (where its level changers lead: their graph
// points), without reading its geometry.
bool readSpawns(const FileSystem& fs, const std::string& level, std::vector<LevelSpawn>& out);

} // namespace xray
