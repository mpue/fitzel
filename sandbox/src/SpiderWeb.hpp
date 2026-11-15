#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <fitzel/asset/AssetId.hpp>

#include "Component.hpp"
#include "EditMesh.hpp"
#include "SceneTypes.hpp"

// A spider's web to print: one flat plate of threads, lying on the bed, no
// support, no infill -- for a window, a wall, a corner or a hoop to hang up.
//
//   an orb web    radii from a hub, the hub's few tight turns, the open ring
//                 round it, and the capture spiral from there out to the frame:
//                 one thread wound round and round, not rings, as a garden
//                 spider spins it. Its threads droop between the radii towards
//                 the hub (the scallops a web is drawn with), and the whole can
//                 be as even as drawn with a ruler or as uneven as spun, a seed
//                 picking which uneven one; torn, some capture threads missing
//   round         in a hoop, with a loop at the top to hang it by
//   framed        on a frame of a few straight threads, its anchor threads
//                 running out from the frame's corners to round pads (to tape or
//                 pin up), to loops, or ending free
//   a corner      a quarter web between two walls, as in the corner of a window
//                 or a room: radii fanning out from the corner, threads draped
//                 across them from wall to wall
//
// The threads are a planar graph -- straight or drooping strands between the
// knots where they cross -- thickened exactly: two threads meeting at a knot
// share the corner between them, a thread ending is rounded off, a hub or a pad
// is a disc the threads run into. So the plate's top is one mesh without
// overlaps, its walls stand along its edges, and the part is closed without
// any booleans.
//
// Built in the print's frame (mm, Z up, the front towards -Y) and handed over
// in metres with Y up: x, z, -y -- lying flat in the scene as on the bed. Pure
// data -> data: no GPU, no ImGui.
namespace spiderweb {

// Saved as indices: append only.
enum class Shape { Round, Frame, Corner, Count };
enum class Ends { Pads, Loops, Free, Count };   // where a framed web's anchor threads end

const char* shapeName(Shape s);   // "Round", "Framed", "Corner"
const char* endsName(Ends e);     // "Pads", "Loops", "Free"

struct Params {
    Shape shape   = Shape::Round;
    float size    = 150.0f;   // round: across the hoop; framed: across the frame's corners; corner: along each wall
    int   spokes  = 20;       // the radii (in a corner: between the two walls)
    float spacing = 6.0f;     // between two turns of the capture spiral
    int   hubTurns = 2;       // the hub's tight turns round its disc
    float freeZone = 6.0f;    // the open ring between the hub and the capture spiral
    float sag     = 0.15f;    // how far a capture thread droops towards the hub, a share of its length
    float irregular = 0.35f;  // 0: drawn with a ruler; 1: as uneven as a spider spins it
    int   seed    = 1;        // which uneven one
    float torn    = 0.0f;     // the share of capture threads missing (0 to 0.5)

    float spiralWidth = 1.0f; // the capture spiral's thread (mm across, in plan)
    float spokeWidth  = 1.4f; // the radii and the hub
    float frameWidth  = 2.4f; // the hoop, the frame, the walls and the anchor threads
    float thickness   = 1.2f; // the plate: every thread as high as this

    bool  eyelet  = true;     // round: a loop at the top to hang it by
    int   corners = 5;        // framed: the frame's corners
    float anchor  = 12.0f;    // framed: how far the anchor threads run out from the frame
    Ends  ends    = Ends::Pads;

    glm::vec3 color{0.90f, 0.90f, 0.92f};

    bool operator==(const Params&) const = default;
};

// The parameters as they are built: counts and sizes to what prints (threads
// at least two lines of a 0.4 mm nozzle, the holes between them at least a
// millimetre), the capture spiral at least so far apart as its threads leave a
// hole between, the hub and the frame to what the size leaves room for.
Params sane(const Params& p);

// A thread of the drawing, for the panel: its middle line (mm, the print's
// plan, the web's middle at the origin) and how wide it is; discs for the hub
// and the pads.
struct Thread {
    std::vector<glm::vec2> pts;
    float width = 1.0f;
};
struct Disc {
    glm::vec2 centre{0.0f};
    float radius = 1.0f;
};

struct Model {
    EditMesh body;              // metres, Y up: centred on X and Z, its bottom on Y 0
    glm::vec3 size{0.0f};       // mm: width (X), depth (Y), thickness
    double volume = 0.0;        // plastic (mm3)
    double grams  = 0.0;        // in PLA
    double metres = 0.0;        // of 1.75 mm filament
    double area   = 0.0;        // the plate's top (mm2)
    int    spokes = 0;
    int    turns  = 0;          // turns of the capture spiral (or threads across a corner)
    int    threads = 0;         // strands between knots
    int    knots  = 0;
    int    holes  = 0;          // the openings between the threads
    double smallestHole = 0.0;  // the narrowest opening: the widest round that fits in it (mm across)
    glm::vec2 smallestHoleAt{0.0f};   // where it is (the print's plan)
    double hubRadius = 0.0;     // the hub's disc (mm)
    bool   ok = true;           // false: the threads could not be joined cleanly (see `error`)
    std::string error;
    std::string printHint;
    std::vector<Thread> threadsDrawn;
    std::vector<Disc>   discs;
};

Model build(const Params& p);

fitzel::AssetId ensureMaterial(std::vector<MaterialDef>& materials, const Params& p);

// The web as an entity subtree lying at `at` (its bottom there): an Empty root
// in its middle carrying the parameters and a "Web" child (a static collider).
// front() is the root.
std::vector<Entity> entities(const Params& p, const fitzel::AssetId& material, int& entityCounter,
                             const glm::vec3& at);

// "Spider web round 150 mm"
std::string autoName(const Params& p);

void saveParams(nlohmann::json& j, const Params& p);
void loadParams(const nlohmann::json& j, Params& p);

} // namespace spiderweb

// The parameters a spider web was built from, on its root -- so the panel can
// re-open it and rebuild it. Saved with the scene and with a prefab.
class SpiderWebComponent : public ComponentBase {
public:
    spiderweb::Params params;

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<SpiderWebComponent>(*this);
    }
    const char* typeId() const override { return "spiderWeb"; }
    const char* displayName() const override { return "Spider web"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> none; return none;   // edited in its panel
    }
    void save(nlohmann::json& j) const override { spiderweb::saveParams(j, params); }
    void load(const nlohmann::json& j) override { spiderweb::loadParams(j, params); }
};
