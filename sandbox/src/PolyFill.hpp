#pragma once

#include <vector>

#include <glm/glm.hpp>

// Filling closed outlines -- a letter with its counters, a plate with a hole --
// in two ways, both public because the callers want both:
//
//   fill()        triangles over the outline's own corners, holes joined to
//                 their outline by a bridge first (ear clipping, Eberly). One
//                 triangle per corner: cheap, and the triangles share their
//                 corners, so an outline filled this way and walled round its
//                 boundary edges is a closed solid.
//   trapezoids()  a sweep in horizontal bands, nonzero winding: knows nothing
//                 of nesting and cannot get it wrong -- the reference fill()
//                 is measured against, and what is left where fill() gives up
//                 (outlines that overlap, which want a union first).
//
// Doubles throughout: font units run to ~2000 and their cross products outrun
// a float. Pure 2D, y up.
namespace polyfill {

using DV = glm::dvec2;

struct Fill {
    std::vector<DV>               points;   // every corner kept, outline by outline
    std::vector<glm::ivec3>       tris;     // into points, counter-clockwise
    std::vector<std::vector<int>> loops;    // the outlines as kept: outer counter-
                                            // clockwise, holes clockwise
};

// Fill `outlines` (any orientation; a contour inside an odd number of others is
// a hole). Repeated corners and corners on a straight line are dropped first.
// False when it could not be done cleanly -- the caller then takes trapezoids.
// With `seal`, only a sealed fill (below) will do: one that is not is tried
// again turned by a few hundredths of a degree -- a bridge run along a
// level edge, the usual reason, then runs along none -- and false if no turn
// seals it. The triangles found turned fit the outlines as given.
bool fill(const std::vector<std::vector<DV>>& outlines, Fill& out, bool seal = false);

// Whether the triangles' own boundary is exactly the outlines -- every edge
// inside crossed once each way, no corner left lying on another triangle's
// edge. Then walls along `loops` close a filled outline into a solid. (The
// clipper drops a corner that has gone flat, which a picture never notices
// and a solid does.)
bool sealed(const Fill& f);

// A band of the region: horizontal bottom and top edges, slanted sides.
struct Trap { double y0, y1, xl0, xr0, xl1, xr1; };

// The region the outlines enclose (nonzero winding), cut into trapezoids. A
// piece whose two sides go on unchanged into the next band is extended rather
// than restarted -- a straight stem is one quad, not one per band -- and the
// corners where pieces meet are the same doubles on both sides.
std::vector<Trap> trapezoids(const std::vector<std::vector<DV>>& outlines);

double area(const Fill& f);
double area(const std::vector<Trap>& t);

} // namespace polyfill
