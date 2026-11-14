#pragma once

#include <functional>
#include <string>

#include <glm/glm.hpp>

#include "CableHolder.hpp"

class CommandStack;
class Document;
class Selection;

// The editor's "Cable holders" panel: pick the kind -- comb, desk clip, tray,
// hook, winder -- set it up while its drawing follows (front, side or from
// above, as fits the kind), and place it as an ordinary entity subtree at the
// 3D cursor or in front of the camera. The root keeps the parameters, so "Edit
// selected" re-opens a placed one and every change rebuilds it in place as one
// undo step; File > Export Selection as 3MF prints it, labels as a second part.
//
// The Add menu's cable holder items land here too, the panel open on the new
// one. Owns its state and its actions, so main only constructs it and calls
// panel() (see GridfinityPanel, the same shape).
namespace cableui {

class CableHolderTool {
public:
    struct Deps {
        Document&     document;
        CommandStack& history;
        Selection&    sel;
        int&          entityCounter;
        std::function<glm::vec3(float)> spawnPoint;   // where a new object lands
        std::string&  status;                          // the editor's footer line
    };
    explicit CableHolderTool(Deps d);

    void panel(bool& show);

    // One with the panel's current settings standing at `at` (world), hung
    // under `parentId` when that is an entity. Selected, and the panel edits it.
    void placeAt(const glm::vec3& at, int parentId = -1);
    // The kind the next one is (the Add menu's items).
    void setStyle(cableholder::Style s);
    // What the next one is made of (the editor's FITZEL_CABLE test hook).
    void setParams(const cableholder::Params& p);
    // The one being edited rebuilt from `p`, as "Update placed" does.
    void updateWith(const cableholder::Params& p);
    const cableholder::Params& params() const { return m_cfg; }

private:
    void place();
    void rebuild();
    void editSelected();
    int  selectedId() const;   // the holder the selection is part of, or -1
    void refresh();            // the preview, when the parameters changed
    void syncText();           // the text fields from m_cfg
    void seatControls(bool& changed);
    void styleControls(bool& changed);

    Deps                m_d;
    cableholder::Params m_cfg;
    int  m_liveId  = -1;     // the holder this panel is editing, if still in the scene
    bool m_auto    = true;   // rebuild the live one on every edit
    bool m_pending = false;  // an edit waiting for the rebuild
    bool m_raise   = false;  // to the front on the next frame (placed from the Add menu)

    char m_diameters[256] = {};
    char m_labels[512]    = {};
    char m_winderLabel[128] = {};

    cableholder::Params m_shown;
    bool                m_haveModel = false;
    cableholder::Model  m_model;
};

} // namespace cableui
