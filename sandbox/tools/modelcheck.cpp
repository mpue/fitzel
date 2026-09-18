// The modelling check: does a loop cut come out as one cut, and does a material
// put on one face stay on that face?
//
// Both of these fail quietly. A loop cut that loses track of which end of an
// edge it is measuring from still produces a mesh -- every face splits, nothing
// crashes, the box still draws as a box -- and the new edge zigzags around it
// instead of running straight, which you find out much later while trying to bend
// a wall along it. A face material is one array beside another (EditMesh::faceMat
// beside EditMesh::faces), so an operation that adds or drops a face without
// moving the materials with it puts brick on the roof three edits afterwards,
// long after the operation that did it.
//
// So this measures what a screenshot cannot: that the cut is planar and closed,
// that the corner count is the one a single ring of new corners implies, and that
// materials survive every operation in the toolbox plus a trip through the scene
// file.
//
// Console program, like meshpaintcheck and shadercheck, and for the same reason:
// the editor is /SUBSYSTEM:WINDOWS in Release and has nowhere to print to.
//   build/release/bin/modelcheck.exe
// Exits non-zero if any check fails.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "../src/Component.hpp"

namespace {

int failures = 0;
int checks   = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

// A surface is closed when every edge is shared by exactly two faces. This is the
// one property a loop cut must not break: it splits faces in pairs across shared
// edges, and a cut that gave one side of an edge a new corner and the other side
// none would leave a crack -- invisible head-on, and a hole to see the inside of
// the moment the light moves.
bool closed(const EditMesh& m) {
    std::map<std::pair<int, int>, int> edges;
    for (const std::vector<int>& f : m.faces)
        for (std::size_t i = 0; i < f.size(); ++i) {
            const int a = f[i], b = f[(i + 1) % f.size()];
            ++edges[{std::min(a, b), std::max(a, b)}];
        }
    for (const auto& e : edges)
        if (e.second != 2) return false;
    return true;
}

// No two corners in the same place. A cut that made its new corner once per FACE
// instead of once per EDGE passes every other check here and leaves the halves
// meeting at doubled corners, which come apart as soon as either side is moved.
bool noDoubledVerts(const EditMesh& m) {
    for (std::size_t i = 0; i < m.verts.size(); ++i)
        for (std::size_t k = i + 1; k < m.verts.size(); ++k)
            if (glm::length(m.verts[i] - m.verts[k]) < 1e-5f) return false;
    return true;
}

// The corners a cut added, in order.
std::vector<glm::vec3> addedVerts(const EditMesh& before, const EditMesh& after) {
    std::vector<glm::vec3> out;
    for (std::size_t i = before.verts.size(); i < after.verts.size(); ++i)
        out.push_back(after.verts[i]);
    return out;
}

// Do these points lie on one plane perpendicular to an axis -- that is, do they
// all share one x, one y or one z? That is what "the cut is a line around the
// mesh" means on a box, and its opposite is the zigzag a mis-oriented walk cuts.
bool flatOnSomeAxis(const std::vector<glm::vec3>& p, float& value, int& axis) {
    if (p.size() < 2) return false;
    for (int a = 0; a < 3; ++a) {
        float lo = p[0][a], hi = p[0][a];
        for (const glm::vec3& v : p) { lo = std::min(lo, v[a]); hi = std::max(hi, v[a]); }
        if (hi - lo < 1e-4f) { value = 0.5f * (lo + hi); axis = a; return true; }
    }
    return false;
}

fitzel::AssetId idA() { return fitzel::AssetId{1, 2}; }
fitzel::AssetId idB() { return fitzel::AssetId{3, 4}; }

// --- 1. One cut, all the way round ------------------------------------------

void checkLoopCut() {
    std::printf("\nA loop cut is one cut, right around\n");

    const EditMesh box = EditMesh::box(glm::vec3(0.5f));
    check(editmesh::loopLength(box, 0, 0) == 4,
          "a box's face lies in a band of four");
    check(editmesh::loopLength(box, 0, 1) == 4,
          "...in both directions (a box has two through every face)");

    EditMesh m = box;
    const int keep = editmesh::loopCut(m, 0, 0, 0.5f);
    check(keep == 0, "the cut face keeps the selection");
    check(m.faces.size() == 10, "four faces became eight, two were left alone");
    check(m.verts.size() == 12, "four new corners: one per crossed EDGE, not per face");
    check(closed(m), "the box is still closed");
    check(noDoubledVerts(m), "no two corners ended up in the same place");

    // The whole point of carrying the reference end around the walk. Cut at a
    // quarter, not the middle: at 0.5 a zigzag is indistinguishable from a
    // straight cut, because both halves of every edge are the same length.
    EditMesh q = box;
    editmesh::loopCut(q, 0, 0, 0.25f);
    const std::vector<glm::vec3> nw = addedVerts(box, q);
    float value = 0.0f;
    int   axis  = -1;
    check(nw.size() == 4, "a quarter-way cut adds the same four corners");
    check(flatOnSomeAxis(nw, value, axis),
          "and they lie on ONE plane -- the cut is a line, not a zigzag");
    check(axis >= 0 && std::fabs(std::fabs(value) - 0.25f) < 1e-4f,
          "at the quarter it was asked for (0.25 of a 1 m box from the side)");

    // The bounds are untouched: a cut adds an edge, it does not move the shape.
    glm::vec3 bmn, bmx, mn, mx;
    box.bounds(bmn, bmx);
    q.bounds(mn, mx);
    check(glm::length(mn - bmn) < 1e-5f && glm::length(mx - bmx) < 1e-5f,
          "the shape itself did not move");

    // Twice in a row: the second cut runs in the band the first one left.
    EditMesh tw = m;
    editmesh::loopCut(tw, 0, 0, 0.5f);
    check(tw.faces.size() == 14 && closed(tw), "a second cut leaves it closed too");

    // The other direction of the SAME face crosses the cut just made.
    EditMesh cross = m;
    editmesh::loopCut(cross, 0, 1, 0.5f);
    check(closed(cross), "cutting across the first cut leaves it closed");

    // Nothing to ring: the cut declines rather than inventing a split.
    EditMesh tri = box;
    tri.faces[0] = {4, 5, 6};                     // a triangle where a quad was
    check(editmesh::loopCut(tri, 0, 0, 0.5f) == -1, "an n-gon has no band");
    EditMesh nofa = box;
    check(editmesh::loopCut(nofa, 99, 0, 0.5f) == -1 &&
          editmesh::loopCut(nofa, -1, 0, 0.5f) == -1,
          "and neither has a face that isn't there");

    // An open shape: the band stops at the hole instead of walking into it.
    EditMesh open = box;
    editmesh::deleteFace(open, 4);                // lift the lid off
    const int len = editmesh::loopLength(open, 0, 0);
    check(len >= 2 && len <= 4, "an open shape rings the part of it that is a band");
    EditMesh cut = open;
    check(editmesh::loopCut(cut, 0, 0, 0.5f) >= 0 &&
          cut.faces.size() == open.faces.size() + static_cast<std::size_t>(len),
          "...and splits exactly the faces it said it would");

    // Paint rides along: the new corners take what the shader would have shown
    // there anyway, so cutting a painted wall does not change how it looks.
    EditMesh pm = box;
    pm.syncPaint();
    for (int i : pm.faces[0]) pm.paint[i] = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    editmesh::loopCut(pm, 0, 0, 0.5f);
    check(pm.paint.size() == pm.verts.size(), "the cut carries the paint weights");
    bool painted = false;
    for (std::size_t i = 12 - 4; i < pm.verts.size(); ++i)
        painted = painted || pm.paintAt(static_cast<int>(i)).x > 0.4f;
    check(painted, "...and the new corners inherit what was painted around them");
}

// --- 2. A material on one face stays on that face ---------------------------

void checkFaceMaterials() {
    std::printf("\nA face wears its own material, and keeps it\n");

    EditMesh m = EditMesh::box(glm::vec3(0.5f));
    check(!m.dressed(), "a fresh box wears the object's material everywhere");
    check(editmesh::buildGroups(m).size() == 1,
          "...and draws as one piece, as it always did");

    m.setFaceMaterial(2, idA());
    check(m.faceMaterial(2) == idA(), "a face takes the material it was given");
    check(!m.faceMaterial(0).valid(), "its neighbours are left alone");
    check(m.dressed(), "the mesh now says it is dressed");

    std::vector<editmesh::Group> g = editmesh::buildGroups(m);
    check(g.size() == 2, "which makes it two pieces to draw");
    check(!g[0].material.valid(), "the object's own material comes first");
    std::size_t total = 0;
    for (const editmesh::Group& p : g) total += p.data.vertices.size();
    check(total == editmesh::build(m).vertices.size(),
          "the pieces add up to the whole mesh -- no triangle lost, none doubled");
    check(g[1].data.vertices.size() == 6, "the dressed face is one quad's worth");

    // Two faces, one material: one extra piece, not two.
    m.setFaceMaterial(3, idA());
    check(editmesh::buildGroups(m).size() == 2,
          "two faces in the same material share one piece");
    m.setFaceMaterial(3, idB());
    check(editmesh::buildGroups(m).size() == 3, "a second material is a third piece");

    // Every face dressed: the object's own piece would be empty, and an empty
    // draw call is a draw call that paints nothing.
    EditMesh all = EditMesh::box(glm::vec3(0.5f));
    for (int f = 0; f < 6; ++f) all.setFaceMaterial(f, idA());
    check(editmesh::buildGroups(all).size() == 1,
          "a fully dressed mesh has no empty piece for the object's material");

    // The parallel-array failure, one array over from the paint. Each operation
    // has to move the materials with the faces it adds or drops.
    EditMesh e = EditMesh::box(glm::vec3(0.5f));
    e.setFaceMaterial(0, idA());
    editmesh::extrude(e, 0, 0.4f);
    check(e.faceMat.size() == e.faces.size(), "extrude keeps the arrays in step");
    check(e.faceMaterial(0) == idA(), "the cap keeps the material");
    int walls = 0;
    for (std::size_t f = 6; f < e.faces.size(); ++f)
        if (e.faceMaterial(static_cast<int>(f)) == idA()) ++walls;
    check(walls == 4, "and the four new walls are grown in it, not in bare grey");

    EditMesh sd = EditMesh::box(glm::vec3(0.5f));
    sd.setFaceMaterial(1, idB());
    editmesh::subdivide(sd, 1);
    check(sd.faceMat.size() == sd.faces.size(), "subdivide keeps them in step");
    int quarters = 0;
    for (int f = 0; f < static_cast<int>(sd.faces.size()); ++f)
        if (sd.faceMaterial(f) == idB()) ++quarters;
    check(quarters == 4, "all four quarters wear what the face wore");

    EditMesh lc = EditMesh::box(glm::vec3(0.5f));
    lc.setFaceMaterial(0, idA());
    editmesh::loopCut(lc, 0, 0, 0.5f);
    check(lc.faceMat.size() == lc.faces.size(), "the loop cut keeps them in step");
    int halves = 0;
    for (int f = 0; f < static_cast<int>(lc.faces.size()); ++f)
        if (lc.faceMaterial(f) == idA()) ++halves;
    check(halves == 2, "both halves of a cut face wear its material");

    // Delete: the materials have to shift with the faces, or every face after the
    // deleted one inherits its neighbour's -- the quiet one.
    EditMesh dl = EditMesh::box(glm::vec3(0.5f));
    dl.setFaceMaterial(4, idA());
    const std::vector<int> kept = dl.faces[4];
    editmesh::deleteFace(dl, 1);
    check(dl.faceMat.size() == dl.faces.size(), "delete keeps them in step");
    int found = -1;
    for (int f = 0; f < static_cast<int>(dl.faces.size()); ++f)
        if (dl.faces[f].size() == kept.size() && dl.faceMaterial(f) == idA()) found = f;
    check(found >= 0, "the dressed face still wears its material after a delete");
    int dressedCount = 0;
    for (int f = 0; f < static_cast<int>(dl.faces.size()); ++f)
        if (dl.faceMaterial(f).valid()) ++dressedCount;
    check(dressedCount == 1, "...and no other face picked one up on the way");
}

// --- 3. Into the scene file and back ----------------------------------------

void checkCarry() {
    std::printf("\nThe dressing survives the scene file\n");

    MeshComponent plain;
    nlohmann::json jp;
    plain.save(jp);
    check(!jp.contains("faceMats"),
          "an undressed mesh writes no face materials at all");

    MeshComponent dressed;
    dressed.mesh.setFaceMaterial(2, idA());
    dressed.mesh.setFaceMaterial(5, idB());
    nlohmann::json jd;
    dressed.save(jd);
    check(jd.contains("faceMats"), "a dressed one writes them");
    check(jd != jp, "which is a change the undo stack can see");

    MeshComponent back;
    back.load(jd);
    check(back.mesh.faces.size() == dressed.mesh.faces.size(), "the geometry comes back");
    check(back.mesh.faceMaterial(2) == idA() && back.mesh.faceMaterial(5) == idB(),
          "...wearing the same materials on the same faces");
    check(!back.mesh.faceMaterial(0).valid() && !back.mesh.faceMaterial(3).valid(),
          "and the rest still wear the object's");

    // A cut mesh through the file, geometry and dressing together.
    MeshComponent cut;
    cut.mesh.setFaceMaterial(0, idA());
    editmesh::loopCut(cut.mesh, 0, 0, 0.3f);
    nlohmann::json jc;
    cut.save(jc);
    MeshComponent cback;
    cback.load(jc);
    check(cback.mesh.faces.size() == cut.mesh.faces.size() &&
          cback.mesh.verts.size() == cut.mesh.verts.size(),
          "a cut mesh comes back with every face and corner");
    check(closed(cback.mesh), "...still closed");
    int same = 0;
    for (int f = 0; f < static_cast<int>(cut.mesh.faces.size()); ++f)
        if (cback.mesh.faceMaterial(f) == cut.mesh.faceMaterial(f)) ++same;
    check(same == static_cast<int>(cut.mesh.faces.size()),
          "...and every face wearing what it wore");
}

// --- 4. Where each face's texture sits ---------------------------------------
//
// EditMesh::faceUV is the same hazard as faceMat one array further over, with
// one difference that makes it worse: a texture placement that slipped onto the
// wrong face still LOOKS like a texture. There is no hole and no missing brick,
// just a course running the wrong way on one wall, which reads as something the
// author did.
//
// And the first thing to prove is not that the new field works but that it
// changes nothing until asked: every mesh in every existing scene carries no
// placements at all, and has to come out of faceUvs() with the exact numbers the
// old planar projection gave it.

void checkFaceUv() {
    std::printf("\nTexture placement per face\n");

    EditMesh m = EditMesh::box(glm::vec3(0.5f));
    check(m.faceUV.empty(), "a fresh mesh carries no placements");
    check(!m.unwrapped(), "...and says so");

    // The default has to reproduce the projection this mesh was authored with:
    // metres in the plane the face faces. A face of a unit box is one metre
    // across, so it spans exactly one tile.
    const std::vector<glm::vec2> d = editmesh::faceUvs(m, 0);
    check(d.size() == m.faces[0].size(), "every corner gets one");
    glm::vec2 lo = d[0], hi = d[0];
    for (const glm::vec2& q : d) { lo = glm::min(lo, q); hi = glm::max(hi, q); }
    check(std::abs((hi.x - lo.x) - 1.0f) < 1e-4f &&
          std::abs((hi.y - lo.y) - 1.0f) < 1e-4f,
          "a one-metre face spans one tile at the default size");

    // Size is metres per tile, so doubling it halves the span.
    EditMesh::FaceUV u;
    u.size = glm::vec2(2.0f);
    const std::vector<glm::vec2> h = editmesh::faceUvs(m, 0, u);
    glm::vec2 hlo = h[0], hhi = h[0];
    for (const glm::vec2& q : h) { hlo = glm::min(hlo, q); hhi = glm::max(hhi, q); }
    check(std::abs((hhi.x - hlo.x) - 0.5f) < 1e-4f,
          "a two-metre tile puts half a tile on it");

    // A quarter turn about the face's own centre must not move the face off its
    // texture -- that is the whole reason the rotation is anchored there.
    EditMesh::FaceUV r;
    r.rotate = 90.0f;
    const std::vector<glm::vec2> t = editmesh::faceUvs(m, 0, r);
    glm::vec2 dc(0.0f), tc(0.0f);
    for (const glm::vec2& q : d) dc += q;
    for (const glm::vec2& q : t) tc += q;
    dc /= static_cast<float>(d.size());
    tc /= static_cast<float>(t.size());
    check(glm::length(dc - tc) < 1e-4f, "a rotation turns the face in place");

    // Every operation that grows the mesh has to carry the placement, or the new
    // faces start their texture over while their neighbours keep theirs.
    EditMesh g = EditMesh::box(glm::vec3(0.5f));
    EditMesh::FaceUV brick;
    brick.size   = glm::vec2(0.25f);
    brick.rotate = 30.0f;
    g.setFaceUv(0, brick);
    editmesh::extrude(g, 0, 0.5f);
    check(g.faceUV.size() == g.faces.size(), "extrude keeps the arrays in step");
    int carried = 0;
    for (int f = 0; f < static_cast<int>(g.faces.size()); ++f)
        if (g.faceUv(f).size.x == 0.25f && g.faceUv(f).rotate == 30.0f) ++carried;
    check(carried == 5, "the cap and its four walls inherit the placement");

    EditMesh c = EditMesh::box(glm::vec3(0.5f));
    c.setFaceUv(0, brick);
    editmesh::loopCut(c, 0, 0, 0.3f);
    check(c.faceUV.size() == c.faces.size(), "a loop cut keeps them in step");
    check(c.faceUv(0).rotate == 30.0f, "the near half keeps the placement");
    // Two faces carry it and no more: the cut splits the whole band, so the far
    // half is somewhere among the appended faces rather than at the end of them,
    // and what matters is that exactly the two halves of the placed face have it
    // and none of the band's other new halves picked it up.
    int halves = 0;
    for (int f = 0; f < static_cast<int>(c.faces.size()); ++f)
        if (c.faceUv(f).rotate == 30.0f) ++halves;
    check(halves == 2, "...and so does the far half, and nothing else does");

    EditMesh s2 = EditMesh::box(glm::vec3(0.5f));
    s2.setFaceUv(3, brick);
    const EditMesh::FaceUV kept = s2.faceUv(3);
    editmesh::deleteFace(s2, 1);
    check(s2.faceUV.size() == s2.faces.size(), "delete keeps them in step");
    int placedFaces = 0;
    for (int f = 0; f < static_cast<int>(s2.faces.size()); ++f)
        if (!s2.faceUv(f).isDefault()) ++placedFaces;
    check(placedFaces == 1, "exactly one face still carries a placement");
    check(s2.faceUv(2).size == kept.size && s2.faceUv(2).rotate == kept.rotate,
          "...and it is the one that had it, renumbered with the faces");

    // Through the scene file.
    MeshComponent plain;
    nlohmann::json jp;
    plain.save(jp);
    check(!jp.contains("faceUVs"), "an unplaced mesh writes no placements at all");

    MeshComponent placedMesh;
    placedMesh.mesh.setFaceUv(2, brick);
    EditMesh::FaceUV axis;
    axis.axis   = 2;
    axis.flipV  = true;
    axis.offset = glm::vec2(0.25f, -0.5f);
    placedMesh.mesh.setFaceUv(4, axis);
    nlohmann::json jd;
    placedMesh.save(jd);
    check(jd.contains("faceUVs"), "a placed one writes them");

    MeshComponent back;
    back.load(jd);
    check(back.mesh.faceUv(2).size == brick.size &&
          back.mesh.faceUv(2).rotate == brick.rotate,
          "size and rotation come back on the same face");
    check(back.mesh.faceUv(4).axis == 2 && back.mesh.faceUv(4).flipV &&
          !back.mesh.faceUv(4).flipU &&
          std::abs(back.mesh.faceUv(4).offset.y + 0.5f) < 1e-4f,
          "so do the axis, the mirror and the offset");
    check(back.mesh.faceUv(0).isDefault() && back.mesh.faceUv(5).isDefault(),
          "and the faces nobody placed are still at the default");
}

} // namespace

// --- 5. Corners and edges -----------------------------------------------------

void checkVertsEdges() {
    std::printf("\nCorners and edges\n");

    const EditMesh box = EditMesh::box(glm::vec3(0.5f));
    const std::vector<editmesh::EdgeInfo> es = editmesh::edges(box);
    bool twoFaces = es.size() == 12;
    for (const editmesh::EdgeInfo& e : es) twoFaces = twoFaces && e.f0 >= 0 && e.f1 >= 0;
    check(twoFaces, "a box has twelve edges, each between two faces");

    EditMesh tv = box;
    editmesh::transformVerts(tv, {1, 1, 99, -3},
                             glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
    check(std::fabs(tv.verts[1].x - (box.verts[1].x + 1.0f)) < 1e-5f &&
          glm::length(tv.verts[0] - box.verts[0]) < 1e-6f,
          "a corner listed twice moves once; bad indices are ignored");

    // Split: the corner goes into BOTH faces beside the edge, or the surface cracks.
    EditMesh sp = box;
    const int nv = editmesh::splitEdge(sp, 4, 5, 0.5f);
    int fives = 0;
    for (const std::vector<int>& f : sp.faces) if (f.size() == 5) ++fives;
    check(nv == 8 && fives == 2, "splitting an edge puts one corner into both its faces");
    check(closed(sp) && sp.paint.size() == sp.verts.size(), "...and the box stays closed");
    check(std::fabs(sp.verts[nv].x) < 1e-5f, "...in the middle of the edge");
    check(editmesh::splitEdge(sp, 0, 6, 0.5f) == -1, "no edge, no split (a diagonal is not one)");

    // Merge.
    EditMesh mg = box;
    mg.setFaceMaterial(3, idA());
    const int kept = editmesh::mergeVerts(mg, {4, 5});
    check(kept >= 0 && mg.verts.size() == 7 && mg.faces.size() == 6,
          "merging two corners leaves seven, and every face still has three");
    check(closed(mg), "...and a closed shape");
    check(mg.faceMat.size() == mg.faces.size() && mg.faceUV.size() == mg.faces.size() &&
          mg.paint.size() == mg.verts.size(), "...with every parallel array in step");
    check(std::fabs(mg.verts[kept].x) < 1e-5f, "the merged corner sits between the two");

    EditMesh py = box;
    editmesh::mergeVerts(py, {4, 5, 6, 7});
    check(py.faces.size() == 5 && py.verts.size() == 5 && closed(py),
          "merging a face's four corners collapses it: a box becomes a pyramid");
    int dressed = 0;
    EditMesh pyd = box;
    pyd.setFaceMaterial(2, idB());
    editmesh::mergeVerts(pyd, {4, 5, 6, 7});
    for (int f = 0; f < static_cast<int>(pyd.faces.size()); ++f)
        if (pyd.faceMaterial(f) == idB()) ++dressed;
    check(dressed == 1, "...and the side that wore a material still wears it");

    // Delete corners.
    EditMesh dv = box;
    editmesh::deleteVerts(dv, {6});
    check(dv.faces.size() == 3 && dv.verts.size() == 7,
          "deleting a corner takes the three faces round it");

    // Dissolve.
    EditMesh ds = box;
    ds.setFaceMaterial(0, idA());
    const int joined = editmesh::dissolveEdge(ds, 7, 6);
    check(joined >= 0 && ds.faces.size() == 5 && ds.faces[joined].size() == 6,
          "dissolving an edge joins its two faces into one six-cornered face");
    check(closed(ds), "...and the surface stays closed");
    check(ds.faceMaterial(joined) == idA() && ds.faceMat.size() == ds.faces.size(),
          "...wearing the first face's material");
    EditMesh op = box;
    editmesh::deleteFace(op, 4);
    const std::vector<editmesh::EdgeInfo> oe = editmesh::edges(op);
    int border = -1;
    for (int k = 0; k < static_cast<int>(oe.size()) && border < 0; ++k)
        if (oe[k].f1 < 0) border = k;
    check(border >= 0 && editmesh::dissolveEdge(op, oe[border].a, oe[border].b) == -1,
          "a border edge has nothing to join and is left alone");

    // Loop cut across an edge: t runs along THAT edge from its lower corner,
    // whichever side of the quad it is on and whichever way round it is named.
    auto cutAt = [&](int a, int b, float t, float& value, int& axis) {
        EditMesh lc = box;
        const bool ok = editmesh::loopCutEdge(lc, a, b, t) >= 0 && closed(lc);
        return ok && flatOnSomeAxis(addedVerts(box, lc), value, axis);
    };
    float value = 0.0f;
    int   axis  = -1;
    check(editmesh::loopLengthEdge(box, 4, 5) == 4, "the ring across an edge of a box is four");
    check(cutAt(4, 5, 0.25f, value, axis) && axis == 0 && std::fabs(value + 0.25f) < 1e-4f,
          "a cut across edge 4-5 at 0.25 falls a quarter from corner 4");
    check(cutAt(5, 4, 0.25f, value, axis) && axis == 0 && std::fabs(value + 0.25f) < 1e-4f,
          "...the same when the edge is named the other way round");
    check(cutAt(6, 7, 0.25f, value, axis) && axis == 0 && std::fabs(value - 0.25f) < 1e-4f,
          "...and on the far side of the quad, from ITS lower corner (6)");
}

// Every corner inside the box [-h, h]^3 (with a hair of slack): a bevel only
// ever takes material away.
bool inside(const EditMesh& m, float h) {
    for (const glm::vec3& v : m.verts)
        if (std::fabs(v.x) > h + 1e-4f || std::fabs(v.y) > h + 1e-4f || std::fabs(v.z) > h + 1e-4f)
            return false;
    return true;
}

// Consistently wound: every edge is run once each way. A face that came out
// facing inwards passes closed() and fails this -- and shows up as a dark or
// missing face the moment it is lit.
bool oriented(const EditMesh& m) {
    std::map<std::pair<int, int>, int> half;
    for (const std::vector<int>& f : m.faces)
        for (std::size_t i = 0; i < f.size(); ++i)
            ++half[{f[i], f[(i + 1) % f.size()]}];
    for (const auto& h : half)
        if (h.second != 1 || !half.count({h.first.second, h.first.first})) return false;
    return true;
}

// Wound consistently where it is open too: no edge is run twice the same way
// (which is what two neighbouring faces facing opposite ways do).
bool consistent(const EditMesh& m) {
    std::map<std::pair<int, int>, int> half;
    for (const std::vector<int>& f : m.faces)
        for (std::size_t i = 0; i < f.size(); ++i)
            if (++half[{f[i], f[(i + 1) % f.size()]}] > 1) return false;
    return true;
}

bool arraysInStep(const EditMesh& m) {
    return m.faceMat.size() == m.faces.size() && m.faceUV.size() == m.faces.size() &&
           m.paint.size() == m.verts.size();
}

void checkNewTools() {
    std::printf("\nSeveral faces, making faces, bevel\n");
    // Faces of the unit box: 0 +Z {4,5,6,7}, 1 -Z, 2 +X, 3 -X, 4 +Y {3,7,6,2}, 5 -Y.
    const EditMesh box = EditMesh::box(glm::vec3(0.5f));

    // Region extrude: top and front pulled out together stay one piece.
    EditMesh rx = box;
    rx.setFaceMaterial(4, idA());
    const int keep = editmesh::extrudeFaces(rx, {4, 0}, 0.5f);
    check(keep == 4, "a region extrude keeps the active face first");
    check(rx.faces.size() == 12, "top + front out together: six walls round the rim, none between");
    check(closed(rx) && noDoubledVerts(rx), "...closed, no doubled corners");
    bool topUp = true, frontOut = true;
    for (int v : rx.faces[4]) topUp = topUp && std::fabs(rx.verts[v].y - 1.0f) < 1e-4f;
    for (int v : rx.faces[0]) frontOut = frontOut && std::fabs(rx.verts[v].z - 1.0f) < 1e-4f;
    check(topUp && frontOut, "...and each face went its full 0.5 m along its own normal");
    check(arraysInStep(rx), "...with the parallel arrays in step");

    // Move two opposite faces: the box gets wider on both sides.
    EditMesh mv = box;
    editmesh::moveFaces(mv, {2, 3}, 0.25f);
    glm::vec3 mn, mx;
    mv.bounds(mn, mx);
    check(std::fabs(mx.x - mn.x - 1.5f) < 1e-4f && std::fabs(mx.y - mn.y - 1.0f) < 1e-4f,
          "moving +X and -X out by 0.25 makes the box 1.5 wide and no taller");

    // Scale two faces that do not touch: each about its own centre.
    EditMesh sc = box;
    const glm::vec3 c2 = sc.faceCenter(2), c3 = sc.faceCenter(3);
    editmesh::scaleFaces(sc, {2, 3}, 0.5f);
    check(glm::length(sc.faceCenter(2) - c2) < 1e-5f && glm::length(sc.faceCenter(3) - c3) < 1e-5f,
          "faces that do not touch scale in place, each about its own centre");

    // Delete several.
    EditMesh df = box;
    editmesh::deleteFaces(df, {4, 5, 4});
    check(df.faces.size() == 4 && df.verts.size() == 8 && arraysInStep(df),
          "deleting top and bottom (one named twice) leaves the four sides");

    // Flip.
    EditMesh fl = box;
    editmesh::flipFaces(fl, {0});
    check(glm::dot(fl.faceNormal(0), glm::vec3(0, 0, 1)) < -0.99f, "a flipped face looks the other way");
    check(fl.faces[0][0] == box.faces[0][0], "...and keeps its first corner");

    // Fill a hole from one of its edges.
    EditMesh fh = box;
    editmesh::deleteFace(fh, 4);
    check(!closed(fh), "a box with its top taken off is open");
    const int filled = editmesh::fillHole(fh, 7, 6);
    check(filled >= 0 && closed(fh), "fill hole from one rim edge closes it again");
    check(filled >= 0 && glm::dot(fh.faceNormal(filled), glm::vec3(0, 1, 0)) > 0.99f,
          "...with the new face looking out, not in");
    EditMesh whole = box;
    check(editmesh::fillHole(whole, 7, 6) == -1, "an edge with faces on both sides is no rim");

    // Make a face from corners picked in any order.
    EditMesh mf = box;
    editmesh::deleteFace(mf, 4);
    const int made = editmesh::makeFace(mf, {6, 3, 2, 7});
    check(made >= 0 && closed(mf), "a face through four picked corners closes the top");
    check(made >= 0 && mf.faces[made].size() == 4 &&
              glm::dot(mf.faceNormal(made), glm::vec3(0, 1, 0)) > 0.99f,
          "...wound to agree with its neighbours, whatever order they were picked in");
    check(editmesh::makeFace(mf, {6, 3, 2, 7}) == -1, "...and not a second time");
    EditMesh loose;
    loose.verts = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}};
    loose.syncPaint();
    check(editmesh::makeFace(loose, {0, 1, 2}) == -1, "three corners in a line make no face");

    // Connect two corners across a face.
    EditMesh cn = box;
    const int half = editmesh::connectVerts(cn, 4, 6);
    check(half == 0 && cn.faces.size() == 7 && closed(cn) && arraysInStep(cn),
          "connecting opposite corners of a quad cuts it into two triangles");
    check(editmesh::connectVerts(cn, 4, 5) == -1, "neighbouring corners are already joined");

    // Weld: a zero-distance extrude doubles four corners; welding undoes it.
    EditMesh wd = box;
    editmesh::extrude(wd, 0, 0.0f);
    std::vector<int> all;
    for (int i = 0; i < static_cast<int>(wd.verts.size()); ++i) all.push_back(i);
    const int gone = editmesh::weldVerts(wd, all, 1e-3f);
    check(gone == 4 && wd.verts.size() == 8 && wd.faces.size() == 6 && closed(wd),
          "welding a flat extrude takes the four doubles and the four flat walls");
    check(arraysInStep(wd), "...with the arrays in step");

    // Bevel one edge (top-front, 6-7): a flat chamfer.
    EditMesh b1 = box;
    b1.setFaceMaterial(4, idA());
    check(editmesh::bevelEdges(b1, {{6, 7}}, 0.1f, 1) == 1, "one edge bevels");
    check(b1.faces.size() == 7 && b1.verts.size() == 10, "...into one strip and two new corners each end");
    check(closed(b1) && noDoubledVerts(b1) && inside(b1, 0.5f), "...closed, and only material taken away");
    check(arraysInStep(b1), "...with the arrays in step");
    // Rounded.
    EditMesh b3 = box;
    check(editmesh::bevelEdges(b3, {{6, 7}}, 0.2f, 3) == 1 && b3.faces.size() == 9 && closed(b3),
          "three segments: three strips, the end faces take the curve");
    check(inside(b3, 0.5f) && noDoubledVerts(b3), "...inside the box, no doubled corners");
    // The four top edges: two bevels meet at every top corner.
    EditMesh bt = box;
    check(editmesh::bevelEdges(bt, {{3, 7}, {7, 6}, {6, 2}, {2, 3}}, 0.1f, 1) == 4 &&
              bt.faces.size() == 10 && closed(bt) && inside(bt, 0.5f),
          "the four top edges: four strips meeting in mitres, closed");
    EditMesh bt2 = box;
    check(editmesh::bevelEdges(bt2, {{3, 7}, {7, 6}, {6, 2}, {2, 3}}, 0.1f, 2) == 4 &&
              closed(bt2) && noDoubledVerts(bt2),
          "...rounded, the strips share the curve they meet along");
    // Every edge: three meet at each corner, and each corner gets a patch.
    std::vector<std::pair<int, int>> every;
    for (const editmesh::EdgeInfo& e : editmesh::edges(box)) every.push_back({e.a, e.b});
    EditMesh ba = box;
    check(editmesh::bevelEdges(ba, every, 0.1f, 1) == 12 && ba.faces.size() == 26 && closed(ba),
          "all twelve edges: 6 faces + 12 strips + 8 corner patches, closed");
    check(inside(ba, 0.5f) && noDoubledVerts(ba) && arraysInStep(ba), "...inside the box, arrays in step");
    EditMesh ba2 = box;
    check(editmesh::bevelEdges(ba2, every, 0.1f, 2) == 12 && ba2.faces.size() == 38 && closed(ba2),
          "...rounded with two segments: 24 strips, still closed");
    EditMesh bw = box;
    editmesh::bevelEdges(bw, {{6, 7}}, 5.0f, 1);
    check(closed(bw) && inside(bw, 0.5f), "a bevel far wider than the box is held back, not inverted");
    check(oriented(rx) && oriented(fh) && oriented(mf) && oriented(cn) && oriented(wd) &&
              oriented(b1) && oriented(b3) && oriented(bt) && oriented(bt2) &&
              oriented(ba) && oriented(ba2) && oriented(bw),
          "every result is wound consistently (each edge run once each way)");
    EditMesh bo = box;
    editmesh::deleteFace(bo, 4);
    check(editmesh::bevelEdges(bo, {{6, 7}}, 0.1f, 1) == 0, "a border edge is not bevelled");
}

// Edges run by one face only: the rim of an open mesh.
int borderEdges(const EditMesh& m) {
    int n = 0;
    for (const editmesh::EdgeInfo& e : editmesh::edges(m)) n += e.f1 < 0 ? 1 : 0;
    return n;
}

void checkBlenderOps() {
    std::printf("\nThe modelling mode's operations\n");
    const EditMesh box = EditMesh::box(glm::vec3(0.5f));

    // Extrude edges: the rim of an open box pulled up into a taller wall.
    EditMesh open = box;
    editmesh::deleteFace(open, 4);
    const std::vector<std::pair<int, int>> rim = {{3, 7}, {7, 6}, {6, 2}, {2, 3}};
    EditMesh ex = open;
    const auto made = editmesh::extrudeEdges(ex, rim);
    check(made.size() == 4 && ex.faces.size() == 9 && ex.verts.size() == 12,
          "extruding the four rim edges makes four quads and four new corners");
    std::vector<int> moved;
    for (const auto& e : made) { moved.push_back(e.first); moved.push_back(e.second); }
    editmesh::transformVerts(ex, moved, glm::translate(glm::mat4(1.0f), glm::vec3(0, 0.5f, 0)));
    check(consistent(ex) && borderEdges(ex) == 4, "...wound with their neighbours, the rim moved up with them");
    check(arraysInStep(ex), "...with the arrays in step");

    // Duplicate.
    EditMesh dp = box;
    const std::vector<int> copies = editmesh::duplicateFaces(dp, {0, 4});
    check(copies.size() == 2 && dp.faces.size() == 8 && dp.verts.size() == 8 + 6,
          "duplicating two touching faces copies them with six corners of their own");
    check(glm::length(dp.faceNormal(copies[0]) - dp.faceNormal(0)) < 1e-5f &&
              glm::length(dp.faceCenter(copies[1]) - dp.faceCenter(4)) < 1e-5f,
          "...sitting exactly on the originals, facing the same way");

    // Recalculate outside.
    EditMesh bad = box;
    editmesh::flipFaces(bad, {0, 3});
    check(!oriented(bad), "a box with two faces turned round is inconsistent");
    const int turned = editmesh::recalcNormals(bad, {});
    check(turned == 2 && oriented(bad), "recalculate turns exactly those two back");
    check(glm::dot(bad.faceNormal(4), glm::vec3(0, 1, 0)) > 0.99f, "...and the box looks outward");
    EditMesh inside = box;
    editmesh::flipFaces(inside, {0, 1, 2, 3, 4, 5});
    check(editmesh::recalcNormals(inside, {}) == 6 &&
              glm::dot(inside.faceNormal(4), glm::vec3(0, 1, 0)) > 0.99f,
          "a box turned inside out is turned right way out");

    // Rings and loops.
    check(editmesh::ringFaces(box, 0, 0).size() == 4, "the ring through a box face is four faces");
    check(editmesh::edgeLoop(box, 4, 5).size() == 1,
          "a box corner has three edges, so an edge loop stops there at once");
    EditMesh cut = box;
    editmesh::loopCut(cut, 0, 0, 0.5f);
    const int nv = static_cast<int>(box.verts.size());
    check(editmesh::edgeLoop(cut, nv, nv + 1).size() == 4 ||
              editmesh::edgeLoop(cut, nv, nv + 2).size() == 4 ||
              editmesh::edgeLoop(cut, nv, nv + 3).size() == 4,
          "the loop a cut made runs all four edges round the box");
    check(editmesh::edgeLoop(open, 7, 6).size() == 4, "from a border edge, the loop follows the border");
}

int main() {
    std::printf("modelcheck -- loop cut, per-face materials and texture placement\n");
    checkLoopCut();
    checkFaceMaterials();
    checkCarry();
    checkFaceUv();
    checkVertsEdges();
    checkNewTools();
    checkBlenderOps();
    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
