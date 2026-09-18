#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>
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

    // How one face's texture sits on it. Parallel to `faces`, the same bargain
    // as faceMat and paint -- and empty on every mesh nobody has opened the UV
    // panel on, which is exactly the projection this mesh had before any of this
    // existed. Every default here is chosen so that the derived UVs come out
    // bit-identical to the old planar projection: an unedited face must not
    // shift by a texel because the field it never used gained a struct.
    struct FaceUV {
        // Which way the texture is projected. 0 lets the face decide (the world
        // axis it faces most), 1/2/3 force X/Y/Z. Forcing it is how four walls
        // of a tower get ONE continuous brick course instead of four separately
        // sensible ones, and it is the only setting here you cannot get to by
        // nudging numbers.
        int       axis = 0;
        // Metres one tile of the texture covers, per UV axis. 1 is the historic
        // "one texture per metre" -- the unit the projection was always in.
        glm::vec2 size{1.0f, 1.0f};
        // Degrees, turned about the FACE's own centre rather than the world
        // origin: a texture rotated about a point ten metres away does not
        // rotate, it swings off the face.
        float     rotate = 0.0f;
        glm::vec2 offset{0.0f};    // in tiles, added last
        bool      flipU = false, flipV = false;

        bool isDefault() const {
            return axis == 0 && size.x == 1.0f && size.y == 1.0f &&
                   rotate == 0.0f && offset.x == 0.0f && offset.y == 0.0f &&
                   !flipU && !flipV;
        }
    };
    std::vector<FaceUV> faceUV;

    // The placement of face `f`, the default where the mesh carries none. Read
    // through this rather than indexing `faceUV`, which is empty on a mesh whose
    // texture nobody has moved.
    FaceUV faceUv(int f) const {
        return (f >= 0 && f < static_cast<int>(faceUV.size())) ? faceUV[f]
                                                               : FaceUV{};
    }
    // Bring `faceUV` up to one entry per face. Cheap and idempotent.
    void syncFaceUv() { faceUV.resize(faces.size(), FaceUV{}); }
    void setFaceUv(int f, const FaceUV& u);
    // Has anyone moved a texture on this mesh? Decides whether the scene file
    // carries the placements at all.
    bool unwrapped() const;

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

// --- Vertices and edges -------------------------------------------------------
// The same small toolbox one level down. A vertex is an index into `verts`, an
// edge is the pair of corners at its ends (in either order -- the two faces that
// share it list it opposite ways round). Every operation that can make a face
// collapse drops that face and any corner nobody uses any more, keeping `paint`,
// `faceMat` and `faceUV` in step, exactly as deleteFace does.

// One edge of the mesh and the (at most two) faces it borders; f1 is -1 on the
// mesh's border. Listed once each, in the order the faces first name them.
struct EdgeInfo {
    int a = -1, b = -1;
    int f0 = -1, f1 = -1;
};
std::vector<EdgeInfo> edges(const EditMesh& m);

// Apply `xform` to each of these corners once, duplicates and out-of-range
// indices ignored. What the gizmo drives in vertex and edge mode; transformFace
// is this with a face's own corners.
void transformVerts(EditMesh& m, const std::vector<int>& idx, const glm::mat4& xform);

// Collapse these corners into one at their centre (paint averaged). Faces that
// shrink below a triangle go. Returns the surviving corner, -1 if fewer than two
// were given or nothing survived.
int mergeVerts(EditMesh& m, const std::vector<int>& idx);

// Remove every face that uses one of these corners, then the corners. Leaves a
// hole, like deleteFace.
void deleteVerts(EditMesh& m, const std::vector<int>& idx);

// Put a new corner on edge (a, b), `t` along it measured from `a`, and thread
// it into every face that has the edge -- a quad beside it becomes a five-sided
// face, so nothing cracks open. Returns the new corner, -1 if there is no such
// edge.
int splitEdge(EditMesh& m, int a, int b, float t = 0.5f);

// Remove edge (a, b) by joining the two faces either side of it into one. Only
// an edge with exactly two consistently wound faces can go; a border edge
// cannot. Returns the joined face, -1 when it did not apply.
int dissolveEdge(EditMesh& m, int a, int b);

// A loop cut that runs ACROSS edge (a, b): the ring through the quad beside it
// that crosses this edge, with `t` measured along the edge from its lower-
// numbered corner. Returns the cut face (as loopCut), -1 when no quad borders it.
int loopCutEdge(EditMesh& m, int a, int b, float t);
int loopLengthEdge(const EditMesh& m, int a, int b);

// --- Several faces at once (EditMeshTools.cpp) ------------------------------
// The face operations for a selection of faces. Each one KEEPS THE INDICES of
// the faces it was handed -- the new geometry is appended -- so the host's
// selection is still the same faces afterwards and pressing Extrude twice builds
// two storeys of the same region. They return the first of `faces` (the one the
// caller treats as active), -1 when none of them was a face.

// Extrude the faces as one REGION: where two picked faces share an edge no wall
// grows between them, and a corner they share moves once. Each corner moves
// along the average of the picked normals around it, lengthened so every face
// ends up `dist` out along its own normal (a box's top and front pulled out
// together stay square).
int extrudeFaces(EditMesh& m, const std::vector<int>& faces, float dist);
// Slide them along those same directions, neighbours stretching to follow.
int moveFaces(EditMesh& m, const std::vector<int>& faces, float dist);
// Scale each connected group of them about ITS OWN centre: five windows on two
// walls shrink in place, instead of all drifting towards a point between them.
int scaleFaces(EditMesh& m, const std::vector<int>& faces, float factor);
// Inset each face on its own.
int insetFaces(EditMesh& m, const std::vector<int>& faces, float amount);
// Subdivide each quad among them into four (other faces are left alone).
int subdivideFaces(EditMesh& m, const std::vector<int>& faces);
// Remove them all, leaving holes.
void deleteFaces(EditMesh& m, const std::vector<int>& faces);
// Turn them inside out: reverse the winding, so the normal points the other way.
// The cure for a face that is dark from outside or invisible from one side.
int flipFaces(EditMesh& m, const std::vector<int>& faces);

// --- Making faces --------------------------------------------------------------

// A new face through these corners (three or more). They are ordered round their
// centre in the plane they lie closest to, so the order they were picked in does
// not matter; it is wound to agree with the faces it borders, or -- touching
// none -- to face away from the middle of the mesh. Returns the new face, -1 when
// the corners are fewer than three, in a line, or already a face.
int makeFace(EditMesh& m, const std::vector<int>& verts);

// Close the hole whose rim runs through border edge (a, b): the whole rim is
// walked, so one picked edge is enough. Returns the new face, -1 when (a, b) is
// not on a border or the rim does not close.
int fillHole(EditMesh& m, int a, int b);

// Split the face that has both corners a and b in two along a new edge between
// them. The corners must not be neighbours already. Returns the half that keeps
// the face's index, -1 when no face has both.
int connectVerts(EditMesh& m, int a, int b);

// Merge those of these corners that lie within `dist` of each other ("merge by
// distance"): the tidy-up after snapping corners together by hand. Returns how
// many corners went.
int weldVerts(EditMesh& m, const std::vector<int>& idx, float dist);

// Bevel these edges: each is replaced by a strip `width` wide -- flat with one
// segment, rounded with more -- and the corners they meet at are cut back to
// fit. Only edges with a face on both sides can be bevelled. The width is
// measured along the neighbouring edges and held below half of the shortest,
// so two bevels on one face never cross. Returns how many edges were bevelled.
int bevelEdges(EditMesh& m, const std::vector<std::pair<int, int>>& edges,
               float width, int segments);

// --- For the Blender-style modelling mode (ModelingKeys.cpp) ------------------

// Pull these edges out into new quads (Blender's E on edges): every corner of
// them gets a copy, and each edge a quad joining it to its copy. The copies sit
// where the originals are -- the caller moves them. Returns the new edges, in
// the order given, as (copy of a, copy of b).
std::vector<std::pair<int, int>> extrudeEdges(EditMesh& m,
                                              const std::vector<std::pair<int, int>>& edges);

// Copy these faces with corners of their own (Shift+D). Returns the copies'
// indices, in the order given; they sit exactly on the originals.
std::vector<int> duplicateFaces(EditMesh& m, const std::vector<int>& faces);

// Make the winding agree across every edge, then turn each connected piece so
// its faces look outward ("Recalculate Outside"). `faces` empty means all of
// them. Returns how many faces were flipped.
int recalcNormals(EditMesh& m, const std::vector<int>& faces);

// The faces of the band a loop cut would split (Alt+click in face mode).
std::vector<int> ringFaces(const EditMesh& m, int face, int dir);

// The edge loop through edge (a, b) (Alt+click in edge mode): on through every
// corner where four edges meet, taking the edge that shares no face with the
// one it came along -- or, from a border edge, round the border. Each edge as
// (lower, higher) corner.
std::vector<std::pair<int, int>> edgeLoop(const EditMesh& m, int a, int b);

// --- Spin and copies (EditMeshSweep.cpp) ------------------------------------
// Operations Blender has as tools rather than keys. Each takes its transforms
// already in MESH space: the panel works them out in the world (an axis, the
// 3D cursor, a spline), and conjugating by the object's transform is its job,
// not the mesh's.

// Sweep these edges round, `steps` times the transform `step` (spinStep builds
// one): a profile turned on a lathe. Between every edge and its next position
// lies a quad. Edges that share corners sweep as one strip, wound one way along
// it -- taken from the face beside it where the strip runs along a border, so
// the new surface continues that face instead of meeting it back to front.
//
// A corner the step does not move (one ON the axis) is not copied: the quads
// beside it close into triangles, a cone's tip rather than a pinch of slivers.
// `close` joins the last step back to the edges themselves (a full turn) instead
// of laying new corners on top of the first ones.
//
// Returns where the profile ended up -- the last copies of the edges, (lower,
// higher) corner -- or the edges themselves after a closed turn. Empty when no
// edge could be swept.
std::vector<std::pair<int, int>> spinEdges(EditMesh& m,
                                           const std::vector<std::pair<int, int>>& edges,
                                           const glm::mat4& step, int steps, bool close);

// One step of a spin, in mesh space: `angle` radians (the WHOLE turn, split
// over `steps`) about the world line through `centre` along `axis`, plus `rise`
// metres (also the whole) along that axis -- a screw, a spiral stair. `model`
// is mesh -> world. With `rise` 0 and a full turn the sweep closes (see
// spinCloses).
glm::mat4 spinStep(const glm::mat4& model, const glm::vec3& centre, const glm::vec3& axis,
                   float angle, float rise, int steps);
bool spinCloses(float angle, float rise);

// Copies of these faces, one set per transform (mesh space), each with corners
// of its own. A mirroring transform winds its copy the other way round, so a
// mirrored copy still looks out. Returns the new faces, set after set.
std::vector<int> duplicateFacesAt(EditMesh& m, const std::vector<int>& faces,
                                  const std::vector<glm::mat4>& xforms);

// `count` places spread evenly along a polyline (world space, e.g. a spline
// path's centreline): both ends included on an open line, the seam not repeated
// on a closed one. Each is a frame -- origin on the line and, with `align`, X
// along the line and Y as close to up as the slope allows; without it, the
// world's own axes. Empty when the line is shorter than two points.
std::vector<glm::mat4> framesAlong(const std::vector<glm::vec3>& line, bool closed,
                                   int count, bool align);

// Length of a polyline (closing segment included when `closed`).
float lineLength(const std::vector<glm::vec3>& line, bool closed);

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

// The texture coordinate of every corner of `face`, in the face's own loop
// order -- the same numbers buildGroups uploads.
//
// One function rather than two, because the UV panel DRAWS these and the
// renderer TEXTURES with them: two implementations of the same projection agree
// until the day one of them is fixed, and then the picture you are editing is
// not the picture you are looking at.
std::vector<glm::vec2> faceUvs(const EditMesh& m, int face);

// The same, with a placement the mesh does not carry yet. This is what lets the
// UV panel show a size or a rotation while it is still being chosen: it draws
// the result of the number under the cursor, and commits one undo step when the
// hand comes off the slider.
std::vector<glm::vec2> faceUvs(const EditMesh& m, int face,
                               const EditMesh::FaceUV& placement);

// Triangulate for the GPU, split by face material: flat-shaded (one normal per
// face, so an extruded box has crisp edges instead of a smoothed blob) with the
// per-face planar projection faceUvs() derives.
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
