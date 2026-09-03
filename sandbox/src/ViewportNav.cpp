#include "ViewportNav.hpp"

#include <algorithm>
#include <cmath>

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <fitzel/core/Input.hpp>
#include <fitzel/scene/Camera.hpp>

namespace viewnav {

namespace {

// Straight down an axis is a pole for a yaw/pitch camera: at exactly +-90 the
// front vector is parallel to world up, the cross product that makes `right`
// collapses, and Camera keeps whatever right it had -- i.e. the top view would
// come out rotated by however you happened to be facing when you asked for it.
// A tenth of a degree short of the pole is visually straight down and
// arithmetically well behaved.
constexpr float kPolePitch = 89.9f;

// How far ahead the pivot sits when there is nothing better to go on.
constexpr float kDefaultPivotDist = 20.0f;

// The dolly's stride, in metres, whatever the fraction of the distance works out
// to. The floor keeps the last approach to something right in front of the lens
// from stalling into nothing; the ceiling keeps one notch over a whole valley
// from being a teleport you then have to find your way back from.
constexpr float kMinDolly = 0.05f;
constexpr float kMaxDolly = 25.0f;

// How far in front of the pivot the dolly stops. Coming to rest ON the target
// puts it exactly at the near plane -- the thing you were zooming towards
// vanishes and the next notch has you inside it -- so the approach keeps a hand's
// width and you go the rest of the way, if you want to, by flying.
constexpr float kStopShort = 0.35f;

// The camera's yaw/pitch for each standard view. Yaw follows Camera's
// convention (front = cos(yaw)cos(pitch), sin(pitch), sin(yaw)cos(pitch)), so
// -90 looks down -Z, which is where the default camera already points -- the
// front view is the view the editor opens on, and that is not a coincidence:
// -Z forward with Y up is the frame the whole engine is built in.
void yawPitchFor(StdView v, float& yaw, float& pitch) {
    switch (v) {
        case StdView::Front:  yaw = -90.0f; pitch =  0.0f; break;   // eye at +Z
        case StdView::Back:   yaw =  90.0f; pitch =  0.0f; break;   // eye at -Z
        case StdView::Right:  yaw = 180.0f; pitch =  0.0f; break;   // eye at +X
        case StdView::Left:   yaw =   0.0f; pitch =  0.0f; break;   // eye at -X
        // Top and bottom keep a FIXED yaw rather than the one you arrived with,
        // so "top" is one reproducible picture (screen up = world -Z, the way
        // the front view looks) instead of a different one every time.
        case StdView::Top:    yaw = -90.0f; pitch = -kPolePitch; break;
        case StdView::Bottom: yaw = -90.0f; pitch =  kPolePitch; break;
        case StdView::User:   break;
    }
}

glm::vec3 dirFor(StdView v) {
    float yaw = 0.0f, pitch = 0.0f;
    yawPitchFor(v, yaw, pitch);
    const float y = glm::radians(yaw), p = glm::radians(pitch);
    return glm::normalize(glm::vec3(std::cos(y) * std::cos(p),
                                    std::sin(p),
                                    std::sin(y) * std::cos(p)));
}

float smoothstep01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The keys, in the order the m_prevKey slots are indexed. Blender's layout: the
// numpad digit gives the view, Ctrl gives the one from the opposite side. The
// number ROW does the same thing, because half the machines this runs on are
// laptops with no numpad at all, and a view you cannot reach is not a feature.
struct Binding {
    int     key;
    StdView plain;
    StdView withCtrl;
};
constexpr Binding kBindings[] = {
    {GLFW_KEY_KP_1, StdView::Front, StdView::Back},
    {GLFW_KEY_KP_3, StdView::Right, StdView::Left},
    {GLFW_KEY_KP_7, StdView::Top,   StdView::Bottom},
    {GLFW_KEY_1,    StdView::Front, StdView::Back},
    {GLFW_KEY_3,    StdView::Right, StdView::Left},
    {GLFW_KEY_7,    StdView::Top,   StdView::Bottom},
};
constexpr int kBindingCount = static_cast<int>(sizeof(kBindings) / sizeof(kBindings[0]));

} // namespace

const char* label(StdView v) {
    switch (v) {
        case StdView::Front:  return "Front";
        case StdView::Back:   return "Back";
        case StdView::Left:   return "Left";
        case StdView::Right:  return "Right";
        case StdView::Top:    return "Top";
        case StdView::Bottom: return "Bottom";
        case StdView::User:   break;
    }
    return nullptr;
}

glm::vec3 Nav::pivot(const fitzel::Camera& cam, const Env& env, bool* aimed) const {
    if (aimed) *aimed = true;
    if (env.haveSelection) return env.selectionCenter;

    // No selection: take the ground the view is aimed at. That matters more than
    // it sounds -- the pivot fixes both how far the camera swings round on a
    // view change and how fast a pan drags, and a fixed distance gets both wrong
    // by the same factor whenever the camera is somewhere unusual (up over a
    // whole track, or down between two kerbs).
    const glm::vec3 p = cam.position();
    const glm::vec3 f = cam.front();
    float dist = kDefaultPivotDist;
    if (f.y < -0.05f) {                        // looking down at all
        const float t = (env.groundY - p.y) / f.y;
        if (t > 0.5f) dist = std::min(t, 400.0f);
        else if (aimed) *aimed = false;
    } else if (aimed) {
        *aimed = false;                        // level or up: nothing is aimed at
    }
    return p + f * dist;
}

void Nav::snapTo(StdView v, const fitzel::Camera& cam, const glm::vec3& piv) {
    if (v == StdView::User) return;

    // The DISTANCE TO THE PIVOT IS KEPT, only the direction is replaced. Framing
    // is a separate wish (F does it) and folding it in here would mean every
    // view change also silently rescaled the shot.
    const float dist = std::max(glm::length(cam.position() - piv), 1.0f);
    m_fromPos = cam.position();
    m_toPos   = piv - dirFor(v) * dist;

    m_fromYaw   = cam.yaw();
    m_fromPitch = cam.pitch();
    yawPitchFor(v, m_toYaw, m_toPitch);
    // Yaw is an angle, not a number: 350 -> 10 is ten degrees, not three hundred
    // and forty. Lift the target into whichever turn is actually shorter.
    while (m_toYaw - m_fromYaw >  180.0f) m_toYaw -= 360.0f;
    while (m_toYaw - m_fromYaw < -180.0f) m_toYaw += 360.0f;

    m_t     = 0.0f;
    m_glide = true;
    m_view  = v;
}

void Nav::finishGlide(fitzel::Camera& cam) {
    if (!m_glide) return;
    cam.setYaw(m_toYaw);
    cam.setPitch(m_toPitch);
    cam.setPosition(m_toPos);
    m_glide = false;
}

void Nav::update(fitzel::Camera& cam, const fitzel::Input& in, const Env& env, float dt) {
    // Looking around by hand ends the standard view -- both the glide, which is
    // now fighting the mouse over the same two angles, and the claim in the
    // corner of the viewport that this is still the front view.
    if (env.looking) {
        m_glide   = false;
        m_view    = StdView::User;
        m_panning = false;
    }

    // --- Middle mouse: pan -------------------------------------------------
    // Latched on the press. A drag that started over the viewport keeps panning
    // when the pointer wanders onto a docked panel, which is the difference
    // between one pan and a series of them.
    const bool mid = in.isMouseButtonDown(GLFW_MOUSE_BUTTON_MIDDLE) &&
                     !in.isCursorLocked();
    if (!mid)                                   m_panning = false;
    else if (!m_panning && env.viewportHovered) m_panning = true;

    if (m_panning) {
        // A pan during a view change is not a contradiction (the view still ends
        // up down its axis), so let the change land at once rather than dropping
        // it half-turned and going on calling the result "Front".
        finishGlide(cam);

        const glm::vec2 d = in.mouseDelta();
        if (d.x != 0.0f || d.y != 0.0f) {
            const glm::vec3 piv  = pivot(cam, env);
            const float     dist = std::max(glm::length(cam.position() - piv), 0.5f);
            // One pixel of drag moves the world under the cursor by one pixel's
            // worth of world AT THE PIVOT'S DEPTH. That is what makes a pan feel
            // like dragging the scene rather than nudging a camera: whatever you
            // grabbed stays under the pointer.
            const float wpp = 2.0f * dist *
                              std::tan(glm::radians(std::max(cam.fov(), 1.0f) * 0.5f)) /
                              std::max(env.viewportH, 1.0f);
            // The scene follows the mouse, so the camera goes the other way.
            // (Input::mouseDelta already has y pointing up.)
            cam.setPosition(cam.position() -
                            (cam.right() * d.x + cam.up() * d.y) * wpp * panSpeed);
        }
    }

    // --- Mouse wheel: dolly, and the field of view with Ctrl ---------------
    // Two ways of getting closer that are worth keeping apart. The dolly WALKS
    // the camera in and the parallax comes with it, which is how you judge where
    // something sits; the field of view only narrows the cone from where you
    // already stand, which is how you frame a shot. Rolling the wheel is the
    // first (the common one, so it is the bare gesture) and Ctrl is the second.
    //
    // CTRL, NOT SHIFT. Windows hands a Shift-held wheel to the application as a
    // HORIZONTAL wheel -- the notch arrives on an axis GLFW reports separately
    // and this engine does not read, so a Shift+wheel gesture would simply do
    // nothing at all. Ctrl comes through as an ordinary wheel, and is the
    // zoom modifier everywhere else besides.
    const float wheel = env.viewportHovered ? in.scrollDelta() : 0.0f;
    if (wheel != 0.0f) {
        const bool ctrl = in.isKeyDown(GLFW_KEY_LEFT_CONTROL) ||
                          in.isKeyDown(GLFW_KEY_RIGHT_CONTROL);
        if (ctrl) {
            // Wheel forward narrows the angle, i.e. the view closes in -- the
            // same direction of travel as the dolly, so the two do not disagree
            // about which way "in" is. (Camera clamps to a sane 1..90 degrees.)
            cam.processScroll(wheel * fovStep);
        } else {
            // A dolly during a view change lands it at once, for the reason the
            // pan does: half a glide plus a move is a position neither asked
            // for. Moving straight down the view axis leaves a standard view
            // standing, so unlike a look, this does not drop back to User.
            finishGlide(cam);

            bool aimed = false;
            const glm::vec3 piv  = pivot(cam, env, &aimed);
            const float     dist = glm::length(cam.position() - piv);
            float step = std::clamp(dist * dollyStep, kMinDolly, kMaxDolly) * wheel;
            if (aimed && step > 0.0f)
                step = std::min(step, std::max(dist - kStopShort, 0.0f));
            cam.setPosition(cam.position() + cam.front() * step);
        }
    }

    // --- The standard views ------------------------------------------------
    // A menu click from last frame first: the menu is drawn well after this
    // runs, so it can only ever be acted on a frame later.
    if (m_request != StdView::User) {
        snapTo(m_request, cam, pivot(cam, env));
        m_request = StdView::User;
    }

    const bool ctrl = in.isKeyDown(GLFW_KEY_LEFT_CONTROL) ||
                      in.isKeyDown(GLFW_KEY_RIGHT_CONTROL);
    for (int i = 0; i < kBindingCount; ++i) {
        const bool down = env.keysFree && in.isKeyDown(kBindings[i].key);
        // Edge, not level: held down, a view key would restart its own glide
        // every frame and the camera would never arrive.
        if (down && !m_prevKey[i])
            snapTo(ctrl ? kBindings[i].withCtrl : kBindings[i].plain,
                   cam, pivot(cam, env));
        m_prevKey[i] = down;
    }

    // --- Advance a view change in flight -----------------------------------
    if (m_glide) {
        const float dur = std::max(glideTime, 0.01f);
        m_t = std::min(m_t + dt, dur);
        const float s = smoothstep01(m_t / dur);
        cam.setYaw(glm::mix(m_fromYaw, m_toYaw, s));
        cam.setPitch(glm::mix(m_fromPitch, m_toPitch, s));
        cam.setPosition(glm::mix(m_fromPos, m_toPos, s));
        if (m_t >= dur) m_glide = false;
    }
}

void Nav::drawMenu() {
    if (!ImGui::BeginMenu("Viewpoint")) return;
    struct Item { StdView v; const char* shortcut; };
    static const Item items[] = {
        {StdView::Front,  "Num 1"},
        {StdView::Back,   "Ctrl+Num 1"},
        {StdView::Right,  "Num 3"},
        {StdView::Left,   "Ctrl+Num 3"},
        {StdView::Top,    "Num 7"},
        {StdView::Bottom, "Ctrl+Num 7"},
    };
    for (const Item& it : items)
        if (ImGui::MenuItem(label(it.v), it.shortcut, m_view == it.v))
            m_request = it.v;
    ImGui::EndMenu();
}

} // namespace viewnav
