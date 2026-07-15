#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <random>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <fitzel/graphics/InstancedMesh.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/graphics/Texture.hpp>
#include <fitzel/world/Terrain.hpp>

#include "FrameRender.hpp" // FrameContext -- the per-frame lighting/fog the draws take

namespace fitzel { class Camera; }

// Owns the scene's vegetation/wildlife and its GPU resources. Migrated out of
// main.cpp one subsystem at a time; currently grass, birds and fireflies. Draw
// methods render into the caller's currently-bound framebuffer.
class VegetationSystem {
public:
    VegetationSystem(fitzel::TerrainStreamer& streamer, fitzel::Camera& camera);
    ~VegetationSystem(); // frees the tree GL buffers

    // Load grass/bird/firefly shaders + build instanced meshes. Needs a live GL
    // context; returns false if a shader failed to compile.
    bool init();
    // Load the tree model + build its instanced/billboard GL buffers. Separate so
    // the caller can supply the content directories. Returns false on shader fail.
    bool initTrees(const std::string& modelDir, const std::string& texDir);
    // Load the flower shader + instanced mesh. Returns false on shader fail.
    bool initFlowers();

    // --- Grass ---------------------------------------------------------------
    // Hand-painted blades: stamp scatters into the brush disc (dropped on the
    // terrain, rejecting steep/underwater/above-snow spots), erase removes those
    // in the disc. `rng` is shared with the other brushes (kept in main).
    void stampGrass(glm::vec2 c, float radius, std::mt19937& rng, float brushDensity,
                    float waterLevel, float snowLevel);
    void eraseGrass(glm::vec2 c, float radius);
    // Async pump: kick off / finish the procedural field regen around camXZ and
    // push any painted-blade edits. Returns true the frame the procedural field
    // just (re)generated -- grassCenter() then holds the center it used, so the
    // caller can regrow flowers to match.
    bool updateGrass(glm::vec2 camXZ, const std::vector<glm::vec2>& road,
                     float roadClear, float waterLevel, float snowLevel);
    void drawGrass(const FrameContext& ctx);
    glm::vec2 grassCenter() const { return m_grassCenter; }

    // --- Trees ---------------------------------------------------------------
    // Regrow the procedural forest when the camera has strayed far enough from
    // its last center (painted trees always ride along). Paint/erase place or
    // remove hand-placed trees (world space). Draw methods cover the shadow pass,
    // the lit 3D pass and the distant-billboard LOD pass.
    void updateTrees(glm::vec2 camXZ, const std::vector<glm::vec2>& road,
                     float roadWidth, float waterLevel, float snowLevel);
    void stampTree(glm::vec2 c, float radius, std::mt19937& rng,
                   float waterLevel, float snowLevel);
    void eraseTree(glm::vec2 c, float radius);
    void clearPaintedTrees() { paintedTrees.clear(); rebuildTreeBuffers(); }
    void drawTreeShadow(const glm::mat4& lightSpace, double time, float weather);
    void drawTrees(const FrameContext& ctx);
    void drawTreeBillboards(const FrameContext& ctx, const glm::vec3& camRight);
    // Tree positions (5 floats/tree: pos3, yaw, scale) so flowers can cluster.
    const std::vector<float>& treeInstances() const { return m_treeInst; }

    // One detail level of a tree species: a mesh file dropped over the terrain,
    // shown only within [prev.dist, dist) of the camera (the last mesh LOD runs
    // out to the billboard start / far plane). LOD0 is the highest detail.
    struct TreeLOD {
        std::string   model;         // .glb filename (relative to the model dir)
        float         dist = 40.0f;  // switch to the next level beyond this (m)
        struct Prim { fitzel::Texture tex; bool hasTex = false;
                      int first = 0, count = 0; bool cutout = false; };
        std::vector<Prim> prims;     // per-material draw groups
        std::uint32_t vao = 0, vbo = 0;
    };
    // A configurable tree type: an ordered LOD chain + a far billboard, its own
    // density/size and its own GPU instance buffer (5 floats/tree: pos3,yaw,scale).
    struct TreeSpecies {
        std::string          name = "Tree";
        std::vector<TreeLOD> lods;               // LOD0..n, ascending dist
        std::string          billboard;          // PNG name ("" = none)
        fitzel::Texture      bbTex;
        bool  bbEnabled = true;
        float bbStart   = 60.0f;                 // distance the billboard takes over
        float bbAspect  = 0.93f;
        float bbSize    = 1.05f;                 // billboard height factor vs mesh
        std::uint32_t bbVAO = 0;
        bool  enabled = true;
        float density = 1.0f;                    // per-species distribution weight
        float size    = 9.0f;                    // average height (m)
        std::uint32_t      instVBO = 0;
        std::vector<float> inst;                 // procedural prefix + painted (this species)
        std::size_t        proceduralFloats = 0;
        int                count = 0;
    };

    // Species/LOD editing (used by panelTrees). Each mutates GPU state and, where
    // it changes placement, expects the caller to force a regrow (treeCenter reset).
    int  addSpecies();                           // returns the new species index
    void removeSpecies(int s);
    void addLOD(int s);
    void removeLOD(int s, int lod);
    void setLODModel(int s, int lod, const std::string& file);
    void setBillboard(int s, const std::string& file);
    // The whole Trees + Paint-trees editor panel (keeps main.cpp small). onGrabLMB
    // switches the sibling viewport brushes off when tree paint mode is enabled.
    void panelTrees(bool& treePaintMode, bool& brushErase,
                    const std::function<void()>& onGrabLMB);
    // Scene persistence for the species config (main threads these into its own
    // settings JSON block). Painted trees are saved separately by main.
    void serializeTrees(nlohmann::json& j) const;
    void deserializeTrees(const nlohmann::json& j);

    // --- Flowers -------------------------------------------------------------
    // Procedural blooms regrow with the grass pass (clustering in moist ground
    // and around tree trunks); paint/erase place hand-flowers (world space).
    void regenFlowers(glm::vec2 c, const std::vector<glm::vec2>& road, float roadWidth,
                      float waterLevel, float snowLevel);
    void stampFlower(glm::vec2 c, float radius, std::mt19937& rng,
                     float waterLevel, float snowLevel);
    void eraseFlower(glm::vec2 c, float radius);
    void clearPaintedFlowers() { paintedFlowers.clear(); rebuildFlowerBuffer(); }
    void drawFlowers(const FrameContext& ctx);

    // --- Birds + fireflies ---------------------------------------------------
    void drawBirds(const glm::mat4& viewProj, double time, const glm::vec3& camPos);
    void drawFireflies(const glm::mat4& viewProj, double time, float night,
                       const glm::vec3& camPos);
    void panelBirdsFireflies();

    // Tunables the editor panel / scene setup drive directly.
    bool  grassEnabled = true;
    float grassHeight  = 0.35f;
    float grassDensity = 1.0f;
    float grassChaos   = 1.0f;  // 0 = even lawn, 1 = wild meadow, >1 = unruly
    float grassRadius  = 46.0f;
    glm::vec3 grassTint{1.0f, 1.0f, 1.0f};
    int   grassCount   = 0;
    bool  grassDirty   = true;         // request a (re)grow of the procedural field
    std::vector<float> paintedBlades;  // 7 floats/blade, world space (saved)
    bool  paintedDirty = false;        // GPU re-upload of painted blades pending

    bool      treeEnabled = true;
    // Independent tree sources: the procedural forest and the hand-painted trees.
    // Either may run alone (only painted / only generated) or both together.
    bool      treeProcedural = true;   // generate the procedural forest
    bool      treePainted    = true;   // include hand-painted trees
    glm::vec2 treeCenter{1e9f};        // last procedural-forest center; 1e9 forces regen
    int       treeCount   = 0;         // total drawn across all species (proc + painted)
    std::vector<float> paintedTrees;   // 6 floats/tree: pos3, yaw, scale, speciesIdx (saved)
    int       paintSpecies    = 0;     // species the tree brush plants
    float     treeBrushRadius  = 8.0f;
    float     treeBrushDensity = 1.0f; // attempts per m^2 (trees stay sparse)
    float     treeMinSpacing   = 4.0f; // reject placements closer than this

    bool  flowerEnabled = true;
    float flowerDensity = 1.0f;
    int   flowerCount   = 0;
    std::vector<float> paintedFlowers;    // 8 floats/flower, world space (saved)
    float flowerBrushRadius  = 6.0f;
    float flowerBrushDensity = 1.0f;

    bool  birdsEnabled   = true;
    bool  fireflyEnabled = true;

private:
    static std::vector<float> computeGrass(
        fitzel::TerrainSettings s, glm::vec2 c, float waterLvl, float snowLvl,
        float gHeight, float gDensity, float R, std::uint32_t seed,
        std::vector<glm::vec2> road, float roadClear, float chaos);

    void regenTrees(glm::vec2 cc, const std::vector<glm::vec2>& road, float roadWidth,
                    float waterLevel, float snowLevel);
    void rebuildTreeBuffers();  // re-upload every species (procedural prefix + painted)
    void rebuildFlowerBuffer(); // re-upload procedural prefix + painted flowers
    // Load a .glb into `lod` (fills prims + creates its VAO/VBO bound to the
    // species' instance buffer). Returns false if the model failed to load.
    bool loadTreeMesh(const std::string& path, TreeSpecies& sp, TreeLOD& lod);
    void scanTreeAssets(); // populate m_modelFiles / m_texFiles from the content dirs

    fitzel::TerrainStreamer& m_streamer;
    fitzel::Camera&          m_camera;

    // Grass.
    fitzel::Shader        m_grass;
    fitzel::InstancedMesh m_grassField, m_paintedGrass;
    glm::vec2                       m_grassCenter{1e9f}; // forces the first regen
    std::future<std::vector<float>> m_grassFuture;
    bool          m_grassPending = false;
    glm::vec2     m_grassPendingCenter{0.0f};
    std::uint32_t m_grassSeed = 1234u;

    // Trees. Shaders are shared across all species (bound once, uniforms per draw).
    fitzel::Shader           m_tree, m_treeDepth, m_billboard;
    std::vector<TreeSpecies> m_species;
    std::vector<std::string> m_modelFiles;  // *.glb in the model dir (panel dropdown)
    std::vector<std::string> m_texFiles;    // *.png in the texture dir (billboard dropdown)
    std::string              m_modelDir, m_texDir;
    const float              m_treeRadius = 120.0f;
    std::mt19937             m_trng{555u};
    // Combined positions of every species (5 floats/tree) -- feeds flower
    // clustering around trunks and the treeInstances() accessor.
    std::vector<float>       m_treeInst;

    // Flowers.
    fitzel::Shader        m_flower;
    fitzel::InstancedMesh m_flowerField;
    int                   m_flowerVerts = 0;
    std::vector<float>    m_flowerInst;
    std::size_t           m_proceduralFlowerFloats = 0;

    // Birds.
    fitzel::Shader        m_bird;
    fitzel::InstancedMesh m_birdField;
    int   m_birdCount = 18;
    float m_birdSize  = 2.2f;

    // Fireflies.
    fitzel::Shader        m_firefly;
    fitzel::InstancedMesh m_fireflyField;
    int         m_fireflyCount  = 70;
    float       m_fireflySize   = 0.09f;
    const float m_fireflyRadius = 34.0f;
    std::mt19937                          m_flyRng{9001u};
    std::uniform_real_distribution<float> m_flyU{0.0f, 1.0f};
    std::vector<glm::vec3>                m_fireflies; // home xz + blink phase
};
