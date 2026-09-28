#pragma once

#include <vector>

#include <glm/glm.hpp>

struct Entity;
struct EditorContext;
struct ViewportFrame;

// The authoring marks drawn over the scene image: things that help aim, not
// things in the scene, so they are a 2D overlay in the Scene window's draw
// list rather than geometry the renderer would light, shadow and fog. Editor
// only.
namespace overlay {

// The 3D cursor: a red/white split ring with crosshair ticks, always on top,
// so it reads like Blender's.
void cursorMark(const ViewportFrame& view, const glm::vec3& at);

// The selection: an oriented wire box around every selected object -- the
// active one bright and drawn last, on top -- and the active object's component
// gizmos (a radius, a path), each drawn by the component itself. Nothing for no
// selection or the sun.
void selection(const EditorContext& ed, const ViewportFrame& view);

// Empties have no mesh, so each gets a constant-size icon with its name --
// otherwise they would be invisible and only reachable from the hierarchy.
// Their pick box still makes them clickable in the viewport.
void empties(const std::vector<Entity>& entities, const ViewportFrame& view);

} // namespace overlay
