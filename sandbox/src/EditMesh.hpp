#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/asset/AssetId.hpp>
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

    // A material per FACE, parallel to `faces`. An invalid id -- which is what
    // every face starts with and most keep -- means "the object's own material",
    // so a mesh nobody has dressed face by face still draws as one thing with one
    // material. A valid one names a library material (MaterialDef::assetId) that
    // this face alone wears, which is how a wall gets a brick side and a plaster
    // side without being two objects.
    //
    // Parallel arrays again, and the same rule as `paint`: every operation that
    // adds or drops a face has to move the materials with it, or the brick turns
    // up on the roof three edits later.
    std::vector<fitzel::AssetId>  faceMat;

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

    // The material of face `f`, invalid where the face wears the object's own.
    // Read through this rather than indexing `faceMat`, which is empty on a mesh
    // that has never been dressed.
    fitzel::AssetId faceMaterial(int f) const {
        return (f >= 0 && f < static_cast<int>(faceMat.size())) ? faceMat[f]
                                                                : fitzel::AssetId{};
    }
    // Bring `faceMat` up to one entry per face (invalid for the new ones). Cheap
    // and idempotent, so operations call it before touching the array.
    void syncFaceMat() { faceMat.resize(faces.size(), fitzel::AssetId{}); }
    // Put `id` on face `f` (an invalid id hands the face back to the object's
    // material). The array grows to fit, so this is the only call a caller needs.
    void setFaceMaterial(int f, const fitzel::AssetId& id);
    // Does any face wear a material of its own? Decides whether the mesh has to be
    // drawn in several pieces at all -- and whether the scene file carries them.
    bool dressed() const;

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

// Ring a cut all the way around the mesh, the way a knife goes round a loaf:
// every quad in the band that `face` belongs to is split in two, and the new
// edge runs across all of them as one line. This is the operation that turns a
// box into a box with a floor line -- storeys, panel joints, a crease to bend a
// wall at -- without subdividing (and quadrupling) every face on the way.
//
// `dir` picks WHICH of the two bands the face lies in (0 or 1: a quad has two,
// at right angles); `t` is where along them the cut falls, 0..1 from one side to
// the other, 0.5 being the middle. The band is walked quad to quad through
// shared edges and stops where the mesh does -- an n-gon, a hole, or the face it
// started from -- so an open shape is cut across whatever part of it is a band.
//
// Returns the half of `face` that keeps the selection.
int loopCut(EditMesh& m, int face, int dir, float t);

// How many faces `loopCut` would split, without touching the mesh. The panel
// shows it, because "this cuts 4 faces" and "this cuts 37" are different
// operations to agree to, and the difference is invisible from one face.
int loopLength(const EditMesh& m, int face, int dir);

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

// One drawable piece of a mesh: the faces that wear one material, triangulated.
// `material` invalid means the object's own material -- the piece every mesh has
// and most have only.
struct Group {
    fitzel::AssetId  material;
    fitzel::MeshData data;
};

// Triangulate for the GPU, split by face material: flat-shaded (one normal per
// face, so an extruded box has crisp edges instead of a smoothed blob) with a
// planar UV projection along the face's dominant axis.
//
// A mesh nobody has dressed comes back as exactly one group with an invalid
// material, which is the same single draw call it always was. The group wearing
// the object's own material comes first when there is one.
std::vector<Group> buildGroups(const EditMesh& m);

// The whole mesh as one triangle soup, materials ignored. What the harnesses and
// anything that only wants the geometry read; the renderer takes the groups.
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
    // One uploaded piece: the faces wearing one material. An invalid `material`
    // is the object's own -- see editmesh::Group.
    struct Sub {
        fitzel::AssetId material;
        fitzel::Mesh    mesh;
    };

    // The uploaded pieces of `entityId` at `revision`, rebuilt from `m` first if
    // the cache is out of date. Never empty for a mesh with faces; a mesh nobody
    // has dressed face by face comes back as the single piece it always was.
    const std::vector<Sub>& submeshes(int entityId, std::uint64_t revision,
                                      const EditMesh& m);
    void clear() { m_entries.clear(); }

private:
    struct Entry {
        std::uint64_t     revision = 0;
        std::vector<Sub>  subs;
    };
    std::unordered_map<int, Entry> m_entries;
};
