#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/asset/AssetId.hpp>
#include <fitzel/asset/AssetTypes.hpp>
#include <fitzel/rhi/Types.hpp>

struct EditorContext;

// The Assets browser: every asset in the database, as tiles, a list or a table
// of details, filtered by name, type and source (Engine vs Project). Drag an
// asset onto the viewport or a material slot, double-click a model to place it,
// drop files on it from Explorer (or pick them with Import...) to copy them into
// the project. Editor only.
namespace assetsui {

enum class View { Grid, List, Details };

// What the Details table can be sorted by -- also the order of its columns.
enum class SortBy { Name, Type, Folder, Source, Size, Modified };

// The browser's own settings and the last drop's outcome.
struct State {
    View        view      = View::Grid;
    float       thumbSize = 76.0f;
    char        filter[64] = "";
    // Only this type (Unknown: every type) and only this source (-1: all).
    fitzel::AssetType type   = fitzel::AssetType::Unknown;
    int               source = -1;
    // The order every view lists in. Set by clicking a Details column header.
    SortBy      sortBy        = SortBy::Name;
    bool        sortAscending = true;
    fitzel::AssetId selected;   // the last asset clicked, highlighted in every view
    std::string dropStatus;     // outcome of the last drop from Explorer or Import...
    // File sizes for the Details view, read once per version of the file
    // rather than asked of the disk every frame.
    struct Size {
        std::filesystem::file_time_type mtime{};
        std::uintmax_t                   bytes = 0;
    };
    std::unordered_map<fitzel::AssetId, Size> sizes;
};

struct Host {
    bool&              show;
    const std::string& projectDir;   // the open project's folder ("" for none)
    // A texture's preview (none while it is decoding, or for none). Only
    // asked to start a decode for a tile that is on screen, so scrolling a big
    // browser does not queue every texture at once.
    std::function<fitzel::rhi::TextureHandle(fitzel::AssetId, bool onScreen)> thumbnail;
    // Files dropped from the OS, and where the cursor was when they landed. The
    // browser takes them (and clears the list) when they landed on it.
    std::vector<std::string>& droppedFiles;
    float dropX = 0.0f, dropY = 0.0f;
    std::function<glm::vec3()> spawnAt;   // where a double-clicked model lands
    // Open a picture in the Image editor (a texture tile's right-click menu).
    std::function<void(const std::string& file)> editImage;
    // Open a sound in the Wave editor (a sound's right-click menu, or a
    // double-click on it).
    std::function<void(const std::string& file)> editSound = {};
};

void panel(EditorContext& ed, State& s, const Host& h);

} // namespace assetsui
