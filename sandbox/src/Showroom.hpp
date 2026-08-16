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
// that circuit, over this distance, against this field, now".
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
    // The second seat, when the start screen was set to two players: -1 means a
    // single-player race and the caller draws one pane. Both players pick from
    // the same catalogue, so this is another craft out of the same scene -- and
    // it may be the SAME craft as player one's, which is a legitimate choice
    // (two of the same machine) and costs the caller nothing extra.
    int         craftId2 = -1;
    std::string craftName2;
    int         laps    = 0;      // laps the circuit asks for (0 = the scene's own)

    // --- What kind of race this is -----------------------------------------
    // The answers the screen gives about the RACE rather than about the craft.
    // Each one is an override the caller lays over the circuit once it is
    // loaded, and each has a value that means "leave the scene alone" -- so a
    // start screen nobody touched changes nothing, and a circuit stays the
    // session its author built. They are applied AFTER Play has snapshotted the
    // scene, which is what keeps a race run at ELITE against two rivals out of
    // the .fitzel on disk.
    int   mode      = -1;    // -1 = the circuit's own, 0 = race, 1 = time trial
    // -1 = the field as authored, else AT MOST that many rivals: the start
    // screen can only take craft out of the field a circuit was built with, it
    // has no way to add ones the scene does not contain.
    int   opponents = -1;
    float aiSkill   = 1.0f;  // pace multiplier on the field (1 = as authored)
    float aiCatchup = 1.0f;  // rubber-band multiplier (how hard the field holds on)
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
    //
    // The order is the order things are DRAWN in, top-left to bottom-right: the
    // two seats' craft down the left, the circuit across the bottom, the race
    // setup down the right, then START. Walking the rows walks the screen; a nav
    // order that disagreed with the layout would have the eye jumping about to
    // find what the last key press moved.
    //
    // Craft2 is skipped entirely with one player rather than shown disabled, so
    // the screen never asks a question that has no meaning -- the same rule the
    // field and its skill follow in a time trial.
    enum class Row { Craft   = 0, Craft2 = 1, Track = 2,
                     Players = 3, Laps   = 4, Mode  = 5, Field = 6, Skill = 7,
                     Start   = 8 };

    // The seat the craft carousel is currently picking for -- Craft2's row means
    // player two. One carousel, not two: the choice is the same catalogue and the
    // same gesture, and two of them side by side would halve the space a craft
    // has to be looked at in.
    int  seatRow() const { return m_row == Row::Craft2 ? 1 : 0; }
    int& craftSelFor(int seat) { return seat == 1 ? m_craftSel2 : m_craftSel; }
    int  craftSelFor(int seat) const { return seat == 1 ? m_craftSel2 : m_craftSel; }
    // The craft standing on the podium: whichever seat is being chosen for.
    int  stageSel() const { return craftSelFor(seatRow()); }
    // Walk to the next/previous row, skipping what this configuration has no use
    // for (the second seat when there is only one player, the field and its
    // skill in a time trial).
    void moveRow(int dir);
    // Can this row be answered as things stand? An unusable row is stepped over
    // rather than drawn greyed out -- the screen never asks a question that has
    // no meaning, which is the same rule the second seat already followed.
    bool rowUsable(Row r) const;
    // Left/right on one of the race-setup rows. Clamped rather than wrapped: a
    // list of numbers has two ends, and wrapping would turn one step past "20
    // laps" into the circuit's own count.
    void moveSetup(int dir);

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
    // How many seats this race is started with, and player two's pick. Chosen on
    // the screen rather than authored on the component: whether two people are
    // sitting there is not a property of the scene.
    int  m_players  = 1;
    int  m_craftSel2 = 0;
    int  m_prevCraft = -1;       // craft leaving the stage during a swap
    Row  m_row = Row::Players;

    // --- Race setup ---------------------------------------------------------
    // Indices into the choice tables in the .cpp, so the screen holds a pick
    // rather than a value and the tables stay the one place a list is written
    // down. They are NOT reset by begin(): coming back from a race, the screen
    // opens on the same answers it was left with, because somebody who just ran
    // a circuit is most likely about to run it again.
    int  m_lapSel   = 0;   // 0 = the circuit's own lap count
    int  m_modeSel  = 0;   // 0 = race, 1 = time trial
    int  m_fieldSel = 0;   // 0 = the field as authored
    int  m_skillSel = 1;   // PRO: the field exactly as it was tuned
    bool m_hadSession = false;  // begin() has run before (the picks above stand)

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
