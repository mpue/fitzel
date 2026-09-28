#pragma once

#include <functional>
#include <string>
#include <vector>

#include <TextEditor.h>   // ImGuiColorTextEdit

#include "LuaCompletion.hpp"

class ScriptSystem;
namespace fitzel { class Gui; }

// The Lua script editor: the "Scripts" window with its syntax-highlighted
// buffer, code completion and the New Script dialog. Open/create/save the .lua
// files under scripts/; saving reloads the script VM so the next Play uses the
// edited code. A script is assigned to an entity in the Inspector, which lists
// and opens them through here. Editor only.
class ScriptEditor {
public:
    // Where entity scripts live (main's scriptsDir: the open project's scripts/
    // folder, or the bundled one when no project is open).
    std::function<std::string()> scriptsDir;
    bool visible = false;   // the window is open (View menu, Inspector's "Edit")

    ScriptEditor();

    // .lua files currently in the scripts dir (bare names, sorted).
    std::vector<std::string> list() const;
    // Open a script (bare filename under the scripts dir) and show the window.
    void open(const std::string& file);
    // Write the buffer back and reload the VM so Play picks it up.
    void save(ScriptSystem& scripts);

    // The window, while `visible`.
    void panel(ScriptSystem& scripts, fitzel::Gui& gui);

private:
    std::string path(const std::string& file) const { return scriptsDir() + "/" + file; }

    TextEditor  m_editor;
    std::string m_path;              // "scripts/<file>.lua" open ("" = none)
    bool        m_dirty = false;     // unsaved changes
    char        m_newName[64] = "";
    int         m_newTemplate = 0;   // 0 = empty component, 1 = documented
    // Code-completion popup state (see luacomplete::Completions): the popup
    // shows while there are matches and the editor is focused -- Tab/Enter
    // accepts, arrows navigate, Esc dismisses.
    luacomplete::Completions m_comp;
};
