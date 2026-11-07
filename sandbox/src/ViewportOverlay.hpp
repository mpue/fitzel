#pragma once

#include <vector>

#include <glm/glm.hpp>
#include <imgui.h>

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

// Objects with no mesh of their own -- lights, cameras, sound sources,
// triggers, plain empties -- show nothing that says what they are (a light a
// glowing cube at most, a camera nothing at all) and would be found only by name in the
// Hierarchy. So each gets an icon saying what it is (ObjectIcons.hpp) on a small
// plate, with its name beside it while it is near: on top of the scene, about
// the same size wherever it stands, farther ones a little smaller and fainter
// and drawn under nearer ones. The selected ones are rimmed in the selection's
// orange, the one under the pointer lights up -- a click on it selects it
// (iconsAt, through viewpick). `scale` is the size the user set (View >
// Interface), 1 = the default.
void icons(const EditorContext& ed, const ViewportFrame& view, float scale = 1.0f);

// The ids of the objects whose icon covers the screen point `at`, the icon
// drawn on top first -- at the same `scale` they are drawn at.
std::vector<int> iconsAt(const std::vector<Entity>& entities, const ViewportFrame& view,
                         ImVec2 at, float scale = 1.0f);

} // namespace overlay
