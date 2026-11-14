#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "Gridfinity.hpp"

class CommandStack;
class Document;
class Selection;

// The editor's "Gridfinity" panel: a bin -- grid units across, front to back
// and high, compartments, a label shelf, a scoop, the stacking lip, magnets and
// screws -- or a baseplate for a drawer, drawn from above and cut through
// while it is set up, then placed as an ordinary entity subtree at the 3D
// cursor or in front of the camera. The root keeps the parameters, so "Edit
// selected" re-opens a placed one and every change rebuilds it in place as one
// undo step; File > Export Selection as 3MF prints it.
//
// The Add menu's "Gridfinity bin" and "Gridfinity baseplate" land here too: a
// default one where the menu was opened, the panel open on it. Owns its state
// and its actions, so main only constructs it and calls panel() (see
// QrCodePanel, the same shape).
namespace gridui {

class GridfinityTool {
public:
    struct Deps {
        Document&     document;
        CommandStack& history;
        Selection&    sel;
        int&          entityCounter;
        std::function<glm::vec3(float)> spawnPoint;   // where a new object lands
        std::string&  status;                          // the editor's footer line
    };
    explicit GridfinityTool(Deps d);

    void panel(bool& show);

    // One with the panel's current settings standing at `at` (world), hung
    // under `parentId` when that is an entity. Selected, and the panel edits it.
    void placeAt(const glm::vec3& at, int parentId = -1);
    // A bin or a baseplate next (the Add menu's two items).
    void setKind(gridfinity::Kind k);
    // What the next one is made of (the editor's FITZEL_GRIDFINITY test hook).
    void setParams(const gridfinity::Params& p);
    // The one being edited rebuilt from `p`, as "Update placed" does
    // (FITZEL_GRIDFINITY_THEN).
    void updateWith(const gridfinity::Params& p);
    const gridfinity::Params& params() const { return m_cfg; }

private:
    void place();
    void rebuild();
    void editSelected();
    int  selectedId() const;   // the bin or plate the selection is part of, or -1
    void refresh();            // the preview, when the parameters changed
    void binControls(bool& changed);
    void plateControls(bool& changed);

    Deps               m_d;
    gridfinity::Params m_cfg;
    int  m_liveId  = -1;     // the one this panel is editing, if still in the scene
    bool m_auto    = true;   // rebuild the live one on every edit
    bool m_pending = false;  // an edit waiting for the rebuild
    bool m_raise   = false;  // to the front on the next frame (placed from the Add menu)

    // The preview: built from m_cfg as it was last drawn.
    gridfinity::Params   m_shown;
    bool                 m_haveModel = false;
    gridfinity::Model    m_model;
    std::vector<std::vector<glm::vec2>> m_section;   // cut front to back (y, z), mm
    float                m_cutX = 0.0f;              // where it was cut (x, mm)
};

} // namespace gridui
