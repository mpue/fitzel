// The S.T.A.L.K.E.R. level converter: a level of an installed game on GSC's
// X-Ray engine (Clear Sky, Call of Pripyat, or any of the 2025 Enhanced
// Editions, Shadow of Chornobyl's included) read straight out of the game's
// .db archives and written into a fitzel project as a model and a scene --
// see StalkerImport.hpp for what lands where.
//
//   stalkerimport <game folder> --list
//   stalkerimport <game folder> <level>|all <project folder> [options]
//       --cell <m>          map cell = one model node (default 64)
//       --max-texture <px>  halve larger maps down to this (default 2048)
//       --terrain <px/m>    texels per metre of the baked terrain (default 8,
//                           0 = the game's stretched colour map)
//       --no-lift           keep the game's heights (parts of the level then lie
//                           under the floor fitzel plays a terrain-less scene on)
//       --no-props          leave out the props level.spawn places
//       --no-life           no people, creatures, anomalies, sounds or logic
//       --no-scene          the model and its textures only
//   stalkerimport <game folder> --files [path prefix]
//       list the files the game has (archives and loose gamedata/)
//   stalkerimport <game folder> --extract <path prefix> <out folder>
//       unpack archive files as they are ("levels/l01_escape/", "textures/mtl/")
//
// Then open <project folder> in the editor and the level's scene in it.
// Exits non-zero if the game cannot be read or a level fails to convert.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/StalkerImport.hpp"
#include "../src/StalkerLife.hpp"
#include "../src/XrArchive.hpp"

namespace fs = std::filesystem;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: stalkerimport <game folder> --list\n"
                 "       stalkerimport <game folder> <level>|all <project folder>\n"
                 "                     [--cell <m>] [--max-texture <px>] [--terrain <px/m>] [--no-lift] [--no-props] [--no-life] [--no-scene]\n"
                 "       stalkerimport <game folder> --files [path prefix]\n"
                 "       stalkerimport <game folder> --extract <path prefix> <out folder>\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const fs::path game = argv[1];
    const std::string what = argv[2];

    xray::FileSystem gfs;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    if (!gfs.mount(game, err)) {
        std::fprintf(stderr, "stalkerimport: %s\n", err.c_str());
        return 1;
    }
    if (!err.empty()) std::fprintf(stderr, "stalkerimport: warning: %s\n", err.c_str());
    std::printf("%zu archives, %zu files\n", gfs.archiveCount(), gfs.fileCount());

    if (what == "--list") {
        for (const std::string& l : gfs.levels()) std::printf("%s\n", l.c_str());
        return 0;
    }

    if (what == "--files") {
        const std::string prefix = argc > 3 ? xray::FileSystem::normalize(argv[3]) : std::string();
        for (const std::string& path : gfs.paths())
            if (path.rfind(prefix, 0) == 0) std::printf("%s\n", path.c_str());
        return 0;
    }

    if (what == "--extract") {
        if (argc < 5) return usage();
        // The same listing the archive has, filtered by prefix; written out by
        // reading each name the levels and textures are known by.
        const std::string prefix = xray::FileSystem::normalize(argv[3]);
        const fs::path out = argv[4];
        int n = 0;
        for (const std::string& path : gfs.paths()) {
            if (path.rfind(prefix, 0) != 0) continue;
            // An archive's names are its own word: none may climb out of `out`.
            if (path.find("..") != std::string::npos || path.find(':') != std::string::npos) {
                std::fprintf(stderr, "stalkerimport: skipping %s\n", path.c_str());
                continue;
            }
            std::vector<std::uint8_t> bytes;
            if (!gfs.read(path, bytes)) {
                std::fprintf(stderr, "stalkerimport: cannot read %s\n", path.c_str());
                continue;
            }
            const fs::path dst = out / path;
            std::error_code ec;
            fs::create_directories(dst.parent_path(), ec);
            std::ofstream f(dst, std::ios::binary);
            f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            ++n;
        }
        std::printf("%d files\n", n);
        return n > 0 ? 0 : 1;
    }

    if (argc < 4) return usage();
    stalker::Options o;
    o.project = argv[3];
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--cell" && i + 1 < argc) o.cell = std::max(8.0f, static_cast<float>(std::atof(argv[++i])));
        else if (a == "--max-texture" && i + 1 < argc) o.maxTexture = std::max(16, std::atoi(argv[++i]));
        else if (a == "--terrain" && i + 1 < argc) o.terrainDensity = std::max(0.0f, static_cast<float>(std::atof(argv[++i])));
        else if (a == "--no-lift") o.liftAboveGround = false;
        else if (a == "--no-props") o.props = false;
        else if (a == "--no-scene") o.writeScene = false;
        else if (a == "--no-life") o.life = false;
        else return usage();
    }

    std::vector<std::string> levels;
    if (what == "all") levels = gfs.levels();
    else levels.push_back(what);
    if (levels.empty()) {
        std::fprintf(stderr, "stalkerimport: the game has no levels\n");
        return 1;
    }

    // One session for the run: the configs are read once, and a figure or sound
    // used by several levels is written once.
    stalker::Session session;
    o.session = &session;
    int failed = 0;
    for (const std::string& l : levels) {
        o.level = l;
        stalker::Report r;
        if (!stalker::importLevel(gfs, o, r, err, [](const std::string& s) { std::printf("  %s\n", s.c_str()); })) {
            std::fprintf(stderr, "stalkerimport: %s: %s\n", l.c_str(), err.c_str());
            ++failed;
            continue;
        }
        for (const std::string& w : r.warnings) std::printf("  warning: %s\n", w.c_str());
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%zu level(s), %d failed, %.1f s\n", levels.size(), failed, secs);
    return failed ? 1 : 0;
}
