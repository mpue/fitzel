#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

struct EditorContext;

// What the Assistant window (AssistantPanel.hpp) can do in the editor: the
// tools a language model calls to look at the scene, make and change objects,
// colour them, give them components, write scripts and hang them on objects,
// press Play and read what went wrong.
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
};

std::vector<Tool> makeTools(EditorContext& ed, const Host& host);

// A few lines on the scene and the project for the model's instructions.
std::string brief(EditorContext& ed, const Host& host);
// The scripting API of the project's language, for the model's instructions:
// it writes scripts against what is there instead of what it guesses.
std::string reference(const Host& host);

} // namespace assistant
