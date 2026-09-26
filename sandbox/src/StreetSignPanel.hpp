#pragma once

#include <functional>
#include <string>

#include <glm/glm.hpp>

#include "StreetSign.hpp"

class CommandStack;
class Document;
class Selection;

// The editor's "Street signs" panel: type a name (or pick one of Frankfurt's),
// click a style, watch the sign redraw, and place it -- an ordinary entity
// subtree in front of the camera (or at the 3D cursor), whose root keeps the
// parameters so "Edit selected" re-opens it and every change rebuilds it in
// place as one undo step.
//
// Unlike the House panel this one owns its state and its actions, so main only
// constructs it and calls panel() and does not grow by another block of lambdas.
namespace signui {

class StreetSignTool {
public:
    struct Deps {
        Document&     document;
        CommandStack& history;
        Selection&    sel;
        int&          entityCounter;
        std::function<glm::vec3(float)> spawnPoint;   // where a new object lands
        std::string&  status;                          // the editor's footer line
    };
    explicit StreetSignTool(Deps d);

    void panel(bool& show);

private:
    void place();
    void rebuild();
    void editSelected();
    int  selectedSignId() const;   // the sign the selection is part of, or -1

    Deps               m_d;
    streetsign::Params m_cfg;
    int  m_liveId  = -1;     // the sign this panel is editing, if still in the scene
    bool m_auto    = true;   // rebuild the live sign on every edit
    bool m_pending = false;  // an edit waiting for the rebuild
    char m_bufA[160] = {};
    char m_bufB[160] = {};
    bool m_second  = false;  // second blade on (Post)
    int  m_pickFor = 0;      // which blade the name list fills (0 = A, 1 = B)
    char m_search[64] = {};
};

} // namespace signui
