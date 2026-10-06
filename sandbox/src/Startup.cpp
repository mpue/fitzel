#include "Startup.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#include <nlohmann/json.hpp>

#include <fitzel/asset/Vfs.hpp>

#if defined(_WIN32) && defined(FITZEL_WINDOWED)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>   // AttachConsole
#endif

using namespace fitzel;

namespace startup {

// --- Startup ---------------------------------------------------------------
// The handful of things that must happen before anything is loaded, hoisted out
// of main() so the top of the function reads as a list of what booting means
// rather than fifty lines of how.

// Release builds link as a GUI app (see sandbox/CMakeLists.txt), so
// double-clicking the exe no longer flashes up a console window. That would also
// throw away every fprintf(stderr) log line -- so if we WERE started from a
// terminal, adopt it and point the C streams back at it. Started from Explorer
// there is no parent console, AttachConsole fails, and the streams stay where
// they were (nowhere). Nothing else changes.
void adoptParentConsole() {
#if defined(_WIN32) && defined(FITZEL_WINDOWED)
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
#endif
}

// Resolve all relative paths (assets/, content/, scripts/, game.json, project/)
// against the executable's own directory, so the app behaves the same whether
// launched from a shell, a shortcut, or a double-click.
void setWorkingDirToExe(int argc, char** argv) {
    if (argc <= 0) return;
    std::error_code ec;
    const auto exePath = std::filesystem::absolute(argv[0], ec);
    if (!ec && exePath.has_parent_path())
        std::filesystem::current_path(exePath.parent_path(), ec);
}

// An exported game ships one encrypted archive next to the exe instead of loose
// content/, project/ and assets/ folders. Mounted before anything is read, so
// every load after this point -- textures, models, sounds, shaders, scenes,
// scripts -- resolves against it. With no archive present (dev runs, the editor)
// nothing changes: the VFS falls straight through to disk.
void mountGameArchive() {
    std::error_code ec;
    if (std::filesystem::exists("game.fpak", ec))
        fitzel::vfs::mount("game.fpak", std::filesystem::current_path(ec));
}

BootConfig loadBootConfig(int argc, char** argv) {
    BootConfig cfg;
    std::error_code ec;
    if (std::filesystem::exists("game.json", ec)) {
        std::ifstream gin("game.json");
        try {
            nlohmann::json gj; gin >> gj;
            cfg.project    = gj.value("project", std::string{});
            cfg.scene      = gj.value("startScene", std::string{});
            cfg.fullscreen = gj.value("fullscreen", true);
        } catch (...) {}
    }
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--play")            cfg.project     = argv[i + 1];
        else if (a == "--scene")      cfg.scene       = argv[i + 1];
        else if (a == "--profile")    cfg.profilePath = argv[i + 1];
        else if (a == "--profile-shot") cfg.profileShot = argv[i + 1];
        else if (a == "--profile-seconds")
            cfg.profileSeconds = std::max(1.0, std::atof(argv[i + 1]));
        else if (a == "--shots")      cfg.shotsPath   = argv[i + 1];
        else if (a == "--shots-out")  cfg.shotsOut    = argv[i + 1];
        else if (a == "--shots-trace") cfg.shotsTrace = std::atoi(argv[i + 1]);
        else if (a == "--shots-trace-gpu") cfg.shotsTraceGpu = std::atoi(argv[i + 1]) != 0;
        else if (a == "--open")       cfg.editorOpen  = argv[i + 1];
        else if (a == "--export")     cfg.exportDir   = argv[i + 1];
        else if (a == "--export-web") { cfg.exportDir = argv[i + 1]; cfg.exportWeb = true; }
    }
    return cfg;
}

ContentRoots resolveContentRoots() {
    const bool local = fitzel::vfs::isDirectory("content");
    ContentRoots r;
    r.content  = local ? std::filesystem::absolute("content").generic_string()
                       : std::string(FITZEL_CONTENT_DIR);
    r.models   = local ? r.content + "/models"   : std::string(FITZEL_MODEL_DIR);
    r.textures = local ? r.content + "/textures" : std::string(FITZEL_TEXTURE_DIR);
    r.sounds   = local ? r.content + "/sounds"   : std::string(FITZEL_SOUND_DIR);
    return r;
}

// All four in one go, so a program that failed to compile is reported here by
// name instead of turning up as a black screen halfway through the first frame.
// Returns false if a REQUIRED one failed. The skybox is not one of them: an HDRI
// background is optional, so losing it costs the background, not the session.
bool loadCoreShaders(CoreShaders& out) {
    const auto load = [](Shader& dst, const char* vert, const char* frag,
                         const char* name) {
        dst = Shader::fromFiles(vert, frag);
        if (!dst.isValid())
            std::fprintf(stderr, "Failed to load %s shader\n", name);
        return dst.isValid();
    };
    bool ok = true;
    ok = load(out.lit,   "assets/shaders/lit.vert",   "assets/shaders/lit.frag",   "lit")   && ok;
    ok = load(out.water, "assets/shaders/water.vert", "assets/shaders/water.frag", "water") && ok;
    ok = load(out.river, "assets/shaders/river.vert", "assets/shaders/river.frag", "river") && ok;
    ok = load(out.sky,   "assets/shaders/sky.vert",   "assets/shaders/sky.frag",   "sky")   && ok;
    load(out.skybox,     "assets/shaders/sky.vert",   "assets/shaders/skybox.frag", "skybox");
    return ok;
}

// --- Startup geometry ------------------------------------------------------

// A tessellated water grid so Gerstner waves can displace its vertices. Unit
// sized in XZ around the origin; the water pass scales it to the world.
Mesh makeWaterGrid(int n) {
    std::vector<Vertex>        verts;
    std::vector<std::uint32_t> idx;
    verts.reserve(static_cast<std::size_t>(n) * n);
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            const float fx = static_cast<float>(x) / (n - 1) - 0.5f;
            const float fz = static_cast<float>(z) / (n - 1) - 0.5f;
            verts.push_back({{fx, 0.0f, fz}, {0, 1, 0},
                             {static_cast<float>(x) / (n - 1),
                              static_cast<float>(z) / (n - 1)}});
        }
    }
    for (int z = 0; z < n - 1; ++z) {
        for (int x = 0; x < n - 1; ++x) {
            const std::uint32_t i0 = static_cast<std::uint32_t>(z * n + x);
            const std::uint32_t i1 = i0 + 1;
            const std::uint32_t i2 = i0 + static_cast<std::uint32_t>(n);
            const std::uint32_t i3 = i2 + 1;
            idx.insert(idx.end(), {i0, i2, i1, i1, i2, i3});
        }
    }
    return Mesh::create(verts, idx);
}

// The quad every fullscreen pass is drawn through: sky, HDRI skybox, post chain.
Mesh makeFullscreenQuad() {
    const std::vector<Vertex> verts = {
        {{-1.0f, -1.0f, 0.0f}, {0, 0, 1}, {0, 0}},
        {{ 1.0f, -1.0f, 0.0f}, {0, 0, 1}, {1, 0}},
        {{ 1.0f,  1.0f, 0.0f}, {0, 0, 1}, {1, 1}},
        {{-1.0f,  1.0f, 0.0f}, {0, 0, 1}, {0, 1}},
    };
    return Mesh::create(verts, {0, 1, 2, 0, 2, 3});
}

} // namespace startup
