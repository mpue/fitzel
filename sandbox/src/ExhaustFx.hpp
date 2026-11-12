#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/RenderTarget.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/rhi/Types.hpp>

#include "ExhaustSim.hpp"

struct Entity;

// Rocket exhaust on screen: flames, and the clouds they raise (ExhaustSim.hpp
// moves them).
//
// The clouds are a VOLUME, because a launch cloud is the one cloud whose inside
// everybody sees: it is lit from within by the flame, orange where it is near
// and grey-white where the sun gets at its top, with every billow shading the
// next. Sprites cannot do that -- each would be lit alone, and a pile of them
// reads as a pile. So, every frame:
//
//   1. the sim's puffs are binned into 8x8x8-voxel bricks on the CPU;
//   2. a compute pass splats them into a 3D density grid -- density, glow, and
//      the noise coordinate of the puff each voxel belongs to, so the detail
//      billows travel and swell with their puff instead of sliding through it;
//   3. a second compute pass marches every voxel towards the sun, towards the
//      flames and straight up, and keeps the optical depths -- the shadowing
//      inside the cloud, paid once per voxel instead of once per sample;
//   4. per view, the screen is raymarched through the grid, the billows carved
//      out of it with baked worley noise, and the flames integrated in the same
//      march as emissive cones (Mach diamonds and all), so smoke in front of a
//      flame hides it and smoke behind it is lit by it.
//
// Three grids: a fine one standing over the pad (the trench clouds, where the
// camera looks closely), a fine one travelling with the vehicle (the column it
// is laying down and the young trail behind it, which a camera following the
// launch looks at just as closely), and a coarse one fitted to everything else.
// A sample uses a fine one where there is one. The coarse grid's voxels are
// bigger than the puffs in a young column, and it shows: a column drawn from it
// is hatched where it should be smooth, and the hatching crawls when the camera
// moves.
class ExhaustFx {
public:
    ExhaustFx() = default;
    ~ExhaustFx();
    ExhaustFx(const ExhaustFx&)            = delete;
    ExhaustFx& operator=(const ExhaustFx&) = delete;

    // How the clouds and flames look. Scene-independent defaults that suit a
    // heavy launcher at its pad; scripts adjust them (game.exhaustLook).
    struct Look {
        float sigma   = 0.32f;  // extinction per metre deep inside a cloud
        float thresh  = 0.28f;  // density-field level at a cloud's surface
        float soft    = 0.16f;  // how quickly it turns solid past the surface
        float erode   = 0.75f;  // how deep the billows cut into the surface
        float noiseK  = 0.45f;  // billow size: noise units per puff radius
        float albedo  = 0.92f;
        glm::vec3 tint{1.0f, 0.98f, 0.96f};
        float sun     = 1.0f;   // multipliers on the frame's sun / sky light
        float sky     = 1.0f;
        float flame   = 1.0f;   // how strongly the flames light the clouds
        float heat    = 1.0f;   // glow of fresh hot exhaust in the cloud
        float flameBright = 1.0f;  // all flames' radiance
        // Grid over the pad: its size in metres (centred on the first trench, or
        // under the first engine) and its voxel edge.
        glm::vec3 nearSize{960.0f, 384.0f, 960.0f};
        float     nearVoxel = 4.0f;
        // Grid travelling with the vehicle: its size in metres, hanging from
        // the engines down the trail; the pad grid's voxel, on the same lattice,
        // so where the two meet nothing changes.
        glm::vec3 tipSize{512.0f, 1280.0f, 512.0f};
        int       resScale  = 1;   // march at 1/n of the pane
        // Steps one ray may take. Generous on purpose: a ray grazing a ground
        // cloud a kilometre long at the fine grid's 2 m steps needs hundreds,
        // and a budget that runs out mid-cloud cuts it off along a curve (the
        // march also lengthens its step so half the budget crosses any span).
        // Empty bricks are skipped and a dense cloud stops a ray early, so the
        // average is far below this.
        int       maxSteps  = 3000;
    };

    // What a view needs to hand over per frame.
    struct Params {
        glm::mat4 viewProjRel{1.0f};   // camera-relative (fitzel::RelCamera)
        glm::vec3 camPos{0.0f};
        float     time = 0.0f;
        glm::vec3 sunDir{0.0f, 1.0f, 0.0f};   // towards the sun
        glm::vec3 sunColor{1.0f};             // the sun's HDR radiance
        glm::vec3 skyColor{0.4f, 0.5f, 0.7f};
        glm::vec3 groundColor{0.2f, 0.18f, 0.15f};
        // The per-pixel noise the march places its samples by: with temporal
        // AA on, another pattern each frame (`frame` counts them) for the
        // resolve to average into smooth cloud; off, the same pattern every
        // frame -- a fixed dither rather than a glitter (Renderer::setNoiseFrame).
        bool      temporal = false;
        unsigned  frame    = 0;
    };

    bool init();
    bool ready() const { return m_ok; }

    // Gather the scene's engines, vents and trenches, step the sim and rebuild
    // the grids. Once per frame (a second call with the same `frame` does
    // nothing, so every view of a split screen can ask). `simulate` false keeps
    // the clouds where they are (paused editor) but still follows the flames.
    void update(const std::vector<Entity>& entities, float dt, std::uint64_t frame,
                const std::function<float(float, float)>& groundAt, bool simulate,
                const glm::vec3& sunDir);

    // March the frame into `hdr` (its depth attached as a texture). Leaves the
    // render state as volumetric fog does.
    void render(const fitzel::RenderTarget& hdr, const Params& p, fitzel::Mesh& fsQuad);

    void clear();
    bool active() const { return !m_sim.puffs().empty() || !m_flames.empty(); }
    int  puffCount() const { return static_cast<int>(m_sim.puffs().size()); }

    Look&       look() { return m_look; }
    const Look& look() const { return m_look; }

private:
    static constexpr int kMaxFlames = 8;

    enum { kNear = 0, kFar = 1, kTip = 2 };

    struct Grid {
        glm::ivec3 dims{0};         // voxels, multiples of 8
        glm::vec3  origin{0.0f};    // world min corner
        float      voxel = 4.0f;
        bool       on = false;
        // The light is eased from one frame to the next, so it is kept twice:
        // l[cur] is the one drawn with, the other is written next. `lit` and
        // where the grid stood then say whether the last one can be eased from
        // (a grid that has moved by whole voxels can; one that changed its
        // voxel cannot).
        fitzel::rhi::TextureHandle a, b, l[2], occ;
        int        cur = 0;
        bool       lit = false;
        glm::vec3  litOrigin{0.0f};
        float      litVoxel = 0.0f;
        std::vector<std::uint32_t> start, list;
        fitzel::rhi::BufferHandle  startBuf, listBuf;
        glm::ivec3 bricks() const { return dims / 8; }
    };

    // One flame as the march takes it.
    struct FlameGpu {
        glm::vec3 a, dir;
        float len, r0, r1, bright, core, diamonds, seed, light;
        glm::vec3 color;
    };

    void allocGrid(Grid& g, glm::ivec3 dims);
    // A box of space a grid need not hold puffs for: well inside a finer grid,
    // which is where every sample there is taken. Empty when lo > hi.
    struct Box {
        glm::vec3 lo{1e30f}, hi{-1e30f};
    };
    static Box inside(const Grid& finer, float margin);
    void binGrid(Grid& g, const std::vector<glm::vec4>& puffs, const Box& skipA, const Box& skipB);
    void splatGrid(Grid& g);
    void lightGrid(Grid& g, int which);
    void fitFarGrid();
    void placeTipGrid();
    void resize(int w, int h);
    bool uploadBuffer(fitzel::rhi::BufferHandle& id, const void* data, std::size_t bytes);

    bool m_ok = false;
    Look m_look;
    exhaust::Sim m_sim;
    std::vector<exhaust::Trench> m_trenches;
    std::vector<FlameGpu>        m_flames;
    // The flames' combined light for the clouds' shadowing: one segment (the
    // thrust-weighted middle of the cluster), radius and colour.
    glm::vec3 m_lightA{0.0f}, m_lightB{0.0f};
    glm::vec3 m_sunDir{0.0f, 1.0f, 0.0f};   // the light pass marches towards it
    float     m_dt = 0.0f;                   // this frame's step, for easing the light
    bool      m_lightOn = false;
    std::uint64_t m_lastFrame = ~0ull;
    // Vehicle velocity per engine, from where its nozzle was last frame.
    std::vector<std::pair<int, glm::vec3>> m_lastNozzle;
    bool      m_nearPlaced = false;
    glm::vec3 m_nearCentre{0.0f};

    Grid m_grid[3];   // kNear, kFar, kTip
    std::vector<glm::vec4> m_puffData;   // 3 vec4 per puff, uploaded once a frame
    fitzel::rhi::BufferHandle m_puffBuf;
    fitzel::rhi::TextureHandle m_noise;  // 3D billow noise, baked once on the GPU

    fitzel::Shader m_noiseBake, m_splat, m_light, m_march, m_up;
    fitzel::RenderTarget m_rt{1, 1};
    int m_w = 0, m_h = 0;
};
