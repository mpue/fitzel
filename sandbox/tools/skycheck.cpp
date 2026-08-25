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
    const char* name;
    float sunHours;      // time of day, the same 0..24 the editor slider uses
    float pitchDeg;      // where the camera looks: up into the layer, or along it
    float yawDeg;        // relative to the sun: 0 = straight at it
    float coverage;      // the panel's 0..1, not the shader's threshold
    float cirrus;
    float contrails;
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

    const Shot shots[] = {
        // Midday, looking up into the layer: this is the one that shows whether
        // a cumulus has bulges and a flat base, or is a smear.
        {"cumulus_noon",     12.0f,  14.0f, 140.0f, 0.55f, 0.0f, 0.0f},
        // The same field with the sun behind it -- the silver lining case.
        {"cumulus_backlit",  12.0f,  12.0f,  25.0f, 0.55f, 0.0f, 0.0f},
        // Low sun along the layer: the shot a race actually opens on.
        {"cumulus_evening",  17.2f,   8.0f,  40.0f, 0.45f, 0.0f, 0.0f},
        // Cirrus on its own, high and thin, with nothing below it.
        {"cirrus_noon",      12.0f,  38.0f, 120.0f, 0.00f, 0.55f, 0.0f},
        {"cirrus_evening",   17.2f,  30.0f,  35.0f, 0.00f, 0.65f, 0.0f},
        // Contrails on their own, so the straightness can be judged.
        {"contrails",        11.0f,  45.0f, 100.0f, 0.00f, 0.15f, 0.9f},
        // Everything at once, which is what the sky is meant to look like.
        {"all",              15.5f,  18.0f,  60.0f, 0.50f, 0.45f, 0.55f},
        // Overcast, to check the tops build rather than the layer just thickening.
        {"overcast",         12.0f,  14.0f, 130.0f, 0.95f, 0.20f, 0.0f},
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
        const glm::vec3 sunCol =
            glm::mix(glm::vec3(1.0f, 0.97f, 0.9f), glm::vec3(1.0f, 0.55f, 0.26f), lowSun) *
            (0.12f + 0.95f * dayF) * 3.4f;

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
        // The same mapping main.cpp applies: the panel's coverage is an amount,
        // the shader's is a threshold, and they run opposite ways.
        f("uCoverage", glm::mix(0.86f, 0.46f, s.coverage));
        f("uCloudDensity", 1.0f);
        f("uCloudScale", 0.0009f);
        f("uCloudSpeed", 5.0f);
        f("uCloudBottom", 700.0f);
        f("uCloudTop", 2400.0f);
        f("uCirrus", s.cirrus);
        f("uCirrusHeight", 1400.0f);
        f("uCirrusSpeed", 2.5f);
        f("uContrails", s.contrails);
        f("uExposure", 1.0f);
        glUniform1i(glGetUniformLocation(prog, "uTonemap"), 1);

        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glFinish();
        glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());

        const fs::path out = outDir / (std::string("sky_") + s.name + ".png");
        if (stbi_write_png(out.string().c_str(), kW, kH, 4, px.data(), kW * 4))
            std::printf("wrote %s\n", out.string().c_str());
        else
            std::printf("[FAIL] could not write %s\n", out.string().c_str());
    }

    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
