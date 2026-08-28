#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <glm/glm.hpp>

// The offline path tracer: the still-image renderer behind the editor's Render
// panel, for making pictures OF the game rather than pictures IN it.
//
// WHY AN OFFLINE RENDERER AT ALL, next to a real-time one that already looks
// good. The raster path buys its frame rate with approximations that are
// invisible at speed and obvious in a held image: reflections come from a
// 256-pixel probe captured a few frames ago from somewhere near the camera, the
// only shadows are the sun's and a handful of opt-in lamps', ambient light is
// one flat colour or a blurred convolution, and nothing bounces -- a red car
// parked on grey tarmac casts no red onto the tarmac. A screenshot inherits
// every one of those. A press shot, a store page or a box render is looked at
// for minutes rather than milliseconds, which is exactly the timescale on which
// the approximations show.
//
// WHAT THIS IS NOT. It is not a second look for the game to be played in, and
// nothing here runs per frame. It is not a replacement for the raster path and
// deliberately shares its vocabulary -- the same albedo/roughness/reflectivity
// a material already carries, the same range-limited lamp falloff, the same
// ACES curve at the end -- so a render reads as the SAME scene, better lit,
// rather than as a different game. Where the two differ it is because the
// approximation was the difference.
//
// WHAT IT DELIBERATELY LEAVES OUT (v1, hero shots). Vegetation, particles and
// water: grass and trees are gl_VertexID instances whose geometry only exists
// inside a vertex shader, and water is a displaced procedural surface. None of
// the three can be read back off the GPU as triangles, so none of them is here.
// That is a scope decision, not an oversight -- the shot this serves is a
// vehicle or a building, framed close, and none of the missing three is what
// the picture is of.
//
// NO GL IN THIS FILE. Everything here is plain data and arithmetic, which is
// what lets the tracer be tested headless by tools/pathcheck.cpp against known
// answers instead of by looking at the editor and forming an opinion.
// PathTraceCapture.hpp is the half that talks to the GPU.
namespace pathtrace {

// A decoded base-colour map. Top-left origin, RGBA8, its values used exactly as
// lit.frag uses them (multiplied by the tint, no decode) so a textured surface
// comes out the colour the viewport showed.
struct Image {
    std::vector<unsigned char> pixels;
    int width = 0, height = 0;

    bool valid() const { return !pixels.empty() && width > 0 && height > 0; }
    // Bilinear, wrapping. Returns white when invalid, so an untextured or
    // failed-to-read surface falls back to its flat albedo instead of black --
    // a missing texture should look plain, not like a hole.
    glm::vec4 sample(float u, float v) const;
};

// A surface, in the same terms the material asset and lit.frag use. See
// MaterialDef in SceneTypes.hpp -- these fields are that struct, resolved.
//
// COLOURS HERE ARE sRGB, exactly as the material asset stores them and as the
// shader's uniforms receive them. The tracer applies the same pow(2.2) that
// lit.frag does before lighting anything. Holding them pre-linearised would
// have been tidier arithmetic and a worse interface: the numbers here would
// then no longer be the numbers in the inspector, and every future comparison
// between a render and the viewport would start by wondering which space a
// value was in.
struct Material {
    glm::vec3 albedo{0.72f, 0.72f, 0.74f};
    glm::vec3 tint{1.0f};          // multiplies the texture (untextured: unused)
    float     roughness    = 0.4f; // 0 sharp .. 1 blurred
    float     reflectivity = 0.0f; // 0 dielectric (4% F0) .. 1 mirror
    float     opacity      = 1.0f;
    bool      glass        = false; // dielectric with refraction, not blending
    glm::vec3 emission{0.0f};       // emissive colour, sRGB
    float     emissionStrength = 1.0f; // linear multiplier (>1 for a real glow)
    int       texture      = -1;    // index into Scene::textures, -1 = flat
    // 0 opaque / 1 cutout / 2 blend, matching AlphaMode in SceneTypes.hpp.
    int       alphaMode    = 0;
    float     alphaCutoff  = 0.5f;
};

// One triangle, world space, with the vertex normals and UVs it was drawn with.
// Fattened deliberately: traversal is bandwidth-bound and a second indirection
// to fetch the shading data costs more than the memory does.
struct Triangle {
    glm::vec3 p0{0.0f}, p1{0.0f}, p2{0.0f};
    glm::vec3 n0{0.0f}, n1{0.0f}, n2{0.0f};
    glm::vec2 uv0{0.0f}, uv1{0.0f}, uv2{0.0f};
    int       material = 0;
};

// The sun. `angularRadiusDeg` is what makes a shadow's edge soft: zero is the
// hard-edged raster look, half a degree is roughly the real sun, and a few
// degrees reads as an overcast day. It is the single most effective dial in
// here for making a render stop looking like a screenshot.
struct Sun {
    glm::vec3 direction{0.5f, 1.0f, 0.35f}; // points TOWARDS the light
    glm::vec3 color{1.0f, 0.97f, 0.9f};
    float     angularRadiusDeg = 0.5f;
    bool      enabled = true;
};

// A point or spot lamp. `cosOuter < -1` marks it omnidirectional (a point
// light); otherwise it is a cone. `radius` is the emitter's physical size and,
// like the sun's angle, is where soft shadows come from.
struct Lamp {
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    glm::vec3 color{1.0f};
    float     range    = 12.0f;
    float     radius   = 0.06f;
    float     cosInner = 1.0f;
    float     cosOuter = -2.0f; // < -1 -> point light
    bool      isSpot() const { return cosOuter >= -1.0f; }
};

// What a ray sees when it hits nothing. An HDRI panorama when the scene has one
// loaded (the same file the raster path lights from), otherwise a three-stop
// gradient built from the renderer's own ambient and fog colours -- which is
// not a sky model, just the flat ambient the raster path would have used, given
// somewhere to come from.
struct Environment {
    std::vector<float> pixels;  // equirectangular RGB float, row 0 = up
    int   width = 0, height = 0;
    float intensity = 1.0f;

    glm::vec3 zenith{0.42f, 0.52f, 0.72f};  // gradient fallback
    glm::vec3 horizon{0.70f, 0.78f, 0.88f};
    glm::vec3 ground{0.22f, 0.21f, 0.20f};

    bool      hasMap() const { return !pixels.empty() && width > 0 && height > 0; }
    glm::vec3 sample(const glm::vec3& dir) const;
};

// The eye. Given as a basis rather than yaw/pitch because that is what the
// scene's cameras produce (see Camera::setBasis) and because a shot may be
// rolled. Aperture > 0 opens a real lens: focus lands at `focusDistance` and
// everything else blurs, which is the other half of what separates a render
// from a screenshot.
struct CameraDesc {
    glm::vec3 position{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    glm::vec3 right{1.0f, 0.0f, 0.0f};
    float     fovDegrees     = 60.0f; // vertical, as Camera::projectionMatrix uses it
    float     apertureRadius = 0.0f;  // metres; 0 = pinhole, everything sharp
    float     focusDistance  = 10.0f;
};

// Height fog, copied from the raster path so a foggy scene renders foggy.
// Applied to the primary ray only -- which is what lit.frag does too, since it
// fogs a fragment by its own depth and knows nothing about the light that
// reached it.
struct FogDesc {
    glm::vec3 color{0.70f, 0.82f, 0.95f};
    glm::vec3 sunColor{1.0f, 0.75f, 0.5f};
    float     density       = 0.0f;   // 0 = off
    float     heightFalloff = 0.03f;
    float     height        = 0.0f;
};

// The colour grade the post chain applies after tonemapping (see the tail of
// composite.frag). It is here because leaving it out was a real bug and not a
// small one: the viewport NEVER shows a raw tonemap. Every frame goes through
// this grade on the way to the screen, the project's defaults are nowhere near
// neutral (saturation 1.35, a warm white balance, a contrast lift), and a
// render without it comes out flat, cool and desaturated next to the very
// picture it is supposed to be a better version of.
//
// The neutral values here are the ones that DO NOTHING, which is not the same
// as the ones the editor starts with -- a scene that has never been graded
// still carries a grade.
struct Grade {
    float hueShift   = 0.0f;   // degrees
    float saturation = 1.0f;
    float value      = 1.0f;
    float warmth     = 0.0f;   // + golden, - cool
    float contrast   = 0.0f;   // S-curve strength around mid grey
};

// Everything a render needs, and nothing that changes while it runs. Handed to
// a Job as a shared_ptr and never touched again: the editor is free to carry on
// editing the scene while a render of the old one finishes.
struct Scene {
    std::vector<Triangle> triangles;
    std::vector<Material> materials;
    std::vector<Image>    textures;
    Sun                   sun;
    std::vector<Lamp>     lamps;
    Environment           env;
    FogDesc               fog;
    CameraDesc            camera;
    float                 exposure = 1.0f;
    Grade                 grade;
};

// What to put in the picture. Anything other than Full is a diagnostic, and
// they exist because a wrong render is otherwise unattributable: a car that
// comes out the wrong colour could be its texture, its material, the light
// reaching it, the tonemap or the grade, and staring at the finished image
// cannot separate those. Each mode below removes everything after one stage, so
// the first mode that looks wrong is the stage that is wrong.
enum class Show {
    Full = 0,   // the render
    BaseColor,  // the surface's own colour, as authored -- no light, no tonemap
    Normal,     // the shading normal as RGB -- catches a bad transform or winding
    Depth,      // distance from the eye -- catches geometry that is not where it looks
};

struct Settings {
    int width  = 1280;
    int height = 720;
    int samples    = 128;  // total per pixel
    int maxBounces = 6;    // 1 = direct light only
    int batch      = 4;    // samples added per progressive pass
    int threads    = 0;    // 0 = hardware_concurrency() - 1
    bool tonemap   = true; // ACES + gamma, as the viewport does
    unsigned seed  = 1u;
    Show show      = Show::Full;

    // Ceiling on what a SINGLE indirect path may contribute, in linear
    // radiance. 0 turns it off.
    //
    // This is the firefly clamp, and it is a deliberate, bounded lie. A path
    // that bounces off the tarmac, finds a chrome bumper and catches the sun in
    // it carries a genuinely enormous amount of light, and it is genuinely rare
    // -- so it lands as one white pixel in a clean image and takes tens of
    // thousands of samples to be averaged away by its neighbours. Capping it
    // loses a little energy in exactly the places that were never going to
    // converge, and the picture is better for it. Direct light and the primary
    // ray are never clamped, so highlights and light sources keep their real
    // brightness.
    float clampIndirect = 24.0f;
};

// A running render.
//
// Progressive on purpose: the image is refined in passes of `batch` samples, so
// the panel can show it converging and the author can stop as soon as it is
// good enough. That matters more than it looks -- "how many samples does this
// shot need" has no answer except watching one, and a renderer that only hands
// over a finished image makes you guess it in advance and start over.
class Job {
public:
    Job() = default;
    ~Job();
    Job(const Job&)            = delete;
    Job& operator=(const Job&) = delete;

    // Begin. Cancels and joins any previous run first.
    void start(std::shared_ptr<const Scene> scene, const Settings& settings);
    // Ask the workers to stop and wait for them. Safe at any time, including on
    // a job that never started.
    void cancel();

    bool  running()      const { return m_running.load(); }
    bool  hasImage()     const { return m_pixels.load() > 0; }
    int   samplesDone()  const { return m_done.load(); }
    int   samplesTotal() const { return m_settings.samples; }
    float progress()     const;
    // Seconds since start(); frozen at the finish once the render is done.
    double elapsedSeconds() const;
    // Triangles in the accelerator, and the seconds its build took. Reported
    // because a slow render is far more often a huge scene than a slow tracer,
    // and the panel should be able to say which.
    long long triangleCount() const { return m_triangles; }
    double    buildSeconds()  const { return m_buildSeconds; }

    const Settings& settings() const { return m_settings; }

    // Copy the image out as it stands. Safe while running: every pixel is
    // divided by the number of samples its own tile has finished, so a
    // half-finished pass shows as a sharper block, never as a darker one.
    //
    // snapshotLdr writes tightly packed RGBA8 (alpha 255), tonemapped when
    // Settings::tonemap is set. snapshotHdr writes packed linear RGB floats --
    // the version worth keeping, since it is the one that can still be graded.
    bool snapshotLdr(std::vector<unsigned char>& out) const;
    bool snapshotHdr(std::vector<float>& out) const;

private:
    void run();  // the coordinator: builds the accelerator, drives the workers

    std::shared_ptr<const Scene> m_scene;
    Settings                     m_settings;

    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
    std::atomic<int>  m_done{0};     // samples finished on the slowest tile
    std::atomic<int>  m_pixels{0};   // > 0 once the buffers exist

    // Radiance sums and, per tile, how many samples went into them.
    //
    // The two have to be READ TOGETHER or the preview flickers: a worker adds a
    // pass's worth of light to the sums and only then raises the count, so a
    // reader landing between the two divides new light by an old count and the
    // tile flashes bright. Hence a lock per tile -- held for the length of one
    // merge or one copy, which is a thousand adds and never contended for long.
    std::vector<glm::vec3>         m_accum;
    std::vector<std::atomic<int>>  m_tileSamples;
    mutable std::vector<std::atomic<bool>> m_tileLock;
    int m_tilesX = 0, m_tilesY = 0;

    long long m_triangles    = 0;
    double    m_buildSeconds = 0.0;
    std::chrono::steady_clock::time_point m_started{};
    std::atomic<double> m_elapsed{0.0};
};

// The post chain's output stage on the CPU: ACES, gamma, then the colour grade
// -- the same three steps, in the same order, as the tail of composite.frag.
// Shared so a render and a screenshot of the same scene land on the same curve
// rather than merely a similar one.
glm::vec3 tonemap(const glm::vec3& linear, float exposure, const Grade& grade);

// Distance from `origin` along `dir` to the nearest surface, or 0 when the ray
// hits nothing. Brute force over every triangle, no accelerator: it is called
// once per render -- to put the lens' focus on whatever the frame is pointed at
// -- and building a BVH to answer a single question costs more than the answer.
float firstHitDistance(const Scene& scene, const glm::vec3& origin,
                       const glm::vec3& dir);

// Tile edge in pixels. Exposed because a snapshot's per-tile sample counts only
// mean anything against it, and pathcheck asserts on both.
constexpr int kTileSize = 32;

} // namespace pathtrace
