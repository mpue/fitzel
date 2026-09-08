#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/world/Terrain.hpp>

#include "PathTrace.hpp"

// The grass field, as data rather than as a draw call.
//
// Two halves that have to agree, which is the whole reason they share a file.
//
// WHERE THE BLADES ARE. generateTile() is the field's definition: deterministic
// placement from a tile coordinate, no GL, no state. VegetationSystem streams it
// onto the GPU through TiledScatter; the tracer calls the SAME function for
// whatever patch of world a picture needs. That is not a convenience -- the
// alternative is a second placement written to look like the first, which would
// agree until the day somebody tuned one of them, and then disagree in a way
// that reads as a rendering bug rather than as two implementations.
//
// WHAT A BLADE LOOKS LIKE. appendToScene() expands each instance into the
// triangles assets/shaders/grass.vert builds from it -- the same 7-vertex strip,
// the same taper, the same static arch, the same wind bend. This one IS a second
// implementation of shader code and there is no way around that: the raster
// blade exists only as vertex-shader arithmetic and the tracer needs triangles.
// Kept adjacent to the placement so at least the two halves of "what the field
// is" are read together, and checked against the viewport by eye through
// tools/tracecheck.cpp.
//
// WHY THE TRACER CAN HAVE THIS AT ALL, when PathTraceCapture cannot see grass:
// capture() harvests the render queue, and the grass in that queue is instance
// data plus a vertex shader, not readable geometry. This module does not read
// the queue. It regenerates the field from its parameters, on the CPU, which
// also means a render is not limited to the tiles that happen to be streamed in.
namespace grassfield {

// Everything the placement depends on -- by value, because the generator runs on
// TiledScatter worker threads and may not reach back into the editor.
struct Field {
    fitzel::TerrainSettings terrain;

    float waterLevel = 0.0f;      // no grass below this (+0.5 m)
    float snowLevel  = 1000.0f;   // ...nor above it (-1.5 m)
    float height     = 0.35f;     // blade height (m) before per-blade jitter
    float density    = 1.0f;      // blades per sampling cell, scaled
    float chaos      = 0.5f;      // 0 lawn .. 1 wild meadow

    std::vector<glm::vec2> road;            // road centreline, a keep-out line
    float                  roadClear = 0.0f; // ...and its half width (m)
    std::vector<glm::vec3> wet;             // brook discs (x, z, radius) to avoid

    glm::vec3 tint{1.0f};         // the grass shader's uTint

    // The tile edge the field is generated on. A constant rather than a setting:
    // the seed is the tile coordinate, so changing this reshuffles every blade in
    // the world. VegetationSystem's TiledScatter config must use the same number.
    static constexpr float kTileSize = 12.0f;
};

// One tile's instances, appended to `out`: 7 floats each -- base position (x,y,z),
// yaw, height, sway phase, lushness. `origin` is the tile's min corner in world
// XZ and `size` its edge. Deterministic in (tx,tz): streaming re-enters a tile
// after the camera leaves and returns, and it must look identical.
void generateTile(std::int32_t tx, std::int32_t tz, glm::vec2 origin, float size,
                  const Field& f, std::vector<float>& out);

// --- The tracer's half -----------------------------------------------------

// How much field to put in a picture, and in what pose.
struct TraceOptions {
    glm::vec2 centerXZ{0.0f};   // usually the camera
    float     radius = 40.0f;   // blades beyond this are not generated at all

    // The blades fade to nothing over the last stretch before `radius`, exactly
    // as the shader does at the streamed ring's edge (uFadeStart/uFadeEnd). Not
    // an effect: without it the field ends at a circular cliff, and the viewport
    // this render is supposed to match does not have one.
    float fadeMargin = 18.0f;

    // The wind, frozen. A still has one instant in it, and this is which one --
    // so a render taken while the field was lying flat in a gust shows it lying
    // flat, rather than standing up because the tracer has no clock.
    float     windTime     = 0.0f;
    glm::vec2 windDir{0.894f, 0.447f};  // normalize(vec2(0.6, 0.3)), as drawGrass
    float     windStrength = 0.2f;

    // Per-blade colour is computed per blade by the shader, and pathtrace has no
    // vertex-colour channel to put it in. So the colours are quantised to this
    // many steps per channel and become materials -- a blade's colour IS its
    // material index.
    //
    // Measured on a 14 m meadow, quantising on the square-root curve the palette
    // uses: 16 steps collapse the whole field into 20 colours, 64 into 115. The
    // first is visibly a palette; the second is not, and 115 materials cost
    // nothing next to two hundred thousand triangles. Hence 48 rather than the
    // smallest number that seemed plausible.
    int colorSteps = 48;

    // How much light goes THROUGH a blade rather than off it. A blade is a thin
    // sheet with a dark albedo, and treated as opaque it can only be as bright
    // as the side facing you -- which is why the first traced meadow came out
    // nearly black whatever the sun did. Most of a real meadow is sunlight that
    // came through the blades. See Material::translucency; the diffuse lobe is
    // split, so this moves light rather than adding any.
    float translucency = 0.4f;

    // How far the blade's normal is tilted toward straight up.
    //
    // grass.vert uses vec3(-sin, 1.2, cos): the ribbon's true normal is
    // horizontal, and the 1.2 is a raster cheat that makes every blade catch
    // light from above however it is turned. The cheat does not survive here.
    // The tracer flips the shading normal to face the ray ("shade the side we
    // can see"), which for an up-biased normal means half the blades end up
    // shaded by a normal pointing DOWN -- and a meadow lit from below is black.
    //
    // 0 is the ribbon's real normal, which the flip handles correctly. Values
    // above it trade physical sense for the raster look and go dark in exactly
    // that way, which is what the two pictures grasscheck writes are for.
    float normalUpBias = 0.0f;

    // A hard ceiling, because a field is millions of blades and each is five
    // triangles: at some radius the honest answer is "this does not fit" rather
    // than an allocation the size of the machine. Generation stops when it is
    // reached and the report says so.
    long long maxTriangles = 8000000;
};

struct TraceReport {
    long long blades    = 0;
    long long triangles = 0;
    int       materials = 0;   // distinct quantised blade colours
    bool      truncated = false;
    double    seconds   = 0.0;
};

// Append the field to a scene the tracer can trace. Adds triangles and the
// materials they need; touches nothing already in it.
void appendToScene(pathtrace::Scene& scene, const Field& f,
                   const TraceOptions& opt, TraceReport* report = nullptr);

} // namespace grassfield
