#include "fitzel/render/OcclusionCuller.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>

#include "fitzel/rhi/Device.hpp"

namespace fitzel {

namespace {

// The copy read back, and the buffer it is moved into: half as wide, so a texel
// of the second has about four samples of the first and a steady camera leaves
// no holes in it. Small on purpose -- moving 256 x 144 samples is a fraction of a
// millisecond, and what it misses (a box under a dozen screen pixels) would have
// cost less to draw than to argue about.
constexpr int kCopyWidth   = 256;
constexpr int kMovedWidth  = kCopyWidth / 2;
// A texel with fewer samples than this may be half a silhouette over a hole.
constexpr int kMinSamples  = 2;
// Older than this past its capture rhythm, the copy is too far from what is on
// screen to bet on; further than this from where it was taken, likewise (a cut).
constexpr int   kMaxAgePastInterval = 4;
constexpr float kMaxTravel          = 60.0f;
// The bias of the comparison in depth-buffer units, and the room a box gets on
// top of its own bounds: a crown swaying, a figure's arm past its last pose.
constexpr float kDepthBias  = 4.0e-7f;
constexpr float kPadRelative = 0.02f;
constexpr float kPadMetres   = 0.05f;
// Below this share of hidden tests for this many frames the view goes lean.
constexpr int   kIdleInterval = 8;
constexpr int   kLeanFrames   = 120;
constexpr float kLeanYield    = 0.02f;
constexpr int   kDepthUnit    = 28;   // docs/invariants.md, section 1

constexpr const char* kReduceVert = R"(#version 330 core
void main() {
    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

// Every source pixel under this one, the farthest wins. Not a chain of 2 x 2
// passes: this touches the same pixels once each, with one target and one draw.
constexpr const char* kReduceFrag = R"(#version 330 core
uniform sampler2D uDepth;
uniform vec2 uSrc;
uniform vec2 uDst;
out vec4 fragColor;
void main() {
    ivec2 src = ivec2(uSrc), dst = ivec2(uDst);
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 a = (p * src) / dst;
    ivec2 b = ((p + ivec2(1)) * src + dst - ivec2(1)) / dst;
    float far = 0.0;
    for (int y = a.y; y < b.y; ++y)
        for (int x = a.x; x < b.x; ++x)
            far = max(far, texelFetch(uDepth, ivec2(x, y), 0).r);
    fragColor = vec4(far);
}
)";

} // namespace

OcclusionCuller::~OcclusionCuller() {
    rhi::Device& gpu = rhi::device();
    for (Slot& s : m_slots) {
        if (s.fence) gpu.destroy(s.fence);
        if (s.buffer) gpu.destroy(s.buffer);
    }
    releaseTarget();
    if (m_noVertices) gpu.destroy(m_noVertices);
}

void OcclusionCuller::releaseTarget() {
    rhi::Device& gpu = rhi::device();
    if (m_targetFbo) gpu.destroy(m_targetFbo);
    if (m_target) gpu.destroy(m_target);
    m_targetFbo = {};
    m_target    = {};
    m_targetW = m_targetH = 0;
}

void OcclusionCuller::harvest() {
    rhi::Device& gpu = rhi::device();
    // Newest first: whatever is older than what is taken is no use to anybody.
    Slot* newest = nullptr;
    for (Slot& s : m_slots) {
        if (!s.fence || !gpu.fenceSignaled(s.fence, 0)) continue;
        if (!newest || s.frame > newest->frame) newest = &s;
    }
    if (!newest) return;
    if (newest->frame > m_sourceFrame) {
        m_source.resize(static_cast<std::size_t>(newest->w) * newest->h);
        gpu.readBuffer(newest->buffer, 0, m_source.size() * sizeof(float), m_source.data());
        m_sourceW     = newest->w;
        m_sourceH     = newest->h;
        m_sourceVP    = newest->viewProj;
        m_sourceEye   = newest->eye;
        m_sourceFrame = newest->frame;
        m_sourceFresh = true;
    }
    for (Slot& s : m_slots) {
        if (!s.fence || s.frame > newest->frame) continue;
        if (!gpu.fenceSignaled(s.fence, 0)) continue;
        gpu.destroy(s.fence);
        s.fence = {};
    }
}

void OcclusionCuller::feedDepth(const float* depth, int w, int h, const glm::mat4& viewProj,
                                const glm::vec3& eye) {
    m_source.assign(depth, depth + static_cast<std::size_t>(w) * h);
    m_sourceW     = w;
    m_sourceH     = h;
    m_sourceVP    = viewProj;
    m_sourceEye   = eye;
    m_sourceFrame = m_frame;
    m_sourceFresh = true;
}

void OcclusionCuller::reproject() {
    const int h = std::max(4, (m_sourceH + 1) / 2);
    if (m_w != kMovedWidth || m_h != h) {
        m_w = kMovedWidth;
        m_h = h;
        m_moved.resize(static_cast<std::size_t>(m_w) * m_h);
        m_samples.resize(m_moved.size());
    }
    std::fill(m_moved.begin(), m_moved.end(), 0.0f);
    std::fill(m_samples.begin(), m_samples.end(), static_cast<unsigned char>(0));

    // From the copy's clip space straight to this frame's, column by column so a
    // row only has to add one term per sample. In doubles: nearly all of a depth
    // buffer is within a thousandth of 1, where everything a float matrix inverse
    // gets slightly wrong is metres of distance -- enough to sink a box behind the
    // wall it stands in front of.
    const glm::dmat4 toNow = glm::dmat4(m_vp) * glm::inverse(glm::dmat4(m_sourceVP));
    const glm::dvec4 cx = toNow[0], cy = toNow[1], cz = toNow[2], cw = toNow[3];
    const double sx = 2.0 / static_cast<double>(m_sourceW);
    const double sy = 2.0 / static_cast<double>(m_sourceH);
    const double mw = static_cast<double>(m_w), mh = static_cast<double>(m_h);
    for (int y = 0; y < m_sourceH; ++y) {
        const double ny = (static_cast<double>(y) + 0.5) * sy - 1.0;
        const glm::dvec4 row = cy * ny + cw;
        const float* in = m_source.data() + static_cast<std::size_t>(y) * m_sourceW;
        for (int x = 0; x < m_sourceW; ++x) {
            const double nx = (static_cast<double>(x) + 0.5) * sx - 1.0;
            const double nz = 2.0 * static_cast<double>(in[x]) - 1.0;
            const glm::dvec4 c = row + cx * nx + cz * nz;
            if (c.w <= 1.0e-5) continue;      // has gone behind the eye
            const double inv = 1.0 / c.w;
            const double ox = (c.x * inv * 0.5 + 0.5) * mw;
            const double oy = (c.y * inv * 0.5 + 0.5) * mh;
            if (ox < 0.0 || oy < 0.0 || ox >= mw || oy >= mh) continue;
            const double d = c.z * inv * 0.5 + 0.5;
            if (d < 0.0) continue;            // in front of the near plane now
            const std::size_t at =
                static_cast<std::size_t>(oy) * m_w + static_cast<std::size_t>(ox);
            // The farthest again. A nearer sample is only a part of this texel,
            // and the other part may be the far one.
            const float df = static_cast<float>(d);
            if (df > m_moved[at]) m_moved[at] = df;
            if (m_samples[at] < 255) ++m_samples[at];
        }
    }
    for (std::size_t i = 0; i < m_moved.size(); ++i)
        if (m_samples[i] < kMinSamples || m_moved[i] > 1.0f) m_moved[i] = 1.0f;

    if (m_levels.empty() || m_levels[0].w != m_w || m_levels[0].h != m_h) {
        m_levels.clear();
        int w = m_w, h2 = m_h;
        for (;;) {
            Level l;
            l.w = w;
            l.h = h2;
            l.depth.resize(static_cast<std::size_t>(w) * h2);
            m_levels.push_back(std::move(l));
            if (w == 1 && h2 == 1) break;
            w  = (w + 1) / 2;
            h2 = (h2 + 1) / 2;
        }
    }
}

void OcclusionCuller::beginFrame(const glm::mat4& viewProj, const glm::vec3& eye) {
    const auto t0 = std::chrono::steady_clock::now();
    // The last frame's yield decides how often this one captures.
    m_stats.tested = m_tested;
    m_stats.hidden = m_hidden;
    if (m_ready && m_tested > 0) {
        if (static_cast<float>(m_hidden) >= kLeanYield * static_cast<float>(m_tested)) {
            m_lean     = 0;
            m_interval = 1;
        } else if (++m_lean >= kLeanFrames) {
            m_interval = kIdleInterval;
        }
    }
    m_tested = m_hidden = 0;

    ++m_frame;
    m_vp     = viewProj;
    m_eye    = eye;
    m_ready  = false;
    m_carved = false;

    harvest();
    const std::int64_t age = m_frame - m_sourceFrame;
    const bool usable = m_sourceFrame >= 0 && !m_source.empty() &&
                        age <= m_interval + kMaxAgePastInterval &&
                        glm::distance(eye, m_sourceEye) <= kMaxTravel;
    m_stats.interval = m_interval;
    m_stats.age      = usable ? static_cast<int>(age) : -1;
    if (usable && (m_sourceFresh || viewProj != m_lastMovedVP)) {
        reproject();
        m_lastMovedVP = viewProj;
        m_sourceFresh = false;
    }
    if (usable && !m_levels.empty()) {
        // What is carved below is carved from this frame's copy only.
        std::copy(m_moved.begin(), m_moved.end(), m_levels[0].depth.begin());
        m_carved = true;
    }
    m_stats.updateMs = std::chrono::duration<float, std::milli>(
                           std::chrono::steady_clock::now() - t0).count();
}

namespace {

// A box on this frame's screen: its rectangle in NDC and the depth-buffer range
// it covers. `cut` when the eye plane goes through it, and then the rectangle is
// the whole screen and the range starts at the eye; `behind` when nothing of it
// is in front.
struct Projected {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    float z0 = 1e30f, z1 = -1e30f;
    bool  cut = false, behind = false;
};

Projected project(const glm::mat4& vp, const glm::vec3& lo, const glm::vec3& hi) {
    const glm::vec4 xl = vp[0] * lo.x, xh = vp[0] * hi.x;
    const glm::vec4 yl = vp[1] * lo.y, yh = vp[1] * hi.y;
    const glm::vec4 zl = vp[2] * lo.z + vp[3], zh = vp[2] * hi.z + vp[3];
    Projected p;
    int inFront = 0;
    for (int i = 0; i < 8; ++i) {
        const glm::vec4 c = ((i & 1) ? xh : xl) + ((i & 2) ? yh : yl) + ((i & 4) ? zh : zl);
        if (c.w <= 1.0e-4f) { p.cut = true; continue; }
        ++inFront;
        const float inv = 1.0f / c.w;
        const float nx = c.x * inv, ny = c.y * inv, nz = c.z * inv;
        p.x0 = std::min(p.x0, nx);
        p.x1 = std::max(p.x1, nx);
        p.y0 = std::min(p.y0, ny);
        p.y1 = std::max(p.y1, ny);
        p.z0 = std::min(p.z0, nz);
        p.z1 = std::max(p.z1, nz);
    }
    p.behind = inFront == 0;
    return p;
}

} // namespace

void OcclusionCuller::carve(const glm::vec3& lo, const glm::vec3& hi) {
    if (!m_carved) return;
    const glm::vec3 pad = (hi - lo) * kPadRelative + glm::vec3(kPadMetres);
    const Projected p = project(m_vp, lo - pad, hi + pad);
    if (p.behind) return;
    Level& base = m_levels[0];
    // A box round the eye opens everything up to its far side, all over the
    // screen: a cockpit moves with the camera and its pixels are stale everywhere
    // it can be, while what lies past it never moved.
    int x0 = 0, y0 = 0, x1 = base.w - 1, y1 = base.h - 1;
    float nearZ = 0.0f;
    if (!p.cut) {
        // One texel of room each way: the rectangle is the box's, the texel that
        // holds its last pixel is not.
        x0 = std::max(x0, static_cast<int>(std::floor((p.x0 * 0.5f + 0.5f) * base.w)) - 1);
        y0 = std::max(y0, static_cast<int>(std::floor((p.y0 * 0.5f + 0.5f) * base.h)) - 1);
        x1 = std::min(x1, static_cast<int>(std::floor((p.x1 * 0.5f + 0.5f) * base.w)) + 1);
        y1 = std::min(y1, static_cast<int>(std::floor((p.y1 * 0.5f + 0.5f) * base.h)) + 1);
        nearZ = std::max(0.0f, p.z0 * 0.5f + 0.5f) - kDepthBias;
    }
    const float farZ = std::min(1.0f, p.z1 * 0.5f + 0.5f) + kDepthBias;
    for (int y = y0; y <= y1; ++y) {
        float* row = base.depth.data() + static_cast<std::size_t>(y) * base.w;
        for (int x = x0; x <= x1; ++x)
            if (row[x] >= nearZ && row[x] <= farZ) row[x] = 1.0f;
    }
}

void OcclusionCuller::endCarve() {
    if (!m_carved) return;
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 1; i < m_levels.size(); ++i) {
        const Level& a = m_levels[i - 1];
        Level&       b = m_levels[i];
        for (int y = 0; y < b.h; ++y) {
            const int y0 = y * 2, y1 = std::min(y0 + 1, a.h - 1);
            const float* r0 = a.depth.data() + static_cast<std::size_t>(y0) * a.w;
            const float* r1 = a.depth.data() + static_cast<std::size_t>(y1) * a.w;
            float* out = b.depth.data() + static_cast<std::size_t>(y) * b.w;
            for (int x = 0; x < b.w; ++x) {
                const int x0 = x * 2, x1 = std::min(x0 + 1, a.w - 1);
                out[x] = std::max(std::max(r0[x0], r0[x1]), std::max(r1[x0], r1[x1]));
            }
        }
    }
    m_ready = true;
    m_stats.updateMs += std::chrono::duration<float, std::milli>(
                            std::chrono::steady_clock::now() - t0).count();
}

bool OcclusionCuller::occluded(const glm::vec3& lo, const glm::vec3& hi) const {
    if (!m_ready) return false;
    ++m_tested;
    const glm::vec3 pad = (hi - lo) * kPadRelative + glm::vec3(kPadMetres);
    const Projected p = project(m_vp, lo - pad, hi + pad);
    if (p.cut || p.behind || p.z0 < -1.0f) return false;
    const Level& base = m_levels[0];
    // The rectangle with a texel of room: the moved copy's edges are only good to
    // about that, and a box that fits inside a single texel at a silhouette would
    // otherwise be judged by that texel alone.
    const int x0 = std::max(0, static_cast<int>(std::floor((p.x0 * 0.5f + 0.5f) * base.w)) - 1);
    const int y0 = std::max(0, static_cast<int>(std::floor((p.y0 * 0.5f + 0.5f) * base.h)) - 1);
    const int x1 = std::min(base.w - 1, static_cast<int>(std::floor((p.x1 * 0.5f + 0.5f) * base.w)) + 1);
    const int y1 = std::min(base.h - 1, static_cast<int>(std::floor((p.y1 * 0.5f + 0.5f) * base.h)) + 1);
    if (x1 < x0 || y1 < y0) return false;   // off screen: the frustum's business

    // The level where the rectangle is at most four texels each way: exact enough
    // for a hidden box to be found so, sixteen reads at the very worst.
    int level = 0;
    while (level + 1 < static_cast<int>(m_levels.size()) &&
           ((x1 >> level) - (x0 >> level) > 3 || (y1 >> level) - (y0 >> level) > 3))
        ++level;
    const Level& l = m_levels[level];
    const float nearZ = p.z0 * 0.5f + 0.5f - kDepthBias;
    for (int y = y0 >> level; y <= (y1 >> level); ++y) {
        const float* row = l.depth.data() + static_cast<std::size_t>(y) * l.w;
        for (int x = x0 >> level; x <= (x1 >> level); ++x)
            if (row[x] >= nearZ) return false;
    }
    ++m_hidden;
    return true;
}

bool OcclusionCuller::initGpu() {
    if (m_gpuTried) return m_reduce.isValid();
    m_gpuTried = true;
    rhi::Device& gpu = rhi::device();
    if (gpu.caps().web) return false;
    m_reduce = Shader::fromSource(kReduceVert, kReduceFrag);
    if (!m_reduce.isValid()) return false;
    m_noVertices = gpu.createVertexArray(rhi::VertexArrayDesc{});
    return true;
}

void OcclusionCuller::captureDepth(rhi::TextureHandle depth, int w, int h) {
    if (!depth || w <= 0 || h <= 0) return;
    if (m_interval > 1 && m_frame % m_interval != 0) return;
    if (!initGpu()) return;
    rhi::Device& gpu = rhi::device();

    // A free slot, or this frame is not read: a copy still on its way is not
    // waited for, and with a stalled GPU nothing could use another one anyway.
    Slot* slot = nullptr;
    for (Slot& s : m_slots)
        if (!s.fence) { slot = &s; break; }
    if (!slot) return;

    // Even, so the moved copy has exactly half of it each way.
    const int tw = kCopyWidth;
    const int th = std::clamp(2 * static_cast<int>(std::lround(
                                  0.5f * static_cast<float>(tw) * static_cast<float>(h) /
                                  static_cast<float>(w))), 8, 512);
    if (tw != m_targetW || th != m_targetH) {
        releaseTarget();
        rhi::TextureDesc d;
        d.format = rhi::Format::R32F;
        d.width  = tw;
        d.height = th;
        m_target = gpu.createTexture(d);
        rhi::FramebufferDesc f;
        f.color[0].texture = m_target;
        m_targetFbo = gpu.createFramebuffer(f);
        m_targetW = tw;
        m_targetH = th;
    }
    const std::size_t bytes = static_cast<std::size_t>(tw) * th * sizeof(float);
    if (!slot->buffer)
        slot->buffer = gpu.createBuffer(rhi::BufferKind::Readback, rhi::BufferUsage::Stream, bytes);
    else if (gpu.bufferSize(slot->buffer) != bytes)
        gpu.bufferData(slot->buffer, bytes, nullptr, rhi::BufferUsage::Stream);

    const rhi::FramebufferHandle prevFbo = gpu.boundFramebuffer();
    const rhi::Rect              prevVp  = gpu.viewport();
    const rhi::RenderState       prevSt  = gpu.renderState();

    rhi::RenderState st;                  // nothing but the one triangle
    st.depth.test  = false;
    st.depth.write = false;
    st.raster.cull = false;
    st.colorMask   = rhi::ColorMask{true, true, true, true};
    gpu.setRenderState(st);
    gpu.bindFramebuffer(m_targetFbo);
    gpu.setViewport(0, 0, tw, th);
    m_reduce.bind();
    gpu.bindTexture(kDepthUnit, depth);
    m_reduce.setInt("uDepth", kDepthUnit);
    m_reduce.setVec2("uSrc", glm::vec2(static_cast<float>(w), static_cast<float>(h)));
    m_reduce.setVec2("uDst", glm::vec2(static_cast<float>(tw), static_cast<float>(th)));
    gpu.bindVertexArray(m_noVertices);
    gpu.draw(rhi::Primitive::Triangles, 0, 3);
    gpu.bindVertexArray({});
    gpu.bindTexture(kDepthUnit, {});

    gpu.readPixelsToBuffer(m_targetFbo, 0, rhi::Rect{0, 0, tw, th}, rhi::Format::R32F,
                           slot->buffer, 0);
    slot->fence    = gpu.createFence();
    slot->w        = tw;
    slot->h        = th;
    slot->viewProj = m_vp;
    slot->eye      = m_eye;
    slot->frame    = m_frame;

    gpu.bindFramebuffer(prevFbo);
    gpu.setViewport(prevVp);
    gpu.setRenderState(prevSt);
}

} // namespace fitzel
