#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Mesh.hpp>

// A small editable polygon mesh -- the box-modelling kind. Faces are polygons
// over a shared vertex list, wound counter-clockwise seen from outside; there is
// no half-edge structure, no manifold bookkeeping and no topology library
// underneath. That is the deliberate ceiling: the operations below are the four
// or five that turn a cube into a building, a ramp or a wall, and each is a
// handful of vector arithmetic over a face's own corners.
//
// Vertices are SHARED between faces, which is what makes "scale a face" behave
// the way one expects on a box: pulling the top face in gives a frustum, because
// its four corners are the same four corners the sides end at. An operation that
// needs its own corners (extrude, inset) says so by duplicating them first.
struct EditMesh {
    std::vector<glm::vec3>        verts;
    std::vector<std::vector<int>> faces;   // indices into verts, CCW from outside

    // Texture-paint weights for the first four terrain layers, one per CORNER --
    // the same vec4 the terrain paints, on the same shader path (see the brush in
    // MeshPaint.hpp). Parallel to `verts`: a corner and its weights are one thing
    // in two arrays, so every operation that adds or drops a corner has to move
    // the weights with it or a later stroke paints the wrong face. Empty means
    // "nothing painted", which is the state every mesh starts in and most stay in.
    std::vector<glm::vec4>        paint;

    // A unit box, the starting point for everything: 8 corners, 6 quads.
    static EditMesh box(const glm::vec3& half);

    // Weights of corner `i`, zero where the mesh carries none. Read through this
    // rather than indexing `paint`, which is empty on an unpainted mesh.
    glm::vec4 paintAt(int i) const {
        return (i >= 0 && i < static_cast<int>(paint.size())) ? paint[i]
                                                              : glm::vec4(0.0f);
    }
    // Bring `paint` up to one entry per corner (zeros for the new ones). Cheap and
    // idempotent, so operations call it before touching the array.
    void syncPaint() { paint.resize(verts.size(), glm::vec4(0.0f)); }
    // Is there any paint at all? Decides whether the entity needs the painted
    // material (and whether the scene file carries the weights).
    bool painted() const;

    bool      validFace(int f) const;
    glm::vec3 faceCenter(int f) const;
    // Newell's normal: correct for any planar polygon and stable for the slivers
    // an over-scaled face can produce, where a single cross product is not.
    glm::vec3 faceNormal(int f) const;
    float     faceArea(int f) const;
    void      bounds(glm::vec3& mn, glm::vec3& mx) const;
};

namespace editmesh {

// Every operation takes a face index and leaves the mesh usable; each returns
// the face the user should still have selected afterwards (-1 if the operation
// did not apply), so a tool can chain "extrude, then inset" without the
// selection jumping somewhere else.

// Pull `face` out along its own normal, walling in the gap. The moved copy keeps
// the selection: extruding twice in a row builds a chimney rather than a crease.
// A negative distance pushes in.
int extrude(EditMesh& m, int face, float dist);

// Scale a face about its own centre, in its own plane. Shrinking the top of a
// box gives a frustum; 0 collapses it to a point.
int scaleFace(EditMesh& m, int face, float factor);

// Slide a face along its normal without adding geometry -- the neighbouring
// faces stretch to follow. This is the "make it taller" of a box, as opposed to
// extrude's "grow something new out of it".
int moveFace(EditMesh& m, int face, float dist);

// A rim inside the face: an extrude that goes nowhere, then a scale. The inner
// face is returned, which is the one you then extrude to make a window recess or
// a raised panel.
int inset(EditMesh& m, int face, float amount);

// Split a quad into four. Only quads -- an n-gon has no obvious four-way split,
// and inventing one is how a modest tool becomes a topology library.
int subdivide(EditMesh& m, int face);

// Remove a face, leaving a hole. Vertices left unused by any face go with it, so
// repeated edits do not grow the mesh forever.
int deleteFace(EditMesh& m, int face);

// Apply an arbitrary transform to a face's corners. This is the one the viewport
// gizmo drives: move, rotate and scale are all the same operation to it, which
// is why there is one entry point rather than three. The matrix is in the mesh's
// own space and already expressed about whatever origin the caller intends --
// the gizmo hands over a delta about the face's centre.
//
// The corners are shared, so this stretches the neighbouring faces to follow, as
// moveFace does. Dragging a face freshly created by extrude moves only that
// face, because extrude gave it corners of its own.
int transformFace(EditMesh& m, int face, const glm::mat4& xform);

// Shift the mesh so its bounding box is centred on the object's origin, and
// report the shift (in the mesh's own space). Callers move the entity by the
// same amount so nothing appears to jump: this is what keeps the entity's
// half-extents -- and with them the pick box, the gizmo and the collider -- an
// honest description of the geometry after every edit.
glm::vec3 recenter(EditMesh& m);

// Triangulate for the GPU: flat-shaded (one normal per face, so an extruded box
// has crisp edges instead of a smoothed blob) with a planar UV projection along
// the face's dominant axis.
fitzel::MeshData build(const EditMesh& m);

// A monotonically increasing stamp. Every edit takes a fresh one, which is how
// the GPU cache below knows its copy is stale -- including after an undo, which
// restores an OLDER stamp and therefore also reads as "not what I have".
std::uint64_t nextRevision();

} // namespace editmesh

// GPU copies of the edited meshes, one per entity, rebuilt when the entity's
// mesh revision no longer matches what was uploaded. Lives outside the entity
// because a fitzel::Mesh owns GL resources and is move-only, while an Entity has
// to stay copyable -- that is what the undo snapshots are built on.
class EditMeshCache {
public:
    // The uploaded mesh for `entityId` at `revision`, rebuilding it from `m`
    // first if the cache is out of date. Never null.
    const fitzel::Mesh& mesh(int entityId, std::uint64_t revision, const EditMesh& m);
    void clear() { m_entries.clear(); }

private:
    struct Entry {
        std::uint64_t revision = 0;
        fitzel::Mesh  mesh;
    };
    std::unordered_map<int, Entry> m_entries;
};
