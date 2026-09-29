#pragma once

#include <cstddef>

#include <glm/glm.hpp>

#include "EditMesh.hpp"

// The geometry behind the modifier stack (Modifiers.hpp): whole-mesh
// operations that turn one EditMesh into another. Pure data in, data out -- no
// scene, no GPU -- so every one of them is testable on its own (modifiercheck).
//
// Each keeps the parallel arrays in step the way the editing operations do:
// a new corner carries paint weights (interpolated where it is made between
// others), a new face wears what the face it came from wore. An array that is
// empty on the input stays empty on the output -- a mesh nobody painted does
// not come back painted with zeros.
namespace editmesh {

// Subdivide every face `levels` times: each n-gon becomes n quads, one per
// corner. `smooth` is Catmull-Clark -- the surface pulls in towards the limit
// a subdivision surface converges to, open borders following a curve of their
// own and corners where a border turns (or three faces meet one edge) held
// where they are. Without it the faces are only cut ("Simple"). Stops before a
// level would pass `maxFaces`; returns how many levels were done.
int subdivideSurface(EditMesh& m, int levels, bool smooth, std::size_t maxFaces = 400000);

// Collapse the mesh to about `ratio` (0..1] of its triangles (quadric error
// edge collapse, see MeshSimplify.hpp). Faces come back as triangles. Each
// material is simplified as its own region, so the line where two materials
// meet is a border the collapse holds rather than smears. 1 changes nothing.
void decimate(EditMesh& m, float ratio);

// Thicken the surface into a shell `thickness` deep, along the corners'
// normals. `offset` says where the shell sits: -1 inward from the surface
// (the surface is its outside), 0 centred on it, 1 outward. With `rim` the
// open borders are walled in, so an open surface becomes a closed solid;
// with `even` a corner goes further where faces meet at an angle, so every
// face is `thickness` deep and not only the flat ones.
void solidify(EditMesh& m, float thickness, float offset, bool rim, bool even);

// Replace every edge with a solid strut `thickness` thick: each face is
// framed by an inset of half the thickness, the frames are joined along the
// edges they share and thickened (offset as solidify). With `replace` false the
// original faces stay too.
void wireframe(EditMesh& m, float thickness, float offset, bool replace);

} // namespace editmesh
