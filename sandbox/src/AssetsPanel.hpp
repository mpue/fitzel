#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/asset/AssetId.hpp>

struct EditorContext;

// The Assets browser: every asset in the database, grouped by source (Engine vs
// Project) and labelled by type. Drag a tile onto the viewport or a material
// slot, double-click a model to place it, drop files on it from Explorer to copy
// them into the project. Editor only.
namespace assetsui {

// The browser's own settings and the last drop's outcome.
struct State {
    float       thumbSize = 76.0f;
    char        filter[64] = "";
    bool        texturesOnly = false;
    std::string dropStatus;   // outcome of the last drop from Explorer
};

struct Host {
    bool&              show;
    const std::string& projectDir;   // the open project's folder ("" for none)
    // A texture's preview (GL name, 0 while it is decoding or for none). Only
    // asked to start a decode for a tile that is on screen, so scrolling a big
    // browser does not queue every texture at once.
    std::function<unsigned(fitzel::AssetId, bool onScreen)> thumbnail;
    // Files dropped from the OS, and where the cursor was when they landed. The
    // browser takes them (and clears the list) when they landed on it.
    std::vector<std::string>& droppedFiles;
    float dropX = 0.0f, dropY = 0.0f;
    std::function<glm::vec3()> spawnAt;   // where a double-clicked model lands
    // Open a picture in the Image editor (a texture tile's right-click menu).
    std::function<void(const std::string& file)> editImage;
};

void panel(EditorContext& ed, State& s, const Host& h);

} // namespace assetsui
