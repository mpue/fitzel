#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
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
// WHAT IT DOES NOT DO YET: no HDRI (the environment is the gradient the CPU
// falls back to) and no depth of field. Base-colour maps, tints, the three
// alpha modes and the terrain's layers ARE here. See the kernel's header for
// the standing list and gpucheck for what the difference costs.
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

    // Compile the kernel from `shaderPath` (assets/shaders/gputrace.comp), and
    // the resolve pass beside it (gpuresolve.comp -- `resolvePath`, defaulting
    // to the same folder). False leaves the reason in error() -- including the
    // compiler's log, which is the only thing worth having when a shader will
    // not build.
    bool init(const std::string& shaderPath, const std::string& resolvePath = {});

    // Hand over a scene. Builds the accelerator (on the CPU, with the tracer's
    // own build) and uploads everything. Keeps no reference to `scene`.
    bool upload(const pathtrace::Scene& scene);

    // Re-aim without re-uploading. A camera move is the common case in a live
    // preview and it changes nothing about the scene, while the upload is the
    // expensive half -- it reads every mesh back off the GPU. The accumulator
    // still has to be thrown away afterwards (resize()), because what is in it
    // was seen from somewhere else.
    void setCamera(const pathtrace::CameraDesc& camera) { m_camera = camera; }

    // Point it at a picture of this size and throw away what was accumulated.
    // Cheap enough to call whenever the camera moves.
    bool resize(int width, int height);

    // How deep a path goes, and the ceiling on what one indirect sample may
    // contribute. The same two numbers pathtrace::Settings carries and for the
    // same reasons; set them before accumulating, and changing either means the
    // accumulator has to be thrown away (resize()) since the samples already in
    // it were drawn from a different estimator.
    void setPath(int maxBounces, float clampIndirect) {
        m_maxBounces    = maxBounces;
        m_clampIndirect = clampIndirect;
    }

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
    //
    // This READS BACK, which waits for the card: fine for a still, and not the
    // way to put a preview on the screen. See resolve().
    bool snapshotHdr(std::vector<float>& out) const;

    // Tonemap and grade the accumulator into an eight-bit texture, on the card.
    // Nothing comes back over the bus and nothing waits, so a preview can do
    // this every frame -- which is the difference between a picture that creeps
    // forward four times a second and one that just gets better.
    //
    // The curve is composite.frag's, the same one pathtrace::tonemap applies to
    // a still; gpucheck holds the two together.
    bool resolve(float exposure, const pathtrace::Grade& grade);
    // The texture resolve() writes: RGBA8, row 0 = top (so ImGui draws it with
    // its default UVs). 0 before the first resolve.
    std::uint32_t ldrTexture() const { return m_ldr; }

    int  samplesDone() const { return m_samples; }
    int  width()       const { return m_width; }
    int  height()      const { return m_height; }
    long long triangleCount() const { return m_triangles; }
    // Base-colour maps uploaded, and how many did not fit the buffer the kernel
    // reads them out of (those surfaces fall back to their flat colour). Worth
    // showing rather than hiding: a preview quietly missing a map looks like a
    // material that is simply the wrong colour.
    int  textureCount()    const { return m_textureCount; }
    int  texturesDropped() const { return m_texturesDropped; }
    const std::string& error() const { return m_error; }

    // How many traversal entries the kernel it is currently running was built
    // with -- i.e. how deep a tree it can walk. Chosen from the scene's own BVH;
    // see upload().
    int  stackSize() const { return m_stackSize; }
    // How deep the scene's tree turned out to be. The stack above follows from
    // it; worth showing because a frame that suddenly costs more is usually a
    // tree that got deeper, not a kernel that got slower.
    int  bvhDepth()  const { return m_bvhDepth; }

    // What one sample per pixel cost on this card, in milliseconds, from the
    // last dispatch the GPU has finished reporting on. 0 until there is one.
    // This is the number a caller pacing itself to a frame budget wants: how
    // many samples it can ask for and still hand the frame back in time.
    double msPerSample() const {
        return m_lastDispatchSamples > 0
                   ? m_lastDispatchMs / static_cast<double>(m_lastDispatchSamples)
                   : 0.0;
    }

private:
    void release();
    // Build the kernel with a given traversal stack, and compile one source.
    bool build(int stackSize);
    bool compile(const std::string& source);
    bool uploadBuffer(std::uint32_t& id, int binding, const void* data,
                      std::size_t bytes);

    std::uint32_t m_program = 0;
    std::uint32_t m_resolve = 0;   // the tonemap pass
    std::uint32_t m_ldr     = 0;   // what it writes, and what the viewport draws
    int           m_ldrW = 0, m_ldrH = 0;
    // One built kernel per traversal stack size. Kept rather than rebuilt: the
    // compile is most of a second on some drivers, and a scene being edited can
    // cross a depth bracket and cross straight back.
    std::unordered_map<int, std::uint32_t> m_programs;
    std::uint32_t m_accum   = 0;   // RGBA32F, the running sum
    // tris, nodes, index, mats, lamps, texture table, texture pixels,
    // terrain layers, terrain paint, triangle corners
    std::uint32_t m_ssbo[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int           m_width = 0, m_height = 0;
    int           m_samples = 0;
    int           m_lampCount = 0;
    int           m_maxBounces = 6;
    float         m_clampIndirect = 24.0f;
    long long     m_triangles = 0;
    int           m_textureCount = 0;
    int           m_texturesDropped = 0;
    int           m_layerCount = 0;
    std::string   m_source;        // the kernel, unbuilt (rebuilt per stack size)
    int           m_stackSize = 0; // traversal entries the built kernel has
    int           m_bvhDepth  = 0; // ...and the tree depth that asked for them
    std::uint32_t m_query = 0;     // GL_TIME_ELAPSED around one dispatch
    bool          m_queryPending = false;
    int           m_queriedSamples = 0;
    double        m_lastDispatchMs = 0.0;
    int           m_lastDispatchSamples = 0;
    bool          m_havePaint = false;
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
