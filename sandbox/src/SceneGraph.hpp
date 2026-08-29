#pragma once

#include <vector>

#include <glm/glm.hpp>

#include "SceneTypes.hpp" // Entity

// The scene graph: turning what a scene FILE stores into what everything else
// reads.
//
// An entity stores its transform relative to its parent (localCenter,
// localRotation) -- that is the source of truth, and what makes a child follow
// a parent that moves. Everything downstream, though, reads world space: the
// renderer, picking, physics, the camera. Somebody has to walk the hierarchy and
// derive one from the other, parents first.
//
// That walk lived as a lambda in main(), which meant a scene loaded anywhere
// else came out with every object sitting at the origin -- and, because it also
// resolves effective visibility, with nothing switched off that should be. The
// first thing viewcheck ever rendered was two spheres stacked on top of each
// other at (0,0,0), forty metres and a hundred and forty from where the author
// put them. Loading a scene is not finished until this has run.
namespace scenegraph {

// A transform to a matrix, and back. Both go through ImGuizmo, which is the
// convention the scene composes in (Rz * Ry * Rx -- see docs/invariants.md) and
// therefore not a detail anyone may reimplement: a second composition that
// agrees to within a rounding error is a different scene.
glm::mat4 compose(const glm::vec3& translation, const glm::vec3& rotationDeg,
                  const glm::vec3& scale);
void      decompose(const glm::mat4& m, glm::vec3& translation,
                    glm::vec3& rotationDeg, glm::vec3& scale);

// Derive every entity's world transform (center, rotation) and effective
// visibility (activeInHierarchy) from its local one. Parents are resolved before
// their children whatever order the list is in.
void resolve(std::vector<Entity>& entities);

} // namespace scenegraph
