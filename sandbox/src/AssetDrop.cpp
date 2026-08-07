#include "AssetDrop.hpp"

#include <cstdio>
#include <filesystem>
#include <functional>
#include <system_error>

#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/asset/AssetTypes.hpp>

#include "VideoImport.hpp"

namespace assetdrop {
namespace {

namespace fs = std::filesystem;

// Where each type lives inside a project. Lower-case, matching the materials/
// folder ProjectIO already writes next to the scene.
const char* folderFor(fitzel::AssetType t) {
    switch (t) {
        case fitzel::AssetType::Texture:  return "textures";
        case fitzel::AssetType::Model:    return "models";
        case fitzel::AssetType::Sound:    return "sounds";
        case fitzel::AssetType::Material: return "materials";
        case fitzel::AssetType::Video:    return "videos";
        case fitzel::AssetType::Unknown:  break;
    }
    return nullptr;
}

// Dragging a texture pack's folder across is the point of accepting folders at
// all; dragging a drive root across is not. Bound both the depth and the total, so
// a misaimed drop can't walk off into the filesystem.
constexpr int kMaxDepth = 8;
constexpr int kMaxFiles = 512;

} // namespace

Result importInto(const std::string& projectFolder,
                  const std::vector<std::string>& paths, fitzel::AssetDatabase& db) {
    Result r;
    if (projectFolder.empty()) {
        r.message = "No project open -- dropped files are copied into the project "
                    "folder, so open or save one first.";
        return r;
    }

    std::error_code ec;
    std::function<void(const fs::path&, int)> take = [&](const fs::path& p, int depth) {
        if (r.imported + r.existing + r.skipped >= kMaxFiles) return;
        if (fs::is_directory(p, ec)) {
            if (depth >= kMaxDepth) return;
            for (const auto& de : fs::directory_iterator(
                     p, fs::directory_options::skip_permission_denied, ec))
                take(de.path(), depth + 1);
            return;
        }
        // A source video (mp4/mov/...) isn't an asset and never becomes one:
        // the importer transcodes it into the engine's own .fvid, and *that* is
        // what lands in the project. Done before the type lookup below, which
        // would otherwise call an .mp4 unknown and skip it.
        if (videoimport::isSourceVideo(p.extension().string())) {
            const fs::path dst =
                fs::path(projectFolder) / "videos" / (p.stem().string() + ".fvid");
            if (fs::exists(dst, ec)) { ++r.existing; return; }
            const videoimport::Result vr =
                videoimport::transcode(p.string(), dst.string());
            if (vr.ok) { ++r.imported; ++r.transcoded; }
            else {
                ++r.skipped;
                std::fprintf(stderr, "[Fitzel] %s\n", vr.message.c_str());
            }
            return;
        }

        const char* sub =
            folderFor(fitzel::assetTypeForExtension(p.extension().string()));
        if (!sub) { ++r.skipped; return; }
        // Already under a mounted source -> already an asset with a GUID. Copying
        // it would give the same bytes a second identity.
        if (db.idForPath(p).valid()) { ++r.existing; return; }

        const fs::path dst = fs::path(projectFolder) / sub / p.filename();
        fs::create_directories(dst.parent_path(), ec);
        // Don't overwrite: a same-named file here is another asset, with its own
        // GUID and possibly scene references. Silently replacing its bytes would
        // change every object using it.
        if (fs::exists(dst, ec)) { ++r.existing; return; }
        if (fs::copy_file(p, dst, ec)) ++r.imported;
        else                           ++r.skipped;
    };
    for (const std::string& s : paths) take(fs::path(s), 0);

    // Mint GUIDs + sidecars for what just landed. The 0.5 s change poll would find
    // them too, but a drop should show up under the cursor, not eventually.
    if (r.imported > 0) db.refresh();

    if (r.imported + r.existing + r.skipped == 0) {
        r.message = "Nothing to import.";
        return r;
    }
    std::string m = std::to_string(r.imported) + " imported";
    if (r.transcoded)
        m += " (" + std::to_string(r.transcoded) + " video transcoded)";
    if (r.existing) m += ", " + std::to_string(r.existing) + " already there";
    if (r.skipped)  m += ", " + std::to_string(r.skipped) + " not an asset type";
    r.message = m + ".";
    return r;
}

} // namespace assetdrop
