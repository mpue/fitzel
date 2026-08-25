// cloudcheck -- render the cloud system on its own and write the frames out.
//
// The same job skycheck does for the sky shader, and for the same reason: the
// previous cloud system was tuned by reasoning about noise functions and hoping,
// and "the clouds look wrong" is not a thing reasoning is good at. It took a
// photograph held up next to a render to see that the shape could never have
// been right, whatever the lighting did.
//
// It drives the REAL CloudField -- same bake, same atlas, same instanced draw --
// rather than a simplified copy, because a look tool that checks a different
// code path from the one that ships is worse than none.
//
// Two kinds of shot. One cloud filling the frame is where a wrong bulge is
// actually visible; a whole sky is where placement, repetition and the sheer
// number of them show up. Both are needed and neither substitutes for the other.
//
//   cloudcheck [outDir] [shaderDir]

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <fitzel/graphics/Shader.hpp>

#include "../src/CloudField.hpp"

namespace fs = std::filesystem;

namespace {

constexpr int kW = 1100, kH = 700;

// A plain sky gradient to stand the clouds against. Its own small shader rather
// than the engine's: this tool is about the CLOUDS, and pulling sky.frag in would
// drag its uniforms, its cirrus and its own march along for no gain.
const char* kSkyVert = R"(#version 330 core
layout(location = 0) in vec2 aPos;
out vec2 vNdc;
void main() { vNdc = aPos; gl_Position = vec4(aPos, 1.0, 1.0); }
)";

const char* kSkyFrag = R"(#version 330 core
in vec2 vNdc;
out vec4 FragColor;
uniform mat4 uInvViewProj;
uniform vec3 uCameraPos;
uniform vec3 uSunDir;
void main() {
    vec4 far = uInvViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 dir = normalize(far.xyz / far.w - uCameraPos);
    float h = clamp(dir.y, 0.0, 1.0);
    vec3 col = mix(vec3(0.62, 0.76, 0.92), vec3(0.13, 0.36, 0.78), pow(h, 0.55));
    float sd = max(dot(dir, uSunDir), 0.0);
    col += vec3(1.0, 0.95, 0.85) * pow(sd, 900.0) * 3.0;
    FragColor = vec4(col, 1.0);
}
)";

struct Shot {
    const char* name;
    bool  single;        // one cloud filling the frame, or a whole sky
    float sunElevDeg;
    float sunAzimDeg;    // 0 = behind the camera, 180 = behind the clouds
    float pitchDeg;      // sky shots only: how far up the camera looks
    // Species weights, so a shot can ask for one kind.
    float wH, wM, wC;
    std::uint32_t seed;
};

} // namespace

int main(int argc, char** argv) {
    const fs::path outDir = (argc > 1) ? fs::path(argv[1]) : fs::path(".");
    const std::string shDir = (argc > 2) ? std::string(argv[2]) : std::string("assets/shaders");

    if (!glfwInit()) { std::printf("[FAIL] glfw\n"); return 2; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "cloudcheck", nullptr, nullptr);
    if (!win) { std::printf("[FAIL] no GL context\n"); glfwTerminate(); return 2; }
    glfwMakeContextCurrent(win);
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress))) {
        std::printf("[FAIL] glad\n"); return 2;
    }
    std::printf("GL %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    clouds::CloudField field;
    if (!field.init(shDir)) return 1;

    fitzel::Shader sky = fitzel::Shader::fromSource(kSkyVert, kSkyFrag);
    if (!sky.isValid()) { std::printf("[FAIL] built-in sky shader\n"); return 1; }

    const float quad[] = {-1, -1,  1, -1,  1, 1,  -1, -1,  1, 1,  -1, 1};
    GLuint quadVao = 0, quadVbo = 0;
    glGenVertexArrays(1, &quadVao);
    glGenBuffers(1, &quadVbo);
    glBindVertexArray(quadVao);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindVertexArray(0);

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
        // One cloud, close up. The picture to hold against a photograph.
        {"one_congestus",  true,  52.0f,  60.0f,  0.0f, 0.0f, 0.0f, 1.0f,  7u},
        {"one_backlit",    true,  38.0f, 168.0f,  0.0f, 0.0f, 0.0f, 1.0f,  7u},
        {"one_humilis",    true,  50.0f,  55.0f,  0.0f, 1.0f, 0.0f, 0.0f, 11u},
        // A whole sky. Flat is what a driver sees; steep is what shows placement
        // and whether eight variants are enough to hide the repetition.
        {"sky_fair",       false, 55.0f,  70.0f, 12.0f, 0.55f, 0.35f, 0.10f, 3u},
        {"sky_fair_up",    false, 55.0f,  70.0f, 34.0f, 0.55f, 0.35f, 0.10f, 3u},
        {"sky_building",   false, 44.0f,  75.0f, 20.0f, 0.20f, 0.40f, 0.40f, 9u},
        {"sky_evening",    false, 12.0f, 120.0f, 14.0f, 0.45f, 0.40f, 0.15f, 5u},
    };

    std::vector<unsigned char> px(static_cast<std::size_t>(kW) * kH * 4);
    stbi_flip_vertically_on_write(1);

    for (const Shot& s : shots) {
        clouds::Settings cfg;
        cfg.seed       = s.seed;
        cfg.wHumilis   = s.wH;
        cfg.wMediocris = s.wM;
        cfg.wCongestus = s.wC;
        if (s.single) {
            // One cloud at the origin, big enough to fill the frame.
            cfg.count      = 1;
            cfg.spread     = 0.0f;
            cfg.baseJitter = 0.0f;
            cfg.sizeMin = cfg.sizeMax = 1100.0f;
            cfg.baseHeight = 1400.0f;
        } else {
            cfg.baseHeight = 1350.0f;   // count/spread/size: the defaults
        }
        field.bake(cfg);
        if (field.instances().empty()) { std::printf("[FAIL] no instances %s\n", s.name); continue; }

        const float el = glm::radians(s.sunElevDeg), az = glm::radians(s.sunAzimDeg);
        const glm::vec3 sunDir = glm::normalize(glm::vec3(
            std::cos(el) * std::sin(az), std::sin(el), -std::cos(el) * std::cos(az)));
        // The ENGINE's sun and ambient, copied line for line from where main.cpp
        // builds them -- not something similar-looking.
        //
        // They were something similar-looking, and it hid two bugs: the fill was
        // being gamma'd in one place and not the other (a factor of ten on the
        // shaded side of every cloud), and the sun was dimmed twice in the
        // engine and once here. A look tool whose light differs from the
        // engine's cannot be used to judge lighting, which is most of what one
        // looks at a cloud for.
        const float dayF   = glm::smoothstep(-0.12f, 0.18f, sunDir.y);
        const float lowSun = 1.0f - glm::clamp(sunDir.y / 0.3f, 0.0f, 1.0f);
        const glm::vec3 sunCol =
            glm::mix(glm::vec3(1.0f, 0.97f, 0.9f), glm::vec3(1.0f, 0.55f, 0.26f), lowSun) *
            (0.12f + 0.95f * dayF) * 3.4f;
        // Linear already; the lit shader takes this value straight.
        const glm::vec3 ambient = glm::mix(glm::vec3(0.015f, 0.02f, 0.04f),
                                           glm::vec3(0.12f, 0.14f, 0.18f), dayF);

        glm::vec3 eye, target;
        float fov = 30.0f;
        if (s.single) {
            const clouds::Instance& in = field.instances()[0];
            eye    = glm::vec3(0.0f, in.centre.y * 0.12f, -5200.0f);
            target = in.centre;
            fov    = 28.0f;
        } else {
            const float pitch = glm::radians(s.pitchDeg);
            eye    = glm::vec3(0.0f, 60.0f, 0.0f);
            target = eye + glm::vec3(std::cos(pitch) * std::sin(glm::radians(20.0f)),
                                     std::sin(pitch),
                                     std::cos(pitch) * std::cos(glm::radians(20.0f)));
            fov    = 62.0f;
        }
        const glm::mat4 view = glm::lookAt(eye, target, glm::vec3(0, 1, 0));
        // The SCENE's far plane, not a generous one. It is capped at 5 km in the
        // engine (fitted to terrain and skyline), and rendering the check at
        // 80 km hid the fact that a cloud field reaching to the horizon is
        // mostly outside it -- the tool showed a sky the editor could not draw.
        // A look tool has to lie about nothing, least of all the frustum.
        const glm::mat4 proj = glm::perspective(glm::radians(fov),
                                                float(kW) / float(kH), 1.0f, 5000.0f);
        const glm::mat4 vp = proj * view;

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        sky.bind();
        sky.setMat4("uInvViewProj", glm::inverse(vp));
        sky.setVec3("uCameraPos", eye);
        sky.setVec3("uSunDir", sunDir);
        glBindVertexArray(quadVao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        field.render(vp, eye, sunDir, sunCol, ambient,
                     /*fogDensity=*/2.5e-5f, /*exposure=*/1.0f, /*tonemap=*/true);
        glFinish();

        // What it costs. Not a benchmark -- one GPU, one resolution, no scene in
        // front of it -- but the number nobody had, and the one the literature
        // sets a bar for: a well-optimised volumetric sky is 2-3 ms. Measured by
        // repeating the cloud pass alone, so the sky gradient and the readback
        // stay out of it.
        constexpr int kReps = 30;
        const auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < kReps; ++r)
            field.render(vp, eye, sunDir, sunCol, ambient, 2.5e-5f, 1.0f, true);
        glFinish();
        const auto t1 = std::chrono::steady_clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count() / kReps;

        glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        const std::string path = (outDir / ("cloud_" + std::string(s.name) + ".png")).string();
        stbi_write_png(path.c_str(), kW, kH, 4, px.data(), kW * 4);
        std::printf("wrote %s  (%d clouds, atlas %.1f MB, %.2f ms)\n", path.c_str(),
                    static_cast<int>(field.instances().size()),
                    static_cast<double>(field.atlasBytes()) / (1024.0 * 1024.0), ms);
    }

    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
