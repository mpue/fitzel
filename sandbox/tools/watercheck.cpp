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
// It is not a test: nothing here passes or fails. It is a way of SEEING.
//
//   watercheck [outDir] [shaderDir]
//   Writes water_<view>_<dry|rain>.png.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

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
float h(vec2 p) { p = fract(p * vec2(123.34, 345.45)); p += dot(p, p + 34.345); return fract(p.x * p.y); }
float n(vec2 p) { vec2 i = floor(p), f = fract(p); vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(h(i), h(i + vec2(1, 0)), u.x), mix(h(i + vec2(0, 1)), h(i + vec2(1, 1)), u.x), u.y); }
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
    if (uMode == 0) { FragColor = vec4(sky(rd), 1.0); return; }
    if (uMode == 1) { FragColor = vec4(sky(reflect(rd, vec3(0, 1, 0))), 1.0); return; }
    if (rd.y >= -1e-4) { FragColor = vec4(0.0); gl_FragDepth = 1.0; return; }
    vec3 p = uCamPos + rd * ((-3.0 - uCamPos.y) / rd.y);
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

    // The lake: a grid fine enough for the swell, [-0.5, 0.5] like the engine's.
    const int N = 400;
    std::vector<float> verts;
    std::vector<unsigned> idx;
    for (int z = 0; z <= N; ++z)
        for (int x = 0; x <= N; ++x) {
            verts.push_back(static_cast<float>(x) / N - 0.5f);
            verts.push_back(0.0f);
            verts.push_back(static_cast<float>(z) / N - 0.5f);
        }
    for (int z = 0; z < N; ++z)
        for (int x = 0; x < N; ++x) {
            const unsigned a = static_cast<unsigned>(z * (N + 1) + x), b = a + 1,
                           c = a + static_cast<unsigned>(N + 1), d = c + 1;
            idx.insert(idx.end(), {a, c, b, b, c, d});
        }
    GLuint gvao = 0, gvbo = 0, gibo = 0;
    glGenVertexArrays(1, &gvao);
    glGenBuffers(1, &gvbo);
    glGenBuffers(1, &gibo);
    glBindVertexArray(gvao);
    glBindBuffer(GL_ARRAY_BUFFER, gvbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(idx.size() * sizeof(unsigned)),
                 idx.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);

    Target refl, refr, out;
    refl.make(false);
    refr.make(true);
    out.make(false);

    const Shot shots[] = {{"eye", 1.7f, 12.0f}, {"jetty", 5.0f, 18.0f}, {"editor", 30.0f, 30.0f}};
    const glm::vec3 sunDir = glm::normalize(glm::vec3(0.2f, 0.35f, -1.0f));
    const float     near = 0.1f, far = 2000.0f;
    const float     time = 12.37f;

    for (const Shot& sh : shots) {
        const glm::vec3 eye(0.0f, sh.eyeY, 0.0f);
        const float pitch = glm::radians(-sh.pitchDeg);
        const glm::vec3 fwd(0.0f, std::sin(pitch), -std::cos(pitch));
        const glm::mat4 view = glm::lookAt(eye, eye + fwd, glm::vec3(0, 1, 0));
        const glm::mat4 proj = glm::perspective(glm::radians(60.0f),
                                                static_cast<float>(kW) / kH, near, far);
        const glm::mat4 vp = proj * view, ivp = glm::inverse(vp);

        glViewport(0, 0, kW, kH);
        glUseProgram(world);
        glUniformMatrix4fv(glGetUniformLocation(world, "uInvViewProj"), 1, GL_FALSE, glm::value_ptr(ivp));
        glUniformMatrix4fv(glGetUniformLocation(world, "uViewProj"), 1, GL_FALSE, glm::value_ptr(vp));
        glUniform3fv(glGetUniformLocation(world, "uCamPos"), 1, glm::value_ptr(eye));
        glUniform3fv(glGetUniformLocation(world, "uSunDir"), 1, glm::value_ptr(sunDir));
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

        // The sky behind the water stays as drawn; the water is drawn over it
        // twice, once dry and once in a steady rain.
        std::vector<unsigned char> sky(static_cast<std::size_t>(kW) * kH * 4);
        for (int rain = 0; rain < 2; ++rain) {
            glBindFramebuffer(GL_FRAMEBUFFER, out.fbo);
            glClear(GL_DEPTH_BUFFER_BIT);
            if (rain == 1) {
                // Redraw the sky (the dry pass painted over it).
                glUseProgram(world);
                glUniform1i(glGetUniformLocation(world, "uMode"), 0);
                glDepthFunc(GL_ALWAYS);
                glBindVertexArray(qvao);
                glDrawArrays(GL_TRIANGLES, 0, 6);
                glDepthFunc(GL_LESS);
                glClear(GL_DEPTH_BUFFER_BIT);
            }
            glUseProgram(water);
            auto f1 = [&](const char* n, float v) { glUniform1f(glGetUniformLocation(water, n), v); };
            auto i1 = [&](const char* n, int v) { glUniform1i(glGetUniformLocation(water, n), v); };
            auto v3 = [&](const char* n, glm::vec3 v) {
                glUniform3fv(glGetUniformLocation(water, n), 1, glm::value_ptr(v));
            };
            const glm::mat4 model = glm::scale(glm::mat4(1.0f), glm::vec3(1200.0f, 1.0f, 1200.0f));
            glUniformMatrix4fv(glGetUniformLocation(water, "uModel"), 1, GL_FALSE, glm::value_ptr(model));
            glUniformMatrix4fv(glGetUniformLocation(water, "uViewProj"), 1, GL_FALSE, glm::value_ptr(vp));
            // The editor's defaults (main.cpp), a light rain's weather: a calm
            // swell, an overcast light.
            f1("uTime", time);
            f1("uWaveHeight", 0.6f);
            f1("uChoppy", 0.6f);
            v3("uCameraPos", eye);
            v3("uLightDir", sunDir);
            v3("uLightColor", glm::vec3(1.6f, 1.5f, 1.4f));
            v3("uAmbient", glm::vec3(0.35f, 0.38f, 0.42f));
            v3("uWaterColor", glm::vec3(0.08f, 0.24f, 0.30f));
            f1("uWaveStrength", 0.022f);
            f1("uWaveScale", 0.06f);
            f1("uReflectivity", 0.65f);
            f1("uClarity", 1.0f);
            f1("uIor", 1.33f);
            f1("uFoamWidth", 2.5f);
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
            glBindVertexArray(gvao);
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(idx.size()), GL_UNSIGNED_INT, nullptr);

            std::vector<unsigned char> px(static_cast<std::size_t>(kW) * kH * 4);
            glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            std::vector<unsigned char> flip(px.size());
            for (int y = 0; y < kH; ++y)
                std::copy_n(&px[static_cast<std::size_t>(kH - 1 - y) * kW * 4], kW * 4,
                            &flip[static_cast<std::size_t>(y) * kW * 4]);
            const fs::path file = outDir / (std::string("water_") + sh.name +
                                            (rain ? "_rain.png" : "_dry.png"));
            if (!stbi_write_png(file.string().c_str(), kW, kH, 4, flip.data(), kW * 4))
                std::printf("[FAIL] write %s\n", file.string().c_str());
            else
                std::printf("wrote %s\n", file.string().c_str());
        }
    }
    glfwTerminate();
    return 0;
}
