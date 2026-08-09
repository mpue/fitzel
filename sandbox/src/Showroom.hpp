#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <imgui.h>

namespace fitzel { class AssetDatabase; class Camera; class Texture; }
struct Entity;

// The showroom: the start screen a race is launched from.
//
// A scene becomes a showroom by carrying a ShowroomComponent (see Component.hpp).
// While one is playing, this module takes the scene over: it arranges the scene's
// gliders on the podium, orbits the camera around whichever is chosen, draws the
// craft + circuit pickers over the rendered image, and reports back which
// combination the player launched. It never loads anything itself -- the caller
// owns scenes, prefabs and the race, and this module only answers "this craft, on
// that circuit, now".
//
// The catalog is built from the scene, so authoring a start screen is placing
// objects and filling in fields -- there is nothing to drag into position, which
// is deliberate (see the tremor-friendly editor goal).
namespace showroom {

// Menu input, edge-detected by the caller (which owns the keyboard and gamepad,
// exactly as it does for the scene UI overlay and the end-of-race question). The
// mouse is handled inside, where the rectangles are.
struct Input {
    bool left = false, right = false, up = false, down = false;
    bool confirm = false;   // Enter / Space / pad A
    bool back    = false;   // Esc / pad B
};

// What the showroom decided this frame. `start` is the launch: the caller loads
// `scene` and flies the craft rooted at `craftId`. `back` is the way out (quit in
// the player, stop Play in the editor).
struct Launch {
    bool        start   = false;
    bool        back    = false;
    std::string scene;            // circuit scene stem
    int         craftId = -1;     // entity id of the chosen craft (its subtree)
    std::string craftName;
    int         laps    = 0;      // laps the circuit asks for (0 = the scene's own)
};

// One sound the showroom wants played: a Sound-asset filename plus its gain,
// drained by the caller (which owns the mixer) after update().
struct Cue {
    std::string sound;
    float       gain = 1.0f;
    float       pitch = 1.0f;
};

class Showroom {
public:
    // Does this scene want to be a showroom? (an active ShowroomComponent).
    static bool isShowroomScene(const std::vector<Entity>& entities);

    // Take the scene over. `fallbackScenes` are the project's other scene stems,
    // used as the circuit list when the scene authors no Track Entry -- so the
    // picker is never empty. Snapshots every craft's pose AND the camera, so
    // end() can put both back exactly as they were.
    void begin(const std::vector<Entity>& entities,
               const std::vector<std::string>& fallbackScenes,
               const fitzel::Camera& camera);
    // Hand the scene and the camera back: craft poses, active flags, and the
    // camera's own settings restored. Safe to call when not active.
    //
    // The camera matters as much as the poses here. A race drives position, yaw
    // and pitch every frame, but nothing downstream ever writes the FOV -- so a
    // showroom that walked off with a 46-degree lens (or mid-launch, a 72-degree
    // one) would hand the race a camera that is silently wrong for the rest of
    // the session, and Play's own snapshot would then carry it back into the
    // editor. The showroom BORROWS the camera; it does not get to keep it.
    //
    // Call before reading the chosen craft out of the scene -- a craft that is
    // off stage is deactivated, and a prefab built from it would carry that.
    void end(std::vector<Entity>& entities, fitzel::Camera& camera);
    bool active() const { return m_active; }

    // Pose the craft, drive the camera, advance the effect clocks. Runs before
    // the frame is rendered.
    void update(std::vector<Entity>& entities, fitzel::Camera& camera,
                float dt, const Input& in);

    // Draw the whole screen into `dl`, anchored to the rendered viewport rect,
    // and hit-test the mouse. `viewProj` is the frame's camera matrix -- the
    // podium is projected through it so the stage effects sit on the craft rather
    // than on a guessed screen point.
    Launch draw(ImDrawList* dl, const ImVec2& vmin, const ImVec2& vsize,
                fitzel::AssetDatabase& assets, const glm::mat4& viewProj);

    // Sound cues raised since the last call (moves, selections, the launch).
    std::vector<Cue> takeCues() { return std::move(m_cues); }

private:
    // One selectable craft, flattened out of the scene at begin().
    struct Craft {
        int         id = -1;              // entity id (the subtree's root)
        std::string title, team, blurb;
        glm::vec3   accent{0.38f, 0.87f, 1.0f};
        float       order = 0.0f;
        // Normalised 0..1 bars, derived from the GliderComponent so a craft's
        // card always tells the truth about how it flies.
        float speed = 0.0f, accel = 0.0f, grip = 0.0f, agility = 0.0f;
        float topSpeedMps = 0.0f;
        // The pose the scene had it in, restored by end().
        glm::vec3 homeLocal{0.0f}, homeRot{0.0f}, homeWorld{0.0f};
        bool      homeActive = true;
        bool      isRoot = true;
    };
    struct Track {
        std::string scene, title, blurb, image;
        int   laps = 0;
        float lengthKm = 0.0f, difficulty = 3.0f, order = 0.0f;
    };

    // Which row the keyboard is on. Left/right moves within a row, up/down
    // between rows -- two axes, no modifier keys, nothing small to hit.
    enum class Row { Craft = 0, Track = 1, Start = 2 };

    void moveCraft(int dir);
    void moveTrack(int dir);
    void cue(const std::string& snd, float pitch);
    glm::vec3 accent() const;
    // Resolve a preview image, holding the handle so the GL texture outlives the
    // draw list ImGui renders at frame end.
    fitzel::Texture* preview(fitzel::AssetDatabase& assets, const std::string& path);

    bool  m_active = false;
    int   m_podiumId = -1;
    glm::vec3 m_podium{0.0f};
    // The authored stage settings, copied at begin() so update()/draw() never
    // have to go looking for the component again.
    std::string m_title, m_subtitle;
    std::string m_sndMove, m_sndSelect, m_sndStart;
    float m_sndGain = 1.0f;
    float m_ringRadius = 0.0f, m_riseHeight = 1.6f, m_spinSpeed = 16.0f, m_bob = 0.18f;
    float m_camDist = 9.0f, m_camHeight = 2.6f, m_camPitch = -8.0f;
    float m_camOrbit = 6.0f, m_camFov = 48.0f;
    glm::vec3 m_accent{0.38f, 0.87f, 1.0f};

    // The camera as it stood when the showroom took it over, restored by end().
    glm::vec3 m_camHome{0.0f};
    float     m_camHomeYaw = 0.0f, m_camHomePitch = 0.0f, m_camHomeFov = 60.0f;

    std::vector<Craft> m_craft;
    std::vector<Track> m_tracks;
    int  m_craftSel = 0, m_trackSel = 0;
    int  m_prevCraft = -1;       // craft leaving the stage during a swap
    Row  m_row = Row::Craft;

    // --- Effect clocks ------------------------------------------------------
    float m_time    = 0.0f;   // seconds the showroom has been up (idle animation)
    float m_intro   = 0.0f;   // 0..1 entry wipe
    float m_swap    = 0.0f;   // 1 -> 0 while a craft changes places
    float m_trackFx = 0.0f;   // 1 -> 0 after a circuit change
    float m_launch  = 0.0f;   // 0 -> 1 once START is hit (the takeoff wipe)
    bool  m_launching = false;
    bool  m_wantBack  = false; // Esc / pad B, reported by the next draw()
    float m_orbit   = 0.0f;   // camera azimuth (deg)
    float m_orbitVel = 0.0f;  // the swing a craft change gives the camera
    float m_spin    = 0.0f;   // turntable angle of the craft on stage (deg)
    float m_ringPos = 0.0f;   // carousel: eased craft index the ring is turned to
    float m_stripPos = 0.0f;  // circuit strip: eased card index at the centre
    // Bars ease toward the chosen craft's numbers instead of snapping, so the
    // difference between two craft is something you watch happen.
    float m_bar[4]{0.0f, 0.0f, 0.0f, 0.0f};

    std::vector<Cue> m_cues;
    std::unordered_map<std::string, std::shared_ptr<fitzel::Texture>> m_texCache;
};

} // namespace showroom
