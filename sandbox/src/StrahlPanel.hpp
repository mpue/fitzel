#pragma once

#include <filesystem>
#include <memory>
#include <string>

struct ImFont;

// The Strahl window (View > Presentation > Strahl): a scene script on the
// left, the picture it makes on the right.
//
// Built for the loop a scene language is written in -- change a number, look,
// change it again -- so the preview follows the text: a moment after typing
// stops, the script is run again (on a worker, so a slow script never stalls
// the editor) and a draft of the picture starts refining in place of the old
// one. A script that fails keeps the last good picture and underlines the
// line that stopped it. Render runs the script's own render { } settings in
// full; Save image writes the PNG and a linear .exr into the project's
// renders/ folder, as the Render panel does.
//
// Scripts live in the project's strahl/ folder; the bundled examples
// (strahl-lib/examples beside the editor) open from the same menu, and
// `import` finds the bundled library (strahl-lib) as well as the project's
// own scripts. The language is Strahl.hpp; this file is only the window.
namespace strahlpanel {

struct State {
    bool open = false;   // the View menu's flag

    State();
    ~State();
    State(const State&)            = delete;
    State& operator=(const State&) = delete;

    // Open a script into the editor (and show the window).
    void openFile(const std::filesystem::path& file);
    // The buffer as it stands, and the file it came from ("" = unsaved).
    std::string text() const;
    const std::filesystem::path& file() const;
    // The window had the keyboard last frame (Ctrl+S saves the script then).
    bool focused() const;

    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Draw the window. `sceneFile` is the open project's scene (its folder holds
// strahl/ and renders/); empty when no project is open. `now` is the editor's
// clock, for the preview's rate limit and the typing pause.
void draw(State& state, const std::filesystem::path& sceneFile, ImFont* mono, double now);

} // namespace strahlpanel
