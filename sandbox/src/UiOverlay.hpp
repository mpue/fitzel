#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <fitzel/asset/AssetId.hpp>

namespace fitzel { class AssetDatabase; class Texture; }
struct ImDrawList;

// A scene's 2D UI overlay: an authored list of screen-space elements (text
// labels, clickable buttons, images) drawn on top of the 3D view while playing.
// Everything renders through ImGui draw lists (like the rest of the HUD), so this
// module needs no renderer of its own -- only a draw list plus the viewport rect.
//
// Layout is resolution-independent and tremor-friendly: each element hangs off a
// 9-point anchor with a pixel offset expressed at a reference height
// (UiOverlay::kRefHeight) and scaled by the actual viewport height. Nothing has to
// be dragged into place -- the editor edits anchor + numeric offset -- which is
// the whole point (see the Parkinson-tauglich editor goal).

// Where an element hangs off the viewport rect (which corner/edge/centre it is
// measured from). The pixel offset is then applied inward from that anchor.
enum class UiAnchor {
    TopLeft, TopCenter, TopRight,
    MidLeft, Center,    MidRight,
    BotLeft, BotCenter, BotRight,
};

enum class UiElementType { Text, Button, Image };

// What a button does when clicked. Data-authored (no scripting), mirroring the
// Trigger/SceneTrigger philosophy: the designer picks an action + a parameter.
enum class UiActionKind {
    None,        // decorative button, does nothing
    LoadScene,   // load another scene of the project (str = scene stem)
    ShowMessage, // set the HUD message line (str = message)
    AddScore,    // add num to the score
    PlaySound,   // play a one-shot sound (str = file under sounds/)
    Quit,        // quit the game / leave play
    Resume,      // close a menu overlay and hand control back to the game
    Restart,     // restart the level from how it stood when Play began
    Graphics,    // open the in-game graphics menu (see GraphicsMenu.hpp)
};

// One overlay element. A flat struct (like the scene's other data-authored
// pieces) so it copies cheaply -- which is what lets the whole list be
// snapshotted for undo (see UiOverlayCmd).
struct UiElement {
    UiElementType type    = UiElementType::Text;
    std::string   name    = "Element";   // editor label only (not drawn)
    bool          visible = true;

    UiAnchor  anchor = UiAnchor::TopLeft;
    glm::vec2 offset{40.0f, 40.0f};      // px from the anchor, at kRefHeight
    glm::vec2 size{220.0f, 64.0f};       // px box (Button/Image; Text auto-sizes)

    // Text / Button label.
    std::string text      = "Text";
    float       fontScale = 1.0f;                     // multiple of the base HUD font
    glm::vec4   color{1.0f, 1.0f, 1.0f, 1.0f};        // text colour (RGBA 0..1)
    glm::vec4   bgColor{0.12f, 0.13f, 0.16f, 0.90f};  // button background (RGBA 0..1)

    // Image / Button icon (a Texture asset; invalid = none).
    fitzel::AssetId image;

    // Button action (data-authored).
    UiActionKind action    = UiActionKind::None;
    std::string  actionStr;               // scene / message / sound, per action
    float        actionNum = 10.0f;       // AddScore amount

    bool operator==(const UiElement& o) const;
    bool operator!=(const UiElement& o) const { return !(*this == o); }
};

// The side effects a clicked button can request. main supplies concrete
// callbacks, so this module never touches the game's globals (window, host, ...).
struct UiActionSink {
    std::function<void(const std::string&)> loadScene;   // deferred scene load
    std::function<void(const std::string&)> showMessage; // set the HUD line
    std::function<void(float)>              addScore;
    std::function<void(const std::string&)> playSound;
    std::function<void()>                   quit;
    std::function<void()>                   resume;   // close the menu
    std::function<void()>                   restart;  // replay the level
    // Open the graphics menu. A pause menu is where a player looks for it, and
    // the alternative -- telling them about a function key -- is not a way to
    // ship a setting the game may be unplayable without.
    std::function<void()>                   graphics;
};

class UiOverlay {
public:
    // Offsets/sizes are authored as pixels at this reference viewport height and
    // scaled by (actual height / kRefHeight), so a layout holds across resolutions.
    static constexpr float kRefHeight = 1080.0f;

    // GLFW_KEY_ESCAPE, spelled out so this header stays free of GLFW.
    static constexpr int kKeyEscape = 256;

    std::vector<UiElement>&       elements()       { return m_elements; }
    const std::vector<UiElement>& elements() const { return m_elements; }
    void setElements(std::vector<UiElement> v)     { m_elements = std::move(v); }
    bool empty() const                             { return m_elements.empty(); }

    // Menu mode. A plain overlay is a HUD: always on screen while playing. A
    // *menu* overlay starts hidden and is opened/closed with `toggleKey` (a GLFW
    // key code, Escape by default). While it is open the game hands it the mouse
    // and takes no gameplay input -- and with Escape as the key, Escape no longer
    // quits: the way out of the game is a button with the Quit action.
    bool menuMode() const          { return m_menuMode; }
    void setMenuMode(bool v)       { m_menuMode = v; resetRuntime(); }
    int  toggleKey() const         { return m_toggleKey; }
    void setToggleKey(int k)       { m_toggleKey = k; }

    // Runtime visibility, honoured by drawRuntime. resetRuntime() puts it back to
    // the authored default (menus closed, HUDs shown) and runs whenever play
    // starts or a scene loads.
    bool runtimeVisible() const    { return m_runtimeVisible; }
    // Showing an overlay also puts the focus back on its first button, so a menu
    // always opens on a known entry rather than wherever it was left.
    void setRuntimeVisible(bool v);
    void resetRuntime();

    // --- Keyboard / gamepad navigation ------------------------------------
    // The overlay keeps a focused button, drawn highlighted, so a menu is usable
    // without a mouse -- which matters because Play locks the cursor, and a
    // gamepad has none to begin with. The mouse still works and simply moves the
    // focus wherever it hovers, so the two never disagree about what is selected.
    bool anyButtons() const;
    // Move the focus by `dir` (+1 next, -1 previous) in reading order: top to
    // bottom, then left to right. Wraps around. No-op without buttons.
    void moveFocus(int dir);
    // Fire the focused button's action. Returns false if nothing was focused.
    bool activateFocus(const UiActionSink& sink);

    // Persist to / restore from the scene's "settings" object (under "uiOverlay").
    // load() clears the list first, so a scene without the key loads as empty.
    void save(nlohmann::json& settings) const;
    void load(const nlohmann::json& settings);

    // Runtime: draw every visible element into `dl`, anchored to the viewport rect
    // (vmin/vsize in screen pixels). Buttons hit-test the mouse and fire their
    // action through `sink` on click. `assets` resolves image GUIDs to textures.
    void drawRuntime(ImDrawList* dl, const glm::vec2& vmin, const glm::vec2& vsize,
                     fitzel::AssetDatabase& assets, const UiActionSink& sink);

    // Editor: preview the overlay in the viewport while NOT playing, outlining the
    // `selected` element (index, -1 = none) so its placement is visible. Draws the
    // same elements as drawRuntime but never hit-tests or fires actions.
    void drawAuthoring(ImDrawList* dl, const glm::vec2& vmin, const glm::vec2& vsize,
                       fitzel::AssetDatabase& assets, int selected);

#ifndef FITZEL_PLAYER
    // Editor: the "UI Overlay" panel -- the element list plus a property editor for
    // the selected element. Opens its own ImGui window (closed via `*open`). `sel`
    // is the selected element index, kept in range. `scenes`/`sounds` populate the
    // LoadScene / PlaySound action pickers; `assets` backs the image slot. Mutates
    // the element list directly; the caller brackets the frame for undo.
    // `copyToScene` backs the "Copy to scene" button: it writes this overlay into
    // the named scene of the project (this module knows nothing about files) and
    // returns a one-line result shown under the button. Omit it to hide the button.
    void drawEditorPanel(bool* open, int& sel, fitzel::AssetDatabase& assets,
                         const std::vector<std::string>& scenes,
                         const std::vector<std::string>& sounds,
                         const std::function<std::string(const std::string&)>&
                             copyToScene = {});
#endif

private:
    // A button worth focusing: visible, and it actually does something (a
    // decorative one would be a dead stop when cycling with the pad).
    static bool navigable(const UiElement& e);
    void focusFirst();
    // Resolve an image GUID to a live texture, caching the shared handle so the GL
    // texture stays alive until ImGui actually renders the draw list at frame end.
    // (Drawing with a texture whose handle is dropped the same frame leaves ImGui
    // binding a stale id -> it falls back to the font atlas.)
    fitzel::Texture* resolve(fitzel::AssetDatabase& assets, const fitzel::AssetId& id);

    std::vector<UiElement> m_elements;
    std::unordered_map<fitzel::AssetId, std::shared_ptr<fitzel::Texture>> m_texCache;
    std::string m_copyTarget;            // editor: "Copy to scene" target + result
    std::string m_copyStatus;
    // Element index of the focused button (-1 = none). Runtime only.
    int  m_focus = -1;
    // Menu entry animation, 0..1, restarted each time the menu opens. Runtime
    // only, never serialized.
    float m_openT = 0.0f;
    bool m_menuMode      = false;        // authored: HUD (false) or menu (true)
    int  m_toggleKey     = kKeyEscape;   // authored: which key opens a menu
    bool m_runtimeVisible = true;        // runtime only, never serialized
};
