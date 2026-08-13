// shadercheck -- compile and link every shader pair in sandbox/assets/shaders
// against a real GL context, and report what the driver says.
//
// Why this exists: a broken shader is silent. The engine logs its compile error
// to a stdio stream that a /SUBSYSTEM:WINDOWS build has no console for, then
// carries on drawing with an unlinked program -- so the effect simply doesn't
// appear, and nothing anywhere says why. A smoke test ("it started, it drew
// something") cannot catch that. This can: it is a console program, it fails
// loudly, and it exits non-zero, so it belongs in the loop after every shader
// edit.
//
// It needs a GL context because GLSL is compiled by the driver, not by us: only
// the real compiler knows whether a uniform is undeclared or a built-in has the
// wrong overload here. The window is created hidden.
//
//   shadercheck [shaderDir]     (default: assets/shaders next to the exe)

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>

namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Compile one stage; on failure print the driver's log with the file name in
// front of it, which is the only thing that makes a GLSL error line useful.
GLuint compile(GLenum stage, const std::string& src, const fs::path& file,
               bool& ok) {
    const GLuint sh = glCreateShader(stage);
    const char* s = src.c_str();
    glShaderSource(sh, 1, &s, nullptr);
    glCompileShader(sh);
    GLint good = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &good);
    GLint logLen = 0;
    glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &logLen);
    if (logLen > 1) {
        std::vector<char> log(static_cast<std::size_t>(logLen));
        glGetShaderInfoLog(sh, logLen, nullptr, log.data());
        std::printf("%s %s:\n%s\n", good ? "[warn]" : "[FAIL]",
                    file.filename().string().c_str(), log.data());
    }
    if (!good) {
        std::printf("[FAIL] %s did not compile\n", file.filename().string().c_str());
        ok = false;
    }
    return sh;
}

} // namespace

int main(int argc, char** argv) {
    if (!glfwInit()) {
        std::printf("[FAIL] glfwInit\n");
        return 2;
    }
    // The same context the renderer asks for: a shader that compiles under 4.6
    // and not under 3.3 core is a shader that is broken for this engine.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "shadercheck", nullptr, nullptr);
    if (!win) {
        std::printf("[FAIL] no GL 3.3 core context\n");
        glfwTerminate();
        return 2;
    }
    glfwMakeContextCurrent(win);
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress))) {
        std::printf("[FAIL] glad\n");
        return 2;
    }
    std::printf("GL %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    fs::path dir = (argc > 1) ? fs::path(argv[1])
                              : fs::path(argv[0]).parent_path() / "assets" / "shaders";
    if (!fs::is_directory(dir)) {
        std::printf("[FAIL] no shader dir: %s\n", dir.string().c_str());
        return 2;
    }

    // Pair up <name>.vert with <name>.frag; a stage without its partner is
    // compiled on its own (still worth knowing whether it is valid).
    std::map<std::string, std::pair<fs::path, fs::path>> pairs;
    for (const auto& e : fs::directory_iterator(dir)) {
        const std::string ext = e.path().extension().string();
        const std::string stem = e.path().stem().string();
        if (ext == ".vert") pairs[stem].first = e.path();
        else if (ext == ".frag") pairs[stem].second = e.path();
    }

    bool ok = true;
    int checked = 0;
    for (const auto& [name, vf] : pairs) {
        const auto& [vp, fp] = vf;
        GLuint prog = glCreateProgram();
        if (!vp.empty()) glAttachShader(prog, compile(GL_VERTEX_SHADER, readFile(vp), vp, ok));
        if (!fp.empty()) glAttachShader(prog, compile(GL_FRAGMENT_SHADER, readFile(fp), fp, ok));
        if (!vp.empty() && !fp.empty()) {
            glLinkProgram(prog);
            GLint linked = GL_FALSE;
            glGetProgramiv(prog, GL_LINK_STATUS, &linked);
            if (!linked) {
                GLint logLen = 0;
                glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &logLen);
                std::vector<char> log(static_cast<std::size_t>(logLen > 1 ? logLen : 1));
                if (logLen > 1) glGetProgramInfoLog(prog, logLen, nullptr, log.data());
                std::printf("[FAIL] %s link:\n%s\n", name.c_str(), log.data());
                ok = false;
            }
        }
        ++checked;
    }

    std::printf("%s -- %d shader(s) in %s\n", ok ? "OK" : "FAILED", checked,
                dir.string().c_str());
    glfwDestroyWindow(win);
    glfwTerminate();
    return ok ? 0 : 1;
}
