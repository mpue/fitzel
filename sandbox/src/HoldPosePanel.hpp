#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include <fitzel/world/Model.hpp>

#include "HoldPose.hpp"
#include "Retarget.hpp"
#include "RetargetPreview.hpp"

// The editor's "Hold Pose Studio" (Assets menu): first-person arms on a weapon,
// put right by eye instead of by numbers in a Blender script.
//
//   * pick the arms (a rigged .glb with "hold_<weapon>" clips) and a pose,
//   * see them as the game shows them -- from the player's eye, at the game's
//     field of view -- or from any side, the weapon see-through if a hand is
//     buried in it,
//   * move and turn each hand on the weapon with a gizmo on it (along the
//     weapon's axes or the hand's own), or in steps (a millimetre to two
//     centimetres, a degree to fifteen), or click the weapon where it should
//     go; swing an elbow out or in; curl each finger; move the shoulders,
//   * write it: the pose into the arms' clip, their place into the game's Lua
//     table (holdpose::writePlacement), the weapon's setup into the recipe
//     beside the model (<arms>.holdposes).
//
// The arms follow the hands (two-bone IK, HoldPose.hpp). Dragging the gizmo
// is offered, never required (see parkinson-tauglicher Editor): the studio
// turns by buttons, every amount is also a stepper, a hand is placed by one
// click on the weapon. Owns its state and its actions, so main only constructs it and
// calls panel().
namespace holdposeui {

class HoldPoseTool {
public:
    struct Deps {
        const std::string&           currentProject;  // the open .fitzel ("" = none)
        std::string&                 status;          // the editor's footer line
        std::function<std::string()> selectedModel;   // the selected entity's model file ("" = none)
    };
    explicit HoldPoseTool(Deps d);
    ~HoldPoseTool();
    HoldPoseTool(const HoldPoseTool&)            = delete;
    HoldPoseTool& operator=(const HoldPoseTool&) = delete;

    void panel(bool& show);

    // Open these arms (an absolute path to their .glb), and a pose of theirs.
    void openArms(const std::string& model);
    void selectPose(const std::string& name);
    // For holdposepanelcheck: the view (0 eye, 1 left, 2 right, 3 top, 4 below,
    // 5 front, 6 back), the weapon see-through, a hand nudged (metres, weapon
    // frame), and whether nothing is loading any more.
    void setView(int v);
    void setXray(bool on) { m_xrayGun = on; m_redraw = true; }
    void setGizmo(int op) { m_gizmoOp = op; }
    // Where the gizmo was drawn last (screen pixels; false if it was not), and
    // where a hand's wrist is meant to be (weapon frame) -- for the check's drag.
    bool gizmoScreen(float& x, float& y) const { x = m_gizmoAt.x; y = m_gizmoAt.y; return m_gizmoShown; }
    glm::vec3 handPos(int side) const { return m_edit.hands[static_cast<std::size_t>(side & 1)].pos; }
    // Move / turn the arms' model on the weapon (as the Arms tab does), and every
    // joint where the solve puts it (view space) -- for the check's round trip.
    void moveArms(const glm::vec3& d) { change(); m_edit.place.t += d; }
    void turnArmsBy(const glm::vec3& axis, float deg) { turnArms(axis, deg); }
    std::vector<glm::vec3> viewJoints();
    void nudge(int side, const glm::vec3& d);
    bool idle() const;
    void setFirstSize(float w, float h) { m_firstW = w; m_firstH = h; }
    // Write the pose (as the button does); false with the reason in `err`.
    bool write(std::string& err);

private:
    using ModelPtr = std::shared_ptr<fitzel::ModelData>;
    struct ArmsLoad {
        ModelPtr                        model;
        std::shared_ptr<retarget::Rig>  rig;
        std::string                     error;
    };

    void poll();
    void drawTopBar();
    void drawStudio(float w, float h);
    void drawViewButtons();
    void drawHandsTab();
    void drawArmsTab();
    void drawWeaponTab();
    void drawWriteBar();

    void scanModels();
    std::vector<std::string> poseNames() const;
    void loadGun(const std::string& file);
    void startPose();                 // base pose, placement and hands for m_pose
    void change();                    // before an edit: undo point, redraw
    void undo();
    void moveHand(int side, const glm::vec3& d);
    void turnHand(int side, const glm::vec3& axis, float deg);
    void turnArms(const glm::vec3& axis, float deg);
    bool placeHandAt(float px, float py);   // studio pixels; true if the weapon was hit
    // The gizmo on the chosen hand, over the studio picture at (x, y, w, h).
    // True while the mouse is on it or dragging it (the click is the gizmo's).
    bool drawGizmo(float x, float y, float w, float h);
    void solve();
    glm::mat4 weaponM() const;
    glm::mat4 armsM() const;
    std::string projectDir() const;
    std::string holdsFile() const;

    Deps                       m_d;
    // The arms.
    std::string                m_armsPath;
    std::future<ArmsLoad>      m_armsJob;
    ModelPtr                   m_arms;
    std::shared_ptr<retarget::Rig> m_rig;
    holdpose::Poser            m_poser;
    glm::vec3                  m_armsCentre{0.0f};
    holdpose::Recipe           m_recipe;
    holdpose::HoldsInfo        m_info;
    std::string                m_infoError;
    // The pose.
    std::string                m_pose;
    holdpose::Weapon           m_weapon;
    std::string                m_gunPath;
    std::future<ModelPtr>      m_gunJob;
    std::string                m_gunJobPath;
    ModelPtr                   m_gun;
    glm::vec3                  m_gunCentre{0.0f};
    std::vector<glm::mat4>     m_base, m_G;
    holdpose::Edit             m_edit;
    std::vector<holdpose::Edit> m_undo;
    bool                       m_dirty = false, m_weaponDirty = false;
    bool                       m_havePlacement = false;
    float                      m_short[2] = {0.0f, 0.0f};
    bool                       m_wantPose = false;   // startPose once the models are in
    std::string                m_wantName;

    // The studio.
    RetargetPreview            m_studio;
    bool                       m_studioReady = false, m_studioOk = false;
    const fitzel::ModelData*   m_shown[2] = {nullptr, nullptr};
    std::uint32_t              m_tex = 0;
    bool                       m_redraw = true;
    int                        m_view = 0;           // 0 eye, else turned round the weapon
    float                      m_yaw = 0.0f, m_pitch = 10.0f, m_dist = 0.9f, m_eyeZoom = 1.0f;
    bool                       m_xrayGun = false, m_xrayArms = false, m_markers = true;
    int                        m_side = 1;           // the hand being edited (0 left, 1 right)
    bool                       m_placing = false;    // the next click on the weapon places the hand
    int                        m_moveStep = 2, m_turnStep = 1;
    int                        m_gizmoOp = 1;        // 0 none, 1 move, 2 turn
    bool                       m_gizmoHandAxes = false; // along the hand's own axes (else the weapon's)
    bool                       m_gizmoDrag = false;  // a drag under way (one undo step for it)
    bool                       m_gizmoShown = false;
    glm::vec2                  m_gizmoAt{0.0f};

    // Lists.
    struct ModelEntry { std::string path, name; bool skinned = false; };
    std::vector<ModelEntry>    m_models;
    std::string                m_modelsFor = "\x01";
    char                       m_newName[48] = {};
    float                      m_firstW = 1280.0f, m_firstH = 860.0f;
    std::string                m_message;
    bool                       m_messageBad = false;
};

} // namespace holdposeui
