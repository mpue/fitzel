#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "PathTrace.hpp"

// The path tracer on the GPU: the harness half. The renderer itself is
// assets/shaders/gputrace.comp, and what lives here is everything that shader
// cannot do for itself -- compile itself, be handed a scene, be dispatched
// without tripping the driver's watchdog, and be read back.
//
// THE CPU TRACER IS THE REFERENCE. Not a figure of speech: this uses the CPU
// tracer's own accelerator (pathtrace::buildBvh), its own Scene, and its own
// arithmetic transliterated term for term, and gpucheck renders the same frame
// both ways and compares them. A second renderer that is merely "about right"
// is worse than none, because from then on every difference in a picture has
// two possible explanations.
//
// WHAT IT DOES NOT DO YET: one bounce (primary ray, emission, direct light),
// no textures, no HDRI, no glass, no depth of field. See the kernel's header
// for the list and gpucheck for what that costs against the CPU.
namespace gputrace {

// Whether the current context can run this at all: OpenGL 4.3, i.e. compute
// shaders and shader storage buffers. False is a normal answer, not an error --
// the machine simply renders on the CPU.
bool available();

class Tracer {
public:
    Tracer() = default;
    ~Tracer();
    Tracer(const Tracer&)            = delete;
    Tracer& operator=(const Tracer&) = delete;

    // Compile the kernel from `shaderPath` (assets/shaders/gputrace.comp).
    // False leaves the reason in error() -- including the compiler's log, which
    // is the only thing worth having when a shader will not build.
    bool init(const std::string& shaderPath);

    // Hand over a scene. Builds the accelerator (on the CPU, with the tracer's
    // own build) and uploads everything. Keeps no reference to `scene`.
    bool upload(const pathtrace::Scene& scene);

    // Point it at a picture of this size and throw away what was accumulated.
    // Cheap enough to call whenever the camera moves.
    bool resize(int width, int height);

    // Add `samples` more samples per pixel to the accumulator.
    //
    // Deliberately not "render the whole thing": a compute dispatch is a single
    // GPU command, and Windows kills the driver if one takes longer than a
    // couple of seconds (TDR). The caller loops over small batches, which is
    // also what makes a progressive preview possible at all.
    bool accumulate(int samples);

    // Linear RGB, packed, row 0 = top -- the same layout and orientation
    // pathtrace::Job::snapshotHdr() hands over, so the two are comparable
    // without anybody flipping anything.
    bool snapshotHdr(std::vector<float>& out) const;

    int  samplesDone() const { return m_samples; }
    int  width()       const { return m_width; }
    int  height()      const { return m_height; }
    long long triangleCount() const { return m_triangles; }
    const std::string& error() const { return m_error; }

private:
    void release();
    bool uploadBuffer(std::uint32_t& id, int binding, const void* data,
                      std::size_t bytes);

    std::uint32_t m_program = 0;
    std::uint32_t m_accum   = 0;   // RGBA32F, the running sum
    std::uint32_t m_ssbo[5] = {0, 0, 0, 0, 0}; // tris, nodes, index, mats, lamps
    int           m_width = 0, m_height = 0;
    int           m_samples = 0;
    int           m_lampCount = 0;
    long long     m_triangles = 0;
    bool          m_haveScene = false;

    // What the uniforms are made of. Held field by field rather than as a whole
    // Scene: a Scene carries its triangles and its panorama, and a copy of
    // those to read five numbers off would be megabytes for nothing.
    pathtrace::CameraDesc m_camera;
    pathtrace::Sun        m_sun;
    pathtrace::FogDesc    m_fog;
    glm::vec3 m_envZenith{0.0f}, m_envHorizon{0.0f}, m_envGround{0.0f};
    float     m_envIntensity = 1.0f;

    std::string   m_error;
};

} // namespace gputrace
