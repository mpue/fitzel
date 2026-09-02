// skycheck -- render the sky shader on its own and write the frames out as PNGs.
//
// The sky is the one thing in this engine nobody can look at while working on
// it. In the editor it is behind the panels, above the default camera's pitch,
// under the fog and washed flat by the tonemapper; you cannot get a plain look
// at a cumulus without first building a scene to stand in. So every change to
// sky.frag has been made by reasoning about noise functions and then hoping --
// and "the clouds look wrong" is not a thing reasoning is good at.
//
// This renders sky.vert/sky.frag through the same fullscreen quad the engine
// uses, into an offscreen buffer, with the camera pointed where the clouds
// actually are, and writes the result to disk. It is not a test: nothing here
// passes or fails. It is a way of SEEING, which is what this particular file
// needed and never had.
//
//   skycheck [outDir] [shaderDir]
//   Writes sky_<name>.png -- one per sun angle and look direction.
//
//   skycheck --presets [outDir] [shaderDir]
//   Writes wx_<preset>.png -- one per built-in weather preset, through the same
//   storm-dial arithmetic main.cpp applies. The presets are a shelf of skies
//   nobody can compare inside the editor either: picking one replaces the last,
//   so telling "Overcast" from "Heavy rain" means remembering a picture. Here
//   they come out side by side in a folder.

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

#include "../src/WeatherPreset.hpp"

// Nothing in the engine writes an image file, so this TU owns the implementation.
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

// One frame's worth of sky. Everything the engine feeds sky.frag, given a name
// so the file it lands in says what it is a picture of.
struct Shot {
    std::string name;
    float sunHours;      // time of day, the same 0..24 the editor slider uses
    float pitchDeg;      // where the camera looks: up into the layer, or along it
    float yawDeg;        // relative to the sun: 0 = straight at it
    // The rest is a weather::Sky plus the storm dial, because that is what the
    // engine actually feeds the shader. The hand-written shots below fill in the
    // defaults for everything they are not about.
    weather::Sky sky;
    float storm = 0.0f;
};

// A shot from the three numbers the hand-written list has always specified,
// leaving the rest of the sky at its defaults.
Shot look(const char* name, float hours, float pitch, float yaw,
          float coverage, float cirrus, float contrails) {
    Shot s{name, hours, pitch, yaw, {}, 0.0f};
    s.sky.coverage  = coverage;
    s.sky.cirrus    = cirrus;
    s.sky.contrails = contrails;
    return s;
}

// One shot per built-in preset, framed the way the "all" shot is: a little above
// the horizon and well off the sun, which is where a deck reads as a deck.
std::vector<Shot> presetShots() {
    std::vector<Shot> out;
    for (const weather::Preset& p : weather::builtins()) {
        Shot s{"wx_" + p.name, p.setTimeOfDay ? p.timeOfDay : 12.0f,
               18.0f, 60.0f, p.sky, p.storm};
        // Spaces in a filename are a nuisance to type at a prompt, and these are
        // looked at from one.
        for (char& c : s.name) if (c == ' ') c = '_';
        out.push_back(s);
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    // --presets may come first or last: this is a tool run by hand, and an
    // argument order nobody can remember is an argument nobody uses.
    bool presetMode = false;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--presets") presetMode = true;
        else pos.push_back(a);
    }
    const fs::path outDir = !pos.empty() ? fs::path(pos[0]) : fs::path(".");
    const fs::path shDir  = pos.size() > 1
                                ? fs::path(pos[1])
                                : fs::path(argv[0]).parent_path() / "assets" / "shaders";
    if (!glfwInit()) { std::printf("[FAIL] glfw\n"); return 2; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "skycheck", nullptr, nullptr);
    if (!win) { std::printf("[FAIL] no GL context\n"); glfwTerminate(); return 2; }
    glfwMakeContextCurrent(win);
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress))) {
        std::printf("[FAIL] glad\n"); return 2;
    }
    std::printf("GL %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    const GLuint vs = compile(GL_VERTEX_SHADER,   readFile(shDir / "sky.vert"), "sky.vert");
    const GLuint fs_ = compile(GL_FRAGMENT_SHADER, readFile(shDir / "sky.frag"), "sky.frag");
    if (!vs || !fs_) return 1;
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs_);
    glLinkProgram(prog);
    GLint linked = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint n = 0; glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &n);
        std::vector<char> log(static_cast<std::size_t>(n > 1 ? n : 1));
        if (n > 1) glGetProgramInfoLog(prog, n, nullptr, log.data());
        std::printf("[FAIL] link:\n%s\n", log.data());
        return 1;
    }

    // The engine's fullscreen quad: two triangles in clip space, position only.
    const float quad[] = {-1, -1,  1, -1,  1, 1,  -1, -1,  1, 1,  -1, 1};
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    // sky.vert takes the engine's Vertex layout (pos/normal/uv); only aPos is
    // read, so a tight 2-float stream bound to location 0 is enough here.
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    GLuint fbo = 0, tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::printf("[FAIL] framebuffer\n"); return 2;
    }
    glViewport(0, 0, kW, kH);

    const std::vector<Shot> shots = presetMode ? presetShots() : std::vector<Shot>{
        // Midday, looking up into the layer: this is the one that shows whether
        // a cumulus has bulges and a flat base, or is a smear.
        look("cumulus_noon",     12.0f,  14.0f, 140.0f, 0.55f, 0.0f, 0.0f),
        // The same field with the sun behind it -- the silver lining case.
        look("cumulus_backlit",  12.0f,  12.0f,  25.0f, 0.55f, 0.0f, 0.0f),
        // Low sun along the layer: the shot a race actually opens on.
        look("cumulus_evening",  17.2f,   8.0f,  40.0f, 0.45f, 0.0f, 0.0f),
        // Cirrus on its own, high and thin, with nothing below it.
        look("cirrus_noon",      12.0f,  38.0f, 120.0f, 0.00f, 0.55f, 0.0f),
        look("cirrus_evening",   17.2f,  30.0f,  35.0f, 0.00f, 0.65f, 0.0f),
        // Contrails on their own, so the straightness can be judged.
        look("contrails",        11.0f,  45.0f, 100.0f, 0.00f, 0.15f, 0.9f),
        // Everything at once, which is what the sky is meant to look like.
        look("all",              15.5f,  18.0f,  60.0f, 0.50f, 0.45f, 0.55f),
        // Overcast, to check the tops build rather than the layer just thickening.
        look("overcast",         12.0f,  14.0f, 130.0f, 0.95f, 0.20f, 0.0f),
    };

    std::vector<unsigned char> px(static_cast<std::size_t>(kW) * kH * 4);
    stbi_flip_vertically_on_write(1);   // GL reads bottom-up

    for (const Shot& s : shots) {
        // The engine's own sun: an angle off the hour, normalised.
        const float phi = (s.sunHours / 24.0f) * 6.2831853f - 1.5707963f;
        const glm::vec3 sunDir =
            glm::normalize(glm::vec3(std::cos(phi), std::sin(phi), 0.18f));
        const float dayF  = glm::smoothstep(-0.12f, 0.18f, sunDir.y);
        const float lowSun = 1.0f - glm::clamp(sunDir.y / 0.3f, 0.0f, 1.0f);
        // ...dimmed by the storm the way main.cpp dims it. Without this every
        // preset comes out lit like a fair day, and a storm ceiling photographed
        // in full sun is a picture of the wrong weather. The height haze and the
        // volumetric mist are still missing from these frames -- both are applied
        // to the SCENE, and there is no scene here.
        const float lightDim = glm::mix(1.0f, 0.30f, s.storm);
        const glm::vec3 sunCol =
            glm::mix(glm::vec3(1.0f, 0.97f, 0.9f), glm::vec3(1.0f, 0.55f, 0.26f), lowSun) *
            (0.12f + 0.95f * dayF) * 3.4f * lightDim;

        // Look `yawDeg` away from the sun's compass bearing, at `pitchDeg` up.
        const float sunYaw = std::atan2(sunDir.z, sunDir.x);
        const float yaw    = sunYaw + glm::radians(s.yawDeg);
        const float pitch  = glm::radians(s.pitchDeg);
        const glm::vec3 eye(0.0f, 40.0f, 0.0f);
        const glm::vec3 fwd(std::cos(pitch) * std::cos(yaw), std::sin(pitch),
                            std::cos(pitch) * std::sin(yaw));
        const glm::mat4 view = glm::lookAt(eye, eye + fwd, glm::vec3(0, 1, 0));
        const glm::mat4 proj = glm::perspective(glm::radians(60.0f),
                                                float(kW) / float(kH), 0.1f, 20000.0f);
        const glm::mat4 invVP = glm::inverse(proj * view);

        glUseProgram(prog);
        auto f  = [&](const char* n, float v){ glUniform1f(glGetUniformLocation(prog, n), v); };
        auto v3 = [&](const char* n, const glm::vec3& v){
            glUniform3fv(glGetUniformLocation(prog, n), 1, glm::value_ptr(v)); };
        glUniformMatrix4fv(glGetUniformLocation(prog, "uInvViewProj"), 1, GL_FALSE,
                           glm::value_ptr(invVP));
        v3("uCameraPos", eye);
        v3("uSunDir", sunDir);
        v3("uSunColor", sunCol);
        f("uTime", 12.0f);
        // The storm dial's own arithmetic, copied from main.cpp because that is
        // the thing being pictured: a preset's cloud numbers are what you see at
        // the bottom of the dial and a storm's are what you see at the top, and a
        // harness that skipped the mix would show a sky the engine never draws.
        const float effCoverage = glm::mix(s.sky.coverage, 0.97f, s.storm);
        const float effDensity  = glm::mix(s.sky.density, 2.7f, s.storm);
        const float effWind     = glm::mix(s.sky.wind, 26.0f, s.storm);
        const float effBase     = glm::mix(s.sky.base, 80.0f, s.storm);
        // The same mapping main.cpp applies: the panel's coverage is an amount,
        // the shader's is a threshold, and they run opposite ways.
        f("uCoverage", glm::mix(0.86f, 0.46f, effCoverage));
        f("uCloudDensity", effDensity);
        f("uCloudScale", s.sky.scale);
        f("uCloudSpeed", effWind);
        f("uCloudBottom", effBase);
        f("uCloudTop", s.sky.top);
        f("uCirrus", s.sky.cirrus);
        f("uCirrusHeight", s.sky.cirrusHeight);
        f("uCirrusSpeed", s.sky.cirrusWind);
        f("uContrails", s.sky.contrails);
        f("uExposure", 1.0f);
        glUniform1i(glGetUniformLocation(prog, "uTonemap"), 1);

        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glFinish();
        glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());

        const fs::path out =
            outDir / ((presetMode ? "" : "sky_") + s.name + ".png");
        if (stbi_write_png(out.string().c_str(), kW, kH, 4, px.data(), kW * 4))
            std::printf("wrote %s\n", out.string().c_str());
        else
            std::printf("[FAIL] could not write %s\n", out.string().c_str());
    }

    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
