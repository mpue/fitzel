#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <fitzel/graphics/InstancedMesh.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/graphics/Texture.hpp>
#include <fitzel/world/Terrain.hpp>

#include "FrameRender.hpp"  // FrameContext -- the per-frame lighting/fog the draws take
#include "Ecology.hpp"     // where the forest is
#include "GrassTrace.hpp"  // the field's definition, shared with the path tracer
#include "TiledScatter.hpp" // per-tile streamed instance buffers (grass field)
#include "TreeField.hpp"   // the procedural forest, streamed to the horizon
#include "Wind.hpp"        // the air everything sways in

namespace fitzel { class Camera; }

// Owns the scene's vegetation/wildlife and its GPU resources. Migrated out of
// main.cpp one subsystem at a time; currently grass, birds and fireflies. Draw
// methods render into the caller's currently-bound framebuffer.
class VegetationSystem {
public:
    // Ground nothing may grow on: discs (xy centre, z radius) covering the
    // wetted channels, handed over by the host from RiverSystem::wetDiscs.
    //
    // A member rather than a parameter on all six placement calls, and set when
    // the WATER is edited rather than per frame: it is a property of the world,
    // the same way the water line and the snow line are. The tile workers take a
    // copy of it with everything else, so it must stay small and flat -- which is
    // exactly why it is a list of discs and not a pointer to the river system.
    //
    // The scalar water line above cannot do this job. It is one height for the
    // whole world, and a brook is above it, not below it: the ground a channel
    // was cut into is still well clear of the sea, so every filter that asks
    // "am I above the water" says yes in the middle of a stream.
    std::vector<glm::vec3> wet;

    VegetationSystem(fitzel::TerrainStreamer& streamer, fitzel::Camera& camera);
    ~VegetationSystem(); // frees the tree GL buffers

    // Load grass/bird/firefly shaders + build instanced meshes. Needs a live GL
    // context; returns false if a shader failed to compile.
    bool init();
    // Load the tree model + build its instanced/billboard GL buffers. Separate so
    // the caller can supply the content directories. Returns false on shader fail.
    bool initTrees(const std::string& modelDir, const std::string& texDir);
    // Rebuild the selectable tree-mesh + billboard lists from the built-in content
    // dirs plus the open project folder (`projectDir`, scanned recursively; pass ""
    // for none), and reload the species' meshes/billboards from the new lists. Call
    // it whenever the project changes so models dropped into a project show up in
    // the tree editor. Assets are referenced by file name, so selections survive.
    void refreshTreeAssets(const std::string& projectDir);
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
    // `maxDist` is how far from camXZ a tree may stand and still be drawn into
    // THIS cascade (0 = no limit). The caller resolves it, because only it knows
    // which cascade this is -- see Renderer::ShadowCaster.
    void drawTreeShadow(const glm::mat4& lightSpace, double time, float weather,
                        glm::vec2 camXZ, float maxDist);
    void drawTrees(const FrameContext& ctx);
    void drawTreeBillboards(const FrameContext& ctx, const glm::vec3& camRight);
    // Motion vectors for the swaying meshes, into the target PostChain::
    // beginMotion bound (TAA would otherwise smear every crown in the wind).
    // `viewProj` is the jittered matrix the lit pass drew with.
    void drawTreeMotion(const glm::mat4& viewProj, const glm::mat4& curVP,
                        const glm::mat4& prevVP, const glm::vec3& camPos);
    // Tree positions (5 floats/tree: pos3, yaw, scale) so flowers can cluster.
    const std::vector<float>& treeInstances() const { return m_treeInst; }

    // Tree instances actually submitted last frame, summed over every pass (the
    // main view, the water reflection and each shadow cascade). Against
    // treeCount -- which counts each tree once -- this says how many times over
    // the forest is being drawn, which is the number that decides the frame.
    int drawnInstances() const { return m_drawnLast; }

    // The same count, and the triangles behind it, for the SHADOW cascades
    // alone. Split out because that is where a forest is drawn most often (once
    // per cascade) and least visibly -- a number nobody looks at until the frame
    // time says the cascade pass is the whole frame.
    int       shadowInstances() const { return m_shadowInstLast; }
    long long shadowTriangles() const { return m_shadowTrisLast; }

    // Roll the per-frame counters. Call once at the top of a frame, before the
    // shadow cascades -- they are the first pass that draws trees, and they are
    // the reason the count is worth having.
    void beginFrame() {
        m_drawnLast = m_drawnInstances; m_drawnInstances = 0;
        m_shadowInstLast = m_shadowInst; m_shadowInst = 0;
        m_shadowTrisLast = m_shadowTris; m_shadowTris = 0;
    }

    // One detail level of a tree species: a mesh file dropped over the terrain,
    // shown only within [prev.dist, dist) of the camera (the last mesh LOD runs
    // out to the billboard start / far plane). LOD0 is the highest detail.
    struct TreeLOD {
        std::string   model;         // .glb filename (relative to the model dir)
        float         dist = 40.0f;  // switch to the next level beyond this (m)
        // `first`/`count` are an offset and a length in the LOD's INDEX buffer,
        // not the vertex buffer: a tree arrives from glTF as a flat triangle
        // soup, and drawn that way every triangle pays three fresh vertex
        // shader runs for corners it shares with its neighbours. At a hundred
        // thousand triangles an instance, redrawn into every shadow cascade,
        // that redundancy IS the frame -- so the mesh is welded back into an
        // indexed one on load (see loadTreeMesh).
        // One material group of the mesh. `tex`/`hasTex`/`modelCutout`/
        // `baseColor`/`name` are what the model shipped; `cutout`, `tint`,
        // `cutoff` and `swapTex` are what is drawn -- the species' PartMat
        // applied over the model (applyPartMats). Every pass reads these, so
        // an edit reaches the shadows, the motion vectors and the impostor too.
        struct Prim {
            fitzel::Texture tex; bool hasTex = false;
            int first = 0, count = 0;
            bool cutout = false, modelCutout = false;
            glm::vec3 baseColor{1.0f};             // glTF factor, sRGB (untextured parts)
            std::string name;                      // glTF material name
            glm::vec3 tint{1.0f};                  // sRGB multiplier (tint * brightness)
            float     cutoff = 0.5f;               // alpha discard threshold
            std::string swapName;                  // texture file drawn instead ("" = model's)
            std::shared_ptr<fitzel::Texture> swapTex;
            bool textured() const { return (swapTex && swapTex->isValid()) || hasTex; }
            void bindTex() const {
                if (swapTex && swapTex->isValid()) swapTex->bind(0);
                else if (hasTex) tex.bind(0);
            }
        };
        std::vector<Prim> prims;     // per-material draw groups
        std::uint32_t vao = 0, vbo = 0, ibo = 0;
        // Bounding-sphere radius of the loaded mesh in UNIT-HEIGHT units, about
        // the point half a height up -- so an instance's world sphere is
        // (pos + (0, 0.5*scale, 0), boundR * scale). Measured on load rather
        // than assumed, because a bush is wider than it is tall and a poplar is
        // the other way round, and a guessed radius either clips crowns at the
        // screen edge or gives the culling nothing to reject.
        float boundR = 0.75f;
        // The welded mesh as uploaded (8 floats a vertex), kept so the
        // generated levels below can be derived from it.
        std::vector<float>         cpuVerts;
        std::vector<std::uint32_t> cpuIdx;
    };
    // A level the engine made itself from the coarsest authored one (see
    // MeshSimplify.hpp): bark collapsed by quadric error, leaf cards thinned
    // and grown. Draw ranges point back at that level's prims for their
    // textures.
    struct AutoLod {
        std::uint32_t vao = 0, vbo = 0, ibo = 0;
        struct Range { int first = 0, count = 0, prim = 0; bool cutout = false; };
        std::vector<Range> ranges;
        long long tris = 0;
        bool valid() const { return vao != 0 && !ranges.empty(); }
    };
    // An author's edit to one material group of a species' mesh, kept apart from
    // the mesh so it survives a reload of the model: matched by file name and
    // group index, like the scene's model-material overrides.
    struct PartMat {
        std::string model;              // LOD mesh file this applies to
        int         prim = 0;           // material group within it
        glm::vec3   tint{1.0f};         // sRGB colour multiplier
        float       brightness = 1.0f;
        std::string texture;            // file drawn instead of the model's ("" = model's)
        int         cutout = -1;        // -1 as the model says, 0 solid, 1 cut out
        float       cutoff = 0.5f;      // alpha below this is a hole
    };

    // A configurable tree type: an ordered LOD chain + a far billboard, its own
    // density/size and its own GPU instance buffer (5 floats/tree: pos3,yaw,scale).
    struct TreeSpecies {
        std::string          name = "Tree";
        std::vector<PartMat> partMats;           // material edits (see applyPartMats)
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
        // Generated from lods.back(): `mid` stands in for everything past
        // LOD0 when the author gave only one level; `shadow` is what the
        // cascades draw. Rebuilt lazily whenever a level's mesh changes.
        AutoLod mid, shadow;
        bool    autoDirty = true;
        // The impostor (TreeImpostor.cpp): LOD0 baked from four sides into an
        // atlas row -- colour, and the normal the card is lit with.
        std::uint32_t impAlbedo = 0, impNormal = 0;
        float     impAspect = 0.6f;          // one view's width / height
        // The side views' share of the atlas width. The rest is the crown seen
        // from straight above, a square cell -- what a tree looks like to an
        // eye high over it, where a side view would lie on the ground edge-on.
        float     impSideFrac = 1.0f;
        bool      impDirty  = true;
        glm::vec3 canopy{0.02f, 0.035f, 0.012f};  // mean foliage albedo, linear
        // Every tree of this species the forest field holds, plus the painted
        // ones: what the impostor pass draws (it skips the near ones itself).
        std::uint32_t farVBO = 0, farVAO = 0;
        int           farCount = 0;
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
    // Kick off a procedural bloom regen (async: the disc pass is heavy, so it
    // runs on a worker and updateFlowers() uploads the result). Debounced -- a
    // call while one is already in flight is ignored.
    void regenFlowers(glm::vec2 c, const std::vector<glm::vec2>& road, float roadWidth,
                      float waterLevel, float snowLevel);
    // Per-frame pump: finish a pending async regen and push it to the GPU.
    void updateFlowers();
    void stampFlower(glm::vec2 c, float radius, std::mt19937& rng,
                     float waterLevel, float snowLevel);
    void eraseFlower(glm::vec2 c, float radius);
    void clearPaintedFlowers() { paintedFlowers.clear(); rebuildFlowerBuffer(); }
    void drawFlowers(const FrameContext& ctx);
    // Every bloom's head, world space (procedural and painted).
    const std::vector<glm::vec3>& flowerHeads() const { return m_flowerHeads; }

    // --- Birds + fireflies ---------------------------------------------------
    void drawBirds(const glm::mat4& viewProj, double time, const glm::vec3& camPos);
    void drawFireflies(const glm::mat4& viewProj, double time, float night,
                       const glm::vec3& camPos);
    void panelBirdsFireflies();

    // Does the scene have ground? Vegetation is scattered ONTO the terrain, so a
    // scene whose terrain object was removed must not keep a carpet of grass and
    // a forest floating over the void. Set by the host from the same state that
    // drives fitzel::setTerrainPresent; the field keeps its contents so putting
    // the terrain back brings the planting back with it.
    bool  terrainPresent = true;

    // Tunables the editor panel / scene setup drive directly.
    bool  grassEnabled = true;
    float grassHeight  = 0.35f;
    float grassDensity = 1.0f;
    float grassChaos   = 1.0f;  // 0 = even lawn, 1 = wild meadow, >1 = unruly
    float grassRadius  = 46.0f;
    float grassDryGrowth = 0.0f; // thin dry sward on ground too dry for grass (0 = bare)
    glm::vec3 grassTint{1.0f, 1.0f, 1.0f};

    // The grass field as data, for the path tracer: what the streamed tiles were
    // generated from, plus the tint the shader multiplies in. Valid after the
    // first updateGrass; before that it describes an empty field.
    const grassfield::Field& traceField() const { return m_field; }
    int   grassCount   = 0;
    bool  grassDirty   = true;         // request a (re)grow of the procedural field
    std::vector<float> paintedBlades;  // 7 floats/blade, world space (saved)
    bool  paintedDirty = false;        // GPU re-upload of painted blades pending

    bool      treeEnabled = true;
    // How far from the camera a tree still casts a sun shadow, in metres
    // (0 = no limit). The cascades cover the whole streamed terrain, so without
    // this the far cascade redraws every tree in the world -- at LOD0, with
    // alpha-cutout leaves, once per cascade. What that buys is a shadow half a
    // kilometre away, spread over a handful of shadow texels, on ground the fog
    // has already taken. It is by far the most expensive picture in the frame
    // per pixel of visible difference.
    float     treeShadowDistance = 120.0f;
    // Global colour correction for the tree material (mesh + billboard albedo).
    // Identity defaults, so a scene without saved values looks unchanged.
    float     treeBrightness = 1.0f; // multiplier (0..2)
    float     treeContrast   = 1.0f; // around mid-grey (0..2)
    float     treeHue        = 0.0f; // degrees (-180..180)
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

    // --- The forest field (Ecology.hpp, TreeField.hpp) -------------------------
    // With eco.enabled the procedural trees come from the streamed field: the
    // near ones as meshes out to impostorStart, the rest as impostors out to
    // forestRadius. Off, the old 120 m scatter, as scenes before it had.
    ecology::Params eco;
    // The air this frame (Wind.hpp): set by the host, read by every draw.
    wind::State wind;
    // What walks through the grass this frame (xyz feet, w radius, up to 8):
    // the blades part around it. Set by the host.
    std::vector<glm::vec4> grassPushers;
    // Discs (x, z, radius) the forest field plants nothing in: the spawn point,
    // the placed models. Set by the host.
    std::vector<glm::vec3> treeClearings;
    float impostorStart = 130.0f;
    float forestRadius  = 1600.0f;
    // How far out the impostors cast shadows (0 = none: only the meshes near
    // treeShadowDistance do). One card per tree, turned to the sun -- the
    // shadow that gives a forest seen from high above its depth, for the price
    // of a quad.
    float impostorShadowDist = 0.0f;
    // Generated levels and impostors for species whose meshes changed. Once a
    // frame, OUTSIDE any render pass: the bake draws into its own target.
    void prepareTrees();
    // The forest's mean foliage colour (linear), for the far terrain's canopy.
    glm::vec3 canopyColour() const;
    int forestTrees() const { return m_treeField.treeCount(); }

private:
    void bakeImpostor(TreeSpecies& sp);
    void drawImpostors(const FrameContext& ctx);
    void drawImpostorShadows(const glm::mat4& lightSpace, glm::vec2 camXZ, float from);
    void uploadFar();   // every field tree + painted, per species
    void regenTrees(glm::vec2 cc, const std::vector<glm::vec2>& road, float roadWidth,
                    float waterLevel, float snowLevel);
    void rebuildTreeBuffers();  // re-upload every species (procedural prefix + painted)
    void rebuildFlowerBuffer(); // re-upload procedural prefix + painted flowers
    // Load a .glb into `lod` (fills prims + creates its VAO/VBO bound to the
    // species' instance buffer). Returns false if the model failed to load.
    bool loadTreeMesh(const std::string& path, TreeSpecies& sp, TreeLOD& lod);
    // Lay the species' PartMat edits over the model values of every prim of
    // `lod` (tint, texture swap, cutout). Marks the generated levels dirty when
    // a part changes between leaf and bark. The impostor is baked from what is
    // drawn too, but rebaking it is the caller's call: a slider being dragged
    // should not rebake an atlas every frame.
    void applyPartMats(TreeSpecies& sp, TreeLOD& lod);
    // The species' edit for (model, prim), created with defaults when `make`.
    PartMat* findPartMat(TreeSpecies& sp, const std::string& model, int prim, bool make);
    // The "Materials" section of the species editor.
    void panelPartMats(int s);
    // Replacement textures, shared across parts and species.
    std::shared_ptr<fitzel::Texture> partTexture(const std::string& file);
    std::vector<std::pair<std::string, std::shared_ptr<fitzel::Texture>>> m_partTexCache;
    // (Re)derive sp.mid / sp.shadow from the species' coarsest authored level.
    void buildAutoLods(TreeSpecies& sp);
    static void freeAutoLod(AutoLod& a);
    // The instances of `sp` that a pass with this view-projection can see AND
    // that belong to this LOD's distance band, uploaded to the shared cull
    // buffer; returns how many to draw. `planeCount` is 4 for a shadow cascade
    // -- see sphereVisible in the .cpp. The band is measured from `eye` in 3D,
    // or across the ground only when `planar` (the shadow reach is a ground
    // distance: it is about which cascade a tree falls into, not detail).
    int  cullInstances(const TreeSpecies& sp, float boundR,
                       const glm::mat4& viewProj, int planeCount,
                       const glm::vec3& eye, bool planar, float lodMin, float lodMax);
    void scanTreeAssets(); // populate m_modelFiles / m_texFiles from the search dirs
    // Absolute path of a model/billboard picked by file name. Falls back to the
    // built-in content dir when the name isn't in the scanned list (e.g. a scene
    // that names an asset the current project doesn't ship).
    std::string modelPath(const std::string& file) const;
    std::string texPath(const std::string& file) const;

    fitzel::TerrainStreamer& m_streamer;
    fitzel::Camera&          m_camera;

    // Grass. The procedural field streams as per-tile instance buffers
    // (m_grassTiles); the hand-painted layer stays one instanced mesh. The blade
    // strip lives in m_grassBaseVAO; each tile's instance VBO is bound into it at
    // draw time (attribs 1..5), the way the tree pass binds its species buffers.
    fitzel::Shader        m_grass;
    fitzel::InstancedMesh m_paintedGrass;
    TiledScatter          m_grassTiles;
    // The parameters the resident tiles were generated from. Held rather than
    // rebuilt on demand because the tracer must render the field the viewport is
    // SHOWING: a second assembly of the same numbers would be right until one of
    // them was added in only one of the two places.
    grassfield::Field     m_field;
    std::uint32_t m_grassBaseVAO = 0, m_grassBaseVBO = 0;
    glm::vec2     m_grassCenter{1e9f}; // last flower-regrow center (camera follow)
    // Cached generator inputs; a change re-places the whole field (invalidate).
    float         m_gWater = 1e9f, m_gSnow = 1e9f, m_gRoadClear = -1.0f;
    std::uint32_t m_gWetHash = 0;
    float         m_gDensity = -1.0f, m_gChaos = -1.0f, m_gHeight = -1.0f;
    float         m_gRadius = -1.0f;
    std::uint32_t m_gRoadHash = 0;
    ecology::Params m_gEco;
    float         m_gDry = -1.0f;

    // Trees. Shaders are shared across all species (bound once, uniforms per draw).
    fitzel::Shader           m_tree, m_treeDepth, m_billboard;
    fitzel::Shader           m_impostor, m_impostorBake, m_treeMotion, m_impostorShadow;
    float                    m_prevWindTime = -1.0f;   // for the motion vectors
    TreeField                m_treeField;
    glm::vec2                m_nearCenter{1e9f};  // where the mesh set was gathered
    std::size_t              m_farPainted = static_cast<std::size_t>(-1);
    bool                     m_farPaintedOn = true;
    std::vector<TreeSpecies> m_species;
    std::vector<std::string> m_modelFiles;  // *.glb display names (panel dropdown)
    std::vector<std::string> m_texFiles;    // *.png display names (billboard dropdown)
    std::vector<std::string> m_modelPaths;  // absolute path per m_modelFiles entry
    std::vector<std::string> m_texPaths;    // absolute path per m_texFiles entry
    std::string              m_modelDir, m_texDir;
    std::string              m_projectDir;  // open project, scanned too ("" = none)
    const float              m_treeRadius = 120.0f;
    // --- Instance culling ----------------------------------------------------
    // The subset of a species' instances a given pass can actually see, rebuilt
    // per pass into one reused GPU buffer. See drawTrees for why a tree mesh
    // cannot be left to the vertex shader to reject.
    std::vector<float>       m_visInst;
    std::uint32_t            m_cullVBO = 0;
    // How many instances the last frame's passes drew, for the Trees panel: the
    // difference between this and treeCount is what the culling saved, and it is
    // the only place an author can see that a forest is being drawn six times
    // over because it stands in every shadow cascade.
    int                      m_drawnInstances = 0;
    int                      m_drawnLast = 0;
    int                      m_shadowInst = 0, m_shadowInstLast = 0;
    long long                m_shadowTris = 0, m_shadowTrisLast = 0;
    std::mt19937             m_trng{555u};
    // Combined positions of every species (5 floats/tree) -- feeds flower
    // clustering around trunks and the treeInstances() accessor.
    std::vector<float>       m_treeInst;

    // Flowers.
    fitzel::Shader        m_flower;
    fitzel::InstancedMesh m_flowerField;
    int                   m_flowerVerts = 0;
    std::vector<float>    m_flowerInst;
    std::vector<glm::vec3> m_flowerHeads;
    std::size_t           m_proceduralFlowerFloats = 0;
    std::future<std::vector<float>> m_flowerFuture; // async procedural regen
    bool                            m_flowerPending = false;

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
