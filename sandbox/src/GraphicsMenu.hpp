#pragma once

#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

namespace fitzel { class Renderer; }

// The in-game graphics menu: the screen a player uses to fit the game to the
// machine it is running on.
//
// It is deliberately NOT the editor's rendering panel with a different skin. The
// editor's sliders are art direction -- fog density, exposure, bloom threshold --
// and belong to the scene its author built. What lives here is only the other
// axis: how much work the same picture costs. So nothing in here overrides an
// authored value; the settings either scale a budget (view distance, shadow
// resolution, probe size) or GATE an effect off at the point it is consumed,
// which is why turning bloom off and back on returns the scene's own bloom
// rather than some default.
//
// Every row is a short list of named steps, never a continuous slider, and every
// row is one wide target that can be hit anywhere along it. That is the same rule
// the showroom and the end-of-race question follow (see the tremor-friendly goal):
// nothing has to be dragged, and nothing needs a steady hand to land on.
//
// Drawing is raw ImDrawList work like the rest of the game's screens -- no ImGui
// windows, because a full-screen menu that goes through Begin/End inherits
// padding, a title bar and a background that all have to be argued back out.
namespace gfxmenu {

// The user's choices, as step indices into the option lists in the .cpp. Indices
// rather than values because that is what the screen steps through and what the
// presets set; the values they mean live in one table next to the labels, so a
// row's steps, its labels and its meaning cannot drift apart.
struct Settings {
    int preset       = 2;  // 0 low, 1 medium, 2 high, 3 ultra, 4 custom
    int viewDistance = 3;  // chunk radius: 2, 3, 4, 5, 6, 7, 8, 9
    int shadows      = 2;  // off, 1024, 2048, 4096
    int reflections  = 2;  // off, 128, 256, 512, 1024
    int reflectRate  = 2;  // probe faces per frame: 1, 2, 3, 4, 6
    int vegetation   = 3;  // off, sparse, medium, full
    int cityDetail   = 1;  // near, balanced, far
    int ao           = 1;  // off, on
    int bloom        = 1;  // off, on
    int dof          = 1;  // off, on
    int motionBlur   = 2;  // off, half, full
    int aa           = 1;  // off, FXAA
    int vsync        = 1;  // off, on

    bool operator==(const Settings& o) const;
    bool operator!=(const Settings& o) const { return !(*this == o); }
};

// Overwrite every row with a preset (0..3). Out-of-range does nothing, which is
// what makes "custom" a preset index that simply changes nothing.
void applyPreset(Settings& s, int preset);
// Set `preset` to whichever preset the current rows match exactly, or to custom
// (4) if they match none. Called after every edit so the header tells the truth.
void refreshPresetLabel(Settings& s);

// Read/write the player's own choices. This is a per-MACHINE file, not part of
// the project: it says what this PC can manage, so it is never exported, never
// version-controlled and never carried to another machine with the game.
Settings load(const std::string& file);
void     save(const std::string& file, const Settings& s);

// --- Applying ---------------------------------------------------------------
// What the settings drive. The caller passes pointers to the state it already
// owns rather than this module reaching for globals, exactly as UiActionSink
// does -- which is also what keeps it testable and keeps main.cpp from growing
// another subsystem.
struct Targets {
    int*   viewRadius     = nullptr;  // terrain streaming radius, in chunks
    int*   envProbeRes    = nullptr;  // mirrored copy of the probe's face size
    int*   envProbeFaces  = nullptr;  // probe faces refreshed per frame
    bool*  fxaa           = nullptr;
    bool*  grassEnabled   = nullptr;
    bool*  flowerEnabled  = nullptr;
    float* grassDensity   = nullptr;
    float* grassRadius    = nullptr;
    float* flowerDensity  = nullptr;
    float* cityMinPixels  = nullptr;  // RoadSystem's screen-size cull, in px
    // Re-seeding the grass is not free, so it is only asked for when a value
    // that changes the layout actually moved -- hence a callback rather than a
    // flag the caller has to remember to act on.
    std::function<void()> regrowVegetation;
    // VSync is the window's, not the renderer's.
    std::function<void(bool)> setVSync;
};

// Push the settings onto the world. Safe to call every frame (it is a handful of
// assignments and two setters that no-op when nothing changed), but `regrow` is
// only invoked when the vegetation row moved -- pass the PREVIOUS settings so
// that can be told apart. Pass the same object for both to force everything.
void apply(const Settings& s, const Settings& prev, fitzel::Renderer& renderer,
           const Targets& t);

// Gate the post-processing effects the menu can switch off. Called where the
// frame's post parameters are assembled, so the authored values stay untouched
// and switching an effect back on restores the scene's own look.
struct PostGate {
    float ssaoStrength   = 0.0f;
    float bloomIntensity = 0.0f;
    float rayIntensity   = 0.0f;
    float dofMax         = 0.0f;
    float blurStrength   = 0.0f;
};
PostGate gatePost(const Settings& s, float ssaoStrength, float bloomIntensity,
                  float rayIntensity, float dofMax, float blurStrength);

// --- The screen -------------------------------------------------------------

// Menu input, edge-detected by the caller (which owns the keyboard and the pad,
// exactly as the showroom and the end-of-race question have it). The mouse is
// handled inside, where the rectangles are.
struct Input {
    bool up = false, down = false, left = false, right = false;
    bool confirm = false;   // Enter / pad A -- steps the focused row forward
    bool back    = false;   // Esc / pad B
};

class Menu {
public:
    bool open() const { return m_open; }
    // Opening resets the intro animation and puts the focus on the first row, so
    // the screen always comes up in the same state however it was left.
    void setOpen(bool v);

    // Advance the animation and the frame-time history. `dt` is real seconds --
    // this screen keeps running while the game behind it is paused.
    void update(float dt);

    // Draw into `dl` over the rendered viewport rect. Returns true while the menu
    // should stay open; false the frame it asks to close. Edits `s` in place, and
    // sets `changed` when a row moved this frame (the caller re-applies then, so
    // every change is visible behind the menu immediately).
    bool draw(ImDrawList* dl, const ImVec2& vmin, const ImVec2& vsize,
              Settings& s, const Input& in, bool* changed);

private:
    bool  m_open  = false;
    int   m_focus = 0;      // focused row
    float m_intro = 0.0f;   // 0..1 open animation
    float m_time  = 0.0f;   // seconds open, for the idle pulse
    // Frame times, newest last, so the screen can show what a change did instead
    // of asking the player to believe it.
    std::vector<float> m_frames;
    float m_fpsShown = 0.0f; // smoothed, so the figure is readable
};

} // namespace gfxmenu
