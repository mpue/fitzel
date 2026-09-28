#pragma once

#include <functional>

#include <glm/glm.hpp>
#include <imgui.h>

namespace fitzel { class Camera; }

// The scene viewport this frame, as a tool needs it: the camera's
// view-projection and where it looks from, the image's place and size on
// screen, the cursor over it, and the ground under a point of it.
//
// main builds one per frame where the Scene window works these out, and every
// viewport tool takes it -- the brushes, the gizmo, the road, spline and water
// editors -- instead of each carrying its own copy of the same dozen fields.
// Editor only.
struct ViewportFrame {
    glm::mat4 viewProj{1.0f};
    ImVec2    origin{0.0f, 0.0f};   // the image's top-left, screen pixels
    float     w = 1.0f, h = 1.0f;   // its size, pixels
    bool      hovered = false;      // the cursor is over it (and not over a widget on it)
    glm::vec2 mouseNdc{0.0f};       // the cursor in NDC [-1, 1]
    ImVec2    mousePos{0.0f, 0.0f}; // ...and in screen pixels
    // The camera it is seen through.
    glm::vec3 cameraPos{0.0f};
    glm::vec3 cameraFront{0.0f, 0.0f, -1.0f};  // for a camera-relative nudge
    float     cameraFov  = 60.0f;              // vertical, degrees
    float     orthoHalfH = 0.0f;   // > 0: the view is orthographic, this tall (half)
    // The terrain under a viewport NDC point (main's roadPickTerrain), and its height.
    std::function<bool(glm::vec2, const glm::mat4&, glm::vec3&)> pickTerrain;
    std::function<float(float, float)>                           groundAt;

    // What `cam` shows through an image at `origin`, `w` x `h` pixels, with the
    // cursor at `mouse` (screen pixels): the view-projection, the cursor in NDC
    // and the camera's own fields. The terrain callbacks are the caller's.
    static ViewportFrame looking(const fitzel::Camera& cam, ImVec2 origin, float w, float h,
                                 ImVec2 mouse, bool hovered);

    // The world ray under the cursor.
    void mouseRay(glm::vec3& origin, glm::vec3& dir) const;
    // A world point in screen pixels; false behind the camera.
    bool toScreen(const glm::vec3& p, ImVec2& out) const;
    // ...and false past the far plane as well: for marks that must not show
    // through from beyond what the view draws.
    bool project(const glm::vec3& p, ImVec2& out) const;
    // World metres one screen pixel spans at `at`: what a vertical drag has to
    // move a thing by for it to stay under the cursor, whatever the zoom.
    float metresPerPixel(const glm::vec3& at) const;
    // A box's twelve edges -- the corners lo..hi, through `model` -- into the
    // current window's draw list. An edge with a corner behind the camera or
    // past the far plane is left out.
    void wireBox(const glm::mat4& model, const glm::vec3& lo, const glm::vec3& hi,
                 ImU32 col, float thick) const;
};
