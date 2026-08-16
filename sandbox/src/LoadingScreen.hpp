#pragma once

#include <string>

#include <glm/glm.hpp>

#include <fitzel/graphics/Texture.hpp>

struct ImDrawList;
struct ImVec2;

namespace fitzel { class Gui; class Window; }

// The screen the game shows while it is loading -- at startup, and between
// levels, where the frame would otherwise sit frozen on whatever the driver last
// put in the window.
//
// It is a STYLE plus a painter, and the painter draws into a rectangle rather
// than "the screen". That is what lets the settings dialog preview the real
// thing instead of a description of it: the same code paints the preview box and
// paints the game, so what is configured is what ships.
//
// Everything here is raw draw-list work -- no ImGui windows. A loading screen is
// a picture with a bar on it, and the moment it goes through Begin/End it
// inherits padding, a background and a title bar that all have to be argued back
// out again.
namespace loadingscreen {

// How the background image is fitted to the frame.
enum class Fit {
    Contain = 0, // whole image visible, letterboxed -- never upscaled past 1:1
    Cover   = 1, // fills the frame, overflow cropped
    Stretch = 2, // fills the frame, aspect ignored
};

// Where the progress bar sits.
enum class BarAt { Bottom = 0, Center = 1, Top = 2 };

// The configurable part, saved per project in its game.json and therefore
// exported with it. Every default is today's look, so a project that never opens
// the dialog keeps the screen it already had.
struct Style {
    // Project-relative image file. Empty falls back to the engine splash, and if
    // that is missing too, to a plain `back` fill.
    std::string background;
    Fit         fit  = Fit::Contain;
    glm::vec3   back = {0.04f, 0.05f, 0.06f}; // frame fill / letterbox colour

    // Optional headline over the art. Empty draws nothing.
    std::string title;
    float       titleScale = 2.0f;            // multiples of the UI font size
    glm::vec3   textColor  = {0.90f, 0.93f, 0.96f};

    BarAt     barAt      = BarAt::Bottom;
    float     barInset   = 56.0f;  // px from the anchored edge (ignored: Center)
    float     barWidth   = 0.55f;  // fraction of the frame's width
    float     barHeight  = 14.0f;
    float     barRound   = 7.0f;
    glm::vec3 barColor   = {0.30f, 0.72f, 1.00f};
    glm::vec3 barBack    = {0.10f, 0.12f, 0.16f};

    bool showLabel   = true;  // "Building entities..." over the bar
    bool showPercent = true;  // "42%" at the right end of the label row
    bool showVersion = true;  // engine version, dimmed, under the bar
};

class Screen {
public:
    void setStyle(const Style& s) { m_style = s; }
    const Style& style() const { return m_style; }

    // Folder the style's `background` is relative to -- the project folder
    // ("project" in an exported game). Changing it drops the cached image.
    void setProjectFolder(std::string folder);

    // Paint into `pos`..`pos+size` on `dl`. `label` may be empty.
    void drawInto(ImDrawList* dl, const ImVec2& pos, const ImVec2& size,
                  float progress, const std::string& label);

    // One whole frame of loading screen: pump events, clear, paint over the
    // window, present. This is what a blocking load calls between its steps --
    // the reason a level change no longer shows a frozen white rectangle.
    void frame(fitzel::Window& window, fitzel::Gui& gui, float progress,
               const std::string& label);

    // Drop the background's VRAM. Called once the game is up: a backdrop nobody
    // is looking at for the next twenty minutes is several megabytes of texture
    // doing nothing. The next draw decodes it again, which is a blink at the
    // start of a level change that is about to take seconds anyway.
    void release();

private:
    // The background is decoded once and kept alive in this member for as long
    // as the screen exists: a draw list holds the texture id until the frame it
    // was queued in is rendered, so a temporary would hand ImGui a dead name and
    // draw the font atlas instead.
    void ensureBackground();

    Style          m_style;
    std::string    m_projectFolder;
    fitzel::Texture m_bg;
    std::string    m_bgLoaded;      // path currently decoded into m_bg
    bool           m_bgTried = false;
};

} // namespace loadingscreen
