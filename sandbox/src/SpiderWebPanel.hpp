#pragma once

#include <functional>
#include <string>

#include <glm/glm.hpp>

#include "SpiderWeb.hpp"

class CommandStack;
class Document;
class Selection;

// The editor's "Spider web" panel: round in a hoop, on a frame or in a corner,
// how many radii, how close the spiral, how uneven and how torn, while the
// drawing follows from above and the plastic it takes is counted in grams;
// place it as an ordinary entity subtree at the 3D cursor or in front of the
// camera. The root keeps the parameters, so "Edit selected" re-opens a placed
// one and every change rebuilds it in place as one undo step; File > Export
// Selection as 3MF prints it.
//
// The Add menu's "Spider web" lands here too, the panel open on the new one.
// Owns its state and its actions, so main only constructs it and calls panel()
// (see PenHolderPanel, the same shape).
namespace webui {

class SpiderWebTool {
public:
    struct Deps {
        Document&     document;
        CommandStack& history;
        Selection&    sel;
        int&          entityCounter;
        std::function<glm::vec3(float)> spawnPoint;   // where a new object lands
        std::string&  status;                          // the editor's footer line
    };
    explicit SpiderWebTool(Deps d);

    void panel(bool& show);

    // One with the panel's current settings standing at `at` (world), hung
    // under `parentId` when that is an entity. Selected, and the panel edits it.
    void placeAt(const glm::vec3& at, int parentId = -1);
    // What the next one is made of (the editor's FITZEL_WEB test hook).
    void setParams(const spiderweb::Params& p) { m_cfg = p; }
    // Its shape (the Add menu's three webs), the radii carried over as the
    // panel's shape buttons do.
    void setShape(spiderweb::Shape s);
    // The one being edited rebuilt from `p`, as "Update placed" does.
    void updateWith(const spiderweb::Params& p);
    const spiderweb::Params& params() const { return m_cfg; }

private:
    void place();
    void rebuild();
    void editSelected();
    int  selectedId() const;   // the web the selection is part of, or -1
    void refresh();            // the preview, when the parameters changed

    Deps              m_d;
    spiderweb::Params m_cfg;
    int  m_liveId  = -1;     // the web this panel is editing, if still in the scene
    bool m_auto    = true;   // rebuild the live one on every edit
    bool m_pending = false;  // an edit waiting for the rebuild
    bool m_raise   = false;  // to the front on the next frame (placed from the Add menu)

    spiderweb::Params m_shown;
    bool              m_haveModel = false;
    spiderweb::Model  m_model;
};

} // namespace webui
