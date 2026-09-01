#pragma once

#include <functional>

#include <glm/glm.hpp>
#include <imgui.h>

class VehicleComponent;

// The vehicle setup's viewport half: the car's tuning geometry drawn where it
// actually is, with a handle on each number that has a place in space.
//
// Its reason for existing is that half of VehicleComponent describes a shape --
// where the axles are, how wide the track is, how big the wheels are, where the
// collision box sits and how far the centre of mass is dropped inside it -- and
// none of that is visible anywhere. Typing 1.35 into "Front axle Z" and looking
// at the car tells you nothing, because the box is invisible, the wheel circles
// are invisible, and the centre of mass is a number about a body you cannot see.
// So the setup was guesswork: change a value, drive, feel that it is wrong,
// guess again.
//
// Two things are worth knowing about what it draws. The collision box is NOT the
// model's bounding box -- it rides a suspension travel above the wheels, which is
// why a car whose box looks too high is usually correct and a car whose box
// swallows its wheels is not. And the centre of mass marker is the anti-rollover
// lever: `comLower` slides it down toward the wheel line, and seeing it sit up
// under the roof explains a car that trips over itself in every corner.
//
// Tremor-friendly, like the road and water tools: a generous pixel grab radius,
// one axis per handle so a shaky drag cannot move two numbers at once, the
// selected handle stays selected and nudges in whole steps with the arrow keys,
// and every handle prints its value next to itself so the answer is on screen
// rather than in the panel.
namespace vehiclegizmo {

// The handles, in the order they are hit-tested. Also the nudge target.
enum Handle {
    kNone = -1,
    kTrack = 0,   // half the left-right wheel distance
    kFrontZ,      // front axle position
    kRearZ,       // rear axle position
    kWheelY,      // wheel centre height
    kRadius,      // wheel radius
    kChassisX,    // collision box half width
    kChassisY,    // collision box half height  (the roof: size)
    kChassisZ,    // collision box half length
    kChassisPos,  // how high the box rides on its springs (the floor: position)
    kCom,         // centre-of-mass drop (comLower)
    kCount
};

struct Context {
    VehicleComponent& vc;
    glm::mat4 world{1.0f}; // the vehicle root's world transform (no scale)

    // Selection + drag state, owned by the caller so it survives the frame.
    int&  sel;      // Handle currently selected, kNone for none
    bool& dragging;

    // False draws the geometry and nothing else -- no picking, no handles. This
    // is the state a vehicle is in merely by being selected: seeing the shape
    // costs nothing and is most of the value, while taking the mouse away from
    // the transform gizmo has to be asked for.
    bool editable = false;

    // The viewport, as the editor already has it.
    ImVec2    origin{0.0f, 0.0f};
    float     viewW = 1.0f, viewH = 1.0f;
    glm::mat4 viewProj{1.0f};
    bool      hovered = false;
    glm::vec2 mouseNdc{0.0f};
    ImVec2    mousePos{0.0f, 0.0f};
    glm::vec3 cameraPos{0.0f};

    // Where the collision box centre sits in the model's local frame. Injected
    // rather than recomputed here because main owns that relation (it is what
    // places the Jolt body at Play), and a gizmo drawing the box a centimetre
    // from where physics puts it would be worse than drawing no box at all.
    std::function<float(const VehicleComponent&)> boxCenterY;

    // Undo bracket, same contract as the road/water tools: open on grab, commit
    // on release. Also used by the keyboard nudge, which opens on the first key
    // and commits when the last one comes up.
    std::function<void()>            beginEdit;
    std::function<void(const char*)> endEdit;
    std::function<bool()>            editOpen;
};

// Run one frame: draw the setup geometry, and (when `editable`) pick, drag and
// nudge its handles. Returns true while a handle is grabbed, so the caller can
// keep the transform gizmo off the same mouse button.
bool handle(const Context& c);

} // namespace vehiclegizmo
