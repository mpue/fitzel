#include "fitzel/render/Renderer.hpp"

#include "fitzel/graphics/Material.hpp"
#include "fitzel/graphics/Mesh.hpp"
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
constexpr const char* kDepthVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uModel;
uniform mat4 uLightSpace;
void main() { gl_Position = uLightSpace * uModel * vec4(aPos, 1.0); }
)";

constexpr const char* kDepthFrag = R"(#version 330 core
void main() {}
)";

// Point-shadow pass: write normalized distance-to-light into an R32F cubemap.
constexpr const char* kCubeVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uModel;
uniform mat4 uVP;
out vec3 vWorld;
void main() { vec4 w = uModel * vec4(aPos, 1.0); vWorld = w.xyz; gl_Position = uVP * w; }
)";

constexpr const char* kCubeFrag = R"(#version 330 core
in vec3 vWorld;
layout(location = 0) out float oDist;
uniform vec3  uLightPos;
uniform float uFar;
void main() { oDist = length(vWorld - uLightPos) / uFar; }
)";

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
    m_queue.push_back({&mesh, &material, model, castsPointShadow, reflective,
                       opacity, forceTransparent});
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

    const int cascades = m_csm.cascadeCount();
    for (int i = 0; i < cascades; ++i) {
        m_csm.beginCascade(i);
        m_depthShader.bind();
        const glm::mat4& lightSpace = m_csm.lightMatrices()[i];
        m_depthShader.setMat4("uLightSpace", lightSpace);
        // Only the four SIDE planes (see aabbVisible): a caster sitting outside
        // the box along the light direction still throws a shadow into it, and
        // culling it on near/far would delete that shadow. Sideways is safe --
        // nothing beside the box can reach into it with the light parallel.
        const std::array<glm::vec4, 6> planes = frustumPlanes(lightSpace);
        for (std::size_t k = 0; k < m_queue.size(); ++k) {
            if (!aabbVisible(planes, m_cullBounds[k], /*count=*/4)) continue;
            m_depthShader.setMat4("uModel", m_queue[k].model);
            m_queue[k].mesh->draw();
        }
        if (extra) extra(lightSpace);
    }
    m_csm.end(m_vpWidth, m_vpHeight);
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
                if (!aabbNearPoint(m_cullBounds[q], l.position, far)) continue;
                if (!aabbVisible(planes, m_cullBounds[q])) continue;
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
    const int faces = m_envPrimed ? 1 : 6;

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
        s->setInt("uHasNormalMap", 0);
        s->setInt("uAlphaCutout", 0); // baseline: material re-enables if Cutout
        s->setFloat("uRoadFade", 0.0f); // baseline: no edge fade (road re-enables)
        s->setFloat("uRainRings", 0.0f); // baseline: no drop impacts (road re-enables)
        s->setInt("uHasWetMap", 0);      // baseline: even wetness (road re-enables)
        s->setFloat("uWetReflect", 0.0f);// baseline: wet surfaces don't mirror
                                         // (only the road turns this on)
        s->setVec3("uEmission", glm::vec3(0.0f)); // baseline: no glow
        s->setFloat("uEmissionStrength", 1.0f);
        s->setInt("uHasEmissionMap", 0);
        s->setVec2("uEmissionUVScale", glm::vec2(1.0f)); // baseline: the mesh's own UVs

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
        (r.opacity < 0.999f || r.forceTransparent ? transparent : opaque).push_back(&r);
    }
    for (const Renderable* r : opaque) drawOne(*r);
    if (!transparent.empty()) {
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
        for (const Renderable* r : transparent) drawOne(*r);
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
