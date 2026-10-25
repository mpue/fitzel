// fogcheck -- render volfog.frag on its own and write the frames out as PNGs.
//
// The same problem skycheck.cpp exists for, one layer lower down. Volumetric fog
// is the hardest thing in this program to get a plain look at: in the editor it
// is half-resolution, tent-blurred, composited under bloom and a tonemapper,
// mixed with the closed-form haze in lit.frag, and only visible at all from a
// camera that happens to be standing inside the volume. So "the fog looks wrong"
// has never had anywhere to be checked -- and a density field is exactly the
// kind of thing that is wrong for arithmetic reasons a screenshot cannot argue
// with, while reasoning about noise functions quietly agrees with itself.
//
// This marches volfog.frag at FULL resolution against a synthetic depth buffer
// (a ground plane and sky), composites it over a plain gradient, and writes the
// result. No upsample and no post: what comes out is the field, not the filter.
//
// It bakes the noise through FogNoise.cpp -- the same translation unit the
// engine uses, deliberately, because a check that renders a different field from
// the one the engine renders is wrong in the way that looks right.
//
// It is not a test: nothing here passes or fails. It is a way of SEEING.
//
//   fogcheck [outDir] [shaderDir]
//   Writes fog_<name>.png -- one per camera and per vertical-detail setting.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fitzel/core/Window.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/rhi/Device.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/FogMedium.hpp"
#include "../src/FogNoise.hpp"

namespace fs = std::filesystem;

namespace {

constexpr int kW = 960, kH = 540;

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// volfog.frag takes its view ray from gl_FragCoord and declares no varying at
// all, so the proxy box is not needed to run it -- any stage that covers the
// screen will do, and covering the WHOLE screen is what a look at the field
// wants anyway. sky.vert is that stage; its unused vNdc output is legal.
constexpr const char* kComposeFrag = R"(#version 330 core
// Put the marched fog over something, so it has a silhouette to be judged
// against. Deliberately plain: a two-stop sky and a flat ground, no haze and no
// shading, because anything richer here would be a second thing to blame.
in  vec2 vNdc;
out vec4 FragColor;
uniform sampler2D uFog;
uniform mat4  uInvViewProj;
uniform vec3  uCamPos;
uniform vec3  uSunDir;
void main() {
    vec2 uv = vNdc * 0.5 + 0.5;
    vec4 farH = uInvViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 rd = normalize(farH.xyz / farH.w - uCamPos);

    vec3 bg;
    if (rd.y > 0.0) {
        bg = mix(vec3(0.42, 0.52, 0.68), vec3(0.10, 0.20, 0.42), sqrt(rd.y));
        bg += vec3(1.0, 0.85, 0.6) * pow(max(dot(rd, uSunDir), 0.0), 300.0) * 6.0;
    } else {
        // The ground: a checker, so a shaft crossing it and the fog's own
        // shadowing can be told apart from a gradient.
        vec3 p = uCamPos + rd * (-uCamPos.y / rd.y);
        float c = mod(floor(p.x / 20.0) + floor(p.z / 20.0), 2.0);
        bg = mix(vec3(0.14, 0.13, 0.11), vec3(0.20, 0.19, 0.16), c);
    }

    vec4 f = texture(uFog, uv);
    vec3 col = f.rgb + bg * f.a;          // the march's own operator
    col = col / (col + 1.0);              // Reinhard, so the in-scatter does not clip
    FragColor = vec4(pow(col, vec3(1.0 / 2.2)), 1.0);
}
)";

// One frame's worth of fog, named so the file it lands in says what it shows.
struct Shot {
    const char* name;
    float pitchDeg;     // where the camera looks
    float yawDeg;       // relative to the sun: 0 = straight into it
    float eyeY;         // inside the layer, or above it looking down
    float vertical;     // FogMedium::verticalDetail -- what this tool is FOR
};

} // namespace

int main(int argc, char** argv) {
    const fs::path outDir = (argc > 1) ? fs::path(argv[1]) : fs::path(".");
    const fs::path shDir  = (argc > 2) ? fs::path(argv[2])
                                       : fs::path(argv[0]).parent_path() / "assets" / "shaders";
    // An off-screen context: the Window owns the device and makes it current.
    std::unique_ptr<fitzel::Window> window;
    try {
        window = std::make_unique<fitzel::Window>(fitzel::WindowConfig{
            .width = 64, .height = 64, .title = "fogcheck", .vsync = false, .visible = false});
    } catch (const std::exception&) {
        std::printf("[FAIL] no GL context\n");
        return 2;
    }
    using namespace fitzel::rhi;
    Device& gpu = device();
    std::printf("%s\n", gpu.caps().apiName);

    // The compiler's own words go to stderr when a stage fails.
    const std::string vs = readFile(shDir / "sky.vert");
    const fitzel::Shader march = fitzel::Shader::fromSource(vs, readFile(shDir / "volfog.frag"));
    if (!march.isValid()) { std::printf("[FAIL] march (sky.vert + volfog.frag)\n"); return 1; }
    const fitzel::Shader compose = fitzel::Shader::fromSource(vs, kComposeFrag);
    if (!compose.isValid()) { std::printf("[FAIL] compose\n"); return 1; }

    const float quad[] = {-1, -1,  1, -1,  1, 1,  -1, -1,  1, 1,  -1, 1};
    const BufferHandle vbo =
        gpu.createBuffer(BufferKind::Vertex, BufferUsage::Static, sizeof quad, quad);
    const VertexAttribute quadAttr{0, vbo, VertexFormat::Float2, 0, 2 * sizeof(float)};
    VertexArrayDesc quadDesc;
    quadDesc.attributes     = &quadAttr;
    quadDesc.attributeCount = 1;
    const VertexArrayHandle vao = gpu.createVertexArray(quadDesc);

    const TextureHandle noiseTex = bakeFogNoise();
    if (!noiseTex) { std::printf("[FAIL] noise bake\n"); return 2; }

    // The field itself, before any marching: three orthogonal slices of the
    // baked volume, written out raw. When the fog comes out streaky the first
    // question is always whether the FIELD is streaky or whether the integral
    // along the ray made it look that way, and those two have completely
    // different fixes. This answers it in one look and costs a read-back.
    {
        const int N = 64;
        std::vector<unsigned char> vol(static_cast<std::size_t>(N) * N * N * 4);
        gpu.readTexture(noiseTex, 0, -1, Format::RGBA8, vol.data());
        auto at = [&](int x, int y, int z, int c) {
            return vol[((static_cast<std::size_t>(z) * N + y) * N + x) * 4 + c];
        };
        struct Slice { const char* name; int axis; };   // axis = the one held fixed
        const Slice slices[] = {{"xz", 1}, {"xy", 2}, {"zy", 0}};
        // Written at NEAREST-NEIGHBOUR zoom. A 64-pixel image is what the field
        // actually holds, and it is also too small to see -- which matters,
        // because the thing this dump exists to catch (an axis carrying no
        // signal) is a judgement about texture, not about any one texel.
        const int Z = 6;
        const int W = N * Z;
        std::vector<unsigned char> img(static_cast<std::size_t>(W) * W * 4);
        fs::create_directories(outDir);
        for (const Slice& sl : slices) {
            for (int b = 0; b < W; ++b)
                for (int a = 0; a < W; ++a) {
                    const int u = a / Z, v = b / Z;
                    int x = u, y = v, z = N / 2;
                    if (sl.axis == 1) { x = u; z = v; y = N / 2; }
                    if (sl.axis == 0) { z = u; y = v; x = N / 2; }
                    const std::size_t o = (static_cast<std::size_t>(b) * W + a) * 4;
                    // R is the shape band and A the worley detail -- the two the
                    // march actually shapes the fog with, in grey and blue.
                    img[o + 0] = at(x, y, z, 0);
                    img[o + 1] = at(x, y, z, 0);
                    img[o + 2] = at(x, y, z, 3);
                    img[o + 3] = 255;
                }
            const fs::path f = outDir / (std::string("noise_") + sl.name + ".png");
            stbi_write_png(f.string().c_str(), W, W, 4, img.data(), W * 4);
            std::printf("  noise slice %-3s -> %s\n", sl.name, f.string().c_str());
        }
    }

    // A shadow-map array with nothing in it. uCascadeCount = 0 switches the
    // lookup off in the shader, but the sampler still has to have a complete
    // texture of the right type bound or the driver may refuse to draw.
    TextureDesc shadowDesc;
    shadowDesc.type    = TextureType::Tex2DArray;
    shadowDesc.format  = Format::R32F;
    shadowDesc.sampler = SamplerDesc::nearestClamp();
    shadowDesc.sampler.wrapU = shadowDesc.sampler.wrapV = shadowDesc.sampler.wrapW = Wrap::Repeat;
    const float one = 1.0f;
    const TextureHandle shadowTex = gpu.createTexture(shadowDesc, &one, Format::R32F);

    // The fog buffer: 16F, because the in-scatter around the sun goes past 1.
    TextureDesc fogDesc;
    fogDesc.format  = Format::RGBA16F;
    fogDesc.width   = kW;
    fogDesc.height  = kH;
    fogDesc.sampler = SamplerDesc::linearClamp();
    const TextureHandle fogTex = gpu.createTexture(fogDesc);
    FramebufferDesc fogFb;
    fogFb.color[0] = Attachment{fogTex};
    const FramebufferHandle fogFbo = gpu.createFramebuffer(fogFb);
    if (!gpu.framebufferComplete(fogFbo)) {
        std::printf("[FAIL] fog framebuffer\n"); return 2;
    }

    TextureDesc outDesc;
    outDesc.format  = Format::RGBA8;
    outDesc.width   = kW;
    outDesc.height  = kH;
    outDesc.sampler = SamplerDesc::linearRepeat();
    const TextureHandle outTex = gpu.createTexture(outDesc);
    FramebufferDesc outFb;
    outFb.color[0] = Attachment{outTex};
    const FramebufferHandle outFbo = gpu.createFramebuffer(outFb);
    if (!gpu.framebufferComplete(outFbo)) {
        std::printf("[FAIL] out framebuffer\n"); return 2;
    }

    // The depth the march stops against, built on the CPU: a ground plane at
    // y = 0 and sky everywhere else, in the same window-space depth the engine's
    // buffer holds. Real geometry would be a second thing that can be wrong.
    TextureDesc depthDesc;
    depthDesc.format  = Format::R32F;
    depthDesc.width   = kW;
    depthDesc.height  = kH;
    depthDesc.sampler = SamplerDesc::nearestClamp();
    const TextureHandle depthTex = gpu.createTexture(depthDesc);
    std::vector<float> depth(static_cast<std::size_t>(kW) * kH);

    gpu.setViewport(0, 0, kW, kH);
    std::vector<unsigned char> px(static_cast<std::size_t>(kW) * kH * 4);
    stbi_flip_vertically_on_write(1);   // read-backs come bottom row first

    const Shot shots[] = {
        // The one the whole change is about: a shallow layer seen almost along
        // itself, which is how a track camera sees ground mist. At 1.0 the field
        // is isotropic in world metres and a forty-metre layer sits inside half
        // a feature -- this is the shot where that reads as a flat pattern.
        {"grazing_flat",  -4.0f,  55.0f, 12.0f, 1.0f},
        {"grazing",       -4.0f,  55.0f, 12.0f, 3.0f},
        {"grazing_high",  -4.0f,  55.0f, 12.0f, 6.0f},
        // Into the sun, low: the shafts and the forward scatter.
        {"backlit_flat",  -2.0f,  10.0f,  8.0f, 1.0f},
        {"backlit",       -2.0f,  10.0f,  8.0f, 3.0f},
        // From above, looking down onto the top of the layer. This is where a
        // smooth analytic lid and a noise contour look nothing like each other.
        {"above_flat",   -32.0f,  70.0f, 60.0f, 1.0f},
        {"above",        -32.0f,  70.0f, 60.0f, 3.0f},
        // Standing in it, looking level -- the inside of a bank.
        {"inside",         0.0f, 110.0f,  6.0f, 3.0f},
        // Straight down from well above. The one view whose rays cross the layer
        // the short way, so what it shows is the field's plan rather than the
        // smear a shallow ray leaves behind -- which is the whole difference
        // between a streaky field and a field seen streakily.
        {"plan_flat",    -88.0f,  70.0f, 260.0f, 1.0f},
        {"plan",         -88.0f,  70.0f, 260.0f, 3.0f},
    };

    for (const Shot& sh : shots) {
        // The scene-wide volume's defaults, which are the ones that looked wrong.
        FogMedium m;
        m.verticalDetail = sh.vertical;
        const glm::vec3 boxCenter(0.0f, 15.0f, 0.0f);
        const glm::vec3 boxSize(600.0f, 40.0f, 600.0f);

        const glm::vec3 sunDir = glm::normalize(glm::vec3(0.72f, 0.22f, 0.12f));
        const glm::vec3 sunCol = glm::vec3(1.0f, 0.72f, 0.45f) * 3.2f;
        const glm::vec3 ambient(0.30f, 0.36f, 0.46f);

        const float sunYaw = std::atan2(sunDir.z, sunDir.x);
        const float yaw    = sunYaw + glm::radians(sh.yawDeg);
        const float pitch  = glm::radians(sh.pitchDeg);
        const glm::vec3 eye(-140.0f, sh.eyeY, -90.0f);
        const glm::vec3 fwd(std::cos(pitch) * std::cos(yaw), std::sin(pitch),
                            std::cos(pitch) * std::sin(yaw));
        const glm::mat4 view = glm::lookAt(eye, eye + fwd, glm::vec3(0, 1, 0));
        const glm::mat4 proj =
            glm::perspective(glm::radians(60.0f), float(kW) / float(kH), 0.1f, 8000.0f);
        const glm::mat4 vp    = proj * view;
        const glm::mat4 invVP = glm::inverse(vp);

        // Depth for this camera.
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                const glm::vec2 ndc((x + 0.5f) / kW * 2.0f - 1.0f,
                                    (y + 0.5f) / kH * 2.0f - 1.0f);
                const glm::vec4 farH = invVP * glm::vec4(ndc, 1.0f, 1.0f);
                const glm::vec3 rd = glm::normalize(glm::vec3(farH) / farH.w - eye);
                float d = 1.0f;   // sky
                if (rd.y < -1e-4f) {
                    const float t = -eye.y / rd.y;
                    const glm::vec4 clip = vp * glm::vec4(eye + rd * t, 1.0f);
                    if (clip.w > 0.0f) d = clip.z / clip.w * 0.5f + 0.5f;
                }
                depth[static_cast<std::size_t>(y) * kW + x] = glm::clamp(d, 0.0f, 1.0f);
            }
        }
        gpu.updateTexture(depthTex, TextureRegion{}, Format::R32F, depth.data());

        // --- The march ------------------------------------------------------
        const glm::mat4 model =
            glm::translate(glm::mat4(1.0f), boxCenter) * glm::scale(glm::mat4(1.0f), boxSize);
        const glm::mat4 invModel = glm::inverse(model);

        gpu.bindFramebuffer(fogFbo);
        gpu.setDepthTest(false);
        gpu.setBlendEnabled(false);
        gpu.setClearColor({0.0f, 0.0f, 0.0f, 1.0f});
        gpu.clear(ClearColor);
        march.bind();

        auto f  = [&](const char* n, float v) { march.setFloat(n, v); };
        auto i1 = [&](const char* n, int v)   { march.setInt(n, v); };
        auto v3 = [&](const char* n, const glm::vec3& v) { march.setVec3(n, v); };
        auto m4 = [&](const char* n, const glm::mat4& v) { march.setMat4(n, v); };

        gpu.bindTexture(0, depthTex);
        i1("uDepth", 0);
        gpu.bindTexture(1, noiseTex);
        i1("uNoise", 1);
        gpu.bindTexture(2, shadowTex);
        i1("uShadowMap", 2);
        i1("uCascadeCount", 0);
        i1("uShafts", 0);        // no cascades here, so no shafts to sample

        m4("uInvViewProj", invVP);
        march.setVec2("uTargetSize", {float(kW), float(kH)});
        v3("uCamPos", eye);
        v3("uCamFwd", fwd);
        f("uTime", 8.0f);
        m4("uInvModel", invModel);
        f("uEdge", m.edge);
        f("uHeightFalloff", m.heightFalloff);
        f("uDensity", m.density);
        v3("uColor", m.color);
        f("uCoverage", m.coverage);
        f("uNoiseScale", m.noiseScale);
        f("uNoiseVertical", m.verticalDetail);
        f("uDetail", m.detail);
        f("uWarp", m.warp);
        v3("uWind", m.wind);
        v3("uSunDir", sunDir);
        v3("uSunDirLocal", glm::mat3(invModel) * sunDir);
        v3("uSunColor", sunCol);
        v3("uAmbient", ambient);
        f("uG", m.anisotropy);
        f("uSunIntensity", m.sunIntensity);
        f("uAmbientIntensity", m.ambientIntensity);
        i1("uSelfShadow", m.selfShadow ? 1 : 0);
        f("uLightStep", boxSize.y * 0.125f);
        // More steps than the engine uses: this is a look at the field, and a
        // step artefact here would be mistaken for a shape.
        i1("uSteps", 96);
        gpu.bindVertexArray(vao);
        gpu.draw(Primitive::Triangles, 0, 6);

        // --- Over the background --------------------------------------------
        gpu.bindFramebuffer(outFbo);
        compose.bind();
        gpu.bindTexture(0, fogTex);
        compose.setInt("uFog", 0);
        compose.setMat4("uInvViewProj", invVP);
        compose.setVec3("uCamPos", eye);
        compose.setVec3("uSunDir", sunDir);
        gpu.draw(Primitive::Triangles, 0, 6);

        gpu.readPixels(outFbo, 0, Rect{0, 0, kW, kH}, Format::RGBA8, px.data());
        const fs::path file = outDir / (std::string("fog_") + sh.name + ".png");
        fs::create_directories(outDir);
        if (!stbi_write_png(file.string().c_str(), kW, kH, 4, px.data(), kW * 4))
            std::printf("[FAIL] write %s\n", file.string().c_str());
        else
            std::printf("  %-16s vertical %.1fx  -> %s\n", sh.name, sh.vertical,
                        file.string().c_str());
    }

    gpu.destroy(noiseTex);
    return 0;
}
