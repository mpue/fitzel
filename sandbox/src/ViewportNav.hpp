#pragma once

#include <glm/glm.hpp>

namespace fitzel { class Camera; class Input; }

// The editor viewport's navigation that is NOT free flight: the axis-aligned
// standard views (front / back / left / right / top / bottom) and middle-mouse
// panning.
//
// WHY THIS IS ITS OWN FILE. The free camera in main's loop is one branch of a
// chain that already handles the physics car, the arcade car, the glider and the
// first-person walk; adding a second pointing device and six view presets to it
// would have been another eighty lines in the one function that is hardest to
// read. None of this touches the scene either -- it moves an eye, so it can live
// on its own with the camera and a handful of facts about the frame.
//
// WHY NOT ORTHOGRAPHIC. Blender's numpad views are orthographic; these are not.
// The projection is perspective everywhere in the engine -- the shadow cascades,
// the frustum culling, the gizmo scaling and the picking ray all derive from
// Camera::projectionMatrix -- so an ortho editor view is a change to all of
// them, not to this. Looking straight down an axis is the part that makes a
// front view useful for placing things; that part is here.
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

    StdView current()  const { return m_view; }
    bool    gliding()  const { return m_glide; }
    bool    panning()  const { return m_panning; }

    float panSpeed = 1.0f;      // multiplier on the 1:1 drag
    float glideTime = 0.28f;    // seconds for a view change

private:
    // Where the camera swings around this frame.
    glm::vec3 pivot(const fitzel::Camera& cam, const Env& env) const;
    void      finishGlide(fitzel::Camera& cam);

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
