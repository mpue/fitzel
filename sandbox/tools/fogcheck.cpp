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
    if (!glfwInit()) { std::printf("[FAIL] glfw\n"); return 2; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "fogcheck", nullptr, nullptr);
    if (!win) { std::printf("[FAIL] no GL context\n"); glfwTerminate(); return 2; }
    glfwMakeContextCurrent(win);
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress))) {
        std::printf("[FAIL] glad\n"); return 2;
    }
    std::printf("GL %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    const GLuint vs = compile(GL_VERTEX_SHADER, readFile(shDir / "sky.vert"), "sky.vert");
    const GLuint marchFs =
        compile(GL_FRAGMENT_SHADER, readFile(shDir / "volfog.frag"), "volfog.frag");
    const GLuint composeFs = compile(GL_FRAGMENT_SHADER, kComposeFrag, "compose");
    if (!vs || !marchFs || !composeFs) return 1;
    const GLuint march   = link(vs, marchFs,   "march");
    const GLuint compose = link(vs, composeFs, "compose");
    if (!march || !compose) return 1;

    const float quad[] = {-1, -1,  1, -1,  1, 1,  -1, -1,  1, 1,  -1, 1};
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    const GLuint noiseTex = bakeFogNoise();
    if (!noiseTex) { std::printf("[FAIL] noise bake\n"); return 2; }

    // The field itself, before any marching: three orthogonal slices of the
    // baked volume, written out raw. When the fog comes out streaky the first
    // question is always whether the FIELD is streaky or whether the integral
    // along the ray made it look that way, and those two have completely
    // different fixes. This answers it in one look and costs a read-back.
    {
        const int N = 64;
        std::vector<unsigned char> vol(static_cast<std::size_t>(N) * N * N * 4);
        glBindTexture(GL_TEXTURE_3D, noiseTex);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glGetTexImage(GL_TEXTURE_3D, 0, GL_RGBA, GL_UNSIGNED_BYTE, vol.data());
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
    GLuint shadowTex = 0;
    glGenTextures(1, &shadowTex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, shadowTex);
    const float one = 1.0f;
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R32F, 1, 1, 1, 0, GL_RED, GL_FLOAT, &one);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // The fog buffer: 16F, because the in-scatter around the sun goes past 1.
    GLuint fogTex = 0, fogFbo = 0;
    glGenTextures(1, &fogTex);
    glBindTexture(GL_TEXTURE_2D, fogTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, kW, kH, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &fogFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fogFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fogTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::printf("[FAIL] fog framebuffer\n"); return 2;
    }

    GLuint outTex = 0, outFbo = 0;
    glGenTextures(1, &outTex);
    glBindTexture(GL_TEXTURE_2D, outTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &outFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, outFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::printf("[FAIL] out framebuffer\n"); return 2;
    }

    // The depth the march stops against, built on the CPU: a ground plane at
    // y = 0 and sky everywhere else, in the same window-space depth the engine's
    // buffer holds. Real geometry would be a second thing that can be wrong.
    GLuint depthTex = 0;
    glGenTextures(1, &depthTex);
    glBindTexture(GL_TEXTURE_2D, depthTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    std::vector<float> depth(static_cast<std::size_t>(kW) * kH);

    glViewport(0, 0, kW, kH);
    std::vector<unsigned char> px(static_cast<std::size_t>(kW) * kH * 4);
    stbi_flip_vertically_on_write(1);   // GL reads bottom-up

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
        glBindTexture(GL_TEXTURE_2D, depthTex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, kW, kH, 0, GL_RED, GL_FLOAT, depth.data());

        // --- The march ------------------------------------------------------
        const glm::mat4 model =
            glm::translate(glm::mat4(1.0f), boxCenter) * glm::scale(glm::mat4(1.0f), boxSize);
        const glm::mat4 invModel = glm::inverse(model);

        glBindFramebuffer(GL_FRAMEBUFFER, fogFbo);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(march);

        auto f  = [&](const char* n, float v) { glUniform1f(glGetUniformLocation(march, n), v); };
        auto i1 = [&](const char* n, int v)   { glUniform1i(glGetUniformLocation(march, n), v); };
        auto v3 = [&](const char* n, const glm::vec3& v) {
            glUniform3fv(glGetUniformLocation(march, n), 1, glm::value_ptr(v)); };
        auto m4 = [&](const char* n, const glm::mat4& v) {
            glUniformMatrix4fv(glGetUniformLocation(march, n), 1, GL_FALSE,
                               glm::value_ptr(v)); };

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, depthTex);
        i1("uDepth", 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_3D, noiseTex);
        i1("uNoise", 1);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D_ARRAY, shadowTex);
        i1("uShadowMap", 2);
        i1("uCascadeCount", 0);
        i1("uShafts", 0);        // no cascades here, so no shafts to sample

        m4("uInvViewProj", invVP);
        glUniform2f(glGetUniformLocation(march, "uTargetSize"), float(kW), float(kH));
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
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // --- Over the background --------------------------------------------
        glBindFramebuffer(GL_FRAMEBUFFER, outFbo);
        glUseProgram(compose);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, fogTex);
        glUniform1i(glGetUniformLocation(compose, "uFog"), 0);
        glUniformMatrix4fv(glGetUniformLocation(compose, "uInvViewProj"), 1, GL_FALSE,
                           glm::value_ptr(invVP));
        glUniform3fv(glGetUniformLocation(compose, "uCamPos"), 1, glm::value_ptr(eye));
        glUniform3fv(glGetUniformLocation(compose, "uSunDir"), 1, glm::value_ptr(sunDir));
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        const fs::path file = outDir / (std::string("fog_") + sh.name + ".png");
        fs::create_directories(outDir);
        if (!stbi_write_png(file.string().c_str(), kW, kH, 4, px.data(), kW * 4))
            std::printf("[FAIL] write %s\n", file.string().c_str());
        else
            std::printf("  %-16s vertical %.1fx  -> %s\n", sh.name, sh.vertical,
                        file.string().c_str());
    }

    glDeleteTextures(1, &noiseTex);
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
