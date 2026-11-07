#pragma once

// A S.T.A.L.K.E.R. level as a fitzel model and scene: the level's geometry
// (XrLevel) with its textures, grouped into square cells of the map, written
// as one GLB -- one node per cell, one primitive per material -- into a
// project's models/ folder, the textures beside it as files of their own; and
// a scene that places every cell where the level has it, adds the level's
// lights, the sun and a player start at the actor's spawn.
//
//   <project>/models/<level>.glb (+ .meta, whose GUID survives a re-import)
//   <project>/models/<level>_props.glb   the props level.spawn places
//   <project>/models/<level>/*.jpg|png
//   <project>/models/characters/*.glb    the figures (StalkerLife.hpp)
//   <project>/sounds/stalker/*.ogg       the sounds
//   <project>/scripts/stalker/           the logic: data per level, the scripts
//   <project>/<level>.fitzel
//   <project>/game.json            only when the project has none yet
//
// What the X-Ray shaders become: lightmapped and vertex-lit surfaces opaque,
// the terrain baked per cell from its colour map, mask and detail maps,
// alpha-tested ones (def_aref, def_trans, leaves) cut out and two-sided,
// wallmarks and water blended (set in the scene's material overrides, glTF has
// no blend fitzel reads), selflight emissive. Glows, light planes and LOD
// billboards are effects, not geometry, and are left out. The level's own
// lighting (lightmaps, hemi) is not carried over: fitzel lights it itself.

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace xray { class FileSystem; }

namespace stalker {

struct Session;   // StalkerLife.hpp

struct Options {
    std::string level;                    // "l01_escape"
    std::filesystem::path project;        // a fitzel project folder (made if missing)
    float    cell = 64.0f;                // metres per side of a map cell = one node
    int      maxTexture = 2048;           // larger maps are halved down to this
    // The terrain's own map per cell, texels per metre (0: keep the game's one
    // colour map stretched over the whole level, without its detail maps).
    float    terrainDensity = 8.0f;
    // Raise the level until nothing is below y = 0, where fitzel puts an
    // invisible floor under a scene that has no terrain of its own.
    bool     liftAboveGround = true;
    // The physics props level.spawn places (crates, barrels, lamps, wrecks),
    // in <level>_props.glb.
    bool     props = true;
    bool     writeScene = true;
    // The level's people, creatures, sounds and logic (StalkerLife.hpp).
    bool     life = true;
    unsigned threads = 0;                 // texture workers; 0 = one per core
    // Shared by the levels of one run (configs, figures, sounds); null = one
    // of its own for this level.
    Session* session = nullptr;
};

struct Report {
    std::filesystem::path model, propModel, scene;
    int nodes = 0;
    int primitives = 0;
    int materials = 0;
    int textures = 0;
    int lights = 0;
    int props = 0;
    int figures = 0;                      // people and creatures placed
    int anomalies = 0;
    int exits = 0;                        // level changers
    int soundSources = 0;
    std::size_t triangles = 0;
    float lift = 0.0f;                    // metres the level was raised by
    std::vector<std::string> warnings;
};

using Log = std::function<void(const std::string&)>;

bool importLevel(const xray::FileSystem& fs, const Options& options, Report& report,
                 std::string& error, const Log& log = {});

} // namespace stalker
