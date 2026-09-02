#include "fitzel/render/Renderer.hpp"

#include "fitzel/graphics/Material.hpp"
#include "fitzel/graphics/Mesh.hpp"
#include "fitzel/graphics/Texture.hpp"
#include "fitzel/graphics/EnvironmentIBL.hpp"
#include "fitzel/scene/Camera.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <string>
#include <utility>

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>

namespace fitzel {

namespace {

// Minimal depth-only shader for the shadow passes -- embedded so the engine
// stays self-contained (no app-provided asset required).
//
// "Depth-only" does not mean "geometry-only": how much light a caster stops is
// a property of its SURFACE, and a pass that only knows where the triangles
// are puts a solid black shadow under a pane of glass and a solid rectangle
// under a leaf billboard. The coverage rules below are the ones SceneTypes.hpp
// defines, lit.frag draws with and pathtrace::coverageAt() already follows --
// which is why the offline render has never had this bug and this one did.
constexpr const char* kDepthVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUV;
uniform mat4 uModel;
uniform mat4 uLightSpace;
out vec2 vUV;
void main() { vUV = aUV; gl_Position = uLightSpace * uModel * vec4(aPos, 1.0); }
)";

constexpr const char* kDepthFrag = R"(#version 330 core
in vec2 vUV;
uniform sampler2D uTex;
uniform int   uAlphaMode; // 0 = opaque, 1 = cutout, 2 = blend (texture alpha)
uniform float uCutoff;    // cutout threshold (mode 1)
uniform float uCoverage;  // scalar opacity, glass already folded in
uniform float uDither;    // this caster's offset into the dither pattern
void main() {
    float a = uCoverage;
    if (uAlphaMode == 1) { if (texture(uTex, vUV).a < uCutoff) discard; }
    else if (uAlphaMode == 2) a *= texture(uTex, vUV).a;
    if (a >= 1.0) return;
    // Stochastic transparency. A depth map holds one occluder per texel and has
    // nowhere to write "70% blocked", so let a fraction 1-a of the texels
    // through and leave the averaging to the 5x5 PCF that reads the map back --
    // the filter is already there, and this is the quantity it is summing.
    // Interleaved gradient noise because it spreads the survivors evenly over
    // any small neighbourhood, which is exactly the neighbourhood the kernel
    // covers; a plain hash clumps and comes back as blotches. uDither
    // decorrelates casters from each other, so two panes one behind the other
    // dim the sun twice instead of both keeping the same texels and dimming
    // once.
    float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy,
                                           vec2(0.06711056, 0.00583715))) + uDither);
    if (n >= a) discard;
}
)";

// Point-shadow pass: write normalized distance-to-light into an R32F cubemap.
constexpr const char* kCubeVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUV;
uniform mat4 uModel;
uniform mat4 uVP;
out vec3 vWorld;
out vec2 vUV;
void main() {
    vec4 w = uModel * vec4(aPos, 1.0);
    vWorld = w.xyz; vUV = aUV; gl_Position = uVP * w;
}
)";

constexpr const char* kCubeFrag = R"(#version 330 core
in vec3 vWorld;
in vec2 vUV;
layout(location = 0) out float oDist;
uniform vec3  uLightPos;
uniform float uFar;
uniform sampler2D uTex;
uniform int   uAlphaMode;
uniform float uCutoff;
uniform float uCoverage;
void main() {
    // Same rules, hard-edged: this cube is read with a single tap and no
    // filter, so the cascades' dither would come back as per-texel speckle
    // rather than as a grey. A point light gets the cheap honest answer --
    // mostly-there casts, mostly-gone does not -- while the cutout test, which
    // is exact per texel, works here as well as it does there.
    float a = uCoverage;
    if (uAlphaMode == 1) { if (texture(uTex, vUV).a < uCutoff) discard; }
    else if (uAlphaMode == 2) a *= texture(uTex, vUV).a;
    if (a < 0.5) discard;
    oDist = length(vWorld - uLightPos) / uFar;
}
)";

// A glass pane refracts the sun rather than stopping it, so it dims what passes
// instead of blocking it. The scalar stand-in pathtrace::shadowFactor() uses
// for the caustics nobody is tracing -- without its tint, which a depth map has
// no room for.
constexpr float kGlassCoverage = 0.15f;

// Below this a caster is not drawn into the shadow map at all: nothing of it
// would survive the dither anyway, and skipping is the cheap path.
constexpr float kMinCoverage = 0.002f;

// "uPointPos[3]" and friends, formatted into a stack buffer.
//
// The renderer sets a dozen indexed uniforms per light per draw call, and
// building those names with std::to_string plus two concatenations cost three
// heap allocations each -- tens of thousands of them a frame once a scene has a
// few hundred objects and a full light budget. The Shader keeps its own copy of
// the name when it caches the location, so the buffer only has to outlive the
// setter call.
class Indexed {
public:
    Indexed(const char* base, int i) {
        char* p = m_buf;
        while (*base) *p++ = *base++;
        *p++ = '[';
        if (i >= 10) *p++ = static_cast<char>('0' + i / 10);
        *p++ = static_cast<char>('0' + i % 10);
        *p++ = ']';
        m_len = static_cast<std::size_t>(p - m_buf);
    }
    operator std::string_view() const { return {m_buf, m_len}; } // NOLINT

private:
    char        m_buf[48]; // longest base here is ~20 chars
    std::size_t m_len;
};

// The point-shadow uniforms are suffixed rather than indexed (uShadowFar0), and
// there are only ever kMaxShadowedPoints of them, so a table beats formatting.
constexpr const char* kShadowFarName[]  = {"uShadowFar0", "uShadowFar1",
                                           "uShadowFar2", "uShadowFar3"};
constexpr const char* kShadowBiasName[] = {"uShadowBias0", "uShadowBias1",
                                           "uShadowBias2", "uShadowBias3"};
constexpr const char* kShadowCubeName[] = {"uShadowCube0", "uShadowCube1",
                                           "uShadowCube2", "uShadowCube3"};

// Extract the 6 world-space frustum planes from a view-projection matrix
// (Gribb-Hartmann). Each plane is (nx, ny, nz, d) with the normal pointing
// inward, so a point p is inside when dot(plane.xyz, p) + plane.w >= 0.
std::array<glm::vec4, 6> frustumPlanes(const glm::mat4& m) {
    // Rows of the column-major matrix.
    const glm::vec4 r0{m[0][0], m[1][0], m[2][0], m[3][0]};
    const glm::vec4 r1{m[0][1], m[1][1], m[2][1], m[3][1]};
    const glm::vec4 r2{m[0][2], m[1][2], m[2][2], m[3][2]};
    const glm::vec4 r3{m[0][3], m[1][3], m[2][3], m[3][3]};

    std::array<glm::vec4, 6> planes{
        r3 + r0, r3 - r0, // left, right
        r3 + r1, r3 - r1, // bottom, top
        r3 + r2, r3 - r2, // near, far
    };
    for (glm::vec4& p : planes) {
        p /= glm::length(glm::vec3(p));
    }
    return planes;
}

WorldAabb worldAabb(const glm::mat4& model, const glm::vec3& localMin,
                    const glm::vec3& localMax) {
    WorldAabb b{glm::vec3(1e30f), glm::vec3(-1e30f)};
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner{
            (i & 1) ? localMax.x : localMin.x,
            (i & 2) ? localMax.y : localMin.y,
            (i & 4) ? localMax.z : localMin.z};
        const glm::vec3 w = glm::vec3(model * glm::vec4(corner, 1.0f));
        b.lo = glm::min(b.lo, w);
        b.hi = glm::max(b.hi, w);
    }
    return b;
}

// Test a world-space AABB against the first `count` frustum planes.
// Conservative (false positives possible, never false negatives).
//
// `count` exists for the shadow cascades: culling a shadow pass against all six
// planes is wrong, because an object BEHIND the light's box still casts into it.
// Testing only the four side planes keeps everything that could reach the
// cascade laterally, however far up or down the light direction it sits.
bool aabbVisible(const std::array<glm::vec4, 6>& planes, const WorldAabb& b,
                 int count = 6) {
    for (int i = 0; i < count; ++i) {
        const glm::vec4& plane = planes[i];
        // Positive vertex: the AABB corner farthest along the plane normal.
        const glm::vec3 pv{
            plane.x >= 0.0f ? b.hi.x : b.lo.x,
            plane.y >= 0.0f ? b.hi.y : b.lo.y,
            plane.z >= 0.0f ? b.hi.z : b.lo.z};
        if (glm::dot(glm::vec3(plane), pv) + plane.w < 0.0f) {
            return false; // fully outside this plane
        }
    }
    return true;
}

// Does the box come within `radius` of `centre`? Used to drop objects that are
// out of a point light's reach: the cube only stores distances out to its range,
// so anything beyond that lies behind everything the light actually lit and
// cannot shadow any of it.
bool aabbNearPoint(const WorldAabb& b, const glm::vec3& centre, float radius) {
    const glm::vec3 closest = glm::clamp(centre, b.lo, b.hi);
    const glm::vec3 d       = closest - centre;
    return glm::dot(d, d) <= radius * radius;
}

} // namespace

const glm::vec4 Renderer::kNoClip = glm::vec4(0.0f, 1.0f, 0.0f, 1.0e6f);

Renderer::Renderer(int shadowResolution, int cascades)
    : m_csm(shadowResolution, cascades),
      m_depthShader(Shader::fromSource(kDepthVert, kDepthFrag)),
      m_cubeDistShader(Shader::fromSource(kCubeVert, kCubeFrag)) {}

void Renderer::setViewport(int width, int height) {
    m_vpWidth  = width;
    m_vpHeight = height;
}

std::vector<Renderer::Submission> Renderer::submissions() const {
    std::vector<Submission> out;
    out.reserve(m_queue.size());
    for (const Renderable& r : m_queue)
        out.push_back({r.mesh, r.material, r.model, r.opacity, r.reflective,
                       r.forceTransparent});
    return out;
}

void Renderer::begin(const Camera& camera, float aspect,
                     const DirectionalLight& light) {
    m_camera = &camera;
    m_aspect = aspect;
    m_light  = light;
    m_queue.clear();
}

void Renderer::submit(const Mesh& mesh, const Material& material,
                      const glm::mat4& model, bool castsPointShadow,
                      bool reflective, float opacity, bool forceTransparent) {
    // Read the surface for the shadow passes once, here, while the material is
    // in hand. AlphaMode as SceneTypes.hpp defines it: Opaque IGNORES the map's
    // alpha (a great many opaque atlases carry one that means nothing, and
    // reading it as transparency punches holes through solid paintwork),
    // Cutout tests the texture against its cutoff, Blend multiplies.
    float          coverage  = opacity;
    int            alphaMode = 0;
    float          cutoff    = 0.5f;
    const Texture* alphaTex  = nullptr;
    if (material.get<int>("uGlass", 0) == 1)
        coverage = std::min(coverage, kGlassCoverage);
    if (const Texture* tex = material.texture("uTexture")) {
        if (material.get<int>("uAlphaCutout", 0) == 1) {
            alphaMode = 1;
            cutoff    = material.get<float>("uAlphaCutoff", 0.5f);
            alphaTex  = tex;
        } else if (forceTransparent) { // transparency lives in the texture
            alphaMode = 2;
            alphaTex  = tex;
        }
    }
    m_queue.push_back({&mesh, &material, model, castsPointShadow, reflective,
                       opacity, forceTransparent,
                       coverage, alphaMode, cutoff, alphaTex});
}

void Renderer::uploadCoverage(const Shader& shader, const Renderable& r,
                              float dither) const {
    // A plain viewport mode draws every surface solid (see renderScene), so its
    // shadows are solid too -- a clay pane throwing a quarter of a shadow would
    // be the picture disagreeing with itself. A CUTOUT still cuts: the hole in a
    // leaf card is its shape, and the lit shader keeps it in every mode.
    const bool  plain     = m_shadingMode != 0;
    const int   alphaMode = plain && r.castAlphaMode == 2 ? 0 : r.castAlphaMode;
    const float coverage  = plain ? 1.0f : r.castCoverage;
    shader.setInt("uAlphaMode", alphaMode);
    shader.setFloat("uCoverage", coverage);
    if (alphaMode != 0) {
        shader.setFloat("uCutoff", r.castCutoff);
        r.castTex->bind(0);
    }
    // Only translucent casters read it, and only the cascade pass has it.
    if (coverage < 1.0f) shader.setFloat("uDither", dither);
}

void Renderer::buildCullBounds() {
    m_cullBounds.clear();
    m_cullBounds.reserve(m_queue.size());
    for (const auto& r : m_queue) {
        m_cullBounds.push_back(
            worldAabb(r.model, r.mesh->boundsMin(), r.mesh->boundsMax()));
    }
}

void Renderer::prepareShadowsFor(const Camera& camera, float aspect,
                                 const ShadowCaster& extra) {
    // Swap the eye the cascades are cut to, run the pass, put it back. The
    // queue is untouched -- what changes is only which frustum the cascade
    // boxes are fitted around, so both panes cast from the same submitted
    // scene without it having to be built twice.
    const Camera* prevCam = m_camera;
    const float   prevAsp = m_aspect;
    m_camera = &camera;
    m_aspect = aspect;
    prepareShadows(extra);
    m_camera = prevCam;
    m_aspect = prevAsp;
}

void Renderer::prepareShadows(const ShadowCaster& extra) {
    if (!m_camera) return;

    m_csm.update(*m_camera, m_aspect, m_light.direction);

    buildCullBounds();

    // A depth pass with the depth test off writes no depth at all (and the
    // clear below is masked out too), so the cascades come back cleared and
    // nothing in the scene casts. The editor never noticed because its frame
    // loop leaves the test on; a caller that renders through the Renderer
    // without one of its own -- viewcheck -- got shadowless pictures.
    // preparePointShadows() has always set this for itself.
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    m_shadowDraws = 0;
    m_shadowTris  = 0;

    const int cascades = m_csm.cascadeCount();
    for (int i = 0; i < cascades; ++i) {
        m_csm.beginCascade(i);   // clears to 1.0, i.e. to "unshadowed"
        // Shadows switched off: the clear above is the whole pass. Leaving the
        // map cleared rather than unbinding it keeps every sampler, uniform and
        // shader branch downstream exactly as they are -- the scene is lit as if
        // nothing casts, and the four replays of the queue are what is saved.
        if (!m_shadowsEnabled) continue;
        m_depthShader.bind();
        const glm::mat4& lightSpace = m_csm.lightMatrices()[i];
        m_depthShader.setMat4("uLightSpace", lightSpace);
        m_depthShader.setInt("uTex", 0); // alpha source for cutout/blend casters
        // Only the four SIDE planes (see aabbVisible): a caster sitting outside
        // the box along the light direction still throws a shadow into it, and
        // culling it on near/far would delete that shadow. Sideways is safe --
        // nothing beside the box can reach into it with the light parallel.
        const std::array<glm::vec4, 6> planes = frustumPlanes(lightSpace);
        for (std::size_t k = 0; k < m_queue.size(); ++k) {
            const Renderable& r = m_queue[k];
            // Invisible to the eye, invisible to the sun -- unless a plain
            // viewport mode is drawing it solid, in which case it is not
            // invisible to the eye at all.
            if (m_shadingMode == 0 && r.castCoverage <= kMinCoverage) continue;
            if (!aabbVisible(planes, m_cullBounds[k], /*count=*/4)) continue;
            // The dither offset is the golden ratio times the caster's index:
            // any two casters get patterns that share as few texels as a low
            // discrepancy sequence can manage, which is what makes stacked
            // panes accumulate.
            uploadCoverage(m_depthShader, r, 0.6180339887f * static_cast<float>(k));
            m_depthShader.setMat4("uModel", r.model);
            r.mesh->draw();
            ++m_shadowDraws;
            m_shadowTris += (r.mesh->indexCount() ? r.mesh->indexCount()
                                                  : r.mesh->vertexCount()) / 3;
        }
        if (extra) extra(lightSpace, i, m_csm.splitDistances()[i]);
    }
    m_csm.end(m_vpWidth, m_vpHeight);
}

void Renderer::captureSceneCopy() {
    // The rectangle the pass is actually drawing into. Not m_vpWidth/Height at
    // the origin: with two panes up, player two's viewport starts halfway across
    // the framebuffer, and copying from (0,0) would hand its glass the other
    // player's view of the world.
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    const int w = vp[2], h = vp[3];
    if (w <= 0 || h <= 0) { m_sceneCopyW = 0; return; }

    glActiveTexture(GL_TEXTURE0 + kSceneCopyUnit);
    if (!m_sceneCopy) {
        glGenTextures(1, &m_sceneCopy);
        glBindTexture(GL_TEXTURE_2D, m_sceneCopy);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // Clamped, because a refracted lookup that walks off the edge of the
        // pane should smear the edge rather than wrap the far side of the
        // screen in -- which reads as a hole in the glass.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_sceneCopyW = m_sceneCopyH = 0;
    } else {
        glBindTexture(GL_TEXTURE_2D, m_sceneCopy);
    }
    // RGBA16F: the scene is HDR at this point, and copying it into an 8-bit
    // texture would clip every highlight the glass then shows -- a sun behind a
    // window would come through as flat white.
    if (w != m_sceneCopyW || h != m_sceneCopyH) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA,
                     GL_HALF_FLOAT, nullptr);
        m_sceneCopyW = w;
        m_sceneCopyH = h;
    }
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vp[0], vp[1], w, h);
    m_sceneCopyX = vp[0];
    m_sceneCopyY = vp[1];
    glActiveTexture(GL_TEXTURE0);
}

void Renderer::preparePointShadows() {
    // Shadow-casting point lights first, so their indices line up with the
    // cubemaps and with the lit shader's first uShadowCount lights.
    std::stable_partition(m_pointLights.begin(), m_pointLights.end(),
                          [](const PointLight& l) { return l.castShadows; });
    m_shadowedCount = 0;
    for (const PointLight& l : m_pointLights)
        if (l.castShadows) ++m_shadowedCount;
    m_shadowedCount = std::min(m_shadowedCount, kMaxShadowedPoints);
    if (m_shadowedCount == 0) return;

    while (static_cast<int>(m_pointShadows.size()) < m_shadowedCount)
        m_pointShadows.emplace_back(512);

    const glm::vec3* dirs = CubeShadowMap::faceDirs();
    const glm::vec3* ups  = CubeShadowMap::faceUps();

    buildCullBounds();

    glDisable(GL_CLIP_DISTANCE0);
    glEnable(GL_DEPTH_TEST);
    // Cull front faces so single-sided ground doesn't self-shadow (only closed
    // casters write their far side); avoids acne blacking out the lit surface.
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    m_cubeDistShader.bind();
    m_cubeDistShader.setInt("uTex", 0);
    for (int k = 0; k < m_shadowedCount; ++k) {
        const PointLight& l = m_pointLights[k];
        const float far = std::max(l.range, 0.5f);
        const glm::mat4 pr = glm::perspective(glm::radians(90.0f), 1.0f, 0.05f, far);
        m_cubeDistShader.setVec3("uLightPos", l.position);
        m_cubeDistShader.setFloat("uFar", far);
        for (int f = 0; f < 6; ++f) {
            m_pointShadows[k].beginFace(f);
            const glm::mat4 vp = pr * glm::lookAt(l.position, l.position + dirs[f], ups[f]);
            m_cubeDistShader.setMat4("uVP", vp);
            // Two tests, cheapest first. Out of the light's reach drops the
            // object for all six faces; the frustum then keeps only the face it
            // actually falls in. Together these are what stop a missile blast
            // with a twenty-metre range from redrawing a city block two dozen
            // times. Unlike the cascades this may cull on all six planes: the
            // six faces tile the whole sphere, so anything dropped here is drawn
            // by one of the others.
            const std::array<glm::vec4, 6> planes = frustumPlanes(vp);
            for (std::size_t q = 0; q < m_queue.size(); ++q) {
                const Renderable& r = m_queue[q];
                if (!r.castsPointShadow) continue; // e.g. the ground
                // Half gone or more is gone here (see kCubeFrag): the cube is
                // sampled unfiltered, so it can only answer yes or no, and a
                // window is a great deal closer to no than to yes.
                if (m_shadingMode == 0 && r.castCoverage < 0.5f) continue;
                if (!aabbNearPoint(m_cullBounds[q], l.position, far)) continue;
                if (!aabbVisible(planes, m_cullBounds[q])) continue;
                uploadCoverage(m_cubeDistShader, r, 0.0f);
                m_cubeDistShader.setMat4("uModel", r.model);
                r.mesh->draw();
            }
        }
    }
    glCullFace(GL_BACK);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_vpWidth, m_vpHeight); // restore from the 512^2 cube faces
}

void Renderer::setEnvProbeResolution(int res) {
    // Down to a power of two: the cube carries a mip chain, and a rough
    // reflection sampling a chain built off an odd size is a blurry mess at the
    // coarse end.
    int r = std::clamp(res, kMinEnvProbeRes, kMaxEnvProbeRes);
    int p = kMinEnvProbeRes;
    while (p * 2 <= r) p *= 2;
    if (p == m_envA.resolution()) return;   // nothing to do (this reallocates)

    m_envA = CubeRenderTarget(p);
    m_envB = CubeRenderTarget(p);
    m_envRead  = &m_envA;
    m_envWrite = &m_envB;
    // Both cubes are empty again, so the next capture has to be a full one and
    // any half-finished sweep is void.
    m_envFace   = 0;
    m_envPrimed = false;
}

void Renderer::bindEnvProbe(std::uint32_t unit) const {
    m_envRead->bindTexture(unit);
}

float Renderer::envProbeMaxLod() const {
    return static_cast<float>(m_envRead->mipLevels() - 1);
}

void Renderer::setEnvProbeMaxFaces(int faces) {
    m_envMaxFaces = glm::clamp(faces, 1, 6);
}

void Renderer::prepareEnvProbe(const glm::vec3& pos, const SkyDrawer& drawSky) {
    if (!m_camera) return;

    // A full capture is six complete scene renders. Paying that every frame is
    // what made a wet carriageway cost more than everything else in the frame
    // put together, so a sweep is spread one face per call. A face is then at
    // most six frames stale, which nobody can pick out of a reflection on
    // asphalt, and because the faces accumulate in the write cube and only swap
    // in once the set is complete, the lit passes never sample a half-updated
    // probe -- they keep reading the previous, consistent one.
    //
    // Until a cube has been filled once there is nothing sensible to sample, so
    // the first sweep is done in one go.
    // The sweep position is frozen once and reused for the remaining faces:
    // it usually tracks the camera, and a viewpoint that drifts between faces
    // puts a seam down the cube. Restarting when it has moved a long way covers
    // a camera cut, and a sweep that was interrupted because nothing asked for
    // a probe for a while and then resumed somewhere else entirely.
    constexpr float kSweepRestartDist = 25.0f; // metres; normal driving stays under
    if (m_envFace == 0 ||
        glm::distance(pos, m_envSweepPos) > kSweepRestartDist) {
        m_envSweepPos = pos;
        m_envFace     = 0;
    }
    // How many faces to refresh this call.
    //
    // One face per frame is what makes a probe affordable, but it is also a
    // latency budget, and the budget is bigger than it looks: a face captured
    // now is not sampled until the sweep completes and the cubes swap, so by the
    // time the lit passes read it, it is between six and twelve frames old. At
    // walking pace nobody can see that. At 500 km/h twelve frames is twenty
    // metres, and a reflection that trails the car by twenty metres is exactly
    // the reflection visibly dragging behind it.
    //
    // So the rate is bought where it is needed: from how far the viewpoint moved
    // since the last call. Standing in the editor costs one face as before; at
    // racing speed the sweep finishes in two frames (or one, if the cap allows),
    // which cuts the lag to a few frames. `moved` is per CALL, not per second,
    // which is the right measure -- what matters is how stale the cube is in
    // FRAMES, and a lower frame rate walks further between captures by itself.
    const float moved  = m_envHasLast ? glm::distance(pos, m_envLastPos) : 0.0f;
    m_envLastPos = pos;
    m_envHasLast = true;
    // ~0.45 m per frame is 100 km/h at 60 Hz; ~2.3 m is 500 km/h. So a city
    // street buys a second face and racing speed asks for everything the cap
    // gives it.
    const int  want  = 1 + static_cast<int>(moved / 0.45f);
    const int  rate  = glm::clamp(want, 1, glm::clamp(m_envMaxFaces, 1, 6));
    const int  faces = m_envPrimed ? rate : 6;

    const glm::mat4 proj = CubeRenderTarget::faceProjection(0.2f, 4000.0f);

    glDisable(GL_CLIP_DISTANCE0);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    for (int i = 0; i < faces; ++i) {
        const int f = m_envFace;
        m_envWrite->beginFace(f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        const glm::mat4 view = CubeRenderTarget::faceView(m_envSweepPos, f);
        if (drawSky) drawSky(glm::inverse(proj * view), m_envSweepPos);
        // Linear (untonemapped) so reflections match the HDR scene; skip the
        // reflective surfaces themselves and sample last frame's probe.
        renderScene(view, proj, m_envSweepPos, kNoClip, /*tonemap=*/false,
                    /*skipReflective=*/true);
        m_envFace = (m_envFace + 1) % 6;
    }

    if (m_envFace == 0) { // sweep complete
        m_envWrite->generateMipmaps();
        // The freshly captured cube becomes the one the lit passes sample.
        std::swap(m_envRead, m_envWrite);
        m_envPrimed = true;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_vpWidth, m_vpHeight);
}

void Renderer::renderScene(const glm::mat4& view, const glm::mat4& proj,
                           const glm::vec3& eye, const glm::vec4& clipPlane,
                           bool tonemap, bool skipReflective) {
    const glm::mat4 viewProj = proj * view;
    const int cascades = m_csm.cascadeCount();

    const std::array<glm::vec4, 6> planes = frustumPlanes(viewProj);
    m_lastDrawn  = 0;
    m_lastCulled = 0;

    m_csm.bindTextureArray(kShadowMapUnit);
    glEnable(GL_CLIP_DISTANCE0);

    auto drawOne = [&](const Renderable& r) {
        if (!aabbVisible(planes, worldAabb(r.model, r.mesh->boundsMin(),
                                           r.mesh->boundsMax()))) {
            ++m_lastCulled;
            return;
        }
        ++m_lastDrawn;

        Shader* s = r.material->shader();
        // Baseline: reflection off + fully opaque. A Material only uploads the
        // uniforms it defines, so without these a reflective/transparent material
        // would leave uReflectivity/uAlpha set on the shared program and later
        // draws (terrain, road) would inherit it. uAlpha carries per-object
        // opacity (1 for the opaque queue).
        s->bind();
        s->setFloat("uReflectivity", 0.0f);
        s->setFloat("uAlpha", r.opacity);
        s->setInt("uGlass", 0);
        // Whether there IS a picture of the scene behind this surface to bend.
        // Baseline like uGlass, and for the same reason: a program that kept
        // last pass's 1 would sample a copy taken from another viewport. The
        // sampler and the rectangle only matter when there is one.
        const int hasCopy = (m_sceneCopyW > 0 && m_sceneCopy) ? 1 : 0;
        s->setInt("uHasSceneCopy", hasCopy);
        if (hasCopy) {
            s->setInt("uSceneCopy", kSceneCopyUnit);
            s->setVec4("uSceneCopyRect",
                       glm::vec4(static_cast<float>(m_sceneCopyX),
                                 static_cast<float>(m_sceneCopyY),
                                 static_cast<float>(m_sceneCopyW),
                                 static_cast<float>(m_sceneCopyH)));
        }
        s->setInt("uHasNormalMap", 0);
        s->setInt("uAlphaCutout", 0); // baseline: material re-enables if Cutout
        s->setInt("uShade", m_shadingMode); // viewport shading; 0 = the material
        s->setFloat("uRoadFade", 0.0f); // baseline: no edge fade (road re-enables)
        s->setFloat("uRainRings", 0.0f); // baseline: no drop impacts (road re-enables)
        s->setFloat("uRainDensity", 0.0f); // ...and nothing falling on them
        s->setInt("uHasWetMap", 0);      // baseline: even wetness (road re-enables)
        s->setFloat("uWetReflect", 0.0f);// baseline: wet surfaces don't mirror
                                         // (only the road turns this on)
        s->setVec3("uEmission", glm::vec3(0.0f)); // baseline: no glow
        s->setFloat("uEmissionStrength", 1.0f);
        s->setInt("uHasEmissionMap", 0);
        s->setVec2("uEmissionUVScale", glm::vec2(1.0f)); // baseline: the mesh's own UVs
        s->setInt("uWindowGrid", 0);              // baseline: no procedural windows

        r.material->apply(); // binds shader + material params/textures

        s->setMat4("uModel", r.model);
        s->setMat4("uView", view);
        s->setMat4("uViewProj", viewProj);
        s->setVec3("uViewPos", eye);
        s->setVec4("uClipPlane", clipPlane);
        s->setVec3("uLightDir", m_light.direction);
        s->setVec3("uLightColor", m_light.color);
        s->setVec3("uAmbient", m_light.ambient);
        s->setVec3("uFogColor", m_fog.color);
        s->setVec3("uFogSunColor", m_fog.sunColor);
        s->setFloat("uFogDensity", m_fog.density);
        s->setFloat("uFogHeightFalloff", m_fog.heightFalloff);
        s->setFloat("uFogHeight", m_fog.height);
        s->setFloat("uExposure", m_exposure);
        s->setInt("uTonemap", tonemap ? 1 : 0);
        s->setInt("uCascadeCount", cascades);
        s->setInt("uShadowMap", kShadowMapUnit);

        // Point lights (no-ops on shaders that don't declare these uniforms).
        const int pc = std::min(static_cast<int>(m_pointLights.size()), kMaxPointLights);
        s->setInt("uPointCount", pc);
        for (int i = 0; i < pc; ++i) {
            s->setVec3(Indexed("uPointPos", i), m_pointLights[i].position);
            s->setVec3(Indexed("uPointColor", i), m_pointLights[i].color);
            s->setFloat(Indexed("uPointRange", i), m_pointLights[i].range);
        }

        // Spot lights (cone lights, e.g. headlights). Unshadowed.
        const int sc = std::min(static_cast<int>(m_spotLights.size()), kMaxSpotLights);
        s->setInt("uSpotCount", sc);
        for (int i = 0; i < sc; ++i) {
            s->setVec3(Indexed("uSpotPos", i), m_spotLights[i].position);
            s->setVec3(Indexed("uSpotDir", i), m_spotLights[i].direction);
            s->setVec3(Indexed("uSpotColor", i), m_spotLights[i].color);
            s->setFloat(Indexed("uSpotRange", i), m_spotLights[i].range);
            s->setFloat(Indexed("uSpotCosInner", i), m_spotLights[i].cosInner);
            s->setFloat(Indexed("uSpotCosOuter", i), m_spotLights[i].cosOuter);
        }
        // Point-shadow cubemaps. Always give ALL four cube samplers their own
        // units (12..15) -- even the unused ones -- so none is left aliasing
        // unit 0, where uTexture (a sampler2D) lives. A samplerCube and a
        // sampler2D pointing at the same unit is a type clash that makes the
        // driver drop the whole draw once the cube is sampled, so every lit
        // surface (the terrain) would vanish when point shadows switch on.
        s->setInt("uShadowCount", m_shadowedCount);
        static_assert(std::size(kShadowFarName) == kMaxShadowedPoints,
                      "point-shadow uniform name tables must cover every slot");
        for (int k = 0; k < kMaxShadowedPoints; ++k) {
            if (k < m_shadowedCount) {
                m_pointShadows[k].bindTexture(kPointShadowUnit + k);
                s->setFloat(kShadowFarName[k], std::max(m_pointLights[k].range, 0.5f));
                s->setFloat(kShadowBiasName[k], m_pointLights[k].shadowBias);
            } else if (m_shadowedCount > 0) {
                // Bind a real cubemap so the unit stays a complete cube texture.
                m_pointShadows[0].bindTexture(kPointShadowUnit + k);
            }
            s->setInt(kShadowCubeName[k], kPointShadowUnit + k);
        }

        // Environment probe for reflective materials. Bound for every lit draw
        // (even non-reflective ones) so this samplerCube never aliases unit 0.
        m_envRead->bindTexture(kEnvProbeUnit);
        s->setInt("uEnvProbe", kEnvProbeUnit);
        s->setFloat("uEnvMaxLod", static_cast<float>(m_envRead->mipLevels() - 1));

        // Image-based lighting from an HDRI. Bind the irradiance + prefilter
        // cubemaps (or the probe cube as a harmless fallback so these samplerCubes
        // never alias unit 0); the shader only reads them when uUseIBL == 1.
        const bool useIbl = m_ibl && m_ibl->valid() && m_iblEnabled;
        if (useIbl) {
            m_ibl->bindIrradiance(kIrradianceUnit);
            m_ibl->bindPrefilter(kPrefilterUnit);
        } else {
            m_envRead->bindTexture(kIrradianceUnit);
            m_envRead->bindTexture(kPrefilterUnit);
        }
        s->setInt("uIrradiance", kIrradianceUnit);
        s->setInt("uPrefilter", kPrefilterUnit);
        s->setInt("uUseIBL", useIbl ? 1 : 0);
        s->setFloat("uIBLIntensity", m_iblIntensity);
        s->setFloat("uPrefilterMaxLod",
                    useIbl ? static_cast<float>(m_ibl->prefilterMipLevels() - 1) : 0.0f);

        // The baked light grid. Always bound, for the same reason as the probe
        // cube above.
        if (!m_gridFallback.isValid())
            m_gridFallback = Texture3D::create(1, 1, 1, std::vector<float>(4, 0.0f));
        const bool useGrid = lightGridEnabled();
        (useGrid ? *m_gridR : m_gridFallback).bind(kLightGridUnit);
        (useGrid ? *m_gridG : m_gridFallback).bind(kLightGridUnit + 1);
        (useGrid ? *m_gridB : m_gridFallback).bind(kLightGridUnit + 2);
        s->setInt("uLightGridR", kLightGridUnit);
        s->setInt("uLightGridG", kLightGridUnit + 1);
        s->setInt("uLightGridB", kLightGridUnit + 2);
        s->setInt("uUseLightGrid", useGrid ? 1 : 0);
        s->setVec3("uLightGridLo", m_gridLo);
        s->setVec3("uLightGridHi", m_gridHi);
        s->setFloat("uLightGridIntensity", m_gridIntensity);

        for (int i = 0; i < cascades; ++i) {
            s->setMat4(Indexed("uLightSpace", i), m_csm.lightMatrices()[i]);
            s->setFloat(Indexed("uCascadeSplits", i), m_csm.splitDistances()[i]);
        }

        r.mesh->draw();
    };

    // Opaque queue first, then transparent surfaces back-to-front with alpha
    // blending and depth writes off (so they blend over the opaque scene and
    // each other without occluding). Reflective probe pass still excludes them.
    std::vector<const Renderable*> opaque, transparent;
    for (const auto& r : m_queue) {
        if (skipReflective && r.reflective) continue;
        // A plain viewport mode has nothing to blend: those modes are there to
        // show where the geometry IS, and a pane you can see through is a pane
        // you cannot point at. It is also what stops a blended surface from
        // coming back BLACK in them -- the transparent pass turns depth writes
        // off, so with one flat colour and no alpha to soften it, whichever of
        // the two faces happened to be drawn last won, and half the time that
        // is the one facing away from the light.
        const bool blended = m_shadingMode == 0 &&
                             (r.opacity < 0.999f || r.forceTransparent);
        (blended ? transparent : opaque).push_back(&r);
    }
    for (const Renderable* r : opaque) drawOne(*r);
    if (!transparent.empty()) {
        // Refraction needs the picture behind the glass, and the only moment it
        // exists is here: the opaque scene is finished and nothing transparent
        // has been drawn over it yet. Copied only when the frame has a
        // refracting surface -- every other frame, and every probe face and
        // water pass without one, pays a scan of a short list.
        bool refracts = false;
        for (const Renderable* r : transparent)
            refracts = refracts || (r->material &&
                                    r->material->get<int>("uGlass", 0) == 1);
        if (refracts) captureSceneCopy();
        else          m_sceneCopyW = 0;   // the shader falls back on its own
        // Sort by where the GEOMETRY is, not by where its model matrix says it
        // is. A mesh baked in world space -- a contrail, a city chunk, the road
        // ribbon -- is submitted with an identity matrix, so model[3] is the
        // origin for every one of them: they all got the same sort key, and
        // which one drew first was then decided by std::sort on equal keys and
        // by the order the frame's culling happened to leave them in. That order
        // changes as the camera moves, and a blended surface swapping places
        // with another between two frames is exactly the flicker you see on
        // glass facades and on the trail of the craft ahead.
        //
        // The bounds centre through the model matrix is right for both cases: it
        // is the object's own centre for a placed mesh, and the real world
        // position for a baked one.
        auto keyOf = [&](const Renderable* r) {
            const glm::vec3 c = 0.5f * (r->mesh->boundsMin() + r->mesh->boundsMax());
            const glm::vec3 d = glm::vec3(r->model * glm::vec4(c, 1.0f)) - eye;
            return glm::dot(d, d);
        };
        std::sort(transparent.begin(), transparent.end(),
            [&](const Renderable* a, const Renderable* b) {
                const float da = keyOf(a), db = keyOf(b);
                // Ties broken deterministically, so two surfaces at the same
                // distance cannot trade places from one frame to the next.
                if (da != db) return da > db;   // farthest first
                return a < b;
            });
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        for (const Renderable* r : transparent) {
            // Glass with a copy behind it composites the background ITSELF and
            // comes out opaque, so it writes depth like an opaque surface would.
            // Without that, the far side of a glass sphere paints over the near
            // side: the sort orders objects, never the triangles inside one, and
            // with depth writes off whichever half happens to be drawn last is
            // the half you see. Every other transparent surface still relies on
            // the sort and must not write.
            const bool solidGlass = refracts && r->material &&
                                    r->material->get<int>("uGlass", 0) == 1;
            glDepthMask(solidGlass ? GL_TRUE : GL_FALSE);
            drawOne(*r);
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    glDisable(GL_CLIP_DISTANCE0);
}

void Renderer::end() {
    if (!m_camera) return;
    prepareShadows();
    renderScene(m_camera->viewMatrix(),
                m_camera->projectionMatrix(m_aspect),
                m_camera->position(), kNoClip);
    m_queue.clear();
}

} // namespace fitzel
