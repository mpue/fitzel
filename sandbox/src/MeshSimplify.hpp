#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// --- Level of detail for meshes nobody made levels of detail for ---------------
//
// The trees people drop into a project are photogrammetry-grade: a hundred
// thousand triangles of bark and another fifty of leaf cards, per tree. Drawn
// once that is merely expensive; drawn into four shadow cascades for every tree
// within reach of the sun, it is the whole frame -- the scaper scene spent 30 of
// its 45 ms on it. A tree forty metres away is a handful of pixels of bark and
// a silhouette of leaves, and it can be drawn as exactly that.
//
// Two tools, because a tree is two kinds of surface:
//
//  - simplify(): quadric error metric edge collapse (Garland & Heckbert) for the
//    solid parts. HALF-edge collapses -- a vertex moves onto a neighbour, never
//    to a new point -- so the result indexes the original vertices and keeps
//    their normals and UVs exactly; an index buffer is all a level costs.
//    Borders (UV seams, open ends) are held by constraint planes so the texture
//    does not tear and branch stumps do not shrink.
//
//  - pruneCards(): collapsing a leaf card destroys it -- it is two triangles
//    whose whole shape is the alpha texture on them. Canopies are simplified
//    the way foliage is in film (Cook et al., "Stochastic Simplification of
//    Aggregate Detail"): keep a fraction of the cards and grow each survivor
//    about its own centre so the crown keeps the same area, i.e. the same
//    density and the same shadow.
namespace meshsimplify {

// Reduce `indices` (triangles over `vertexCount` vertices, positions at float
// offset 0 of every `stride`-float vertex) to at most `targetTris` triangles.
// Returns the new index list; it references the same vertices.
std::vector<std::uint32_t> simplify(const float* vertices, std::size_t stride,
                                    std::size_t vertexCount,
                                    const std::uint32_t* indices, std::size_t indexCount,
                                    std::size_t targetTris);

// Keep about `keep` (0..1] of the connected pieces (cards) in `indices` and
// scale each kept piece about its centroid by 1/sqrt(keep). Vertices are
// modified in place (only those of kept pieces), so pass a copy.
void pruneCards(std::vector<float>& vertices, std::size_t stride,
                std::vector<std::uint32_t>& indices, float keep, std::uint32_t seed);

} // namespace meshsimplify
