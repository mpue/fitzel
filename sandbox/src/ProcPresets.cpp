#include "ProcPresets.hpp"

#include <memory>

#include <nlohmann/json.hpp>

#include "PropertyMeta.hpp"   // readProps

namespace procpreset {

namespace {

using nlohmann::json;

// A material by name, made with these values if the library has none.
std::string material(std::vector<MaterialDef>& mats, const char* name, glm::vec3 albedo,
                     float refl, float rough, glm::vec3 glow = glm::vec3(0.0f),
                     float glowStrength = 1.0f) {
    for (MaterialDef& m : mats)
        if (m.name == name) {
            if (!m.assetId.valid()) m.assetId = fitzel::AssetId::generate();
            return m.assetId.toString();
        }
    MaterialDef md;
    md.assetId          = fitzel::AssetId::generate();
    md.name             = name;
    md.albedo           = albedo;
    md.reflectivity     = refl;
    md.roughness        = rough;
    md.emission         = glow;
    md.emissionStrength = glowStrength;
    mats.push_back(md);
    return md.assetId.toString();
}

struct Palette {
    std::string hull, plate, dark, solar, glow, gold;
};
Palette palette(std::vector<MaterialDef>& mats) {
    Palette p;
    p.hull  = material(mats, "Station Hull",  {0.78f, 0.79f, 0.80f}, 0.15f, 0.45f);
    p.plate = material(mats, "Station Plate", {0.52f, 0.54f, 0.58f}, 0.20f, 0.50f);
    p.dark  = material(mats, "Station Dark",  {0.15f, 0.16f, 0.18f}, 0.35f, 0.40f);
    p.solar = material(mats, "Station Solar", {0.05f, 0.08f, 0.22f}, 0.60f, 0.18f);
    p.glow  = material(mats, "Station Glow",  {0.90f, 0.86f, 0.72f}, 0.00f, 0.50f,
                       {1.0f, 0.82f, 0.55f}, 4.0f);
    p.gold  = material(mats, "Station Gold",  {0.83f, 0.62f, 0.25f}, 1.00f, 0.35f);
    return p;
}

// Nodes by kind, settings by their file keys -- the same keys a scene keeps,
// so a preset reads like the graph it saves as.
struct Builder {
    proc::Graph g;
    int node(const char* kind, const char* name, const json& set = json::object(),
             const std::vector<int>& in = {}) {
        std::unique_ptr<proc::Node> n = proc::make(kind);
        if (!n) return -1;
        n->name = name;
        readProps(set, n->props(), n.get());
        const int id = g.add(std::move(n)).id;
        for (std::size_t s = 0; s < in.size(); ++s) g.connect(id, static_cast<int>(s), in[s]);
        return id;
    }
    int mat(const char* name, const std::string& m, int input, int faces = 0,
            float share = 1.0f, int seed = 1) {
        return node("material", name,
                    {{"material", m}, {"faces", faces}, {"share", share}, {"seed", seed}}, {input});
    }
};

// A spoked wheel, as in every film: hub on a spine, a box-section habitat ring
// on four spokes inside trusses, a band of windows round the ring, solar wings
// on a cross at the top, a docking collar at the bottom.
proc::Graph ringStation(std::vector<MaterialDef>& mats) {
    const Palette p = palette(mats);
    Builder b;

    // The hub and the spine through it.
    int hub = b.node("tube", "hub", {{"radius", 8.0}, {"radiusTop", 8.0}, {"length", 24.0},
                                     {"segments", 32}, {"rows", 6}});
    hub = b.node("panels", "hub_plates", {{"faces", 3}, {"share", 0.7}, {"inset", 0.12},
                                          {"depthMin", 0.15}, {"depthMax", 0.5}, {"seed", 2}}, {hub});
    hub = b.mat("hub_paint", p.hull, hub);
    int spine = b.node("tube", "spine", {{"radius", 3.0}, {"radiusTop", 3.0}, {"length", 84.0},
                                         {"segments", 16}, {"rows", 18}});
    spine = b.node("panels", "spine_plates", {{"faces", 3}, {"share", 0.5}, {"inset", 0.15},
                                              {"depthMin", 0.1}, {"depthMax", 0.35}, {"seed", 5}}, {spine});
    spine = b.mat("spine_paint", p.plate, spine);

    // The ring: a box section, plated, with a band of lit windows round it.
    const int ringShape = b.node("torus", "ring", {{"radius", 60.0}, {"width", 10.0}, {"height", 7.0},
                                                   {"segments", 96}, {"sides", 4}});
    int ring = b.node("panels", "ring_plates", {{"faces", 0}, {"share", 0.55}, {"inset", 0.1},
                                                {"depthMin", 0.1}, {"depthMax", 0.3}, {"seed", 3}},
                      {ringShape});
    ring = b.mat("ring_paint", p.hull, ring);
    ring = b.mat("ring_patches", p.plate, ring, 0, 0.25f, 11);
    int windows = b.node("torus", "window_band", {{"radius", 60.0}, {"width", 10.9}, {"height", 1.2},
                                                   {"segments", 96}, {"sides", 4}});
    windows = b.mat("window_glow", p.glow, windows);

    // Beacons: small lights on a tenth of the ring's faces.
    int beacon = b.node("sphere", "beacon", {{"radius", 0.6}, {"rings", 4}, {"segments", 8}});
    beacon = b.mat("beacon_glow", p.glow, beacon);
    const int beacons = b.node("copytopoints", "beacons", {{"where", 1}, {"share", 0.08}, {"seed", 4}},
                               {beacon, ringShape});

    // Spokes: a tube inside a truss, from the hub to the ring, four times round.
    int spoke = b.node("tube", "spoke", {{"radius", 1.6}, {"radiusTop", 1.6}, {"length", 47.0},
                                         {"segments", 12}, {"rows", 1}, {"axis", 0},
                                         {"center", {31.5, 0.0, 0.0}}});
    spoke = b.mat("spoke_paint", p.hull, spoke);
    int truss = b.node("tube", "truss_frame", {{"radius", 2.8}, {"radiusTop", 2.8}, {"length", 47.0},
                                               {"segments", 4}, {"rows", 12}, {"caps", false},
                                               {"axis", 0}, {"center", {31.5, 0.0, 0.0}}});
    truss = b.node("lattice", "truss", {{"thickness", 0.22}}, {truss});
    truss = b.mat("truss_paint", p.dark, truss);
    int spokes = b.node("merge", "spoke_and_truss", json::object(), {spoke, truss});
    spokes = b.node("copyradial", "four_spokes", {{"count", 4}}, {spokes});

    // Solar wings on a cross at the top of the spine.
    int cell = b.node("grid", "wing", {{"sizeX", 22.0}, {"sizeZ", 9.0}, {"cellsX", 8}, {"cellsZ", 3}});
    int sheet = b.node("solidify", "wing_slab", {{"thickness", 0.12}}, {cell});
    sheet = b.mat("solar_cells", p.solar, sheet);
    int frame = b.node("lattice", "wing_frame", {{"thickness", 0.16}}, {cell});
    frame = b.mat("frame_paint", p.dark, frame);
    int wing = b.node("merge", "wing_parts", json::object(), {sheet, frame});
    wing = b.node("transform", "wing_place", {{"move", {18.0, 36.0, 0.0}}}, {wing});
    int wings = b.node("copyradial", "four_wings", {{"count", 4}}, {wing});
    int mast = b.node("tube", "mast", {{"radius", 0.5}, {"radiusTop", 0.5}, {"length", 58.0},
                                       {"segments", 8}, {"axis", 0}, {"center", {0.0, 36.0, 0.0}}});
    mast = b.node("copyradial", "mast_cross", {{"count", 2}, {"sweep", 90.0}}, {mast});
    mast = b.mat("mast_paint", p.dark, mast);

    // The docking end: a tapered collar with a lit ring.
    int dock = b.node("tube", "dock", {{"radius", 5.0}, {"radiusTop", 3.2}, {"length", 6.0},
                                       {"segments", 24}, {"rows", 2}, {"center", {0.0, -45.0, 0.0}}});
    dock = b.node("extrude", "dock_collar", {{"faces", 2}, {"inset", 1.2}, {"distance", 2.5}}, {dock});
    dock = b.mat("dock_foil", p.gold, dock);
    // Round the wide end of the collar, where the ships come in.
    int dockLights = b.node("torus", "dock_lights", {{"radius", 5.0}, {"width", 0.5}, {"height", 0.5},
                                                     {"segments", 32}, {"sides", 6},
                                                     {"center", {0.0, -47.5, 0.0}}});
    dockLights = b.mat("dock_glow", p.glow, dockLights);

    const int all = b.node("merge", "station", json::object(),
                           {hub, spine, ring, windows, beacons, spokes, wings, mast, dock, dockLights});
    b.g.output = all;
    return std::move(b.g);
}

// An ISS-like stack along a lattice truss: modules joined by nodes, solar
// wings at the truss ends, radiators below.
proc::Graph modularStation(std::vector<MaterialDef>& mats) {
    const Palette p = palette(mats);
    Builder b;

    int truss = b.node("tube", "truss_frame", {{"radius", 1.8}, {"radiusTop", 1.8}, {"length", 90.0},
                                               {"segments", 4}, {"rows", 30}, {"caps", false},
                                               {"axis", 0}});
    truss = b.node("lattice", "truss", {{"thickness", 0.16}}, {truss});
    truss = b.mat("truss_paint", p.dark, truss);

    int module = b.node("tube", "module", {{"radius", 2.2}, {"radiusTop", 2.2}, {"length", 11.0},
                                           {"segments", 24}, {"rows", 5}, {"axis", 2}});
    module = b.node("panels", "module_plates", {{"faces", 3}, {"share", 0.6}, {"inset", 0.1},
                                                {"depthMin", 0.05}, {"depthMax", 0.18}}, {module});
    module = b.mat("module_paint", p.hull, module);
    module = b.mat("module_foil", p.gold, module, 3, 0.2f, 7);
    int modules = b.node("copylinear", "module_row", {{"count", 3}, {"step", {0.0, 0.0, 13.0}}},
                         {module});
    int joint = b.node("sphere", "joint", {{"radius", 2.7}, {"rings", 10}, {"segments", 20}});
    joint = b.mat("joint_paint", p.plate, joint);
    int joints = b.node("copylinear", "joint_row", {{"count", 2}, {"step", {0.0, 0.0, 13.0}}}, {joint});

    int cell = b.node("grid", "wing", {{"sizeX", 8.0}, {"sizeZ", 30.0}, {"cellsX", 2}, {"cellsZ", 10}});
    int sheet = b.node("solidify", "wing_slab", {{"thickness", 0.1}}, {cell});
    sheet = b.mat("solar_cells", p.solar, sheet);
    int frame = b.node("lattice", "wing_frame", {{"thickness", 0.12}}, {cell});
    frame = b.mat("frame_paint", p.dark, frame);
    int wing = b.node("merge", "wing_parts", json::object(), {sheet, frame});
    wing = b.node("transform", "wing_place", {{"move", {41.0, 0.0, 0.0}}}, {wing});
    wing = b.node("copylinear", "wing_pair", {{"count", 2}, {"step", {0.0, 0.0, 34.0}}}, {wing});
    int wings = b.node("mirror", "both_ends", {{"axis", 0}}, {wing});

    // Hung edge-on under the truss, its top on the truss's lowest edge.
    int radiator = b.node("box", "radiator", {{"size", {12.0, 5.0, 0.2}}, {"center", {20.0, -4.3, 0.0}}});
    radiator = b.mat("radiator_paint", p.hull, radiator);
    int radiators = b.node("mirror", "radiator_pair", {{"axis", 0}}, {radiator});

    const int all = b.node("merge", "station", json::object(),
                           {truss, modules, joints, wings, radiators});
    b.g.output = all;
    return std::move(b.g);
}

// The curve tools at work: tanks turned on a lathe from a drawn profile, pipes
// swept along smooth paths from a hub to each of them, a lamp on every third
// point along each pipe (a selection, lifted clear of the pipe), all on a deck
// made from a filled rectangle with a railing swept round a circle.
proc::Graph fuelDepot(std::vector<MaterialDef>& mats) {
    const Palette p = palette(mats);
    Builder b;

    int profile = b.node("curve", "tank_profile",
                         {{"points", "0 -6 0; 5 -6 0; 6 -4.5 0; 6 4.5 0; 5 6 0; 0 7 0"}, {"smooth", 4}});
    int tank = b.node("revolve", "tank", {{"segments", 32}}, {profile});
    tank = b.node("panels", "tank_plates", {{"faces", 3}, {"share", 0.5}, {"inset", 0.15},
                                            {"depthMin", 0.05}, {"depthMax", 0.15}, {"seed", 3}}, {tank});
    tank = b.mat("tank_paint", p.hull, tank);
    const int tanks = b.node("copyradial", "three_tanks", {{"count", 3}, {"radius", 18.0}}, {tank});

    int hub = b.node("sphere", "hub", {{"radius", 7.0}, {"rings", 14}, {"segments", 28}});
    hub = b.mat("hub_paint", p.plate, hub);

    const int path = b.node("curve", "pipe_path",
                            {{"points", "6.5 0 0; 8.5 2.5 0; 10.5 2.5 0; 12.2 0 0"}, {"smooth", 6}});
    int pipe = b.node("sweep", "pipe", {{"radius", 0.8}, {"sides", 12}}, {path});
    pipe = b.mat("pipe_paint", p.dark, pipe);
    const int pipes = b.node("copyradial", "three_pipes", {{"count", 3}}, {pipe});

    const int along = b.node("resample", "lamp_spots", {{"spacing", 1.0}}, {path});
    const int every = b.node("selectpoints", "every_third", {{"by", 3}, {"every", 3}, {"offset", 1}}, {along});
    const int lifted = b.node("transform", "above_the_pipe", {{"move", {0.0, 1.1, 0.0}}}, {every});
    int lamp = b.node("sphere", "lamp", {{"radius", 0.35}, {"rings", 4}, {"segments", 8}});
    lamp = b.mat("lamp_glow", p.glow, lamp);
    int lamps = b.node("copytopoints", "lamps", {{"where", 0}}, {lamp, lifted});
    lamps = b.node("copyradial", "lamps_all_pipes", {{"count", 3}}, {lamps});

    int deck = b.node("rect", "deck", {{"sizeX", 60.0}, {"sizeZ", 60.0}, {"filled", true},
                                       {"center", {0.0, -7.0, 0.0}}});
    deck = b.node("extrude", "deck_slab", {{"faces", 0}, {"distance", 1.0}}, {deck});
    deck = b.mat("deck_paint", p.dark, deck);

    const int rim = b.node("circle", "railing_line", {{"radius", 28.0}, {"segments", 64},
                                                      {"center", {0.0, -4.8, 0.0}}});
    int rail = b.node("sweep", "railing", {{"radius", 0.12}, {"sides", 6}}, {rim});
    rail = b.mat("railing_paint", p.gold, rail);
    const int postSpots = b.node("resample", "post_spots", {{"spacing", 4.0}}, {rim});
    const int post = b.node("tube", "post", {{"radius", 0.1}, {"radiusTop", 0.1}, {"length", 1.2},
                                             {"segments", 6}, {"center", {0.0, -0.6, 0.0}}});
    int posts = b.node("copytopoints", "posts", {{"where", 0}, {"align", false}}, {post, postSpots});
    posts = b.mat("post_paint", p.gold, posts);

    const int all = b.node("merge", "depot", json::object(),
                           {tanks, hub, pipes, lamps, deck, rail, posts});
    b.g.output = all;
    return std::move(b.g);
}

proc::Graph empty() {
    Builder b;
    b.g.output = b.node("box", "box");
    return std::move(b.g);
}

} // namespace

const std::vector<Preset>& list() {
    static const std::vector<Preset> l = {
        {"Ring station", "A spoked wheel: hub, spine, plated habitat ring with\n"
                         "windows, spokes in trusses, solar wings, a docking collar."},
        {"Modular station", "Modules along a lattice truss, solar wings at\n"
                            "both ends, radiators -- the ISS way of building."},
        {"Fuel depot", "The curve tools at work: tanks revolved from a drawn profile,\n"
                       "pipes swept along curves, lamps on every third point."},
        {"Empty", "One box, ready for your own graph."},
    };
    return l;
}

proc::Graph build(int i, std::vector<MaterialDef>& materials) {
    switch (i) {
        case 0:  return ringStation(materials);
        case 1:  return modularStation(materials);
        case 2:  return fuelDepot(materials);
        default: return empty();
    }
}

} // namespace procpreset