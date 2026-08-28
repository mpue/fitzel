#pragma once

#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "PathTrace.hpp"

namespace fitzel {
class Camera;
class Renderer;
}

// Turning the frame the renderer just drew into a scene the path tracer can
// trace. The GL-facing half of the offline renderer; PathTrace.hpp is the half
// that does the arithmetic and touches no GL at all.
//
// WHY IT HARVESTS THE RENDER QUEUE and not the scene document. By the time a
// mesh reaches Renderer::submit it has stopped mattering which system built it:
// a terrain chunk, a road ribbon, a bridge deck, a city tower, a spline fence
// and an imported car are all just a mesh, a material and a place. Building the
// tracer's scene from the queue therefore covers every one of them, including
// the ones written after this file, and cannot silently omit a system nobody
// remembered to wire up. Harvesting the document instead would mean
// re-deriving all that geometry a second way -- which is the version that
// drifts, and drifts invisibly, because both pictures still look like pictures.
//
// The cost is a full GPU stall: every mesh is read back out of its buffer and
// every texture out of its unit. That is fine exactly once, when somebody asks
// for a render, and would be ruinous in a frame. Call it from the editor's
// render command, never from the loop.
//
// WHAT IT CANNOT SEE, and says so in the report rather than leaving it to be
// noticed later:
//   * Vegetation, particles, rain and spray -- their vertices are computed
//     inside a vertex shader from gl_VertexID and exist nowhere else.
//   * Water -- a procedurally displaced surface, likewise.
//   * The sky -- an analytic shader, replaced here by the HDRI when the scene
//     has one and by a gradient when it does not.
//   * Terrain layer TEXTURES -- the painted layers are triplanar-blended in the
//     shader; the terrain's base colour is used instead.
namespace pathcapture {

struct Options {
    // Only geometry within this distance of the camera is harvested. 0 means
    // everything.
    //
    // This is the setting that decides whether a render takes ten seconds or
    // ten minutes, because a landscape scene is millions of triangles of which
    // a hero shot can see a few hundred thousand. It is a radius rather than a
    // frustum cull on purpose: what is behind the camera still shows up in the
    // reflections, and a car whose flank mirrors a hole where the world was cut
    // away is worse than a slow render.
    float maxDistance = 250.0f;

    // The sun's disc, in degrees. ~0.27 is the real sun; larger reads as hazier
    // and softens every shadow edge in the picture. 0 reproduces the raster
    // path's hard edge exactly.
    float sunAngleDeg = 0.5f;
    // Physical size of a point/spot lamp's bulb, in metres. Same trade.
    float lampRadius = 0.08f;

    // The HDRI the scene lights from, and its intensity. Left empty when the
    // scene has no environment map, in which case the gradient below stands in.
    std::string hdriPath;
    float       hdriIntensity = 1.0f;

    // Base-colour maps bigger than this are box-filtered DOWN to it, not
    // dropped. Dropping was the first version and it was quietly wrong: a car
    // whose livery lives in a 4K atlas rendered as a white blank, which reads
    // as a lighting problem rather than as a missing texture. A 4K map also
    // costs 64 MB of readback and as much again in RAM, and a still has never
    // yet needed one at full size.
    int maxTextureSize = 2048;

    // Transparent surfaces (glass, blended decals). Off renders them as if they
    // were not there, which is occasionally what a product shot wants.
    bool includeTransparent = true;

    // The colour grade the post chain puts on every viewport frame. It has to be
    // handed in rather than read off the Renderer, because the grade belongs to
    // the post chain and the renderer has never heard of it. Omitting it is not
    // a subtle difference: the project's own defaults warm, saturate and add
    // contrast to everything on screen, so an ungraded render looks like a
    // different scene rather than a better one.
    pathtrace::Grade grade;
};

// What the harvest found, for the panel to show and for anyone wondering why a
// render does not match the viewport. Every omission above lands here as a
// note, so a missing thing is reported rather than merely absent.
struct Report {
    long long triangles = 0;
    int       meshes    = 0;   // distinct meshes read back
    int       instances = 0;   // submissions harvested (a mesh may appear often)
    int       materials = 0;
    int       textures  = 0;
    int       texturesShrunk = 0; // maps box-filtered down to the size limit
    long long culled    = 0;   // triangles dropped by maxDistance
    long long textureBytes = 0;
    double    seconds   = 0.0;
    std::vector<std::string> notes;

    std::string summary() const;
};

// Harvest the renderer's current queue. Must be called with a current GL
// context, after the frame's submit() calls and before the next begin().
//
// The render's own aspect ratio comes from the output size, not from the
// viewport: the camera's field of view is vertical, so a 16:9 still and a tall
// editor pane frame the same amount of the scene top to bottom and differ only
// in how much they show either side -- which is what "the same shot at a
// different size" ought to mean.
std::shared_ptr<pathtrace::Scene> capture(const fitzel::Renderer& renderer,
                                          const fitzel::Camera& camera,
                                          const Options& options,
                                          Report* report = nullptr);

} // namespace pathcapture
