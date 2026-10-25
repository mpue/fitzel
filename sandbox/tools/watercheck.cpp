// watercheck -- render the lake (water.vert + water.frag) on its own and write
// the frames out as PNGs, dry and in the rain.
//
// The same idea as skycheck and fogcheck. Rain on the lake is the hardest thing
// on it to get a plain look at in the editor: it needs a weather that rains, a
// camera low enough over the water to resolve a drop, and a reflection with
// something in it for a ring to bend -- and "it does nothing" from the usual
// editor height could be the rings, the fade that hides them, or the weather.
// Here the reflection is a known sky with a tree line and clouds, the bed is a
// known depth, and the same view is drawn with and without rain from the eye,
// from a jetty and from the editor's height.
//
// The sea is the engine's own: Ocean (the spectral waves), WaveSim (a boat
// crossing in front, for its wake) and OceanRenderer (the ring mesh and the
// textures) -- the same code the editor runs, in three sea states.
//
// It is not a test: nothing here passes or fails. It is a way of SEEING.
//
//   watercheck [outDir] [shaderDir]
//   Writes water_<sea>_<view>.png, and water_lake_eye_rain.png.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

#include <fitzel/core/Window.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/rhi/Device.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/Ocean.hpp"
#include "../src/OceanRenderer.hpp"
#include "../src/ShoreMap.hpp"
#include <chrono>
#include <thread>
#include "../src/WaveSim.hpp"

namespace fs = std::filesystem;

namespace {

constexpr int kW = 960, kH = 540;

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The engine's loader splices `#include "x.glsl"` in from the shader's own
// folder; so does this, one level deep, which is all water.frag uses.
std::string withIncludes(const fs::path& p) {
    std::istringstream in(readFile(p));
    std::string out, line;
    while (std::getline(in, line)) {
        const auto q0 = line.find("#include \"");
        if (q0 != std::string::npos) {
            const auto q1 = line.find('"', q0 + 10);
            out += readFile(p.parent_path() / line.substr(q0 + 10, q1 - q0 - 10));
            out += '\n';
            continue;
        }
        out += line;
        out += '\n';
    }
    return out;
}

constexpr const char* kQuadVert = R"(#version 330 core
layout(location = 0) in vec2 aPos;
out vec2 vNdc;
void main() { vNdc = aPos; gl_Position = vec4(aPos, 0.0, 1.0); }
)";

// The world around the lake, in three modes: the sky as seen (behind the water),
// the sky MIRRORED in a flat lake (what the reflection target holds), and the
// bed under three metres of water with its depth (the refraction target). Plain
// on purpose -- a gradient, a few clouds, a ragged tree line on the far shore
// and a checker on the bed -- so that anything bending them is the water.
constexpr const char* kWorldFrag = R"(#version 330 core
in  vec2 vNdc;
out vec4 FragColor;
uniform mat4 uInvViewProj;
uniform mat4 uViewProj;
uniform vec3 uCamPos;
uniform vec3 uSunDir;
uniform int  uMode;   // 0 sky, 1 reflection, 2 bed
uniform int  uShore;  // 1: a beach ahead (landH below), ray-marched
uniform float uClipY; // the refraction pass keeps what lies below this
float h(vec2 p) { p = fract(p * vec2(123.34, 345.45)); p += dot(p, p + 34.345); return fract(p.x * p.y); }
float n(vec2 p) { vec2 i = floor(p), f = fract(p); vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(h(i), h(i + vec2(1, 0)), u.x), mix(h(i + vec2(0, 1)), h(i + vec2(1, 1)), u.x), u.y); }
// The beach: ground rising 1 in 8 toward -z from 20 m out, meeting the still
// water near 78 m with a wandering line -- the same function watercheck hands
// the shore map, so the breakers and the ground agree.
float landH(vec2 p) {
    return -7.0 + 0.12 * max(0.0, -p.y - 20.0) + 1.5 * sin(p.x * 0.05) + 0.8 * sin(p.x * 0.13 + 1.3);
}
bool marchLand(vec3 ro, vec3 rd, float clipY, out vec3 hit) {
    float t = 0.3;
    for (int i = 0; i < 400; ++i) {
        vec3 p = ro + rd * t;
        if (p.y < landH(p.xz) && p.y <= clipY) {
            float a = t - max(0.25, t * 0.01), b = t;
            for (int j = 0; j < 8; ++j) {
                float m = 0.5 * (a + b);
                vec3 q = ro + rd * m;
                if (q.y < landH(q.xz) && q.y <= clipY) b = m; else a = m;
            }
            hit = ro + rd * b;
            return true;
        }
        t += max(0.25, t * 0.01);
        if (t > 3000.0) break;
    }
    return false;
}
vec3 landColour(vec3 p) {
    vec3 sand = vec3(0.62, 0.55, 0.40);
    vec3 wet  = vec3(0.36, 0.31, 0.22);
    vec3 grass = vec3(0.20, 0.30, 0.10);
    vec3 c = mix(wet, sand, smoothstep(0.1, 0.6, p.y));
    c = mix(c, grass, smoothstep(2.5, 3.5, p.y));
    return c * (0.8 + 0.4 * n(p.xz * 0.7));
}
vec3 sky(vec3 r) {
    float az = atan(r.z, r.x);
    float el = asin(clamp(r.y, -1.0, 1.0));
    // The far shore: trees 300 m off, 12-30 m tall, ragged.
    float top = atan((12.0 + 18.0 * n(vec2(az * 40.0, 0.0)) + 6.0 * n(vec2(az * 160.0, 3.0))) / 300.0);
    if (el < top) return vec3(0.05, 0.09, 0.05) * (0.7 + 0.6 * n(vec2(az * 300.0, el * 300.0)));
    vec3 c = mix(vec3(0.55, 0.62, 0.70), vec3(0.20, 0.32, 0.55), sqrt(max(r.y, 0.0)));
    vec2 cp = r.xz / max(r.y, 0.05) * 3.0;
    float cl = smoothstep(0.45, 0.8, n(cp) * 0.6 + n(cp * 2.3) * 0.4);
    c = mix(c, vec3(0.85, 0.87, 0.9), cl * 0.8);
    c += vec3(1.0, 0.9, 0.7) * pow(max(dot(r, uSunDir), 0.0), 400.0) * 20.0;
    return c;
}
void main() {
    vec4 farH = uInvViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 rd = normalize(farH.xyz / farH.w - uCamPos);
    gl_FragDepth = 1.0;
    if (uMode == 0) {
        vec3 hp;
        if (uShore == 1 && marchLand(uCamPos, rd, 1e9, hp)) {
            FragColor = vec4(landColour(hp) * (0.5 + 0.8 * max(dot(normalize(vec3(0.0, 1.0, 0.12)), uSunDir), 0.0)), 1.0);
            vec4 clip = uViewProj * vec4(hp, 1.0);
            gl_FragDepth = clamp(clip.z / clip.w * 0.5 + 0.5, 0.0, 1.0);
            return;
        }
        FragColor = vec4(sky(rd), 1.0);
        return;
    }
    if (uMode == 1) { FragColor = vec4(sky(reflect(rd, vec3(0, 1, 0))), 1.0); return; }
    if (uShore == 1) {
        vec3 hp;
        if (marchLand(uCamPos, rd, uClipY, hp)) {
            FragColor = vec4(landColour(hp) * 0.7, 1.0);
            vec4 clip = uViewProj * vec4(hp, 1.0);
            gl_FragDepth = clamp(clip.z / clip.w * 0.5 + 0.5, 0.0, 1.0);
        } else FragColor = vec4(0.0);
        return;
    }
    if (rd.y >= -1e-4) { FragColor = vec4(0.0); gl_FragDepth = 1.0; return; }
    vec3 p = uCamPos + rd * ((-7.0 - uCamPos.y) / rd.y);
    float c = mod(floor(p.x / 2.0) + floor(p.z / 2.0), 2.0);
    FragColor = vec4(mix(vec3(0.30, 0.26, 0.18), vec3(0.42, 0.37, 0.27), c), 1.0);
    vec4 clip = uViewProj * vec4(p, 1.0);
    gl_FragDepth = clamp(clip.z / clip.w * 0.5 + 0.5, 0.0, 1.0);
}
)";

struct Target {
    fitzel::rhi::FramebufferHandle fbo;
    fitzel::rhi::TextureHandle     color, depth;
    void make(bool depthTex) {
        using namespace fitzel::rhi;
        Device& gpu = device();
        TextureDesc c;
        c.format = Format::RGBA16F;
        c.width  = kW;
        c.height = kH;
        c.sampler = SamplerDesc::linearClamp();
        color = gpu.createTexture(c);
        TextureDesc d;
        d.format = Format::Depth24;
        d.width  = kW;
        d.height = kH;
        d.sampler = SamplerDesc::nearestClamp();
        d.sampler.wrapU = d.sampler.wrapV = d.sampler.wrapW = Wrap::Repeat;
        depth = gpu.createTexture(d);
        FramebufferDesc fb;
        fb.color[0] = Attachment{color};
        fb.depth    = Attachment{depth};
        fbo = gpu.createFramebuffer(fb);
        (void)depthTex;
    }
};

struct Shot {
    const char* name;
    float eyeY;      // metres above the still water line
    float pitchDeg;  // looking down by this much
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
            .width = 64, .height = 64, .title = "watercheck", .vsync = false, .visible = false});
    } catch (const std::exception&) {
        std::printf("[FAIL] no GL context\n");
        return 2;
    }
    fitzel::rhi::Device& gpu = fitzel::rhi::device();

    const fitzel::Shader world = fitzel::Shader::fromSource(kQuadVert, kWorldFrag);
    const fitzel::Shader water = fitzel::Shader::fromSource(withIncludes(shDir / "water.vert"),
                                                            withIncludes(shDir / "water.frag"));
    if (!world.isValid()) { std::printf("[FAIL] world\n"); return 1; }
    if (!water.isValid()) { std::printf("[FAIL] water\n"); return 1; }

    const float quad[] = {-1, -1,  1, -1,  1, 1,  -1, -1,  1, 1,  -1, 1};
    const fitzel::rhi::BufferHandle qvbo = gpu.createBuffer(
        fitzel::rhi::BufferKind::Vertex, fitzel::rhi::BufferUsage::Static, sizeof quad, quad);
    fitzel::rhi::VertexAttribute qattr;
    qattr.location = 0;
    qattr.buffer   = qvbo;
    qattr.format   = fitzel::rhi::VertexFormat::Float2;
    qattr.stride   = 2 * sizeof(float);
    fitzel::rhi::VertexArrayDesc qdesc;
    qdesc.attributes     = &qattr;
    qdesc.attributeCount = 1;
    const fitzel::rhi::VertexArrayHandle qvao = gpu.createVertexArray(qdesc);

    Target refl, refr, out;
    refl.make(false);
    refr.make(true);
    out.make(false);

    const Shot shots[] = {{"eye", 1.7f, 12.0f}, {"jetty", 5.0f, 18.0f}, {"editor", 30.0f, 30.0f},
                          {"high", 150.0f, 35.0f}, {"plane", 600.0f, 40.0f}};
    const glm::vec3 sunDir = glm::normalize(glm::vec3(0.2f, 0.35f, -1.0f));
    const float     near = 0.1f, far = 5000.0f;
    const double    time = (argc > 3) ? std::atof(argv[3]) : 12.37;

    struct Sea { const char* name; ocean::Settings set; bool boat; bool shore = false; };
    std::vector<Sea> seas;
    { ocean::Settings o; o.windSpeed = 3.0f;  o.fetch = 2000.0f;   o.choppiness = 0.7f; seas.push_back({"calm", o, false}); }
    { ocean::Settings o; o.windSpeed = 9.0f;  o.fetch = 6000.0f;   o.choppiness = 0.9f; seas.push_back({"lake", o, true}); }
    { ocean::Settings o; o.windSpeed = 17.0f; o.fetch = 120000.0f; o.choppiness = 1.2f; o.swell = 0.5f;
      seas.push_back({"storm", o, false}); }
    { ocean::Settings o; o.windSpeed = 8.0f; o.fetch = 100000.0f; o.choppiness = 1.0f; o.swell = 0.6f;
      seas.push_back({"surf", o, false, true}); }
    const Shot shoreShots[] = {{"beach", 3.0f, 6.0f}, {"cliff", 25.0f, 22.0f}, {"aerial", 120.0f, 45.0f}};
    // The same beach the world shader draws.
    auto landH = [](float x, float z) {
        return -7.0f + 0.12f * std::max(0.0f, -z - 20.0f) + 1.5f * std::sin(x * 0.05f) +
               0.8f * std::sin(x * 0.13f + 1.3f);
    };
    ocean::ShoreMap shore;
    for (int i = 0; i < 500 && !shore.valid(); ++i) {
        shore.update(glm::vec2(0.0f), 0.0f, 0.0, landH, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ocean::Ocean         sea;
    ocean::WaveSim       waves;
    // GPU textures inside: gone before the window (and its device) is.
    auto oceanGl = std::make_unique<ocean::OceanRenderer>();

    for (const Sea& sc : seas) {
        sea.configure(sc.set);
        // Run the clock up to the moment, so foam has had crests to come from
        // and the boat has had time to drag its wake across.
        waves.clear();
        waves.follow(glm::vec2(0.0f));
        for (double t = 0.0; t <= time; t += 1.0 / 30.0) {
            sea.computeNow(t);
            std::vector<ocean::WaveSim::Body> bodies;
            if (sc.boat && t > time - 8.0)
                bodies.push_back({1, glm::vec2(-40.0f + static_cast<float>(t - (time - 8.0)) * 6.0f, -16.0f),
                                  1.3f, 0.5f});
            waves.step(1.0f / 30.0f, bodies);
        }
        std::printf("%s: wind %.0f m/s, Hs %.2f m, peak wavelength %.1f m\n", sc.name,
                    sc.set.windSpeed, sea.significantHeight(), sea.peakWavelength());

    const Shot* shotList = sc.shore ? shoreShots : shots;
    const int   shotCount = sc.shore ? 3 : static_cast<int>(sizeof(shots) / sizeof(shots[0]));
    for (int si = 0; si < shotCount; ++si) {
        const Shot& sh = shotList[si];
        const glm::vec3 eye(0.0f, sh.eyeY, 0.0f);
        const float pitch = glm::radians(-sh.pitchDeg);
        const glm::vec3 fwd(0.0f, std::sin(pitch), -std::cos(pitch));
        const glm::mat4 view = glm::lookAt(eye, eye + fwd, glm::vec3(0, 1, 0));
        const glm::mat4 proj = glm::perspective(glm::radians(60.0f),
                                                static_cast<float>(kW) / kH, near, far);
        const glm::mat4 vp = proj * view, ivp = glm::inverse(vp);

        gpu.setViewport(0, 0, kW, kH);
        world.bind();
        world.setMat4("uInvViewProj", ivp);
        world.setMat4("uViewProj", vp);
        world.setVec3("uCamPos", eye);
        world.setVec3("uSunDir", sunDir);
        world.setInt("uShore", sc.shore ? 1 : 0);
        world.setFloat("uClipY", 0.1f + std::min(sea.significantHeight(), 4.0f) * 0.7f);
        gpu.bindVertexArray(qvao);
        gpu.setDepthTest(true);
        gpu.setDepthFunc(fitzel::rhi::CompareOp::Always);
        const Target* passes[] = {&out, &refl, &refr};
        for (int m = 0; m < 3; ++m) {
            gpu.bindFramebuffer(passes[m]->fbo);
            gpu.setClearColor(glm::vec4(0, 0, 0, 1));
            gpu.setClearDepth(1.0f);
            gpu.clear(fitzel::rhi::ClearColor | fitzel::rhi::ClearDepth);
            world.setInt("uMode", m);
            gpu.draw(fitzel::rhi::Primitive::Triangles, 0, 6);
        }
        gpu.setDepthFunc(fitzel::rhi::CompareOp::Less);

        const bool rainy = sc.boat && std::string(sh.name) == "eye";
        for (int rain = 0; rain < (rainy ? 2 : 1); ++rain) {
            gpu.bindFramebuffer(out.fbo);
            if (rain == 1) {
                world.bind();
                world.setInt("uMode", 0);
                gpu.setDepthFunc(fitzel::rhi::CompareOp::Always);
                gpu.bindVertexArray(qvao);
                gpu.draw(fitzel::rhi::Primitive::Triangles, 0, 6);
                gpu.setDepthFunc(fitzel::rhi::CompareOp::Less);
            }
            if (!sc.shore) gpu.clear(fitzel::rhi::ClearDepth);   // the beach's depth stays: the sea meets it
            ocean::Breakers br;
            br.height = glm::clamp(0.15f + 0.45f * sea.significantHeight() + 1.2f * sc.set.swell, 0.0f, 3.0f);
            br.length = glm::clamp(6.0f + 0.3f * sea.peakWavelength() + 25.0f * sc.set.swell, 8.0f, 45.0f);
            br.speed  = glm::clamp(0.8f * std::sqrt(9.81f * br.length / 6.2831853f), 2.5f, 8.0f);
            oceanGl->upload(sea, waves, sc.shore ? &shore : nullptr);
            oceanGl->bind(water, 3, eye, 0.0f, sea, waves, sc.shore ? &shore : nullptr, br);
            auto f1 = [&](const char* n, float v) { water.setFloat(n, v); };
            auto i1 = [&](const char* n, int v) { water.setInt(n, v); };
            auto v3 = [&](const char* n, glm::vec3 v) { water.setVec3(n, v); };
            water.setMat4("uViewProj", vp);
            // The editor's defaults (main.cpp), under a bright, hazy sun.
            f1("uTime", static_cast<float>(time));
            v3("uCameraPos", eye);
            v3("uLightDir", sunDir);
            v3("uLightColor", glm::vec3(1.6f, 1.5f, 1.4f));
            v3("uAmbient", glm::vec3(0.35f, 0.38f, 0.42f));
            v3("uWaterColor", glm::vec3(0.08f, 0.24f, 0.30f));
            f1("uWaveStrength", 0.022f);
            f1("uReflectivity", 0.65f);
            f1("uClarity", 1.0f);
            f1("uIor", 1.33f);
            f1("uFoamWidth", 2.5f);
            f1("uWhitecaps", 1.0f);
            f1("uFogDensity", 0.0f);
            f1("uFogHeightFalloff", 0.01f);
            f1("uExposure", 1.0f);
            i1("uTonemap", 1);
            f1("uNear", near);
            f1("uFar", far);
            i1("uOrtho", 0);
            f1("uRainRings", 1.0f);
            f1("uRainDensity", rain ? 1.0f : 0.0f);
            i1("uFishRingCount", 0);
            water.setVec4("uWaterClip", glm::vec4(-1e9f, -1e9f, 1e9f, 1e9f));
            i1("uReflection", 0);
            i1("uRefraction", 1);
            i1("uRefractionDepth", 2);
            gpu.bindTexture(0, refl.color);
            gpu.bindTexture(1, refr.color);
            gpu.bindTexture(2, refr.depth);
            gpu.setCullEnabled(true);
            gpu.setCullFace(fitzel::rhi::CullMode::Back);
            oceanGl->mesh().draw();
            gpu.setCullEnabled(false);

            std::vector<unsigned char> px(static_cast<std::size_t>(kW) * kH * 4);
            gpu.readPixels(out.fbo, 0, fitzel::rhi::Rect{0, 0, kW, kH}, fitzel::rhi::Format::RGBA8,
                           px.data());
            std::vector<unsigned char> flip(px.size());
            for (int y = 0; y < kH; ++y)
                std::copy_n(&px[static_cast<std::size_t>(kH - 1 - y) * kW * 4], kW * 4,
                            &flip[static_cast<std::size_t>(y) * kW * 4]);
            const fs::path file = outDir / (std::string("water_") + sc.name + "_" + sh.name +
                                            (rain ? "_rain.png" : ".png"));
            if (!stbi_write_png(file.string().c_str(), kW, kH, 4, flip.data(), kW * 4))
                std::printf("[FAIL] write %s\n", file.string().c_str());
            else
                std::printf("wrote %s\n", file.string().c_str());
        }
    }
    }
    oceanGl.reset();
    return 0;
}
