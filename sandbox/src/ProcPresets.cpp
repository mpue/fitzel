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
    // A parameter of the object's panel (see proc::Parm).
    proc::Parm& parm(proc::ParmType type, const char* name, const char* label, const char* folder,
                     float value, float lo = 0.0f, float hi = 0.0f, float step = 0.1f) {
        proc::Parm p;
        p.type = type; p.name = name; p.label = label; p.folder = folder;
        p.value.x = value; p.min = lo; p.max = hi; p.step = step;
        g.parms.push_back(std::move(p));
        return g.parms.back();
    }
    // Setting `key` of node `name` follows `expr`.
    void follow(const char* name, const char* key, const char* expr) {
        for (const auto& n : g.nodes)
            if (n->name == name) n->exprs[key] = expr;
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

    // The station's own controls: one ring radius that the ring, its windows,
    // the spokes and their trusses all follow -- the spokes reach from the
    // hub's skin (hub.radius, a setting of the hub node) to the ring's inner
    // wall, however either is set.
    using proc::ParmType;
    b.parm(ParmType::Float, "ring_radius", "Ring radius", "Ring", 60.0f, 30.0f, 200.0f, 1.0f).tip =
        "From the axis to the middle of the habitat ring";
    b.parm(ParmType::Float, "ring_width", "Ring width", "Ring", 10.0f, 4.0f, 30.0f, 0.5f);
    b.parm(ParmType::Float, "ring_height", "Ring height", "Ring", 7.0f, 2.0f, 20.0f, 0.5f);
    b.parm(ParmType::Int, "spokes", "Spokes", "Spokes", 4.0f, 2.0f, 12.0f, 1.0f);
    b.parm(ParmType::Float, "spoke_radius", "Spoke radius", "Spokes", 1.6f, 0.4f, 4.0f, 0.1f);
    b.parm(ParmType::Toggle, "beacons_on", "Beacons", "Lights", 1.0f).tip = "Small lights on the ring";
    b.parm(ParmType::Float, "beacon_share", "Beacon share", "Lights", 0.08f, 0.0f, 1.0f, 0.01f).hideWhen =
        "!beacons_on";
    proc::Parm& hull = b.parm(ParmType::Material, "hull", "Hull paint", "Look", 0.0f);
    hull.text = p.hull;
    hull.tip  = "The hub's, the ring's and the spokes' paint";

    b.follow("ring", "radius", "ring_radius");
    b.follow("ring", "width", "ring_width");
    b.follow("ring", "height", "ring_height");
    b.follow("window_band", "radius", "ring_radius");
    b.follow("window_band", "width", "ring_width + 0.9");
    b.follow("spoke", "length", "ring_radius - ring_width / 2 - hub.radius");
    b.follow("spoke", "center.x", "hub.radius + spoke.length / 2");
    b.follow("spoke", "radius", "spoke_radius");
    b.follow("spoke", "radiusTop", "spoke_radius");
    b.follow("truss_frame", "length", "spoke.length");
    b.follow("truss_frame", "center.x", "spoke.center.x");
    b.follow("truss_frame", "radius", "spoke_radius * 1.75");
    b.follow("truss_frame", "radiusTop", "spoke_radius * 1.75");
    b.follow("four_spokes", "count", "spokes");
    b.follow("beacons", "share", "beacons_on ? beacon_share : 0");
    for (const char* paint : {"hub_paint", "ring_paint", "spoke_paint"}) b.follow(paint, "material", "$hull");
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

// --- Buildings and bridges -----------------------------------------------------------

// The windows' dressing, the same on every building here.
json facadeLook(std::vector<MaterialDef>& mats, json set) {
    set["materialGlass"] = material(mats, "Building Glass", {0.08f, 0.11f, 0.15f}, 0.75f, 0.08f);
    set["materialFrame"] = material(mats, "Building Frame", {0.92f, 0.92f, 0.90f}, 0.10f, 0.50f);
    set["materialDoor"]  = material(mats, "Building Door", {0.32f, 0.19f, 0.11f}, 0.08f, 0.60f);
    set["materialLedge"] = material(mats, "Building Stone", {0.62f, 0.58f, 0.52f}, 0.04f, 0.90f);
    return set;
}

// A house the way a town has them: a main block of three storeys over a tall
// ground floor under a hip roof, a lower wing of shops with a gable, and an
// octagonal tower on the corner with a spire. Each is footprint, walls,
// paint, roof, facade -- the order that works.
proc::Graph townHouse(std::vector<MaterialDef>& mats) {
    const std::string plaster = material(mats, "Building Plaster", {0.86f, 0.82f, 0.74f}, 0.04f, 0.85f);
    const std::string ochre   = material(mats, "Building Ochre", {0.80f, 0.60f, 0.38f}, 0.04f, 0.85f);
    const std::string tiles   = material(mats, "Building Roof Tiles", {0.55f, 0.22f, 0.14f}, 0.05f, 0.75f);
    const std::string slate   = material(mats, "Building Slate", {0.24f, 0.26f, 0.29f}, 0.15f, 0.60f);
    Builder b;

    int block = b.node("rect", "main_plan", {{"sizeX", 18.0}, {"sizeZ", 11.0}, {"filled", true}});
    block = b.node("extrude", "main_walls", {{"faces", 0}, {"distance", 13.6}}, {block});
    block = b.mat("main_plaster", plaster, block);
    block = b.node("roof", "main_roof", {{"shape", 2}, {"pitch", 35.0}, {"overhang", 0.5}, {"material", tiles}},
                   {block});
    block = b.node("facade", "main_facade", facadeLook(mats, {{"ground", 2}}), {block});

    int wing = b.node("rect", "wing_plan", {{"sizeX", 10.0}, {"sizeZ", 9.0}, {"filled", true},
                                            {"center", {13.5, 0.0, -1.0}}});
    wing = b.node("extrude", "wing_walls", {{"faces", 0}, {"distance", 7.2}}, {wing});
    wing = b.mat("wing_paint", ochre, wing);
    wing = b.node("roof", "wing_roof", {{"shape", 1}, {"pitch", 40.0}, {"overhang", 0.4}, {"material", tiles}},
                  {wing});
    wing = b.node("facade", "wing_shops", facadeLook(mats, {{"ground", 1}}), {wing});

    int tower = b.node("tube", "tower_body", {{"radius", 3.4}, {"radiusTop", 3.4}, {"length", 17.2},
                                              {"segments", 8}, {"center", {-9.0, 8.6, 5.5}}});
    tower = b.mat("tower_plaster", plaster, tower);
    tower = b.node("roof", "spire", {{"shape", 2}, {"pitch", 62.0}, {"overhang", 0.3}, {"material", slate}},
                   {tower});
    tower = b.node("facade", "tower_windows", facadeLook(mats, {{"ground", 0}, {"winWidth", 1.1}}), {tower});

    b.g.output = b.node("merge", "house", json::object(), {block, wing, tower});
    return std::move(b.g);
}

// Three blocks stacked smaller and smaller, flat roofs behind parapets, and
// one Facade over all of it: ribbon windows on every storey, shops only on
// the ground floor -- the walls that reach lowest.
proc::Graph officeTower(std::vector<MaterialDef>& mats) {
    const std::string concrete = material(mats, "Building Concrete", {0.60f, 0.60f, 0.58f}, 0.05f, 0.85f);
    const std::string dark     = material(mats, "Building Frame Dark", {0.18f, 0.19f, 0.20f}, 0.40f, 0.40f);
    const std::string blue     = material(mats, "Building Glass Blue", {0.10f, 0.16f, 0.24f}, 0.80f, 0.06f);
    Builder b;
    auto block = [&](const char* name, double size, double base, double tall, double parapet) {
        int s = b.node("rect", (std::string(name) + "_plan").c_str(),
                       {{"sizeX", size}, {"sizeZ", size}, {"filled", true}, {"center", {0.0, base, 0.0}}});
        s = b.node("extrude", (std::string(name) + "_walls").c_str(), {{"faces", 0}, {"distance", tall}}, {s});
        return b.node("roof", (std::string(name) + "_roof").c_str(),
                      {{"shape", 0}, {"parapet", parapet}, {"wall", 0.3}}, {s});
    };
    const int podium = block("podium", 32.0, 0.0, 9.0, 0.8);
    const int shaft  = block("shaft", 22.0, 9.0, 36.0, 1.2);
    const int crown  = block("crown", 14.0, 45.0, 14.4, 1.5);
    int all = b.node("merge", "blocks", json::object(), {podium, shaft, crown});
    all = b.mat("concrete", concrete, all);
    json look = facadeLook(mats, {{"ground", 1}, {"floorHeight", 3.6}, {"groundHeight", 5.4}, {"bay", 3.0},
                                  {"winWidth", 2.4}, {"winHeight", 2.5}, {"sill", 0.6}, {"depth", 0.15},
                                  {"frame", 0.06}, {"ledge", 0.08}, {"ledgeHeight", 0.35}});
    look["materialGlass"] = blue;
    look["materialFrame"] = dark;
    look["materialLedge"] = concrete;
    all = b.node("facade", "ribbon_windows", look, {all});
    int mast = b.node("cylinder", "mast", {{"radius", 0.25}, {"height", 12.0}, {"segments", 8},
                                           {"center", {0.0, 65.4, 0.0}}});
    mast = b.mat("mast_paint", dark, mast);
    b.g.output = b.node("merge", "tower", json::object(), {all, mast});
    return std::move(b.g);
}

// A stone viaduct: five Arch bays copied in a row, a road on top, parapets
// along both edges -- the middle line offset to each side, a Railing of
// solid panels on it.
proc::Graph archBridge(std::vector<MaterialDef>& mats) {
    const std::string stone   = material(mats, "Bridge Stone", {0.58f, 0.54f, 0.47f}, 0.04f, 0.90f);
    const std::string asphalt = material(mats, "Bridge Asphalt", {0.12f, 0.12f, 0.13f}, 0.05f, 0.90f);
    Builder b;
    int bay = b.node("arch", "bay", {{"width", 14.0}, {"height", 16.0}, {"thickness", 9.0}, {"span", 10.0},
                                     {"rise", 5.0}, {"spring", 7.0}, {"segments", 16}});
    bay = b.node("copylinear", "five_bays", {{"count", 5}, {"step", {14.0, 0.0, 0.0}}}, {bay});
    bay = b.mat("bay_stone", stone, bay);
    int road = b.node("box", "road", {{"size", {70.0, 0.4, 8.6}}, {"center", {0.0, 16.2, 0.0}}});
    road = b.mat("road_asphalt", asphalt, road);
    const int line = b.node("curve", "middle_line", {{"points", "-35 16.4 0; 35 16.4 0"}});
    const int edges = b.node("offset", "edges", {{"distance", 4.2}, {"both", true}}, {line});
    int parapet = b.node("railing", "parapets", {{"height", 1.0}, {"spacing", 3.5}, {"post", 0.4},
                                                 {"rail", 0.35}, {"midRails", 0}, {"fill", 2}}, {edges});
    parapet = b.mat("parapet_stone", stone, parapet);
    b.g.output = b.node("merge", "viaduct", json::object(), {bay, road, parapet});
    return std::move(b.g);
}

// Two towers (Arch walls standing across the deck), the main cable hung
// between them as an Arch curve with a negative rise, side cables down to
// the ends, both offset to the deck's edges; hangers dropped from points
// along the cables onto the deck (Drop lines onto the deck itself), swept
// thin; railings along the edges; anchor blocks at the ends.
proc::Graph suspensionBridge(std::vector<MaterialDef>& mats) {
    const std::string concrete = material(mats, "Bridge Concrete", {0.66f, 0.65f, 0.62f}, 0.05f, 0.85f);
    const std::string asphalt  = material(mats, "Bridge Asphalt", {0.12f, 0.12f, 0.13f}, 0.05f, 0.90f);
    const std::string red      = material(mats, "Bridge Steel Red", {0.62f, 0.16f, 0.10f}, 0.45f, 0.40f);
    const std::string cable    = material(mats, "Bridge Cable", {0.70f, 0.70f, 0.72f}, 0.80f, 0.30f);
    Builder b;
    const int path = b.node("curve", "deck_path", {{"points", "-80 0 0; 80 0 0"}});
    const int section = b.node("rect", "deck_section", {{"sizeX", 14.0}, {"sizeZ", 1.6}});
    int deck = b.node("sweep", "deck", json::object(), {path, section});
    deck = b.mat("deck_asphalt", asphalt, deck);

    int tower = b.node("arch", "tower", {{"width", 24.0}, {"height", 52.0}, {"thickness", 4.0}, {"span", 16.0},
                                         {"rise", 5.0}, {"spring", 30.0}, {"along", 1},
                                         {"center", {0.0, -22.0, 0.0}}});
    tower = b.node("copylinear", "two_towers", {{"count", 2}, {"step", {90.0, 0.0, 0.0}}}, {tower});
    tower = b.mat("tower_paint", red, tower);

    const int mainCable = b.node("archcurve", "main_cable", {{"span", 90.0}, {"rise", -24.0}, {"shape", 3},
                                                        {"segments", 30}, {"center", {0.0, 29.0, 0.0}}});
    const int side = b.node("curve", "side_cable", {{"points", "45 29 0; 80 1.2 0"}});
    const int sides = b.node("mirror", "side_cables", {{"axis", 0}}, {side});
    const int lines = b.node("merge", "cable_lines", json::object(), {mainCable, sides});
    const int both = b.node("offset", "over_the_edges", {{"distance", 6.8}, {"both", true}}, {lines});
    int cables = b.node("sweep", "cables", {{"radius", 0.4}, {"sides", 10}}, {both});
    cables = b.mat("cable_steel", cable, cables);
    const int spots = b.node("resample", "hanger_spots", {{"spacing", 5.0}}, {both});
    const int drops = b.node("droplines", "hanger_lines", json::object(), {spots, deck});
    int hangers = b.node("sweep", "hangers", {{"radius", 0.06}, {"sides", 6}}, {drops});
    hangers = b.mat("hanger_steel", cable, hangers);

    const int top = b.node("transform", "deck_top", {{"move", {0.0, 0.8, 0.0}}}, {path});
    const int edges = b.node("offset", "deck_edges", {{"distance", 6.6}, {"both", true}}, {top});
    int rails = b.node("railing", "railings", {{"height", 1.1}, {"spacing", 2.5}, {"midRails", 2}}, {edges});
    rails = b.mat("railing_paint", red, rails);

    int anchor = b.node("box", "anchor", {{"size", {6.0, 4.0, 18.0}}, {"center", {83.0, -1.0, 0.0}}});
    anchor = b.node("mirror", "anchors", {{"axis", 0}}, {anchor});
    anchor = b.mat("anchor_concrete", concrete, anchor);

    b.g.output = b.node("merge", "bridge", json::object(), {deck, tower, cables, hangers, rails, anchor});
    return std::move(b.g);
}

// A through truss: a Pratt Truss along the path, the deck swept between its
// floor beams, stone abutments at both ends.
proc::Graph trussBridge(std::vector<MaterialDef>& mats) {
    const std::string red     = material(mats, "Bridge Steel Red", {0.62f, 0.16f, 0.10f}, 0.45f, 0.40f);
    const std::string asphalt = material(mats, "Bridge Asphalt", {0.12f, 0.12f, 0.13f}, 0.05f, 0.90f);
    const std::string stone   = material(mats, "Bridge Stone", {0.58f, 0.54f, 0.47f}, 0.04f, 0.90f);
    Builder b;
    const int path = b.node("curve", "path", {{"points", "-36 0 0; 36 0 0"}});
    int truss = b.node("truss", "truss", {{"height", 7.0}, {"width", 9.0}, {"panel", 6.0}, {"size", 0.35},
                                          {"pattern", 1}}, {path});
    truss = b.mat("truss_paint", red, truss);
    const int raised = b.node("transform", "deck_level", {{"move", {0.0, 0.45, 0.0}}}, {path});
    const int section = b.node("rect", "deck_section", {{"sizeX", 8.4}, {"sizeZ", 0.5}});
    int deck = b.node("sweep", "deck", json::object(), {raised, section});
    deck = b.mat("deck_asphalt", asphalt, deck);
    int abut = b.node("box", "abutment", {{"size", {6.0, 10.0, 12.0}}, {"center", {39.0, -5.2, 0.0}}});
    abut = b.node("mirror", "abutments", {{"axis", 0}}, {abut});
    abut = b.mat("abutment_stone", stone, abut);
    b.g.output = b.node("merge", "bridge", json::object(), {truss, deck, abut});
    return std::move(b.g);
}

// --- Rooms ------------------------------------------------------------------------
// The Furniture nodes at work: each room is pieces put where they belong by
// their own Position and Turn, the repeats (chairs round a table, a pair of
// nightstands) copied and mirrored, all merged.

struct Interior {
    std::string oak, walnut, white, black, steel, fabric, fabricBlue, fabricSand, linen, mattress,
                rug, rugBorder, bookRed, bookGreen, bookCream, shade, glass, stone;
};
Interior interior(std::vector<MaterialDef>& mats) {
    Interior m;
    m.oak        = material(mats, "Furniture Oak",          {0.64f, 0.48f, 0.32f}, 0.04f, 0.55f);
    m.walnut     = material(mats, "Furniture Walnut",       {0.34f, 0.22f, 0.14f}, 0.05f, 0.50f);
    m.white      = material(mats, "Furniture White",        {0.91f, 0.91f, 0.89f}, 0.08f, 0.35f);
    m.black      = material(mats, "Furniture Black Steel",  {0.07f, 0.07f, 0.08f}, 0.40f, 0.45f);
    m.steel      = material(mats, "Furniture Steel",        {0.72f, 0.73f, 0.74f}, 0.75f, 0.28f);
    m.fabric     = material(mats, "Furniture Fabric Grey",  {0.42f, 0.43f, 0.45f}, 0.00f, 0.95f);
    m.fabricBlue = material(mats, "Furniture Fabric Blue",  {0.20f, 0.28f, 0.40f}, 0.00f, 0.95f);
    m.fabricSand = material(mats, "Furniture Fabric Sand",  {0.76f, 0.69f, 0.57f}, 0.00f, 0.95f);
    m.linen      = material(mats, "Furniture Linen",        {0.92f, 0.91f, 0.88f}, 0.00f, 0.95f);
    m.mattress   = material(mats, "Furniture Mattress",     {0.84f, 0.85f, 0.86f}, 0.00f, 0.90f);
    m.rug        = material(mats, "Furniture Rug",          {0.62f, 0.45f, 0.36f}, 0.00f, 1.00f);
    m.rugBorder  = material(mats, "Furniture Rug Border",   {0.27f, 0.20f, 0.17f}, 0.00f, 1.00f);
    m.bookRed    = material(mats, "Furniture Book Red",     {0.52f, 0.12f, 0.10f}, 0.02f, 0.70f);
    m.bookGreen  = material(mats, "Furniture Book Green",   {0.14f, 0.30f, 0.22f}, 0.02f, 0.70f);
    m.bookCream  = material(mats, "Furniture Book Cream",   {0.84f, 0.79f, 0.66f}, 0.02f, 0.70f);
    m.shade      = material(mats, "Furniture Lamp Shade",   {0.95f, 0.92f, 0.84f}, 0.00f, 0.90f,
                            {1.0f, 0.86f, 0.62f}, 1.5f);
    m.stone      = material(mats, "Furniture Worktop Stone", {0.16f, 0.16f, 0.17f}, 0.30f, 0.30f);
    // Glass, the stairs' way: Fresnel alpha from the flag, clear head-on.
    m.glass      = material(mats, "Furniture Glass",        {0.85f, 0.92f, 0.95f}, 0.50f, 0.03f);
    for (MaterialDef& d : mats)
        if (d.name == "Furniture Glass" && !d.glass && d.opacity >= 1.0f) { d.glass = true; d.opacity = 0.28f; }
    return m;
}

json books(const Interior& m) {
    return {{"materialBooks", m.bookRed}, {"materialBooksB", m.bookGreen}, {"materialBooksC", m.bookCream}};
}
json with(json a, const json& b) {
    for (const auto& [k, v] : b.items()) a[k] = v;
    return a;
}

// Six chairs round a dining table on a rug, a pendant over it.
proc::Graph diningRoom(std::vector<MaterialDef>& mats) {
    const Interior m = interior(mats);
    Builder b;
    const int table = b.node("table", "table", {{"width", 1.8}, {"depth", 0.9}, {"legShape", 2}, {"legSize", 0.065},
                                                {"materialTop", m.oak}, {"materialBase", m.oak}});
    const int chair = b.node("chair", "chair", {{"back", 1}, {"slats", 3}, {"cushion", true},
                                                {"materialFrame", m.oak}, {"materialSeat", m.fabric},
                                                {"at", {0.0, 0.0, 0.58}}, {"turn", 180.0}});
    const int row  = b.node("copylinear", "chairs_along", {{"count", 3}, {"step", {0.6, 0.0, 0.0}}}, {chair});
    const int long2 = b.node("mirror", "both_long_sides", {{"axis", 2}}, {row});
    const int head = b.node("chair", "head_chair", {{"back", 4}, {"arms", true}, {"cushion", true},
                                                    {"materialFrame", m.oak}, {"materialSeat", m.fabric},
                                                    {"at", {1.08, 0.0, 0.0}}, {"turn", -90.0}});
    const int heads = b.node("mirror", "both_ends", {{"axis", 0}}, {head});
    const int rug = b.node("rug", "rug", {{"width", 3.2}, {"depth", 2.4}, {"materialRug", m.rug},
                                          {"materialBorder", m.rugBorder}});
    const int lamp = b.node("lamp", "pendant", {{"kind", 2}, {"shade", 2}, {"height", 2.5}, {"drop", 0.95},
                                                {"shadeRadius", 0.26}, {"shadeHeight", 0.24},
                                                {"materialBody", m.black}, {"materialShade", m.shade}});
    b.g.output = b.node("merge", "dining_room", json::object(), {table, long2, heads, rug, lamp});
    return std::move(b.g);
}

// A corner sofa with its chaise, an armchair, a coffee table, a TV board, a
// bookcase and a floor lamp round a rug.
proc::Graph livingRoom(std::vector<MaterialDef>& mats) {
    const Interior m = interior(mats);
    Builder b;
    const int rug = b.node("rug", "rug", {{"width", 2.8}, {"depth", 2.0}, {"at", {0.0, 0.0, 0.25}},
                                          {"materialRug", m.fabricSand}, {"materialBorder", m.rugBorder}});
    const int sofa = b.node("sofa", "sofa", {{"width", 2.6}, {"chaise", 2}, {"chaiseDepth", 1.6}, {"pillows", 2},
                                             {"materialFabric", m.fabricBlue}, {"materialCushion", m.fabricBlue},
                                             {"materialLegs", m.walnut}, {"at", {0.0, 0.0, -0.9}}});
    const int armchair = b.node("sofa", "armchair", {{"width", 0.92}, {"depth", 0.86}, {"seats", 1}, {"arms", 2},
                                                     {"armWidth", 0.15}, {"pillows", 0},
                                                     {"materialFabric", m.fabricSand}, {"materialCushion", m.fabricSand},
                                                     {"materialLegs", m.walnut}, {"at", {-2.0, 0.0, 0.35}},
                                                     {"turn", 90.0}});
    const int coffee = b.node("table", "coffee_table", {{"shape", 1}, {"width", 1.1}, {"depth", 0.6},
                                                        {"height", 0.42}, {"top", 0.04}, {"corner", 0.12},
                                                        {"legShape", 1}, {"legSize", 0.045}, {"apron", false},
                                                        {"shelf", true}, {"materialTop", m.walnut},
                                                        {"materialBase", m.walnut}, {"at", {-0.35, 0.0, 0.3}}});
    const int tv = b.node("cabinet", "tv_board", {{"width", 1.8}, {"height", 0.55}, {"depth", 0.42}, {"columns", 3},
                                                  {"fronts", 1}, {"base", 1}, {"baseHeight", 0.16}, {"handles", 3},
                                                  {"materialBody", m.white}, {"materialFront", m.white},
                                                  {"materialHandle", m.black}, {"at", {0.0, 0.0, 1.75}},
                                                  {"turn", 180.0}});
    const int shelf = b.node("shelf", "bookcase", with({{"width", 0.9}, {"height", 2.0}, {"depth", 0.32},
                                                        {"boards", 6}, {"books", 0.75}, {"seed", 4},
                                                        {"materialBody", m.oak}, {"at", {2.1, 0.0, -0.7}},
                                                        {"turn", -90.0}}, books(m)));
    const int lamp = b.node("lamp", "floor_lamp", {{"kind", 0}, {"shade", 0}, {"height", 1.6},
                                                   {"materialBody", m.black}, {"materialShade", m.shade},
                                                   {"at", {-1.8, 0.0, -0.8}}});
    b.g.output = b.node("merge", "living_room", json::object(), {rug, sofa, armchair, coffee, tv, shelf, lamp});
    return std::move(b.g);
}

// A double bed with a padded headboard, nightstands with lamps either side,
// a wardrobe and a rug at the foot.
proc::Graph bedroom(std::vector<MaterialDef>& mats) {
    const Interior m = interior(mats);
    Builder b;
    const int bed = b.node("bed", "bed", {{"width", 1.6}, {"length", 2.0}, {"headboard", 2}, {"headHeight", 1.1},
                                          {"materialFrame", m.oak}, {"materialHeadboard", m.fabric},
                                          {"materialMattress", m.mattress}, {"materialBedding", m.linen}});
    const int stand = b.node("cabinet", "nightstand", {{"width", 0.45}, {"height", 0.5}, {"depth", 0.4},
                                                       {"columns", 1}, {"rows", 2}, {"fronts", 1}, {"base", 1},
                                                       {"baseHeight", 0.14}, {"handles", 2},
                                                       {"materialBody", m.oak}, {"materialFront", m.oak},
                                                       {"materialHandle", m.steel}, {"at", {1.12, 0.0, -0.78}}});
    const int lamp = b.node("lamp", "bedside_lamp", {{"kind", 1}, {"height", 0.46}, {"shadeRadius", 0.13},
                                                     {"shadeHeight", 0.17}, {"materialBody", m.white},
                                                     {"materialShade", m.shade}, {"at", {1.12, 0.5, -0.82}}});
    const int pair = b.node("merge", "bedside", json::object(), {stand, lamp});
    const int sides = b.node("mirror", "both_sides", {{"axis", 0}}, {pair});
    const int wardrobe = b.node("cabinet", "wardrobe", {{"width", 2.0}, {"height", 2.2}, {"depth", 0.6},
                                                        {"columns", 3}, {"rows", 1}, {"fronts", 0}, {"base", 0},
                                                        {"handles", 1}, {"materialBody", m.white},
                                                        {"materialFront", m.white}, {"materialHandle", m.steel},
                                                        {"at", {2.45, 0.0, 0.0}}, {"turn", -90.0}});
    const int rug = b.node("rug", "rug", {{"width", 2.4}, {"depth", 1.6}, {"at", {0.0, 0.0, 0.7}},
                                          {"materialRug", m.fabricSand}, {"materialBorder", m.rugBorder}});
    b.g.output = b.node("merge", "bedroom", json::object(), {bed, sides, wardrobe, rug});
    return std::move(b.g);
}

// A desk on steel sled frames, an office chair, boards over the desk and an
// open bookcase beside it.
proc::Graph homeOffice(std::vector<MaterialDef>& mats) {
    const Interior m = interior(mats);
    Builder b;
    const int desk = b.node("table", "desk", {{"width", 1.4}, {"depth", 0.7}, {"height", 0.74}, {"base", 3},
                                              {"legSize", 0.05}, {"inset", 0.04}, {"apron", false},
                                              {"materialTop", m.oak}, {"materialBase", m.black}});
    const int chair = b.node("chair", "office_chair", {{"base", 2}, {"back", 4}, {"arms", true},
                                                       {"seatWidth", 0.5}, {"seatDepth", 0.48}, {"backHeight", 0.5},
                                                       {"materialFrame", m.black}, {"materialSeat", m.fabric},
                                                       {"at", {0.0, 0.0, 0.55}}, {"turn", 200.0}});
    const int boards = b.node("shelf", "wall_boards", with({{"style", 2}, {"width", 1.2}, {"height", 1.75},
                                                            {"depth", 0.25}, {"boards", 2}, {"books", 0.5},
                                                            {"seed", 7}, {"materialBody", m.oak},
                                                            {"at", {0.0, 0.0, -0.27}}}, books(m)));
    const int shelf = b.node("shelf", "open_shelf", with({{"style", 1}, {"width", 0.8}, {"height", 1.8},
                                                          {"depth", 0.35}, {"boards", 5}, {"back", false},
                                                          {"books", 0.65}, {"seed", 2}, {"materialBody", m.black},
                                                          {"at", {1.3, 0.0, -0.22}}}, books(m)));
    const int lamp = b.node("lamp", "desk_lamp", {{"kind", 1}, {"shade", 1}, {"height", 0.42},
                                                  {"shadeRadius", 0.12}, {"shadeHeight", 0.14},
                                                  {"materialBody", m.black}, {"materialShade", m.shade},
                                                  {"at", {0.5, 0.74, -0.15}}});
    b.g.output = b.node("merge", "home_office", json::object(), {desk, chair, boards, shelf, lamp});
    return std::move(b.g);
}

// A kitchen run: base units with drawers over doors under a stone worktop,
// wall units over it, a tall unit at the end and a hob set into the top.
proc::Graph kitchen(std::vector<MaterialDef>& mats) {
    const Interior m = interior(mats);
    Builder b;
    const int base = b.node("cabinet", "base_units", {{"width", 2.4}, {"height", 0.9}, {"depth", 0.6},
                                                      {"columns", 4}, {"fronts", 4}, {"base", 0},
                                                      {"baseHeight", 0.1}, {"worktop", true},
                                                      {"topThickness", 0.04}, {"overhang", 0.02}, {"handles", 1},
                                                      {"materialBody", m.white}, {"materialFront", m.white},
                                                      {"materialHandle", m.steel}, {"materialTop", m.stone}});
    const int wall = b.node("cabinet", "wall_units", {{"width", 2.4}, {"height", 2.2}, {"depth", 0.35},
                                                      {"columns", 4}, {"fronts", 0}, {"base", 2},
                                                      {"baseHeight", 1.45}, {"handles", 1},
                                                      {"materialBody", m.white}, {"materialFront", m.white},
                                                      {"materialHandle", m.steel}, {"at", {0.0, 0.0, -0.125}}});
    const int tall = b.node("cabinet", "tall_unit", {{"width", 0.6}, {"height", 2.2}, {"depth", 0.6},
                                                     {"columns", 1}, {"rows", 2}, {"fronts", 0}, {"base", 0},
                                                     {"baseHeight", 0.1}, {"handles", 1},
                                                     {"materialBody", m.white}, {"materialFront", m.white},
                                                     {"materialHandle", m.steel}, {"at", {1.5, 0.0, 0.0}}});
    int hob = b.node("box", "hob", {{"size", {0.58, 0.006, 0.5}}, {"center", {-0.6, 0.903, 0.0}}});
    hob = b.mat("hob_glass", m.black, hob);
    b.g.output = b.node("merge", "kitchen", json::object(), {base, wall, tall, hob});
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
        {"Town house", "A main block under a hip roof, a shop wing with a gable, a\n"
                       "corner tower with a spire: Roof and Facade at work."},
        {"Office tower", "Setbacks: podium, shaft and crown behind parapets, ribbon\n"
                         "windows, shops on the ground floor only."},
        {"Arch bridge", "A stone viaduct: Arch bays copied in a row, a road, parapets\n"
                        "from Offset and Railing."},
        {"Suspension bridge", "Towers, cables hung as Arch curves, hangers dropped onto\n"
                              "the deck with Drop lines, railings along the edges."},
        {"Truss bridge", "A Pratt Truss along a path, a deck swept under it, abutments."},
        {"Dining room", "A table on tapered legs, six chairs copied and mirrored round\n"
                        "it, a rug and a pendant: the Furniture nodes."},
        {"Living room", "A sofa with a chaise, an armchair, a coffee table, a TV board,\n"
                        "a bookcase full of books and a floor lamp."},
        {"Bedroom", "A double bed with a padded headboard, nightstands and lamps\n"
                    "mirrored either side, a wardrobe, a rug."},
        {"Home office", "A desk on steel sled frames, an office chair, boards over the\n"
                        "desk and an open shelf beside it."},
        {"Kitchen", "Base units with drawers over doors under a stone worktop, wall\n"
                    "units, a tall unit and a hob."},
        {"Empty", "One box, ready for your own graph."},
    };
    return l;
}

proc::Graph build(int i, std::vector<MaterialDef>& materials) {
    switch (i) {
        case 0:  return ringStation(materials);
        case 1:  return modularStation(materials);
        case 2:  return fuelDepot(materials);
        case 3:  return townHouse(materials);
        case 4:  return officeTower(materials);
        case 5:  return archBridge(materials);
        case 6:  return suspensionBridge(materials);
        case 7:  return trussBridge(materials);
        case 8:  return diningRoom(materials);
        case 9:  return livingRoom(materials);
        case 10: return bedroom(materials);
        case 11: return homeOffice(materials);
        case 12: return kitchen(materials);
        default: return empty();
    }
}

} // namespace procpreset