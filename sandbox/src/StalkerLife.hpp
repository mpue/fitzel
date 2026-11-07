#pragma once

// The life of a S.T.A.L.K.E.R. level in fitzel: what StalkerImport adds to a
// level's scene beyond its geometry.
//
//   - Its people and creatures as animated figures: every visual a stalker, a
//     trader or a mutant of level.spawn wears becomes a skinned GLB with the
//     clips the game plays (idle, walk, run, sit, guitar, attack, death, ...),
//     shared by all levels in models/characters/.
//   - Its sounds: the level's own sound sources (level.snd_static), the
//     ambience and music of the game's configs, the anomalies', the
//     creatures' and the camp songs -- copied from the game into
//     sounds/stalker/ under flat names (fitzel finds a sound by its file name).
//   - Its logic as data and Lua: scripts/stalker/levels/<level>.lua holds what
//     the level says (who walks which patrol path, who sits at which camp
//     fire, where the anomalies and the radiation are, where each exit leads),
//     and scripts/stalker/*.lua play it -- one controller on one object of the
//     scene drives every figure, anomaly and exit, the player's health and the
//     HUD.
//
// The game's own scripts (quests, dialogs, the A-Life simulation) are written
// against the X-Ray engine and are not run; what the level's data says about
// its people and places is, by these scripts.

#include <map>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "StalkerImport.hpp"
#include "XrCharacter.hpp"
#include "XrLtx.hpp"

namespace xray { struct Level; }

namespace stalker {

// A figure written to models/characters/.
struct Figure {
    bool        ok = false;
    std::string guid;
    std::string file;              // "green_stalker_2.glb", under models/characters/
    std::string family;            // "human", "trader", "mutant"
    glm::vec3   lo{0.0f}, hi{0.0f};// rest-pose bounds in the model's space
    std::set<std::string> clips;
};

// What converting several levels in one run shares.
struct Session {
    bool      configLoaded = false;
    xray::Ltx config;
    std::map<std::string, xray::MotionLibrary> motions;   // by reference
    std::map<std::string, Figure> figures;                 // by visual
    std::map<std::string, std::string> sounds;             // game name -> file ("" = missing)
    std::map<std::string, std::vector<std::string>> soundDirs;   // "sounds/monsters/dog/" -> files
    bool      scriptsWritten = false;
};

struct LifeReport {
    int figures = 0;               // placed in the scene
    int models = 0;                // figure GLBs written this level
    int sounds = 0;                // sound sources placed
    int anomalies = 0;
    int exits = 0;
    int soundFiles = 0;            // copied
};

// Adds the level's life to the scene being written: entities (appended to
// `ents`, ids from `nextId`), the animation graphs and the sound settings in
// `settings`, and the files beside it. `level` is already lifted by `lift`.
bool addLife(const xray::FileSystem& fs, const Options& o, const xray::Level& level, float lift,
             bool indoor, Session& session, nlohmann::json& ents, int& nextId,
             nlohmann::json& settings, Report& report, LifeReport& life, const Log& log);

// The Lua the scene runs (StalkerScripts.cpp): file name -> source.
const std::map<std::string, std::string>& luaScripts();

} // namespace stalker
