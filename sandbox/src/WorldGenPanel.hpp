#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "WorldGen.hpp"

namespace fitzel { class Texture; }
namespace geoclient { class Client; class AreaLoad; }
namespace geo { struct Place; }

// The world generator's panel: a region, a seed, the stages, a map of the plan
// and the one button that puts it into the scene.
//
// The map is the point of it. Every other generator here is judged by looking
// at the scene after it ran; this one replaces most of the scene, so the plan is
// shown FIRST -- rivers, towns, roads, bridges, farms and rock on the hill-shaded
// ground, redrawn whenever a slider settles -- and Generate is pressed for a plan
// that has already been looked at. A click on the map flies the camera there.
//
// The panel owns no world state -- the params and the plan live in main, which
// also does the apply (see WorldGen.hpp for why the split) -- only the texture
// the map is drawn from, which it rebuilds when the plan changes.
namespace worldui {

struct MapCache {
    std::shared_ptr<fitzel::Texture> tex;
    int rev = -1;     // the plan revision the texture shows
    // The shade drawn on it: on or off, for which sun, and the cells it found.
    bool                      showShade = true;
    bool                      shaded    = false;
    glm::vec3                 sun{0.0f};
    std::vector<std::uint8_t> shade;
};

// The real map a place is picked on: a slippy map of OpenStreetMap's tiles
// through fitzelgeo, its view and the tile pictures already uploaded.
struct RealMap {
    int    zoom = 13;
    double cx = 0.0, cy = 0.0;          // view centre, in tile units at `zoom`
    bool   placed = false;              // centred on the params once
    bool   dragging = false;
    std::map<std::string, std::shared_ptr<fitzel::Texture>> tex;
    std::map<std::string, double> used;  // when a picture was last drawn
    std::string search;
    std::string searchPath;              // the request in flight ("" = none)
    std::vector<std::pair<std::string, glm::dvec4>> found;   // name, (lon, lat, zoom, 0)
    std::string searchNote;
    // The plan's buildings drawn on the plan map, by what is built.
    bool showBuildings = true;
};

struct PanelState {
    bool&                   show;
    worldgen::Params&       params;
    const worldgen::Plan&   plan;
    bool                    planValid = false;
    int                     planRev   = 0;   // bumps with every new plan
    MapCache&               map;
    bool&                   discardPaint;

    // A plan is being made on another thread / one is being put into the scene.
    bool planning = false;
    bool applying = false;

    // Where the camera is, for the marker on the map and "centre here", and
    // which way it looks (unit XZ).
    glm::vec2 camera{0.0f};
    glm::vec2 cameraDir{0.0f, -1.0f};

    // How much the confirmation has to warn about, counted by main.
    int sculptCells = 0, paintCells = 0;
    int sceneRoads = 0, sceneTowns = 0, sceneRivers = 0;

    std::function<void()>          preview;    // re-plan; the scene is untouched
    std::function<void()>          generate;   // ...and put it into the scene
    std::function<void(glm::vec2)> lookAt;     // fly the camera over a map point

    // The forest's link to the Tree generator: the trees saved in the project
    // (trees/*.glb, by file name) a row can plant, the row the Tree generator's
    // next save goes to (-1 = none), showing a row's tree there, and planting
    // the rows now without generating the world again.
    std::vector<std::string>  projectTrees;
    bool                      haveProject = false;
    int                       editingRow  = -1;
    std::function<void(int)>  editTree;
    std::function<void()>     replant;

    // A real place: the map cache's client, the square being fetched, and the
    // picker's own state. Null client: no real places in this build.
    geoclient::Client*   geo  = nullptr;
    geoclient::AreaLoad* load = nullptr;
    RealMap*             realMap = nullptr;
    std::function<void()> loadArea;     // fetch the params' square
};

void drawPanel(const PanelState& s);

} // namespace worldui
