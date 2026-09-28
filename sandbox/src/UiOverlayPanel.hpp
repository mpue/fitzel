#pragma once

#include <string>
#include <vector>

#include "UiOverlay.hpp"

class CommandStack;
class Document;
namespace fitzel { class AssetDatabase; }

// The editor's side of the scene's 2D UI overlay: the "UI Overlay" panel
// (UiOverlay::drawEditorPanel) with its edits bracketed into undo steps, and
// "Copy to scene" -- the overlay written into a sibling scene file. UiOverlay
// itself is runtime and knows nothing about files; this is where those live.
// Editor only.
namespace uioverlayui {

// An edit of the overlay in flight: the element list as it was when the edit
// began. The overlay is not in the Document, so it brackets its own undo: the
// step opens when a field is first touched and commits once nothing is active,
// so a slider dragged across many frames is one step.
struct Bracket {
    std::vector<UiElement> before;
    bool                   open = false;
};

struct Host {
    bool&                  show;
    int&                   sel;              // the selected element
    fitzel::AssetDatabase& assetDb;          // the image slot
    const std::string&     currentProject;   // the open scene file ("" for none)
    std::vector<std::string> sounds;         // for PlaySound
    CommandStack&          history;
    Document&              document;
};

void panel(UiOverlay& overlay, Bracket& bracket, const Host& h);

// Write `overlay` into the scene `stem` beside `currentProject` without opening
// it: only the overlay keys of its settings change, its entities and everything
// else stay. Returns the one-line result the panel shows.
std::string copyToScene(UiOverlay& overlay, const std::string& currentProject,
                        const std::string& stem);

} // namespace uioverlayui
