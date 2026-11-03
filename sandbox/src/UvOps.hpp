#pragma once

#include <cstdint>
#include <set>
#include <vector>

#include <glm/glm.hpp>

#include "EditMesh.hpp"

// The UV editor's operations, without a window -- what Blender's UV editor
// does, on an EditMesh. UvPanel.cpp is the window around them.
//
// A UV vertex is a LOOP: corner k of face f. Faces do not share loops, so two
// faces meeting at a mesh corner have two loops there, and whether the
// texture is continuous across them is only whether the two sit at the same
// place in UV space. That is Blender's model, and it is what lets an island
// be cut free by moving it.
//
// Everything works on a set of faces -- the ones the window shows (the faces
// picked in the viewport, or all of them). Operations that move coordinates
// first give the faces they touch coordinates of their own (ensureCorners):
// a face that never had any shows its projection, and from the first move on
// it carries what was moved.
namespace uvops {

struct Loop {
    int f = -1, k = -1;
    bool valid() const { return f >= 0; }
    bool operator==(const Loop& o) const { return f == o.f && k == o.k; }
    bool operator<(const Loop& o) const { return f != o.f ? f < o.f : k < o.k; }
};
using Selection = std::set<Loop>;

enum class SelectMode { Vertex, Edge, Face, Island };

// The coordinates of the shown faces, read once (faceUvs for each) and what is
// built from them: which loops sit on each mesh corner. Build it again after
// any change.
struct View {
    const EditMesh*                     mesh = nullptr;
    std::vector<int>                    faces;        // shown, in this order
    std::vector<int>                    slot;         // mesh face -> index in `faces`, -1 = not shown
    std::vector<std::vector<glm::vec2>> uv;           // per shown face, per corner
    std::vector<std::vector<Loop>>      atVert;       // mesh corner -> loops on it (shown faces only)

    glm::vec2 at(const Loop& l) const { return uv[static_cast<std::size_t>(slot[static_cast<std::size_t>(l.f)])][static_cast<std::size_t>(l.k)]; }
    bool      shown(int f) const { return f >= 0 && f < static_cast<int>(slot.size()) && slot[static_cast<std::size_t>(f)] >= 0; }
    int       vert(const Loop& l) const { return mesh->faces[static_cast<std::size_t>(l.f)][static_cast<std::size_t>(l.k)]; }
};
View view(const EditMesh& m, const std::vector<int>& faces);

// UV distance under which two loops count as one place ("welded").
constexpr float kWeld = 1e-4f;

// The loops at the same mesh corner and the same place as `l` (`l` among them):
// Blender's sticky selection, "shared location".
std::vector<Loop> coincident(const View& v, const Loop& l);
// The islands: shown faces joined across edges whose both ends are welded.
std::vector<std::vector<int>> islands(const View& v);

// --- Picking (UV-space points and distances) ------------------------------------
Loop nearestLoop(const View& v, glm::vec2 p, float maxDist);
// The edge from loop (f, k) to (f, k + 1) nearest `p`.
Loop nearestEdge(const View& v, glm::vec2 p, float maxDist);
// The face whose polygon holds `p`; failing that, the one whose middle is
// nearest within maxDist.
int faceAt(const View& v, glm::vec2 p, float maxDist);

// --- Selecting ------------------------------------------------------------------
// A click: what is under `p` in this mode -- with `sticky`, the loops welded to
// it come along. `toggle` adds it, or takes it away when it is all selected
// already. False when nothing was under the pointer (a plain click then clears).
bool click(const View& v, Selection& s, SelectMode mode, glm::vec2 p, float maxDist, bool toggle,
           bool sticky = true);
void selectAll(const View& v, Selection& s);
void invert(const View& v, Selection& s);
// The loops inside the box; `remove` takes them away instead. Face and island
// mode take whole faces whose middle is inside.
void selectBox(const View& v, Selection& s, SelectMode mode, glm::vec2 lo, glm::vec2 hi, bool remove);
// Every island something is selected in, whole.
void selectLinked(const View& v, Selection& s);
void selectPinned(const View& v, Selection& s);
// Drop loops that are no longer shown or no longer exist.
void validate(const View& v, Selection& s);
bool edgeSelected(const Selection& s, const EditMesh& m, int f, int k);
bool faceSelected(const Selection& s, const EditMesh& m, int f);
// The faces of the selection: every face any selected loop belongs to.
std::vector<int> facesOf(const Selection& s);

// --- Moving coordinates ----------------------------------------------------------
// Give these faces coordinates of their own (their projection, written down).
void ensureCorners(EditMesh& m, const std::vector<int>& faces);
bool bounds(const View& v, const Selection& s, glm::vec2& lo, glm::vec2& hi);
// Every selected loop through p -> A (p - pivot) + pivot + t.
void transform(EditMesh& m, const Selection& s, const glm::mat2& a, glm::vec2 pivot, glm::vec2 t);
// The same, each island about the middle of its own selected loops.
void transformEach(EditMesh& m, const View& v, const Selection& s, const glm::mat2& a, glm::vec2 t);
// U (axis 0) or V (1) of every selected loop set to their average.
void align(EditMesh& m, const View& v, const Selection& s, int axis);
// Every selected loop to the middle of them all.
void mergeAtCenter(EditMesh& m, const View& v, const Selection& s);
// Selected loops closer than `dist` to each other joined at their average.
// Returns how many loops moved.
int mergeByDistance(EditMesh& m, const View& v, const Selection& s, float dist);
// Onto the nearest texel corner of a w x h texture.
void snapToPixels(EditMesh& m, const Selection& s, int w, int h);
void setPin(EditMesh& m, const Selection& s, bool on);

// --- Unwrapping and projecting (these faces) ----------------------------------------
// Each face cast along the axis it faces most, `size` metres to the tile.
void cubeProject(EditMesh& m, const std::vector<int>& faces, float size);
// Round the vertical (Y) axis through the faces' middle: U around, V up
// (cylinder) or by latitude (sphere).
void cylinderProject(EditMesh& m, const std::vector<int>& faces);
void sphereProject(EditMesh& m, const std::vector<int>& faces);
// Every face filling the whole tile.
void resetUvs(EditMesh& m, const std::vector<int>& faces);
// Back to the planar projection in metres (the placement's axis, size,
// rotation and offset), coordinates of their own dropped.
void projectWorld(EditMesh& m, const std::vector<int>& faces, const EditMesh::FaceUV& placement);
// Faces grouped by the way they face (within `angleLimit` degrees), each group
// flattened along its direction, the pieces packed. Returns the island count.
int smartProject(EditMesh& m, const std::vector<int>& faces, float angleLimit, float margin);
// Blender's Unwrap (conformal): the faces cut open along the seams into
// charts, each flattened with least-squares conformal maps. A chart with two
// or more pinned loops is solved around them and stays where the pins are;
// the others are scaled to their size in metres and packed. Returns the chart
// count; `closed` gets how many of them have no edge to open along (a whole
// box with no seam) -- those come out crumpled, and want seams first.
int unwrap(EditMesh& m, const std::vector<int>& faces, float margin, int* closed = nullptr);

// --- Islands ------------------------------------------------------------------------
// The islands of these faces into the 0..1 tile, `margin` apart, turned to their
// tightest box first when `rotate`.
void pack(EditMesh& m, const std::vector<int>& faces, float margin, bool rotate);
// Every island scaled to the same texture density (UV area per square metre).
void averageScale(EditMesh& m, const std::vector<int>& faces);

// --- Seams --------------------------------------------------------------------------
// Seams where the islands of these faces end. Returns how many edges were marked.
int seamsFromIslands(EditMesh& m, const std::vector<int>& faces);
// Seams on the edges whose faces meet at more than `angle` degrees.
int seamsFromSharp(EditMesh& m, const std::vector<int>& faces, float angle);

// --- Measuring ----------------------------------------------------------------------
// How much each shown face is stretched: log2 of its UV area per square metre
// against the mean of all of them. 0 = average, +1 = twice the density.
std::vector<float> stretch(const View& v);

} // namespace uvops
