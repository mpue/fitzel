#pragma once

#include <functional>
#include <string>

#include <glm/glm.hpp>

#include "WinterStencil.hpp"

class CommandStack;
class Document;
class Selection;

// The editor's "Winter stencil" panel: the frame, what stands on the snow and
// what hangs in the sky, how thick the lines, while the drawing follows (the
// plate dark against the light) and the plastic it takes is counted in grams;
// place it as an ordinary entity subtree at the 3D cursor or in front of the
// camera. The root keeps the parameters, so "Edit selected" re-opens a placed
// one and every change rebuilds it in place as one undo step; File > Export
// Selection as 3MF prints it.
//
// The Add menu's "Winter stencil" lands here too, the panel open on the new
// one. Owns its state and its actions, so main only constructs it and calls
// panel() (see SpiderWebPanel, the same shape).
namespace stencilui {

class WinterStencilTool {
public:
    struct Deps {
        Document&     document;
        CommandStack& history;
        Selection&    sel;
        int&          entityCounter;
        std::function<glm::vec3(float)> spawnPoint;   // where a new object lands
        std::string&  status;                          // the editor's footer line
    };
    explicit WinterStencilTool(Deps d);

    void panel(bool& show);

    // One with the panel's current settings standing at `at` (world), hung
    // under `parentId` when that is an entity. Selected, and the panel edits it.
    void placeAt(const glm::vec3& at, int parentId = -1);
    // What the next one is made of (the editor's FITZEL_STENCIL test hook).
    void setParams(const winterstencil::Params& p) { m_cfg = p; }
    // The one being edited rebuilt from `p`, as "Update placed" does.
    void updateWith(const winterstencil::Params& p);
    const winterstencil::Params& params() const { return m_cfg; }

private:
    void place();
    void rebuild();
    void editSelected();
    int  selectedId() const;   // the stencil the selection is part of, or -1
    void refresh();            // the preview, when the parameters changed

    Deps              m_d;
    winterstencil::Params m_cfg;
    int  m_liveId  = -1;     // the stencil this panel is editing, if still in the scene
    bool m_auto    = true;   // rebuild the live one on every edit
    bool m_pending = false;  // an edit waiting for the rebuild
    bool m_raise   = false;  // to the front on the next frame (placed from the Add menu)

    winterstencil::Params m_shown;
    bool              m_haveModel = false;
    winterstencil::Model  m_model;
};

} // namespace stencilui
