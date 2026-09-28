#pragma once

#include <functional>

#include <glm/glm.hpp>
#include <imgui.h>

struct ViewportFrame;

// What the brushes that work on the ground have in common: the grass, tree,
// flower and object-scatter brushes and the terrain's texture paint. Each aims
// at the ground under the cursor, takes Alt (or its own Erase toggle) to mean
// "take it away", and shows a ring lying on the terrain; the stamping ones lay
// a stamp every so many metres of a held drag. What a stamp PUTS stays with the
// brush that owns it. Editor only.
namespace groundbrush {

// Where the brush is this frame.
struct Aim {
    bool      onGround = false;  // the cursor is over the viewport and the terrain under it
    glm::vec3 center{0.0f};      // ...where it hits the ground
    bool      erasing  = false;  // the brush's Erase toggle, or Alt held
};

Aim aim(const ViewportFrame& view, bool eraseToggle);

// A stamping brush's drag: `last` is where it stamped last (the brush keeps it
// between frames). A fresh press forgets it; while the left button is held on
// the ground, `rub` runs every frame when erasing, and `put` once the cursor is
// `spacing` metres from the last stamp -- so a slow drag does not pile stamps
// up, and the trail comes out even.
void drag(const Aim& at, glm::vec2& last, float spacing,
          const std::function<void(glm::vec2)>& put,
          const std::function<void(glm::vec2)>& rub);

// The brush cursor: a ring of `radius` lying on the ground around the aim,
// lifted a hair off it; `eraseCol` while erasing. Nothing when off the ground.
void ring(const ViewportFrame& view, const Aim& at, float radius, ImU32 paintCol,
          ImU32 eraseCol = IM_COL32(255, 90, 70, 220), int segments = 48);

} // namespace groundbrush
