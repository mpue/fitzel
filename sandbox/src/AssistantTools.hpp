#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

struct EditorContext;

// What the Assistant window (AssistantPanel.hpp) can do in the editor: the
// tools a language model calls to look at the scene, make and change objects,
// colour them, give them components, place models and prefabs, set the weather
// and the clock, open the editor's panels, write scripts and hang them on
// objects, press Play and read what went wrong -- and look things up: the
// manuals, and what each component does (its header's own comments).
//
// Each tool is a name, a description and a JSON schema for the model, and a
// function the window runs on the main thread, between two frames. Scene edits
// go through the undo history exactly as a hand's would, one step per call --
// Ctrl+Z takes back what the model did. Objects are named by id or by name,
// whichever the model has. Editor only.
namespace assistant {

struct Tool {
    std::string    name, description;
    nlohmann::json parameters;      // JSON schema of the arguments (an object)
    bool           changes = false; // changes the scene or a file: asked first when wanted
    std::function<nlohmann::json(const nlohmann::json& args)> run;
};

// What only main can do.
struct Host {
    std::function<std::string()> scriptsDir;    // the project's scripts/ folder
    std::function<std::string()> projectFile;   // the open scene file ("" = none)
    std::function<bool()>        csharp;        // the project scripts in C#
    std::function<bool()>        playing;
    std::function<void()>        play, stop;
    std::function<bool()>        saveScene;     // false: no file to save into
    std::function<void(const std::string& file)> openScript;   // in its editor
    std::function<void()>        reloadScripts; // a .lua written: the VM again
    std::function<std::string()> scriptError;   // the last Lua error ("" = none)
    std::function<glm::vec3()>   spawnPoint;    // where a new object goes by default
    std::function<void(int id)>  frame;         // the view onto an object
    // The View menu's panels ("Construct > Roads", ...), and opening one.
    std::function<std::vector<std::string>()>    panels;
    std::function<bool(const std::string& name)> openPanel;
    // The weather presets by name, the sky turned into one at once; the clock.
    std::function<std::vector<std::string>()>    weatherPresets;
    std::function<bool(const std::string& name)> setWeather;
    std::function<float()>                       timeOfDay;
    std::function<void(float hours)>             setTimeOfDay;
    // The project's prefabs (name, file), and one placed: its root's id, -1
    // when it could not be.
    std::function<std::vector<std::pair<std::string, std::string>>()> prefabs;
    std::function<int(const std::string& file, glm::vec3 pos)>        placePrefab;
    // Roads: a new one laid and built (graded into the terrain, as the Roads
    // panel's Build does) -- its index, -1 when it could not be; and the list.
    struct Road {
        std::string                      name;
        std::vector<glm::vec2>           points;    // control points, world x,z
        std::vector<float>               lifts;     // metres above the ground, per point
        std::vector<std::pair<int, int>> bridges;   // point index spans the road flies over
        bool                             closed = false;
    };
    std::function<int(const Road&)>           createRoad;
    std::function<std::vector<std::string>()> roads;
};

std::vector<Tool> makeTools(EditorContext& ed, const Host& host);

// A few lines on the scene and the project for the model's instructions.
std::string brief(EditorContext& ed, const Host& host);
// The scripting API of the project's language, for the model's instructions:
// it writes scripts against what is there instead of what it guesses.
std::string reference(const Host& host);
// What Fitzel is and has, in a few lines a feature (from docs/FEATURES.md),
// and where in the editor each panel is: the model's map of the engine.
std::string overview(const Host& host);

// A manual beside the exe (or in the source tree): "" when it is not there.
std::string readManual(const std::string& name);
// The chapters of the manuals that fit a question best -- only those that
// share at least two of its words with it -- up to `budget` characters. ""
// when nothing fits. Handed to the model with every question, so what it says
// about Fitzel comes from the manuals rather than from other engines.
std::string lookup(const std::string& question, std::size_t budget);

} // namespace assistant
