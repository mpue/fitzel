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
#include <sstream>
#include <string>
#include <vector>

#include <glad/gl.h>

#include <fitzel/graphics/Shader.hpp>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

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

GLuint compile(GLenum stage, const std::string& src, const char* what) {
    const GLuint sh = glCreateShader(stage);
    const char* s = src.c_str();
    glShaderSource(sh, 1, &s, nullptr);
    glCompileShader(sh);
    GLint good = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &good);
    if (!good) {
        GLint n = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &n);
        std::vector<char> log(static_cast<std::size_t>(n > 1 ? n : 1));
        if (n > 1) glGetShaderInfoLog(sh, n, nullptr, log.data());
        std::printf("[FAIL] %s:\n%s\n", what, log.data());
        return 0;
    }
    return sh;
}

GLuint link(GLuint vs, GLuint fs, const char* what) {
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint n = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &n);
        std::vector<char> log(static_cast<std::size_t>(n > 1 ? n : 1));
        if (n > 1) glGetProgramInfoLog(prog, n, nullptr, log.data());
        std::printf("[FAIL] link %s:\n%s\n", what, log.data());
        return 0;
    }
    return prog;
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
    GLuint fbo = 0, color = 0, depth = 0;
    void make(bool depthTex) {
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glGenTextures(1, &color);
        glBindTexture(GL_TEXTURE_2D, color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, kW, kH, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        glGenTextures(1, &depth);
        glBindTexture(GL_TEXTURE_2D, depth);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, kW, kH, 0, GL_DEPTH_COMPONENT,
                     GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
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
    if (!glfwInit()) { std::printf("[FAIL] glfw\n"); return 2; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "watercheck", nullptr, nullptr);
    if (!win) { std::printf("[FAIL] no GL context\n"); glfwTerminate(); return 2; }
    glfwMakeContextCurrent(win);
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress))) {
        std::printf("[FAIL] glad\n"); return 2;
    }

    const GLuint qv = compile(GL_VERTEX_SHADER, kQuadVert, "quad.vert");
    const GLuint wf = compile(GL_FRAGMENT_SHADER, kWorldFrag, "world");
    const GLuint vv = compile(GL_VERTEX_SHADER, withIncludes(shDir / "water.vert"), "water.vert");
    const GLuint vf = compile(GL_FRAGMENT_SHADER, withIncludes(shDir / "water.frag"), "water.frag");
    if (!qv || !wf || !vv || !vf) return 1;
    const GLuint world = link(qv, wf, "world");
    const GLuint water = link(vv, vf, "water");
    if (!world || !water) return 1;

    const float quad[] = {-1, -1,  1, -1,  1, 1,  -1, -1,  1, 1,  -1, 1};
    GLuint qvao = 0, qvbo = 0;
    glGenVertexArrays(1, &qvao);
    glGenBuffers(1, &qvbo);
    glBindVertexArray(qvao);
    glBindBuffer(GL_ARRAY_BUFFER, qvbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

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
    // GL objects inside: gone before the context is.
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

        glViewport(0, 0, kW, kH);
        fitzel::Shader::use(world);
        glUniformMatrix4fv(glGetUniformLocation(world, "uInvViewProj"), 1, GL_FALSE, glm::value_ptr(ivp));
        glUniformMatrix4fv(glGetUniformLocation(world, "uViewProj"), 1, GL_FALSE, glm::value_ptr(vp));
        glUniform3fv(glGetUniformLocation(world, "uCamPos"), 1, glm::value_ptr(eye));
        glUniform3fv(glGetUniformLocation(world, "uSunDir"), 1, glm::value_ptr(sunDir));
        glUniform1i(glGetUniformLocation(world, "uShore"), sc.shore ? 1 : 0);
        glUniform1f(glGetUniformLocation(world, "uClipY"),
                    0.1f + std::min(sea.significantHeight(), 4.0f) * 0.7f);
        glBindVertexArray(qvao);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
        const Target* passes[] = {&out, &refl, &refr};
        for (int m = 0; m < 3; ++m) {
            glBindFramebuffer(GL_FRAMEBUFFER, passes[m]->fbo);
            glClearColor(0, 0, 0, 1);
            glClearDepth(1.0);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glUniform1i(glGetUniformLocation(world, "uMode"), m);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
        glDepthFunc(GL_LESS);

        const bool rainy = sc.boat && std::string(sh.name) == "eye";
        for (int rain = 0; rain < (rainy ? 2 : 1); ++rain) {
            glBindFramebuffer(GL_FRAMEBUFFER, out.fbo);
            if (rain == 1) {
                fitzel::Shader::use(world);
                glUniform1i(glGetUniformLocation(world, "uMode"), 0);
                glDepthFunc(GL_ALWAYS);
                glBindVertexArray(qvao);
                glDrawArrays(GL_TRIANGLES, 0, 6);
                glDepthFunc(GL_LESS);
            }
            if (!sc.shore) glClear(GL_DEPTH_BUFFER_BIT);   // the beach's depth stays: the sea meets it
            ocean::Breakers br;
            br.height = glm::clamp(0.15f + 0.45f * sea.significantHeight() + 1.2f * sc.set.swell, 0.0f, 3.0f);
            br.length = glm::clamp(6.0f + 0.3f * sea.peakWavelength() + 25.0f * sc.set.swell, 8.0f, 45.0f);
            br.speed  = glm::clamp(0.8f * std::sqrt(9.81f * br.length / 6.2831853f), 2.5f, 8.0f);
            oceanGl->upload(sea, waves, sc.shore ? &shore : nullptr);
            oceanGl->bind(water, 3, eye, 0.0f, sea, waves, sc.shore ? &shore : nullptr, br);
            auto f1 = [&](const char* n, float v) { glUniform1f(glGetUniformLocation(water, n), v); };
            auto i1 = [&](const char* n, int v) { glUniform1i(glGetUniformLocation(water, n), v); };
            auto v3 = [&](const char* n, glm::vec3 v) {
                glUniform3fv(glGetUniformLocation(water, n), 1, glm::value_ptr(v));
            };
            glUniformMatrix4fv(glGetUniformLocation(water, "uViewProj"), 1, GL_FALSE, glm::value_ptr(vp));
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
            glUniform4f(glGetUniformLocation(water, "uWaterClip"), -1e9f, -1e9f, 1e9f, 1e9f);
            i1("uReflection", 0);
            i1("uRefraction", 1);
            i1("uRefractionDepth", 2);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, refl.color);
            glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, refr.color);
            glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, refr.depth);
            glActiveTexture(GL_TEXTURE0);
            glEnable(GL_CULL_FACE); glCullFace(GL_BACK);
            oceanGl->mesh().draw();
            glDisable(GL_CULL_FACE);

            std::vector<unsigned char> px(static_cast<std::size_t>(kW) * kH * 4);
            glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
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
    glfwTerminate();
    return 0;
}
