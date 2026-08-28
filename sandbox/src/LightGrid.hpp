#pragma once

#include <filesystem>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Texture3D.hpp>

#include "PathTrace.hpp"

namespace fitzel { class Renderer; }

// Baked indirect light, as a grid of probes over the world.
//
// WHY A GRID AND NOT LIGHTMAPS. A lightmap needs every static surface given a
// second, non-overlapping UV set with padding between charts -- an unwrapper, a
// new vertex attribute through every mesh producer in the engine, an atlas, and
// seam dilation. A grid needs none of that: it is sampled by world position, so
// nothing about the geometry has to change. It also does something a lightmap
// structurally cannot, which matters more here than the saving: MOVING objects
// pick it up too. A car driving under a bridge gets dark, and takes the colour
// of the tarmac it is over.
//
// WHAT IS BAKED, AND WHY IT IS NOT EVERYTHING. The sun is left out. This engine
// runs a day cycle -- twenty-four hours in four minutes by default -- so light
// baked with the sun in it would be a photograph of one moment that the game
// contradicts within seconds of pressing Play. What holds still is the sky, the
// static lamps, and everything those bounce off; that is what goes in the grid,
// and the direct sun stays dynamic exactly as it is now.
//
// WHAT IT REPLACES. `uAmbient`: one colour, applied to every surface in the
// world. Under it the inside of a tunnel is as bright as an open field, and the
// SSAO pass exists largely to paper over that. A probe under a bridge has no
// sky above it and says so.
//
// No GL in this file, so the bake and the layout can be checked headless.
namespace lightgrid {

struct Settings {
    // Probes along the longest horizontal axis; the others follow so cells stay
    // roughly cubic. The cost is cubic in this, so it is the one number worth
    // thinking about before pressing Bake.
    int   resolution = 24;
    int   rays       = 128;  // per probe
    int   bounces    = 3;
    float padding    = 4.0f; // metres of grid beyond the geometry, so surfaces
                             // at the very edge still have probes around them
    // Height of the grid above the geometry's top. A racing world is wide and
    // shallow, and probes fifty metres above the tarmac are probes nothing will
    // ever sample.
    float headroom   = 12.0f;
};

struct Grid {
    glm::vec3 lo{0.0f}, hi{1.0f};
    int nx = 0, ny = 0, nz = 0;
    std::vector<pathtrace::ProbeSh> probes;  // x fastest, then y, then z

    int  count() const { return nx * ny * nz; }
    bool valid() const {
        return nx > 1 && ny > 1 && nz > 1 &&
               static_cast<int>(probes.size()) == count();
    }
    // The world position of one probe: the centre of its cell, which is where a
    // 3D texture's texel centre sits -- so the hardware's interpolation lands
    // exactly between the probes that were actually traced.
    glm::vec3 positionOf(int ix, int iy, int iz) const;

    // One colour channel as RGBA floats, ready for a Texture3D: each texel is
    // that channel's four coefficients (constant, x, y, z).
    std::vector<float> channel(int c) const;
};

// Bounds and resolution for a scene, without tracing anything. Separate from
// the bake so the editor can say how many probes it is about to ask for before
// anybody waits for them.
Grid layout(const pathtrace::Scene& scene, const Settings& settings);

// Trace it. EDITOR ONLY -- it lives in LightGridBake.cpp, which the shipped
// player does not compile, because baking needs the whole path tracer and a
// game has no use for one. Everything above and below this is runtime: loading
// a bake and lighting from it is what the player does, and it needs none of the
// tracer to do it.
//
// Blocking and threaded; `progress` is called with 0..1 and returns
// false to cancel, in which case the grid is left unusable rather than half
// right. Probes that landed inside solid geometry are filled from their
// neighbours before this returns -- an unfilled one is a black spot that bleeds
// into every surface near it.
bool bake(Grid& grid, const pathtrace::Scene& scene, const Settings& settings,
          const std::function<bool(float)>& progress = {});

// A baked grid belongs to its scene and outlives the session. Written beside
// it; a missing or stale file simply means no baked light, not an error.
bool save(const Grid& grid, const std::filesystem::path& file);
bool load(Grid& grid, const std::filesystem::path& file);

// The grid a scene is actually lit by, together with its copy on the GPU.
//
// Owned by the application rather than by the editor's panel, which is the
// whole point of the split: a shipped game loads the .fgrid its scene was baked
// with and lights from it, and never links a line of the tracer that made it.
struct Runtime {
    Grid              grid;
    fitzel::Texture3D tex[3];
    std::string       scene;      // which scene the grid in memory belongs to
    bool              enabled   = true;
    float             intensity = 1.0f;
    std::string       status;

    // Push `grid` to the GPU. Needs a current GL context.
    void upload();
    // Hand the current state to the renderer. Called every frame: the renderer
    // borrows the textures rather than owning them, and the strength is live.
    void apply(fitzel::Renderer& renderer) const;
    // Load the bake belonging to `scenePath` if the scene has changed, then
    // apply. One call per frame from the render loop is the whole integration.
    void syncTo(const std::filesystem::path& scenePath, fitzel::Renderer& renderer);
};

// Where a scene keeps its baked light: beside the scene file, named after it.
// Derived rather than stored, so a renamed or copied scene simply has no bake
// until somebody presses the button again -- which is the honest answer.
std::filesystem::path pathFor(const std::filesystem::path& scenePath);

} // namespace lightgrid
