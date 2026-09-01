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

} // namespace

int main() {
    std::printf("modelcheck -- loop cut and per-face materials\n");
    checkLoopCut();
    checkFaceMaterials();
    checkCarry();
    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
