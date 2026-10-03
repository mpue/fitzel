#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <fitzel/asset/AssetId.hpp>
#include <fitzel/graphics/Mesh.hpp>

#include "SceneTypes.hpp"   // Entity, MaterialDef, LoadedModel

class ModelLibrary;
class Document;
namespace fitzel {
class Material;
class Renderer;
}

// --- Glass that breaks -----------------------------------------------------------
// A window pane in the old factory, a glass door, a shop front: shot (game.shatter,
// which weapon.lua calls on every hit), the pane cracks from where it was struck
// into shards -- small ones at the hole, larger towards the frame -- that fly on
// with the shot, tumble, fall and stay lying where they land until Play ends. The
// pane is gone: drawn no more, and a shot after it goes through the hole.
//
// What counts as glass is the material: one with Glass ticked, or any that is see-
// through (opacity under 1) and not cut out. The pane is found in the triangles,
// not authored: from the one the shot struck, through those it shares corners
// with in the same plane, and whatever lies on that outline just behind it -- the
// back of a two-sided pane, the thin edges of a slab. So a window of twenty panes
// in one imported model loses exactly the one that was hit, and a thin glass
// object (a Plane, a flat Box) breaks as a whole.
//
// An imported model is shared by every copy of it, so nothing is taken from the
// model itself: the broken copy draws its glass through an index list of what is
// left (Mesh::createView over the model's own vertices), and the others draw as
// before. The model's collider stays as it was -- a shot passes a broken pane
// because castRay asks gone() and looks past it, not because the triangles left
// the physics world. Like every runtime change in Play, all of it is forgotten
// when Play stops.
namespace shatter {

// Is a surface of this material glass that a shot breaks?
bool breakable(const MaterialDef& md);

// A flat pane of glass in the world: its plane (`origin` in the middle of its
// thickness, `u` and `v` across it, `n` the struck face's normal), how thick it
// is, and its outline as triangles in (u, v) metres from `origin`.
struct Pane {
    glm::vec3 origin{0.0f};
    glm::vec3 u{1.0f, 0.0f, 0.0f}, v{0.0f, 1.0f, 0.0f}, n{0.0f, 0.0f, 1.0f};
    float     thickness = 0.004f;
    std::vector<glm::vec2> outline;   // three per triangle, counter-clockwise
    // The texture coordinates across it: uv = uv0 + x * uvU + y * uvV.
    glm::vec2 uv0{0.0f}, uvU{1.0f, 0.0f}, uvV{0.0f, 1.0f};

    glm::vec2 flat(const glm::vec3& p) const { return {glm::dot(p - origin, u), glm::dot(p - origin, v)}; }
    glm::vec3 world(const glm::vec2& p) const { return origin + u * p.x + v * p.y; }
    glm::vec2 uvAt(const glm::vec2& p) const { return uv0 + uvU * p.x + uvV * p.y; }
    // Does `p` lie in the pane (within `slack` metres of it)?
    bool contains(const glm::vec3& p, float slack) const;
    float area() const;
};

// The pane struck at `hit` among `tris` (world space, three corners each; `uvs`
// alongside them, or empty) -- those for which `skip` is set left out. False when
// no triangle lies within `reach` of the hit. `taken` gets the triangle numbers of
// all of it: the face, its back, its edges.
bool paneAt(const std::vector<glm::vec3>& tris, const std::vector<glm::vec2>& uvs,
            const std::vector<bool>& skip, const glm::vec3& hit, float reach, Pane& pane,
            std::vector<std::uint32_t>& taken);

// A pane broken from (u, v) point `at`: cracks running out from it, rings round
// it, each cell cut to the pane's outline. A piece is one or more polygons
// (counter-clockwise, (u, v)) that go off together -- one cell over two of the
// outline's triangles. `seed` makes it repeatable.
using Piece = std::vector<std::vector<glm::vec2>>;
std::vector<Piece> crack(const Pane& pane, const glm::vec2& at, std::uint32_t seed);

// One shard: its shape round its middle (as it lay in the pane), where its middle
// is, how it is turned from how it lay, how it moves.
struct Shard {
    std::vector<fitzel::Vertex> shape;   // triangles, three vertices each, around `pos`
    glm::vec3 pos{0.0f};
    glm::quat q{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 vel{0.0f};
    glm::vec3 spin{0.0f};               // world, rad/s
    glm::vec3 face{0.0f, 0.0f, 1.0f};   // the pane's normal as it lay (to lie flat on)
    float     wait = 0.0f;      // seconds it still hangs in the frame before it goes
    bool      settling = false; // on the ground, turning flat
    bool      resting = false;  // lying still for good
    bool      lost = false;     // fell out of the world: dropped
    bool      hasFloor = false;
    float     floorY = -1.0e30f;
    glm::vec3 floorAt{1.0e30f}; // where the floor under it was last asked for
};

// The pieces as solid shards (the pane's thickness), sent off by a shot along
// `dir` that struck at (u, v) `at`: fast and tumbling near the hole, the outer
// ones barely pushed, some of them hanging a moment before they drop.
std::vector<Shard> makeShards(const Pane& pane, const std::vector<Piece>& pieces, const glm::vec2& at,
                              const glm::vec3& dir, float strength, std::uint32_t seed);

// What lies under `from` (world): its height, or false when nothing does.
using FloorFn = std::function<bool(const glm::vec3& from, float& y)>;
// One step of a falling shard: gravity, a little air, a bounce or two, and on
// the ground it turns flat and lies still.
void step(Shard& s, float dt, const FloorFn& floor);
// Its lowest point now.
float lowest(const Shard& s);

class System {
public:
    System();
    ~System();
    System(const System&)            = delete;
    System& operator=(const System&) = delete;

    // Break the glass of object `id` at `hit` (world), struck along `dir`. True
    // when there was a pane there and it is now shards. `id` -1 is what castRay
    // calls the world -- the parts of a hall whose collider its group root made
    // -- and then whatever object has glass at that point breaks. `vanished` gets
    // the object that went as a whole (a glass Plane or slab), else -1.
    bool breakAt(const std::vector<Entity>& entities, ModelLibrary& models,
                 const std::vector<MaterialDef>& materials, const Document& document, int id,
                 const glm::vec3& hit, const glm::vec3& dir, float strength, int& vanished);
    // Is `p` in a broken pane of object `id` (or on an object that went)? With
    // `id` -1: in any broken pane at all.
    bool gone(int id, const glm::vec3& p) const;
    // What is left of model object `id`'s primitive `prim`: null when it is whole;
    // a mesh with nothing in it when none of it is.
    const fitzel::Mesh* leftOf(int id, std::size_t prim) const;
    // Has object `id` broken as a whole?
    bool vanished(int id) const { return m_vanished.count(id) != 0; }

    // Once a frame in Play: the shards fly on, their meshes follow.
    void update(float dt, const FloorFn& floor);
    void submit(fitzel::Renderer& renderer, const std::vector<fitzel::Material>& gpuMats,
                const std::vector<MaterialDef>& materials, const Document& document) const;
    void clear();
    std::size_t shardCount() const;

    // Shards kept at most: the oldest panes' go first.
    static constexpr std::size_t kMaxShards = 6000;

private:
    struct Fall {
        fitzel::AssetId    material;
        std::vector<Shard> shards;
        fitzel::Mesh       mesh;
        bool               moving = true;
    };
    struct Left {
        std::vector<bool> gone;   // per triangle of the primitive
        fitzel::Mesh      mesh;   // a view of what is left
    };
    void addFall(const fitzel::AssetId& material, const Pane& pane, const glm::vec3& hit,
                 const glm::vec3& dir, float strength);
    static void build(Fall& f);

    std::vector<std::unique_ptr<Fall>>                    m_falls;
    std::map<std::pair<int, std::size_t>, Left>           m_left;    // (object, primitive)
    std::unordered_map<int, std::vector<Pane>>            m_holes;   // broken panes, by object
    std::unordered_set<int>                               m_vanished;
    // The model primitives' vertices, read back once when one first breaks.
    std::map<std::pair<int, std::size_t>, std::vector<fitzel::Vertex>> m_cpu;
    std::uint32_t m_seed = 0x9e3779b9u;
};

} // namespace shatter
