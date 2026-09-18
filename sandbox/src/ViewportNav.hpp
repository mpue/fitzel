#pragma once

#include <glm/glm.hpp>

namespace fitzel { class Camera; class Input; }

// The editor viewport's navigation that is NOT free flight: the axis-aligned
// standard views (front / back / left / right / top / bottom), middle-mouse
// panning and the mouse wheel (dolly, and the field of view with Ctrl).
//
// WHY THIS IS ITS OWN FILE. The free camera in main's loop is one branch of a
// chain that already handles the physics car, the arcade car, the glider and the
// first-person walk; adding a second pointing device and six view presets to it
// would have been another eighty lines in the one function that is hardest to
// read. None of this touches the scene either -- it moves an eye, so it can live
// on its own with the camera and a handful of facts about the frame.
//
// ORTHOGRAPHIC is a switch of its own (Num 5, or View > Viewpoint), not a side
// effect of the standard views: a top view in perspective is still the better
// one for judging how high something stands, and which of the two is wanted is
// the user's call. This module only holds the wish; main hands it to the Camera
// while the editor's free camera is the one in use, so play, the drive modes
// and a camera preview always look through a real lens. In ortho the eye stays
// where it was -- it still decides what the near plane cuts away -- and the
// wheel zooms instead of walking (see update()).
namespace viewnav {

// Where the camera is looking, when it is looking along an axis at all. `User`
// is "anywhere else" -- the moment the view is swung by hand it stops being a
// standard view, and saying so is the point of keeping this.
enum class StdView { User, Front, Back, Left, Right, Top, Bottom };

// "Front", "Top", ... -- nullptr for User, which has no name worth showing.
const char* label(StdView v);

// What the frame knows and this module does not. Filled in by main once a frame.
struct Env {
    bool  viewportHovered = false; // pointer over the scene viewport
    bool  keysFree        = true;  // no text field / running game owns the keyboard
    bool  looking         = false; // right-mouse fly is active this frame
    bool  numberRow       = true;  // 1/3/5/7 on the number row too (off while
                                   // modelling: there they pick corner/edge/face)
    float viewportH       = 1.0f;  // scene viewport height in pixels

    // What the view swings around. A selection is the obvious answer; without
    // one the ground under the view is a far better guess than a fixed radius,
    // because "how far away is what I am looking at" is the whole question.
    bool      haveSelection = false;
    glm::vec3 selectionCenter{0.0f};
    float     groundY       = 0.0f; // terrain height under the camera
};

class Nav {
public:
    // Call once a frame from the editor's free-camera branch, before the frame
    // is drawn. Reads the numpad, applies a pan, and advances a view change in
    // flight.
    void update(fitzel::Camera& cam, const fitzel::Input& in, const Env& env, float dt);

    // Swing the camera onto a standard view, gliding rather than cutting.
    //
    // The GLIDE IS NOT DECORATION. A cut to another axis leaves you working out
    // which way the scene turned; a quarter-second sweep shows it, and that is
    // the difference between a view button you trust and one you press twice to
    // check. It is also why this drives yaw/pitch rather than Camera::setBasis:
    // the basis override is dropped by the next mouse movement, so a camera
    // snapped that way would spring back the moment you looked around.
    void snapTo(StdView v, const fitzel::Camera& cam, const glm::vec3& pivot);

    // View > Viewpoint. Records what was clicked; update() runs it on the next
    // frame (the menu is drawn long after the camera has been moved this one).
    // Reachable without the keyboard on purpose -- see the editor's aim of not
    // requiring a steady hand or a full-size keyboard for anything.
    void drawMenu();

    // The orthographic wish, and the way to change it. A request lands on the
    // next update(), which knows the pivot the new lens has to match.
    bool ortho() const { return m_ortho; }
    void requestOrthoToggle() { m_orthoRequest = true; }

    StdView current()  const { return m_view; }
    bool    gliding()  const { return m_glide; }
    bool    panning()  const { return m_panning; }

    float panSpeed = 1.0f;      // multiplier on the 1:1 drag
    float glideTime = 0.28f;    // seconds for a view change

    // A wheel notch moves the camera a FRACTION OF THE WAY to what it is
    // pointed at, not a fixed number of metres. That is the whole of "precise":
    // stepping 12% of the remaining distance is a stride when the target is a
    // hill on the horizon and a nudge when it is a kerb under the nose, and the
    // approach never overshoots -- which is the point for an editor meant to be
    // driven without a steady hand. A fixed step can only be right at one
    // distance and is wrong (unusably slow, or straight through the ground) at
    // every other.
    float dollyStep = 0.12f;    // fraction of the distance to the pivot, per notch
    float fovStep   = 2.0f;     // degrees of field of view per notch, with Ctrl

private:
    // Where the camera swings around this frame. `aimed` reports whether that
    // is a real thing out there (a selection, or ground the view actually meets)
    // rather than the fallback distance -- the dolly must not brake in front of
    // a pivot that was only ever a guess.
    glm::vec3 pivot(const fitzel::Camera& cam, const Env& env,
                    bool* aimed = nullptr) const;
    void      finishGlide(fitzel::Camera& cam);

    // Switch the lens over at the pivot's distance, so what sits there keeps its
    // size on screen: a toggle that also rescaled the picture would be two
    // changes for the price of one keypress.
    void      toggleOrtho(fitzel::Camera& cam, const glm::vec3& piv);

    bool m_ortho        = false;
    bool m_orthoRequest = false;
    bool m_prevOrthoKey = false;

    StdView m_view    = StdView::User;
    StdView m_request = StdView::User;  // pending menu click

    bool  m_glide = false;
    float m_t     = 0.0f;
    glm::vec3 m_fromPos{0.0f}, m_toPos{0.0f};
    float m_fromYaw = 0.0f, m_toYaw = 0.0f;
    float m_fromPitch = 0.0f, m_toPitch = 0.0f;

    bool m_panning = false;     // latched: a drag that started in the viewport
                                // keeps panning when it leaves it
    bool m_prevKey[12] = {false};
};

} // namespace viewnav
