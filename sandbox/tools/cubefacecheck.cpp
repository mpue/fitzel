// cubefacecheck: does cubeface.glsl pick the texel a samplerCube picks?
//
// The browser build has no room for four point-shadow cubemaps (WebGL2: 16
// samplers per fragment shader), so it renders their faces into the layers of
// one 2D array and finds face and texel itself (cubeface.glsl, lit.frag). The
// layers hold exactly what the cube faces would, so the whole question is the
// lookup -- and that is asked here of the driver, not of the spec as read:
//
//  - the same numbers go into a cubemap and into a 6-layer array: texel (x, y)
//    of face f holds f * 10000 + y * 100 + x, so a value names where it came
//    from;
//  - a fragment shader turns 65536 directions over the whole sphere into one
//    texture() on the cube and one cubeFaceUv() + texture() on the array;
//  - every pair must agree. The only allowed differences are where the answer
//    is a coin toss -- a direction on a cube EDGE (two axes equal) or on a
//    texel boundary -- and those may only be off by one neighbouring texel or
//    land on the neighbouring face.
//
// Exit 0 = the lookups agree, 1 = they do not, 2 = no GL / no shader file.
//   build\release\bin\cubefacecheck.exe sandbox\assets\shaders\cubeface.glsl
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>

namespace {

constexpr int kFace = 64;   // texels per cube face edge
constexpr int kOutW = 256, kOutH = 256;

GLuint compile(GLenum stage, const std::string& src) {
    const GLuint sh = glCreateShader(stage);
    const char* s = src.c_str();
    glShaderSource(sh, 1, &s, nullptr);
    glCompileShader(sh);
    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(sh, sizeof log, nullptr, log);
        std::printf("[FAIL] shader:\n%s\n", log);
    }
    return sh;
}

} // namespace

int main(int argc, char** argv) {
    const std::string glslPath = argc > 1 ? argv[1] : "assets/shaders/cubeface.glsl";
    std::ifstream in(glslPath);
    if (!in) {
        std::printf("[FAIL] cannot read %s\n", glslPath.c_str());
        return 2;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string cubeFace = ss.str();

    if (!glfwInit()) return 2;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "cubefacecheck", nullptr, nullptr);
    if (!win) { std::printf("[FAIL] no GL 3.3 context\n"); return 2; }
    glfwMakeContextCurrent(win);
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress))) return 2;

    // The same numbers into both.
    std::vector<float> face(static_cast<std::size_t>(kFace) * kFace);
    GLuint cube = 0, arr = 0;
    glGenTextures(1, &cube);
    glGenTextures(1, &arr);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
    glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R32F, kFace, kFace, 6, 0, GL_RED, GL_FLOAT, nullptr);
    for (int f = 0; f < 6; ++f) {
        for (int y = 0; y < kFace; ++y)
            for (int x = 0; x < kFace; ++x)
                face[static_cast<std::size_t>(y) * kFace + x] =
                    static_cast<float>(f * 10000 + y * 100 + x);
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_R32F, kFace, kFace, 0,
                     GL_RED, GL_FLOAT, face.data());
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, f, kFace, kFace, 1, GL_RED,
                        GL_FLOAT, face.data());
    }
    for (GLenum t : {GLenum(GL_TEXTURE_CUBE_MAP), GLenum(GL_TEXTURE_2D_ARRAY)}) {
        glTexParameteri(t, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(t, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(t, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(t, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    // Directions on a Fibonacci sphere, one per output pixel; out = (cube,
    // array, how far from a cube edge: 0 = exactly on one).
    const std::string vs =
        "#version 330 core\n"
        "void main() {\n"
        "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
        "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    const std::string fs =
        "#version 330 core\n"
        "uniform samplerCube uCube;\n"
        "uniform sampler2DArray uArr;\n"
        "out vec4 o;\n" + cubeFace +
        "\nvoid main() {\n"
        "    float i = floor(gl_FragCoord.y) * 256.0 + floor(gl_FragCoord.x);\n"
        "    float n = 65536.0;\n"
        "    float z = 1.0 - 2.0 * (i + 0.5) / n;\n"
        "    float r = sqrt(max(0.0, 1.0 - z * z));\n"
        "    float phi = i * 2.39996323;\n"
        "    vec3 d = vec3(r * cos(phi), z, r * sin(phi));\n"
        "    vec3 f = cubeFaceUv(d);\n"
        "    vec3 a = abs(d);\n"
        "    float hi = max(a.x, max(a.y, a.z));\n"
        "    float mid = a.x + a.y + a.z - hi - min(a.x, min(a.y, a.z));\n"
        "    o = vec4(texture(uCube, d).r, texture(uArr, vec3(f.xy, f.z)).r,\n"
        "             (hi - mid) / hi, 1.0);\n"
        "}\n";
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(prog);
    GLint linked = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) { std::printf("[FAIL] link\n"); return 2; }

    GLuint out = 0, fbo = 0, vao = 0;
    glGenTextures(1, &out);
    glBindTexture(GL_TEXTURE_2D, out);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, kOutW, kOutH, 0, GL_RGBA, GL_FLOAT, nullptr);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out, 0);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glViewport(0, 0, kOutW, kOutH);
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
    glUniform1i(glGetUniformLocation(prog, "uCube"), 0);
    glUniform1i(glGetUniformLocation(prog, "uArr"), 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    std::vector<float> px(static_cast<std::size_t>(kOutW) * kOutH * 4);
    glReadPixels(0, 0, kOutW, kOutH, GL_RGBA, GL_FLOAT, px.data());

    int same = 0, edge = 0, neighbour = 0, wrong = 0;
    for (std::size_t i = 0; i < px.size(); i += 4) {
        const int c = static_cast<int>(px[i]), a = static_cast<int>(px[i + 1]);
        const float margin = px[i + 2];
        if (c == a) { ++same; continue; }
        const int cf = c / 10000, af = a / 10000;
        const int dx = std::abs(c % 100 - a % 100), dy = std::abs((c / 100) % 100 - (a / 100) % 100);
        if (cf != af) {
            if (margin < 1e-3f) { ++edge; continue; }
        } else if (dx <= 1 && dy <= 1) {
            ++neighbour;
            continue;
        }
        if (wrong < 8)
            std::printf("  mismatch: cube %d (face %d), array %d (face %d), edge margin %g\n",
                        c, cf, a, af, margin);
        ++wrong;
    }
    const int total = kOutW * kOutH;
    std::printf("GL %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    std::printf("%d directions: %d identical, %d on a cube edge, %d one texel off, %d wrong\n",
                total, same, edge, neighbour, wrong);
    // A texel boundary falls between the two by float rounding now and then;
    // more than a sliver of those means the (s, t) mapping is not the same.
    const bool ok = wrong == 0 && neighbour < total / 200;
    std::printf("%s\n", ok ? "OK -- cubeface.glsl matches the driver's cube lookup"
                           : "FAIL -- cubeface.glsl does not pick the cube's texels");
    glfwTerminate();
    return ok ? 0 : 1;
}
