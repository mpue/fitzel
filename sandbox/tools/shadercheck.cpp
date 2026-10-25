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
// wrong overload here. The window is created hidden, and the shaders go through
// the window's rhi::Device -- the same createProgram the engine compiles with.
//
//   shadercheck [shaderDir]     (default: assets/shaders next to the exe)

#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

#include "fitzel/core/Window.hpp"
#include "fitzel/graphics/Shader.hpp"
#include "fitzel/rhi/Device.hpp"

namespace fs = std::filesystem;

namespace {

namespace rhi = fitzel::rhi;

// The device compiles a stage only as part of a program, so a stage checked on
// its own is paired with one of these. Neither reads or writes anything the
// real stage could clash with on its own terms; the program may still fail to
// link (a fragment input the stub does not write), which says nothing about the
// stage and is not reported.
constexpr const char* kStubVertex =
    "#version 330 core\nvoid main() { gl_Position = vec4(0.0); }\n";
constexpr const char* kStubFragment =
    "#version 330 core\nout vec4 fragColor;\nvoid main() { fragColor = vec4(1.0); }\n";

// Compile one stage; print the driver's log (an error, or warnings on success)
// with the file name in front of it, which is the only thing that makes a GLSL
// error line useful. The device writes a stage's words into the log as
// "<stage>: <driver log>"; a log without that prefix is a link error against
// the stub, or the stub's own.
void compile(bool vertex, const std::string& src, const fs::path& file, bool& ok) {
    rhi::ProgramSource ps;
    ps.vertex   = vertex ? src.c_str() : kStubVertex;
    ps.fragment = vertex ? kStubFragment : src.c_str();
    std::string log;
    const rhi::ProgramHandle prog = rhi::device().createProgram(ps, &log);
    const std::string prefix = vertex ? "vertex: " : "fragment: ";
    const bool ours = log.compare(0, prefix.size(), prefix) == 0;
    if (prog) {
        rhi::device().destroy(prog);
        if (ours)
            std::printf("[warn] %s:\n%s\n", file.filename().string().c_str(),
                        log.c_str() + prefix.size());
        return;
    }
    if (!ours) return;
    std::printf("[FAIL] %s:\n%s\n", file.filename().string().c_str(),
                log.c_str() + prefix.size());
    std::printf("[FAIL] %s did not compile\n", file.filename().string().c_str());
    ok = false;
}

} // namespace

int main(int argc, char** argv) {
    // The same context the renderer gets (4.3 core where the driver has it, 3.3
    // core where not): a shader that compiles in a compatibility context and not
    // in a core one is a shader that is broken for this engine.
    std::unique_ptr<fitzel::Window> window;
    try {
        fitzel::WindowConfig cfg;
        cfg.width   = 64;
        cfg.height  = 64;
        cfg.title   = "shadercheck";
        cfg.vsync   = false;
        cfg.visible = false;
        window = std::make_unique<fitzel::Window>(cfg);
    } catch (const std::exception& e) {
        std::printf("[FAIL] no GL core context: %s\n", e.what());
        return 2;
    }
    rhi::Device& gpu = rhi::device();
    std::printf("GL %s\n", gpu.caps().apiName);

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
        // With `#include "file"` expanded relative to the including file, as the
        // engine reads them. Without it a shader that includes sunshadow.glsl
        // would fail here and nowhere else.
        const std::string vs = vp.empty() ? std::string() : fitzel::Shader::readSource(vp.string());
        const std::string fsrc = fp.empty() ? std::string() : fitzel::Shader::readSource(fp.string());
        bool stagesOk = true;
        if (!vp.empty()) compile(true, vs, vp, stagesOk);
        if (!fp.empty()) compile(false, fsrc, fp, stagesOk);
        if (!stagesOk) ok = false;
        // Linked only when both stages compiled: otherwise the link can only
        // repeat the compile error already printed.
        if (!vp.empty() && !fp.empty() && stagesOk) {
            rhi::ProgramSource ps;
            ps.vertex   = vs.c_str();
            ps.fragment = fsrc.c_str();
            std::string log;
            const rhi::ProgramHandle prog = gpu.createProgram(ps, &log);
            if (prog) {
                gpu.destroy(prog);
            } else {
                std::printf("[FAIL] %s link:\n%s\n", name.c_str(), log.c_str());
                ok = false;
            }
        }
        ++checked;
    }

    std::printf("%s -- %d shader(s) in %s\n", ok ? "OK" : "FAILED", checked,
                dir.string().c_str());
    return ok ? 0 : 1;
}
