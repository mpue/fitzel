#pragma once

#include <memory>
#include <string>

namespace fitzel { class Audio; }

// The editor's Synth panel: where a patch is built and heard.
//
// It is a modular synth's editor without a cable in it. Synthlab (and every
// other modular) has you drag a lead from an output to an input; this editor
// asks you to press the input, then press the module that should feed it --
// two clicks, each on a target the size of a button, and nothing held down in
// between. That is the same rule the rest of this editor follows (see
// ModelingPanel.hpp), and it is why the patch is a LIST here rather than a
// canvas: a list has no positions to hit.
//
// What it shows back is the signal itself. Every module draws the samples it
// produced in the last block, so "why can I not hear this" is answered by
// looking down the column -- the flat line is where the sound stops.
//
// Panel state (the patch being edited, the preview voice, what is being wired)
// lives in here, not in main.
namespace synthui {

class Panel {
public:
    Panel();
    ~Panel();
    Panel(const Panel&)            = delete;
    Panel& operator=(const Panel&) = delete;

    // `projectFile` is the open .fitzel ("" when none): patches live beside it
    // in <project>/content/patches. `audio` is the engine the preview plays on.
    void draw(bool& show, fitzel::Audio& audio, const std::string& projectFile);

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace synthui
