#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <fitzel/asset/AssetId.hpp>

#include "Component.hpp"
#include "EditMesh.hpp"
#include "SceneTypes.hpp"

// A piece of landscape to print: a square of the scene's own terrain (the
// world generator's mountains, a sculpted valley) or any heightmap image (a
// real elevation tile), made a model a hand can hold.
//
//   relief    one solid: the ground sampled on a grid and triangulated, walls
//             down its four sides to a flat bottom, a base under the lowest
//             point. Below the water level the ground is levelled to it, a
//             still lake. The heights where a filament change colours it --
//             shore, meadow, rock, snow -- come with it.
//   terraces  a contour model, the kind cut from stacked sheets: the land
//             between two heights a flat slab whose outline is that height's
//             contour, every slab its own part in its own colour (water blue
//             at the bottom, then green, olive, rock, snow), so a printer with
//             several filaments prints the map in colour. The contours are
//             marching squares over the grid, joined across saddles by the
//             cell's middle, closed along the edge of the square.
//
// North stays north: the model lies in the scene the way the land does (the
// print's +Y is the scene's -Z). Heights may be exaggerated, as maps are.
//
// Every part a closed solid made by SolidBuild (no booleans), so File > Export
// Selection as 3MF hands a slicer clean parts. Built in the print's frame (mm, Z
// up) and handed over in metres with Y up: x, z, -y. Data -> data: no GPU, no
// ImGui; the scene's heights come in as a function.
namespace terrainmodel {

// Saved as indices: append only.
enum class Source { Scene, Image, Count };
enum class Style { Relief, Terraces, Count };

struct Params {
    Source source = Source::Scene;
    // The scene's terrain: a rectangle round a centre (m).
    float centreX = 0.0f, centreZ = 0.0f;
    float width = 800.0f, depth = 800.0f;
    // A heightmap image (8 or 16 bit; dark low, light high): what it covers.
    std::string image;
    float imageWidth  = 2000.0f;   // the image's width on the ground (m)
    float imageRelief = 300.0f;    // black to white (m)

    float printWidth   = 120.0f;   // the model's width (mm); its depth follows
    float exaggeration = 1.5f;     // heights times this
    float base         = 3.0f;     // plastic under the lowest point (mm)
    int   resolution   = 160;      // samples across
    Style style = Style::Relief;
    int   bands = 10;              // terraces: slabs from the bottom to the top
    bool  water = true;            // a lake where the ground is below `waterLevel`
    float waterLevel = -2.0f;      // m (scene) or m above black (image)

    glm::vec3 reliefColor{0.78f, 0.74f, 0.66f};
    glm::vec3 waterColor{0.20f, 0.42f, 0.78f};
    glm::vec3 lowColor{0.36f, 0.58f, 0.26f};
    glm::vec3 midColor{0.62f, 0.62f, 0.30f};
    glm::vec3 highColor{0.52f, 0.43f, 0.35f};
    glm::vec3 peakColor{0.95f, 0.95f, 0.96f};

    bool operator==(const Params&) const = default;
};

// The scene's ground height (m) at a world point (x, z).
using Heights = std::function<float(float, float)>;

Params sane(const Params& p);

// The region the model covers (m): width (x) and depth (z), given the image's
// shape for an image source (0 x 0 when it cannot be read).
glm::vec2 regionOf(const Params& p);

struct Part {
    std::string name;
    glm::vec3 color{0.8f};
    EditMesh mesh;   // metres, Y up, the model's frame: centred on X and Z, bottom on Y 0
};

struct Model {
    std::vector<Part> parts;     // the relief, or one slab a terrace
    glm::vec3 size{0.0f};        // mm: width (X), depth (Y), height
    double volume = 0.0;         // mm3, all parts
    bool ok = true;
    std::string error;
    // What was sampled, for the panel's map: nx x ny heights (m), row by row
    // from the front (south) edge, west to east.
    int nx = 0, ny = 0;
    std::vector<float> heights;
    float lo = 0.0f, hi = 0.0f;           // the lowest and highest ground (m)
    std::vector<float> levels;            // terraces: where each slab starts (m)
    std::vector<glm::vec3> levelColors;   // and its colour
    float mmPerMetre = 0.0f;              // horizontally
    // A relief's filament changes: the height on the print (mm) and what starts there.
    std::vector<std::pair<float, std::string>> changes;
};

// Build `p` (as sane() leaves it); `scene` answers the scene's heights for a
// scene source (null: none, the model says so).
Model build(const Params& p, const Heights& scene);

// The colour a terrace starting at height `h` (m) takes, the land spread over
// low, mid, high and peak from `landLo` to `landHi`.
glm::vec3 bandColor(const Params& p, float h, float landLo, float landHi, bool water);

// Materials ("Terrain #5D943F"), one a colour.
fitzel::AssetId ensureMaterial(std::vector<MaterialDef>& materials, const glm::vec3& color);

// The model as an entity subtree standing at `at` (its bottom there): an Empty
// root in its middle carrying the parameters, one child a part. front() is the
// root. Empty when nothing could be built.
std::vector<Entity> entities(const Params& p, const Model& m, std::vector<MaterialDef>& materials,
                             int& entityCounter, const glm::vec3& at);

// "Terrain relief 800 m", "Terrain contours 800 m", "Terrain relief: alps.png".
std::string autoName(const Params& p);

void saveParams(nlohmann::json& j, const Params& p);
void loadParams(const nlohmann::json& j, Params& p);

} // namespace terrainmodel

// The parameters a terrain model was built from, on its root -- so the panel
// can re-open it and rebuild it from the terrain as it is now. Saved with the
// scene and with a prefab.
class TerrainModelComponent : public ComponentBase {
public:
    terrainmodel::Params params;

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<TerrainModelComponent>(*this);
    }
    const char* typeId() const override { return "terrainModel"; }
    const char* displayName() const override { return "Terrain model"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> none; return none;   // edited in its panel
    }
    void save(nlohmann::json& j) const override { terrainmodel::saveParams(j, params); }
    void load(const nlohmann::json& j) override { terrainmodel::loadParams(j, params); }
};
