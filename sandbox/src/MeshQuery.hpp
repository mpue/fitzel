#pragma once

#include <vector>

#include "EditMesh.hpp"
#include "SceneTypes.hpp" // Entity

// Where a modelled mesh (MeshComponent) is solid, asked along a vertical line --
// the one question the cheap ground and wall tests outside the physics world
// need: walking in the editor and hovering a glider. Those used to read the
// entity's half-extents, which is right for a box and wrong for anything
// modelled: an extruded tower stood you on its bounding box, an arch had no way
// through, and a ramp was the ideal wedge however it had been bent.
//
// The mesh is taken exactly where it is drawn (position, rotation and the
// fitScale stretch), rotation included, which the box tests never did. The line
// is taken into the mesh's own space rather than every corner into the world,
// so a query costs one matrix inverse and a pass over the faces.
namespace meshquery {

// One place the vertical line crosses the mesh: the world height, and whether
// the face there looks up (something to stand on) or down (a ceiling, the
// underside of a solid). Faces are wound counter-clockwise from outside, which
// is what says which.
struct Hit {
    float y;
    bool  up;
};

// Every crossing of the vertical line through (x, z) with the mesh, highest
// first.
void verticalHits(const Entity& e, const EditMesh& m, float x, float z,
                  std::vector<Hit>& out);

// The highest upward-facing surface of the mesh at (x, z) that is no higher
// than `yMax`, written to `y`. False where the mesh has none there. Only faces
// that look up count: the underside of a tower is not a floor, even for a
// craft whose ceiling happens to be inside it.
bool surfaceBelow(const Entity& e, const EditMesh& m, float x, float z,
                  float yMax, float& y);

// Is the mesh solid anywhere on the vertical span [yLo, yHi] at (x, z)? A face
// crossing the span, or the span inside a closed mesh (an odd number of faces
// above it), both count.
bool blocks(const Entity& e, const EditMesh& m, float x, float z, float yLo, float yHi);

} // namespace meshquery
