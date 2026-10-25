#include "fitzel/graphics/EnvironmentIBL.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <stb_image.h>
#include <tinyexr.h>

#include "fitzel/asset/Vfs.hpp"
#include "fitzel/rhi/Device.hpp"

namespace fitzel {

namespace {

// A cube capture rig: 90-degree projection and the six face views from the origin.
const glm::mat4 kProj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
const glm::mat4 kViews[6] = {
    glm::lookAt(glm::vec3(0), glm::vec3( 1, 0, 0), glm::vec3(0, -1, 0)),
    glm::lookAt(glm::vec3(0), glm::vec3(-1, 0, 0), glm::vec3(0, -1, 0)),
    glm::lookAt(glm::vec3(0), glm::vec3( 0, 1, 0), glm::vec3(0, 0,  1)),
    glm::lookAt(glm::vec3(0), glm::vec3( 0,-1, 0), glm::vec3(0, 0, -1)),
    glm::lookAt(glm::vec3(0), glm::vec3( 0, 0, 1), glm::vec3(0, -1, 0)),
    glm::lookAt(glm::vec3(0), glm::vec3( 0, 0,-1), glm::vec3(0, -1, 0)),
};

constexpr const char* kCubeVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
out vec3 vLocal;
uniform mat4 uProj;
uniform mat4 uView;
void main() { vLocal = aPos; gl_Position = uProj * uView * vec4(aPos, 1.0); }
)";

constexpr const char* kToCubeFrag = R"(#version 330 core
in vec3 vLocal;
out vec4 FragColor;
uniform sampler2D uEquirect;
const vec2 invAtan = vec2(0.1591, 0.3183);
vec2 sampleSpherical(vec3 v) {
    vec2 uv = vec2(atan(v.z, v.x), asin(v.y));
    uv *= invAtan; uv += 0.5; return uv;
}
void main() {
    vec3 c = texture(uEquirect, sampleSpherical(normalize(vLocal))).rgb;
    FragColor = vec4(c, 1.0);
}
)";

constexpr const char* kIrradianceFrag = R"(#version 330 core
in vec3 vLocal;
out vec4 FragColor;
uniform samplerCube uEnv;
uniform float uLod;
const float PI = 3.14159265359;
void main() {
    vec3 N = normalize(vLocal);
    vec3 up    = abs(N.y) < 0.999 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    vec3 right = normalize(cross(up, N));
    up = normalize(cross(N, right));
    vec3 irr = vec3(0.0);
    float n = 0.0;
    // Irradiance is so low-frequency that a 0.05 rad step is plenty -- as long
    // as each tap reads a mip whose texels are about that wide (a 512 face is
    // ~0.003 rad per texel, its mip 4 ~0.05) instead of point-sampling the top.
    const float d = 0.05;
    for (float phi = 0.0; phi < 2.0 * PI; phi += d)
        for (float theta = 0.5 * d; theta < 0.5 * PI; theta += d) {
            vec3 t = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            vec3 s = t.x * right + t.y * up + t.z * N;
            float w = cos(theta) * sin(theta);
            irr += textureLod(uEnv, s, uLod).rgb * w;
            n += w;
        }
    // Normalised by the weights themselves (the integral of cos over the
    // hemisphere is pi, which cancels the 1/pi of the result): a constant sky
    // comes out exactly as itself, however coarse the step.
    FragColor = vec4(irr / n, 1.0);
}
)";

constexpr const char* kPrefilterFrag = R"(#version 330 core
in vec3 vLocal;
out vec4 FragColor;
uniform samplerCube uEnv;
uniform float uRoughness;
uniform float uEnvSize;
uniform float uCopyLod;
const float PI = 3.14159265359;
float radicalInverse(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
vec2 hammersley(uint i, uint n) { return vec2(float(i) / float(n), radicalInverse(i)); }
vec3 importanceGGX(vec2 xi, vec3 N, float rough) {
    float a = rough * rough;
    float phi = 2.0 * PI * xi.x;
    float ct = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float st = sqrt(1.0 - ct * ct);
    vec3 H = vec3(cos(phi) * st, sin(phi) * st, ct);
    vec3 up = abs(N.z) < 0.999 ? vec3(0, 0, 1) : vec3(1, 0, 0);
    vec3 tx = normalize(cross(up, N));
    vec3 ty = cross(N, tx);
    return normalize(tx * H.x + ty * H.y + N * H.z);
}
void main() {
    vec3 N = normalize(vLocal);
    // Roughness 0 is a mirror: every GGX sample would land on N, so the level
    // is a plain resample of the radiance cube at the matching mip.
    if (uRoughness <= 0.0) {
        FragColor = vec4(textureLod(uEnv, N, uCopyLod).rgb, 1.0);
        return;
    }
    vec3 V = N;
    const uint SAMPLES = 256u;
    float a2 = uRoughness * uRoughness;
    a2 *= a2;
    // Solid angle of one texel of the source cube's top level.
    float saTexel = 4.0 * PI / (6.0 * uEnvSize * uEnvSize);
    vec3 col = vec3(0.0);
    float wsum = 0.0;
    for (uint i = 0u; i < SAMPLES; ++i) {
        vec2 xi = hammersley(i, SAMPLES);
        vec3 H  = importanceGGX(xi, N, uRoughness);
        vec3 L  = normalize(2.0 * dot(V, H) * H - V);
        float ndl = max(dot(N, L), 0.0);
        // Filtered importance sampling: each sample reads the mip whose texel
        // covers the solid angle the sample stands for (from the GGX pdf; V=N
        // makes N.H == V.H so the pdf reduces to D/4). Explicit LOD, too: an
        // implicit one inside this non-uniform branch is undefined.
        float ndh = max(dot(N, H), 0.0);
        float dd = ndh * ndh * (a2 - 1.0) + 1.0;
        float D = a2 / (PI * dd * dd);
        float saSample = 1.0 / (float(SAMPLES) * D * 0.25 + 0.0001);
        float lod = max(0.5 * log2(saSample / saTexel) + 1.0, 0.0);
        if (ndl > 0.0) { col += textureLod(uEnv, L, lod).rgb * ndl; wsum += ndl; }
    }
    FragColor = vec4(col / max(wsum, 0.001), 1.0);
}
)";

// Load an equirectangular HDR panorama as tightly-packed RGB floats (caller
// frees with std::free). Supports Radiance .hdr (stb) and OpenEXR .exr (tinyexr).
float* loadEquirect(const std::string& path, int& w, int& h) {
    const auto dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot);
    for (char& c : ext) c = static_cast<char>(::tolower(c));

    // Bytes first: a packed build has the panorama inside the archive, where
    // neither stb nor tinyexr can open a path.
    const std::vector<std::uint8_t> bytes = vfs::read(path);
    if (bytes.empty()) {
        std::fprintf(stderr, "[Fitzel] cannot read panorama '%s'\n", path.c_str());
        return nullptr;
    }

    if (ext == ".exr") {
        float* rgba = nullptr;
        const char* err = nullptr;
        if (LoadEXRFromMemory(&rgba, &w, &h, bytes.data(), bytes.size(), &err) !=
            TINYEXR_SUCCESS) {
            std::fprintf(stderr, "[Fitzel] EXR load failed '%s': %s\n",
                         path.c_str(), err ? err : "?");
            if (err) FreeEXRErrorMessage(err);
            return nullptr;
        }
        auto* rgb = static_cast<float*>(std::malloc(sizeof(float) * 3 * w * h));
        for (int y = 0; y < h; ++y)          // flip vertically (EXR is top-down)
            for (int x = 0; x < w; ++x) {
                const int s = ((h - 1 - y) * w + x) * 4;
                const int d = (y * w + x) * 3;
                rgb[d + 0] = rgba[s + 0];
                rgb[d + 1] = rgba[s + 1];
                rgb[d + 2] = rgba[s + 2];
            }
        std::free(rgba);
        return rgb;
    }

    stbi_set_flip_vertically_on_load_thread(1);   // .hdr panoramas are bottom-up for us
    int n = 0;
    float* d = stbi_loadf_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                      &w, &h, &n, 3);
    stbi_set_flip_vertically_on_load_thread(0);   // the resting value (stb_image_impl.cpp)
    if (!d) std::fprintf(stderr, "[Fitzel] HDR load failed '%s'\n", path.c_str());
    return d;
}

// Panoramas store absolute radiance, and files disagree about that scale by
// orders of magnitude: an overcast sky averages a fraction of one, a clear noon
// sky tens, and the sun disc of a Radiance .hdr encodes 1e5 and up. Fed in raw,
// one HDRI lit the scene nicely and the next one blew it out at the very same
// Intensity -- and any texel above 65504 turns Inf in the RGB16F cubemap, where
// a single Inf poisons the whole irradiance convolution: the "everything is
// white" case. So every panorama is rescaled to one common key and clamped.
constexpr float kTargetKey   = 0.25f;   // solid-angle mean radiance after scaling
constexpr float kMaxRadiance = 8000.0f; // keeps even the sun finite in RGB16F

float luma(const float* p) {
    return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
}

// Rescales the panorama in place and returns the factor applied.
float normalizeEquirect(float* px, int w, int h) {
    // A texel's solid angle shrinks toward the poles, so weight rows by sin(theta).
    std::vector<float> rowW(static_cast<std::size_t>(h));
    double wsum = 0.0;
    for (int y = 0; y < h; ++y) {
        rowW[y] = std::sin(3.14159265f * (static_cast<float>(y) + 0.5f) / static_cast<float>(h));
        wsum += static_cast<double>(rowW[y]) * w;
    }
    if (wsum <= 0.0) return 1.0f;

    // Pass 1: sanitise (a NaN or a negative texel survives every later stage)
    // and take the weighted mean luminance.
    double sum = 0.0;
    for (int y = 0; y < h; ++y) {
        double row = 0.0;
        for (int x = 0; x < w; ++x) {
            float* p = px + (static_cast<std::size_t>(y) * w + x) * 3;
            for (int c = 0; c < 3; ++c)
                if (!std::isfinite(p[c]) || p[c] < 0.0f) p[c] = 0.0f;
            row += luma(p);
        }
        sum += row * rowW[y];
    }
    const double mean = sum / wsum;
    if (!(mean > 0.0)) return 1.0f;   // black panorama: nothing to normalise

    // Pass 2: the same mean without the highlight tail. On a clear-sky HDRI the
    // sun disc alone carries a large share of the energy, and letting it set the
    // exposure would dim the sky the eye actually reads.
    const double cap = mean * 50.0;
    double capped = 0.0;
    for (int y = 0; y < h; ++y) {
        double row = 0.0;
        for (int x = 0; x < w; ++x)
            row += std::min(static_cast<double>(
                       luma(px + (static_cast<std::size_t>(y) * w + x) * 3)), cap);
        capped += row * rowW[y];
    }
    const double key = std::max(capped / wsum, 1e-6);

    const float scale = static_cast<float>(kTargetKey / key);
    const std::size_t n = static_cast<std::size_t>(w) * h * 3;
    for (std::size_t i = 0; i < n; ++i)
        px[i] = std::min(px[i] * scale, kMaxRadiance);
    return scale;
}

rhi::TextureHandle makeCube(int size, bool mips) {
    rhi::Device& gpu = rhi::device();
    rhi::TextureDesc d;
    d.type = rhi::TextureType::Cube;
    // The faces are rendered into. WebGL2 can render to RGBA16F (with
    // EXT_color_buffer_float), never to RGB16F.
    d.format    = gpu.caps().web ? rhi::Format::RGBA16F : rhi::Format::RGB16F;
    d.width     = size;
    d.height    = size;
    d.mipLevels = mips ? 0 : 1;
    d.sampler   = rhi::SamplerDesc::linearClamp();
    if (mips) d.sampler.mipFilter = rhi::MipFilter::Linear;
    const rhi::TextureHandle tex = gpu.createTexture(d);
    if (mips) gpu.generateMipmaps(tex);
    return tex;
}

} // namespace

EnvironmentIBL::EnvironmentIBL()
    : m_toCube(Shader::fromSource(kCubeVert, kToCubeFrag)),
      m_irradiance(Shader::fromSource(kCubeVert, kIrradianceFrag)),
      m_prefilterSh(Shader::fromSource(kCubeVert, kPrefilterFrag)) {
    buildCubeMesh();
    m_captureFbo = rhi::device().createFramebuffer(rhi::FramebufferDesc{});
}

EnvironmentIBL::~EnvironmentIBL() {
    deleteMaps();
    rhi::Device& gpu = rhi::device();
    if (m_captureFbo) gpu.destroy(m_captureFbo);
    if (m_cubeVbo)    gpu.destroy(m_cubeVbo);
    if (m_cubeVao)    gpu.destroy(m_cubeVao);
}

void EnvironmentIBL::deleteMaps() {
    rhi::Device& gpu = rhi::device();
    if (m_prefilter) { gpu.destroy(m_prefilter); m_prefilter = {}; }
    if (m_irradMap)  { gpu.destroy(m_irradMap);  m_irradMap = {}; }
    if (m_envCube)   { gpu.destroy(m_envCube);   m_envCube = {}; }
    m_valid = false;
}

void EnvironmentIBL::buildCubeMesh() {
    const float v[] = {
        -1,-1,-1, -1,-1, 1, -1, 1, 1, -1, 1, 1, -1, 1,-1, -1,-1,-1, // -X
         1,-1,-1,  1, 1,-1,  1, 1, 1,  1, 1, 1,  1,-1, 1,  1,-1,-1, // +X
        -1,-1,-1,  1,-1,-1,  1,-1, 1,  1,-1, 1, -1,-1, 1, -1,-1,-1, // -Y
        -1, 1,-1, -1, 1, 1,  1, 1, 1,  1, 1, 1,  1, 1,-1, -1, 1,-1, // +Y
        -1,-1,-1, -1, 1,-1,  1, 1,-1,  1, 1,-1,  1,-1,-1, -1,-1,-1, // -Z
        -1,-1, 1,  1,-1, 1,  1, 1, 1,  1, 1, 1, -1, 1, 1, -1,-1, 1, // +Z
    };
    rhi::Device& gpu = rhi::device();
    m_cubeVbo = gpu.createBuffer(rhi::BufferKind::Vertex, rhi::BufferUsage::Static, sizeof(v), v);
    const rhi::VertexAttribute pos{0, m_cubeVbo, rhi::VertexFormat::Float3, 0, 3 * sizeof(float), 0};
    rhi::VertexArrayDesc d;
    d.attributes     = &pos;
    d.attributeCount = 1;
    m_cubeVao = gpu.createVertexArray(d);
}

void EnvironmentIBL::renderCube() const {
    rhi::Device& gpu = rhi::device();
    gpu.bindVertexArray(m_cubeVao);
    gpu.draw(rhi::Primitive::Triangles, 0, 36);
    gpu.bindVertexArray(rhi::VertexArrayHandle{});
}

EnvironmentIBL::Panorama EnvironmentIBL::loadPanorama(const std::string& path) {
    Panorama pano;
    int w = 0, h = 0;
    float* px = loadEquirect(path, w, h);
    if (!px) return pano;
    pano.exposureScale = normalizeEquirect(px, w, h);
    pano.width  = w;
    pano.height = h;
    pano.pixels.assign(px, px + static_cast<std::size_t>(w) * h * 3);
    std::free(px);
    return pano;
}

bool EnvironmentIBL::load(const std::string& path) {
    if (!m_toCube.isValid() || !m_irradiance.isValid() || !m_prefilterSh.isValid()) {
        std::fprintf(stderr, "[Fitzel] IBL shaders did not compile; '%s' not loaded\n",
                     path.c_str());
        return false;
    }
    int w = 0, h = 0;
    float* pixels = loadEquirect(path, w, h);
    if (!pixels) return false;
    rhi::Device& gpu = rhi::device();
    const int maxTex = gpu.caps().maxTextureSize;
    if (w > maxTex || h > maxTex) {
        std::fprintf(stderr, "[Fitzel] panorama '%s' is %dx%d, the GPU takes at most %d\n",
                     path.c_str(), w, h, maxTex);
        std::free(pixels);
        return false;
    }
    m_exposureScale = normalizeEquirect(pixels, w, h);

    // Save the state we touch, so we don't disturb the caller's frame.
    const rhi::Rect              prevVp    = gpu.viewport();
    const rhi::FramebufferHandle prevFbo   = gpu.boundFramebuffer();
    const rhi::TextureHandle     prevTex0  = gpu.boundTexture(0);
    const rhi::ProgramHandle     prevProg  = gpu.boundProgram();
    const rhi::RenderState       prevState = gpu.renderState();

    // Upload the panorama to a float 2D texture.
    rhi::TextureDesc hd;
    hd.format  = rhi::Format::RGB16F;
    hd.width   = w;
    hd.height  = h;
    hd.sampler = rhi::SamplerDesc::linearClamp();
    const rhi::TextureHandle hdr = gpu.createTexture(hd, pixels, rhi::Format::RGB32F);
    std::free(pixels);

    deleteMaps();
    const int   kEnv = 512, kIrr = 32, kPre = 128;
    m_envCube   = makeCube(kEnv, true);
    m_irradMap  = makeCube(kIrr, false);
    m_prefilter = makeCube(kPre, true);

    gpu.setDepthTest(false);
    gpu.setCullEnabled(false);
    gpu.setScissorTest(false);
    gpu.setBlendEnabled(false);
    gpu.bindFramebuffer(m_captureFbo);

    bool complete = true;
    auto renderFaces = [&](const Shader& sh, rhi::TextureHandle cube, int size, int mip) {
        // A depth buffer of the face's size, for as long as these faces take.
        rhi::TextureDesc dd;
        dd.format = rhi::Format::Depth24;
        dd.width  = size;
        dd.height = size;
        dd.usage  = rhi::UsageRenderTarget;
        const rhi::TextureHandle depth = gpu.createTexture(dd);
        gpu.setViewport(0, 0, size, size);
        for (int f = 0; f < 6; ++f) {
            sh.setMat4("uView", kViews[f]);
            rhi::FramebufferDesc fb;
            fb.color[0]      = rhi::Attachment{cube, mip, f};
            fb.depth.texture = depth;
            gpu.setAttachments(m_captureFbo, fb);
            if (f == 0 && !gpu.framebufferComplete(m_captureFbo)) {
                complete = false;
                break;
            }
            gpu.clear(rhi::ClearColor | rhi::ClearDepth);
            renderCube();
        }
        gpu.setAttachments(m_captureFbo, rhi::FramebufferDesc{});
        gpu.destroy(depth);
    };

    // 1) Equirect panorama -> radiance cubemap, then build its mip chain.
    m_toCube.bind();
    m_toCube.setInt("uEquirect", 0);
    m_toCube.setMat4("uProj", kProj);
    gpu.bindTexture(0, hdr);
    renderFaces(m_toCube, m_envCube, kEnv, 0);
    gpu.generateMipmaps(m_envCube);
    // The radiance cube is what the next two passes read (uEnv, unit 0).
    gpu.bindTexture(0, m_envCube);

    // 2) Diffuse irradiance convolution, reading mip 4 (32 px faces).
    if (complete) {
        m_irradiance.bind();
        m_irradiance.setInt("uEnv", 0);
        m_irradiance.setFloat("uLod", 4.0f);
        m_irradiance.setMat4("uProj", kProj);
        renderFaces(m_irradiance, m_irradMap, kIrr, 0);
    }

    // 3) Specular prefilter: one roughness per mip level.
    if (complete) {
        m_prefilterSh.bind();
        m_prefilterSh.setInt("uEnv", 0);
        m_prefilterSh.setFloat("uEnvSize", static_cast<float>(kEnv));
        // Mip 0 at roughness 0 copies the radiance mip of the same size.
        m_prefilterSh.setFloat("uCopyLod", std::log2(static_cast<float>(kEnv) / kPre));
        m_prefilterSh.setMat4("uProj", kProj);
        for (int mip = 0; mip < m_prefilterMips && complete; ++mip) {
            const int mipSize = kPre >> mip;
            const float rough = static_cast<float>(mip) /
                                static_cast<float>(m_prefilterMips - 1);
            m_prefilterSh.setFloat("uRoughness", rough);
            renderFaces(m_prefilterSh, m_prefilter, mipSize, mip);
        }
    }

    gpu.bindFramebuffer(prevFbo);
    gpu.setViewport(prevVp);
    gpu.setRenderState(prevState);
    gpu.destroy(hdr);
    gpu.bindTexture(0, rhi::TextureHandle{});   // the cube comes off unit 0 too
    gpu.bindTexture(0, prevTex0);
    gpu.bindProgram(prevProg);

    if (!complete) {
        std::fprintf(stderr, "[Fitzel] IBL capture framebuffer incomplete; '%s' not loaded\n",
                     path.c_str());
        deleteMaps();
        return false;
    }
    m_valid = true;
    std::printf("[Fitzel] IBL environment loaded: %s (%dx%d, normalised x%.4g)\n",
                path.c_str(), w, h, m_exposureScale);
    return true;
}

void EnvironmentIBL::bindIrradiance(std::uint32_t unit) const {
    rhi::device().bindTexture(static_cast<int>(unit), m_irradMap);
}
void EnvironmentIBL::bindPrefilter(std::uint32_t unit) const {
    rhi::device().bindTexture(static_cast<int>(unit), m_prefilter);
}
void EnvironmentIBL::bindEnvCube(std::uint32_t unit) const {
    rhi::device().bindTexture(static_cast<int>(unit), m_envCube);
}

} // namespace fitzel
