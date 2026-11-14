#pragma once

#include <functional>
#include <string>

#include <glm/glm.hpp>

#include "CardGen.hpp"
#include "QrCodePanel.hpp"

class CommandStack;
class Document;
class Selection;

// The editor's "Cards" panel: pick a size, type the text, put a QR code on it
// -- and see the card as it will print, at the top of the panel, where the text
// and the code can be dragged about (they snap to the nine anchors; the 3x3
// pickers below do the same with a click, and every offset can be typed). The
// font list shows each font on the card while the pointer rests on it. Every
// change rebuilds the placed card as one undo step; "Edit selected card"
// re-opens one from the scene.
//
// Owns its state and its actions, so main only constructs it and calls panel()
// (the shape of StreetSignPanel and QrCodePanel).
namespace cardui {

class CardTool {
public:
    struct Deps {
        Document&     document;
        CommandStack& history;
        Selection&    sel;
        int&          entityCounter;
        std::function<glm::vec3(float)> spawnPoint;   // where a new object lands
        std::string&  status;                          // the editor's footer line
    };
    explicit CardTool(Deps d);

    void panel(bool& show);

    // A card with the panel's current settings standing at `at` (world), hung
    // under `parentId` when that is an entity. Selected, and the panel edits it.
    void placeAt(const glm::vec3& at, int parentId = -1);
    // What the next card is made of (the editor's FITZEL_CARD test hook).
    void setParams(const card::Params& p);

private:
    void place();
    void rebuild();
    void editSelected();
    int  selectedCardId() const;
    bool preview(const card::Params& shown, const card::Layout& l, float width);
    bool fontCombo(float width);

    Deps         m_d;
    card::Params m_cfg;
    int  m_liveId  = -1;     // the card this panel is editing, if still in the scene
    bool m_auto    = true;   // rebuild the live card on every edit
    bool m_pending = false;  // an edit waiting for the rebuild
    char m_text[2048]  = {};
    char m_qrText[1024] = {};
    char m_fontSearch[64] = {};
    qrui::WifiForm m_wifi;
    std::string m_hoverFont;   // the font list's row under the pointer, shown on the card
    int  m_drag = 0;           // dragging in the preview: 1 the text, 2 the code
    glm::dvec2 m_dragLo{0.0};  // the dragged thing's lower left (mm)
};

} // namespace cardui
