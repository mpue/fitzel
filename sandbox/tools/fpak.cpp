// fpak -- pack, inspect and verify .fpak archives from the command line.
//
// The editor's "Export Game" writes one of these itself; this tool is for
// everything around that: staging an export by hand, checking what actually
// ended up inside one, and proving an archive reads back byte for byte before
// shipping it. It links the same fitzel::pak and fitzel::vfs the player uses, so
// what it reports is what the player will see.
//
//   fpak pack   <dir> <out.fpak>   pack every file under <dir>
//   fpak list   <archive>          entry paths and sizes
//   fpak verify <archive> <dir>    compare every entry against the original tree
//   fpak decode <archive>          decode every model, image and sound in it
//
// Verify answers "did the bytes survive"; decode answers the question that
// actually decides whether a packed game runs: do the importers -- assimp,
// cgltf, stb -- still get what they need when there is no file behind the path.
// Both mount the archive through the VFS exactly as the player does. Neither
// needs a GL context: model import and thumbnail decoding are pure CPU.

#include <cstdio>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fitzel/audio/Audio.hpp>
#include <fitzel/asset/Pak.hpp>
#include <fitzel/asset/Vfs.hpp>
#include <fitzel/graphics/Texture.hpp>
#include <fitzel/world/Model.hpp>

namespace fs = std::filesystem;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  fpak pack   <dir> <out.fpak>\n"
                 "  fpak list   <archive>\n"
                 "  fpak verify <archive> <dir>\n"
                 "  fpak decode <archive>\n");
    return 2;
}

std::vector<std::uint8_t> readDisk(const fs::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const std::streamoff size = in.tellg();
    std::vector<std::uint8_t> out(static_cast<std::size_t>(size < 0 ? 0 : size));
    in.seekg(0);
    if (!out.empty())
        in.read(reinterpret_cast<char*>(out.data()),
                static_cast<std::streamsize>(out.size()));
    return out;
}

int doPack(const fs::path& dir, const fs::path& out) {
    const fitzel::pak::WriteResult r = fitzel::pak::write(dir, out);
    if (!r.ok) {
        std::fprintf(stderr, "pack failed: %s\n", r.error.c_str());
        return 1;
    }
    std::printf("packed %zu files, %.1f MB payload -> %s\n", r.files,
                static_cast<double>(r.bytes) / (1024.0 * 1024.0),
                out.generic_string().c_str());
    return 0;
}

// Mount at the archive's own directory, which is how the player mounts it: the
// entries are relative to wherever the exe lives.
bool mountAt(const fs::path& archive) {
    fs::path root = archive.parent_path();
    if (root.empty()) root = fs::current_path();
    if (!fitzel::vfs::mount(archive, root)) {
        std::fprintf(stderr, "cannot open archive '%s'\n",
                     archive.generic_string().c_str());
        return false;
    }
    return true;
}

int doList(const fs::path& archive) {
    if (!mountAt(archive)) return 1;
    const fs::path root = archive.parent_path().empty() ? fs::current_path()
                                                        : archive.parent_path();
    std::size_t     count = 0;
    std::uint64_t   total = 0;
    for (const std::string& f : fitzel::vfs::listFiles(root.generic_string(), true)) {
        if (!fitzel::vfs::inPak(f)) continue; // loose files next to it are not entries
        const std::size_t size = fitzel::vfs::read(f).size();
        std::printf("%10zu  %s\n", size,
                    fs::relative(f, root).generic_string().c_str());
        total += size;
        ++count;
    }
    std::printf("%zu entries, %.1f MB\n", count,
                static_cast<double>(total) / (1024.0 * 1024.0));
    return 0;
}

int doVerify(const fs::path& archive, const fs::path& dir) {
    if (!mountAt(archive)) return 1;
    std::size_t checked = 0, bad = 0, missing = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string rel = fs::relative(it->path(), dir, ec).generic_string();
        if (rel.empty()) continue;

        // Ask the VFS the way the engine does -- relative to the mount root -- so
        // a hit proves the archive answered, not the file still sitting on disk.
        const fs::path mounted = (archive.parent_path().empty()
                                      ? fs::current_path()
                                      : archive.parent_path()) / rel;
        if (!fitzel::vfs::inPak(mounted.generic_string())) {
            std::fprintf(stderr, "MISSING  %s\n", rel.c_str());
            ++missing;
            continue;
        }
        if (fitzel::vfs::read(mounted.generic_string()) != readDisk(it->path())) {
            std::fprintf(stderr, "MISMATCH %s\n", rel.c_str());
            ++bad;
        }
        ++checked;
    }
    std::printf("verified %zu files, %zu mismatched, %zu missing\n", checked, bad,
                missing);
    return (bad || missing) ? 1 : 0;
}

std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

int doDecode(const fs::path& archive) {
    if (!mountAt(archive)) return 1;
    const fs::path root = archive.parent_path().empty() ? fs::current_path()
                                                        : archive.parent_path();
    // Sounds go through the real audio engine (it opens the default device), so
    // what is exercised is miniaudio's own VFS hook -- the thing that decides
    // whether a packed game has any sound at all.
    fitzel::Audio audio;
    std::size_t models = 0, images = 0, sounds = 0, failed = 0;
    for (const std::string& f : fitzel::vfs::listFiles(root.generic_string(), true)) {
        if (!fitzel::vfs::inPak(f)) continue; // loose neighbours are not our business
        const fs::path    p   = f;
        const std::string ext = lowerExt(p);
        const std::string rel = fs::relative(f, root).generic_string();

        if (ext == ".glb" || ext == ".gltf" || ext == ".dae" || ext == ".fbx") {
            const fitzel::ModelData md =
                ext == ".dae"   ? fitzel::loadCollada(f)
                : ext == ".fbx" ? fitzel::loadSkinnedModel(f)
                                : fitzel::loadGltf(f);
            if (md.empty()) {
                std::fprintf(stderr, "FAIL  model  %s\n", rel.c_str());
                ++failed;
            } else {
                std::size_t verts = 0;
                for (const auto& prim : md.primitives) verts += prim.vertices.size();
                std::printf("ok    model  %-52s %zu prims, %zu floats\n", rel.c_str(),
                            md.primitives.size(), verts);
                ++models;
            }
        } else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
                   ext == ".tga" || ext == ".bmp" || ext == ".exr") {
            const fitzel::ImagePixels img = fitzel::Texture::decodeThumbnail(f, 64);
            if (!img.valid()) {
                std::fprintf(stderr, "FAIL  image  %s\n", rel.c_str());
                ++failed;
            } else {
                std::printf("ok    image  %-52s %dx%d\n", rel.c_str(), img.width,
                            img.height);
                ++images;
            }
        } else if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" ||
                   ext == ".flac") {
            if (!audio.ok()) continue; // no output device: nothing to prove here
            const fitzel::Sound s = fitzel::Sound::fromFile(audio, f);
            if (!s.isValid()) {
                std::fprintf(stderr, "FAIL  sound  %s\n", rel.c_str());
                ++failed;
            } else {
                std::printf("ok    sound  %s\n", rel.c_str());
                ++sounds;
            }
        }
    }
    std::printf("decoded %zu models, %zu images, %zu sounds, %zu failed\n", models,
                images, sounds, failed);
    return failed ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string cmd = argv[1];
    if (cmd == "pack" && argc == 4)   return doPack(argv[2], argv[3]);
    if (cmd == "list" && argc == 3)   return doList(argv[2]);
    if (cmd == "verify" && argc == 4) return doVerify(argv[2], argv[3]);
    if (cmd == "decode" && argc == 3) return doDecode(argv[2]);
    return usage();
}
