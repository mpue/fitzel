#pragma once

#include <functional>
#include <string>

#include <glm/glm.hpp>

#include "QrCode.hpp"

class CommandStack;
class Document;
class Selection;

// The editor's "QR codes" panel: type a text (or fill in a Wi-Fi network),
// pick how much may be lost, set the width and how thick the plate and the
// code are, and place it -- an ordinary entity subtree at the 3D cursor or in
// front of the camera, whose root keeps the parameters so "Edit selected"
// re-opens it and every change rebuilds it in place as one undo step.
//
// The Add menu's "QR code" lands here too: a default code where the menu was
// opened, the panel open on it. Owns its state and its actions, so main only
// constructs it and calls panel() (see StreetSignPanel, the same shape).
namespace qrui {

// A Wi-Fi network as phones read it from a code: WIFI:T:WPA;S:name;P:pass;;
// (security 0 WPA, 1 WEP, 2 open), with \ ; , : " escaped.
std::string wifiText(const char* ssid, const char* pass, int security, bool hidden);

// The little form that writes one, as a popup the caller opened as `id`.
// True on the frame "Use as the text" was clicked, `text` set.
struct WifiForm {
    char ssid[64] = {};
    char pass[64] = {};
    int  security = 0;
    bool hidden   = false;
};
bool wifiPopup(const char* id, WifiForm& w, std::string& text);

class QrCodeTool {
public:
    struct Deps {
        Document&     document;
        CommandStack& history;
        Selection&    sel;
        int&          entityCounter;
        std::function<glm::vec3(float)> spawnPoint;   // where a new object lands
        std::string&  status;                          // the editor's footer line
    };
    explicit QrCodeTool(Deps d);

    void panel(bool& show);

    // A code with the panel's current settings standing at `at` (world), hung
    // under `parentId` when that is an entity. Selected, and the panel edits it.
    void placeAt(const glm::vec3& at, int parentId = -1);
    // What the next code is made of (the editor's FITZEL_QR test hook).
    void setParams(const qrcode::Params& p);

private:
    void place();
    void rebuild();
    void editSelected();
    int  selectedCodeId() const;   // the code the selection is part of, or -1

    Deps           m_d;
    qrcode::Params m_cfg;
    int  m_liveId  = -1;     // the code this panel is editing, if still in the scene
    bool m_auto    = true;   // rebuild the live code on every edit
    bool m_pending = false;  // an edit waiting for the rebuild
    char m_text[4096] = {};
    WifiForm m_wifi;
};

} // namespace qrui
