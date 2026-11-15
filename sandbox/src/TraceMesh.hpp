#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "TraceShapes.hpp"

// An exact solid (TraceShapes.hpp) as triangles: what a 3D printer needs from
// a shape that was only ever an equation -- an isosurface above all, but any
// sphere, torus or CSG of them just as well.
//
// THE SOLID'S OWN INSIDE TEST, NOT A DISTANCE. Shapes::inside() is exact for
// every kind and for every CSG of them, which a signed distance would not be
// (a difference has none in closed form, an isosurface's field is any number
// the script liked). So the solid is sampled on a grid as in or out, and
// wherever an edge of the grid runs from in to out the crossing is found by
// bisecting that edge with the same test: every corner of the result lies on
// the real surface, to a few thousandths of a cell.
//
// MARCHING TETRAHEDRA, FOR A SURFACE THAT IS CLOSED. Every cube of the grid is
// cut into the same six tetrahedra (Kuhn's: from its low corner to its high
// one along each order of the three axes), so neighbouring cubes cut their
// shared faces the same way. In a tetrahedron there is no ambiguous case --
// one corner apart is a triangle, two are a quad -- and every triangle's edge
// lies in a face that exactly two tetrahedra share, with its corner shared by
// edge index. The result is a closed 2-manifold by construction: no cracks,
// no edge used three times, which is what a slicer wants to be sure of. The
// grid's outermost layer is outside by construction, so a solid cut by the
// grid's box is still closed.
//
// SHARP EDGES PUT BACK. A grid knows nothing of creases: where a gyroid is cut
// by a sphere, or a twisted bar has its corners, the triangles bevel the edge
// in a sawtooth a cell high -- on a print, a rim that is visibly chewed. So
// afterwards every edge whose two ends' exact tangent planes (a short ray at
// the solid for each) disagree by more than ~30 degrees is split at the point
// where those planes meet nearest its middle -- a point ON the crease, kept
// only when it is on the surface -- and a triangle with two such edges gets
// the two new corners joined: that edge is the crease (the idea of Kobbelt et
// al.'s Extended Marching Cubes, done on edges). Old corners never move and a
// triangle is only ever cut into pieces, so the surface stays closed; a split
// that would turn a piece over against the surface it lies on is not made.
//
// No GL. Threads for the sampling, since an isosurface's field is the cost.
namespace pathtrace {

struct SolidMesh {
    std::vector<glm::dvec3> verts;   // world space
    std::vector<glm::ivec3> tris;    // counter-clockwise seen from outside
    std::vector<int>        triLeaf; // per triangle: the leaf whose surface it is (-1 none found)
};

struct SolidMeshOptions {
    double cell = 0.01;          // grid spacing, world units
    int    threads = 0;          // 0: all but one
    int    maxCells = 1500;      // per axis; more is refused (the caller coarsens)
    bool   leaves = true;        // fill triLeaf (a ray per triangle)
    bool   sharpen = true;       // put creases back (a ray per corner, one per crease edge)
    // Called now and then with 0..1; false stops the work.
    std::function<bool(float)> progress;
};

// Cells per axis meshSolid() would use for `node` at `cell`: its bounds
// padded by a cell and a half on every side. Zero when it is unbounded.
glm::ivec3 solidGrid(const Shapes& shapes, int node, double cell);

// Triangulate one node of `shapes` (finalize()d). False with `err` set when it
// is unbounded, too fine, cancelled, or has nothing inside at this spacing.
bool meshSolid(const Shapes& shapes, int node, const SolidMeshOptions& opt, SolidMesh& out,
               std::string& err);

} // namespace pathtrace
