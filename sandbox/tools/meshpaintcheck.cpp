// The mesh-paint check: does a stroke on a modelled object land where it was
// aimed, stay bounded, and survive everything done to the mesh afterwards?
//
// Mesh paint is two arrays that have to stay in step: a corner in EditMesh::verts
// and its weights in EditMesh::paint. Nothing about a desync is visible when it
// happens -- an extrude that forgets to carry the weights along, a deleteFace
// that renumbers one array and not the other, and the mesh still draws. The
// paint simply turns up on the wrong faces some strokes later, by which point
// the operation that did it is far behind. That is the failure this measures.
//
// The other half is the brush's bound. Paint lives on corners, so the brush
// splits the faces it crosses -- and a split under a HELD mouse button, running
// at frame rate, is exactly the shape of a runaway. It has to stop.
//
// Console program, like shadercheck and autosavecheck, and for the same reason:
// the editor is /SUBSYSTEM:WINDOWS in Release and has nowhere to print to.
//   build/release/bin/meshpaintcheck.exe
// Exits non-zero if any check fails.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "../src/Component.hpp"
#include "../src/MeshPaint.hpp"

namespace {

int failures = 0;
int checks   = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

// The identity placement: a mesh authored at world scale, sitting at the origin.
// Most checks want the brush measured in the mesh's own metres.
const glm::mat4 kIdentity(1.0f);

// How many corners carry any weight at all.
int paintedCorners(const EditMesh& m) {
    int n = 0;
    for (const glm::vec4& w : m.paint)
        if (w.x > 0.0f || w.y > 0.0f || w.z > 0.0f || w.w > 0.0f) ++n;
    return n;
}

bool inStep(const EditMesh& m) { return m.paint.size() == m.verts.size(); }

// Does corner `i` carry anything?
bool cornerPainted(const EditMesh& m, std::size_t i) {
    const glm::vec4 w = m.paintAt(static_cast<int>(i));
    return w.x > 0.0f || w.y > 0.0f || w.z > 0.0f || w.w > 0.0f;
}

// --- 1. The two arrays stay one thing ---------------------------------------
// Every operation that adds or drops a corner has to move its weights with it.
// This walks the whole toolbox over a painted mesh and insists on that after
// each one -- the check that would have caught a new operation added later
// without the paint half.

void checkTopology() {
    std::printf("\nCorners and weights stay in step\n");

    EditMesh m = EditMesh::box(glm::vec3(0.5f));
    check(inStep(m), "a fresh box has weights for every corner");

    meshpaint::dab(m, kIdentity, glm::vec3(0.0f, 0.0f, 0.5f), 1.5f, 0, 1.0f, false);
    check(paintedCorners(m) > 0, "a dab lands on corners");

    const int before = paintedCorners(m);
    editmesh::extrude(m, 0, 0.4f);
    check(inStep(m), "extrude carries the weights");
    check(paintedCorners(m) >= before,
          "the extruded cap inherits the paint it was pulled out of");

    editmesh::subdivide(m, 1);
    check(inStep(m), "subdivide carries the weights");

    editmesh::inset(m, 2, 0.1f);
    check(inStep(m), "inset carries the weights");

    editmesh::moveFace(m, 3, 0.2f);
    editmesh::scaleFace(m, 3, 0.8f);
    editmesh::transformFace(m, 3, glm::translate(glm::mat4(1.0f), glm::vec3(0.1f)));
    check(inStep(m), "moving corners leaves the weights alone");

    // deleteFace is the one that RENUMBERS, so it is the one that can silently
    // shift every weight one corner along. Mark a single corner with weights
    // nobody else has, delete a face, and go looking for it.
    EditMesh d = EditMesh::box(glm::vec3(0.5f));
    const glm::vec3 markPos = d.verts[4];       // a corner of the +Z face
    meshpaint::dab(d, kIdentity, markPos, 0.2f, 1, 1.0f, false); // only that corner
    const glm::vec4 markW = d.paintAt(4);
    check(markW.y > 0.9f && paintedCorners(d) == 1, "one corner marked, alone");
    editmesh::deleteFace(d, 5);                 // -Y -- corner 4 survives on +Z/-X
    check(inStep(d), "deleteFace renumbers both arrays");
    // Find where that corner ended up and confirm its weights came along.
    int moved = -1;
    for (std::size_t i = 0; i < d.verts.size(); ++i)
        if (glm::length(d.verts[i] - markPos) < 1e-5f) moved = static_cast<int>(i);
    check(moved >= 0 && glm::length(d.paintAt(moved) - markW) < 1e-5f,
          "a surviving corner keeps its own weights, not its neighbour's");
}

// --- 2. The brush -----------------------------------------------------------

void checkBrush() {
    std::printf("\nThe brush\n");

    // Aim at the +Z face of a 1 m box from straight ahead.
    EditMesh m = EditMesh::box(glm::vec3(0.5f));
    meshpaint::Hit hit;
    const bool hitFront = meshpaint::pick(m, kIdentity, glm::vec3(0.0f, 0.0f, 5.0f),
                                          glm::vec3(0.0f, 0.0f, -1.0f), hit);
    check(hitFront, "a ray at the box finds a face");
    check(hitFront && std::abs(hit.world.z - 0.5f) < 1e-3f,
          "...the NEAR one, at the surface");
    meshpaint::Hit miss;
    check(!meshpaint::pick(m, kIdentity, glm::vec3(0.0f, 0.0f, 5.0f),
                           glm::vec3(0.0f, 1.0f, 0.0f), miss),
          "a ray past the box misses");

    // A brush finer than the face has to split it, or the stroke is the whole
    // face. This is the difference between painting and recolouring.
    const int facesBefore = static_cast<int>(m.faces.size());
    meshpaint::refine(m, kIdentity, hit.world, 0.25f, 0.15f, 4000);
    check(static_cast<int>(m.faces.size()) > facesBefore,
          "a fine brush splits the face it is over");

    // ...but only under the brush. The far side of the box is not touched.
    bool farSideIntact = false;
    for (const std::vector<int>& f : m.faces) {
        if (f.size() != 4) continue;
        glm::vec3 c(0.0f);
        for (int i : f) c += m.verts[i];
        c *= 0.25f;
        if (c.z < -0.49f) farSideIntact = true; // the -Z face, still one quad
    }
    check(farSideIntact, "the far side of the box is left whole");

    // Held down: the split has to stop somewhere.
    EditMesh held = EditMesh::box(glm::vec3(0.5f));
    for (int i = 0; i < 400; ++i)
        meshpaint::refine(held, kIdentity, glm::vec3(0.0f, 0.0f, 0.5f), 0.4f, 0.001f, 600);
    check(static_cast<int>(held.faces.size()) <= 600 + 3,
          "a held brush stops splitting at the cap");
}

// --- 3. What a stroke is worth ----------------------------------------------

void checkWeights() {
    std::printf("\nWhat a stroke leaves behind\n");

    EditMesh m = EditMesh::box(glm::vec3(0.5f));
    const glm::vec3 c(0.0f, 0.0f, 0.5f);
    meshpaint::refine(m, kIdentity, c, 0.4f, 0.2f, 4000);
    for (int i = 0; i < 40; ++i)  // a stroke held over one spot
        meshpaint::dab(m, kIdentity, c, 0.4f, 2, 0.25f, false);

    float best = 0.0f, others = 0.0f, worst = 0.0f;
    for (const glm::vec4& w : m.paint) {
        best   = std::max(best, w.z);                          // slot 2
        others = std::max(others, std::max(w.x, std::max(w.y, w.w)));
        worst  = std::max(worst, w.x + w.y + w.z + w.w);
    }
    check(best > 0.9f, "the painted layer converges on the corner under the brush");
    check(others < 0.05f, "the other three fade out of the way");
    check(worst <= 1.0f + 1e-4f, "the four weights never sum past one");

    // Outside the brush nothing moves -- a stroke on one face is not a stroke on
    // the object.
    bool backClean = true;
    for (std::size_t i = 0; i < m.verts.size(); ++i)
        if (m.verts[i].z < -0.49f && cornerPainted(m, i)) backClean = false;
    check(backClean, "the far side stays unpainted");

    // Erase takes it back off, and back to nothing rather than back to some
    // other layer -- the object's own material is what shows through.
    for (int i = 0; i < 80; ++i)
        meshpaint::dab(m, kIdentity, c, 0.4f, 0, 0.25f, true);
    check(paintedCorners(m) == 0, "erase puts the material back");

    // A scaled object paints with the brush the user sees, not one stretched by
    // its scale: five metres across, a 0.4 m brush still covers 0.4 m of it.
    EditMesh big = EditMesh::box(glm::vec3(0.5f));
    const glm::mat4 scaled = glm::scale(glm::mat4(1.0f), glm::vec3(5.0f));
    meshpaint::refine(big, scaled, glm::vec3(0.0f, 0.0f, 2.5f), 0.4f, 0.2f, 4000);
    meshpaint::dab(big, scaled, glm::vec3(0.0f, 0.0f, 2.5f), 0.4f, 0, 1.0f, false);
    float reach = 0.0f;
    for (std::size_t i = 0; i < big.verts.size(); ++i) {
        const glm::vec4& w = big.paint[i];
        if (w.x <= 0.0f && w.y <= 0.0f && w.z <= 0.0f && w.w <= 0.0f) continue;
        reach = std::max(reach, glm::length(glm::vec3(scaled * glm::vec4(big.verts[i], 1.0f))
                                            - glm::vec3(0.0f, 0.0f, 2.5f)));
    }
    check(reach > 0.0f && reach <= 0.4f + 1e-3f,
          "on a scaled object the brush is still the radius that was asked for");
}

// --- 4. Onto the GPU and into the scene file --------------------------------

void checkCarry() {
    std::printf("\nThe weights reach the shader and the file\n");

    EditMesh m = EditMesh::box(glm::vec3(0.5f));
    for (int i = 0; i < 10; ++i)  // held until the layer has taken over
        meshpaint::dab(m, kIdentity, glm::vec3(0.0f, 0.0f, 0.5f), 1.5f, 3, 1.0f, false);
    const fitzel::MeshData d = editmesh::build(m);
    float maxW = 0.0f;
    for (const fitzel::Vertex& v : d.vertices) maxW = std::max(maxW, v.paint.w);
    check(maxW > 0.9f, "build() puts the weights on the vertices the shader reads");

    // Round trip through the scene file. Sparse on purpose, so a mesh nobody
    // painted adds nothing to the file at all.
    MeshComponent unpainted;
    nlohmann::json ju;
    unpainted.save(ju);
    check(!ju.contains("paint"), "an unpainted mesh writes no paint at all");

    MeshComponent painted;
    painted.mesh = m;
    nlohmann::json jp;
    painted.save(jp);
    check(jp.contains("paint"), "a painted mesh writes its weights");

    MeshComponent back;
    back.load(jp);
    check(back.mesh.verts.size() == m.verts.size(), "the geometry comes back");
    check(inStep(back.mesh), "...with weights for every corner");
    bool same = true;
    for (std::size_t i = 0; i < m.paint.size() && same; ++i)
        if (glm::length(back.mesh.paintAt(static_cast<int>(i)) - m.paint[i]) > 2e-3f)
            same = false;
    check(same, "and every corner's weights are the ones that went in");

    // The undo stack compares components by their saved form, so a stroke has to
    // be visible there or painting would not be undoable.
    MeshComponent clean;
    clean.mesh = EditMesh::box(glm::vec3(0.5f));
    nlohmann::json jc;
    clean.save(jc);
    check(jc != jp, "a stroke is a change the undo stack can see");
}

// --- 5. The slots belong to the object --------------------------------------
// Weights are indices; without what they point AT they mean nothing. The four
// slots have to travel with the mesh -- through the scene file, and through the
// copy an undo snapshot or a prefab instance is made of -- or a painted object
// carried anywhere would come out wearing whatever it found there.

void checkSlots() {
    std::printf("\nThe slots travel with the object\n");

    MeshComponent m;
    const fitzel::AssetId a = fitzel::AssetId::generate();
    const fitzel::AssetId b = fitzel::AssetId::generate();
    m.paintSlots[0] = {a, 0.25f};
    m.paintSlots[2] = {b, 1.5f};
    meshpaint::dab(m.mesh, kIdentity, glm::vec3(0.0f, 0.0f, 0.5f), 1.5f, 2, 1.0f, false);

    nlohmann::json j;
    m.save(j);
    check(j.contains("paintSlots"), "a filled slot is written");

    MeshComponent back;
    back.load(j);
    check(back.paintSlots[0].material == a && back.paintSlots[2].material == b,
          "both slots come back pointing at the same materials");
    check(std::abs(back.paintSlots[0].scale - 0.25f) < 1e-4f &&
              std::abs(back.paintSlots[2].scale - 1.5f) < 1e-4f,
          "...at the size they were painted at");
    check(!back.paintSlots[1].material.valid() && !back.paintSlots[3].material.valid(),
          "the empty slots stay empty");

    // A component nobody filled writes nothing -- an old scene, or a mesh that
    // was only ever modelled, does not grow a paint block.
    MeshComponent bare;
    nlohmann::json jb;
    bare.save(jb);
    check(!jb.contains("paintSlots"), "an unpainted mesh writes no slots");

    // clone() is what the undo stack and the prefab instancer copy through.
    const std::unique_ptr<ComponentBase> copy = m.clone();
    const auto* mc = dynamic_cast<const MeshComponent*>(copy.get());
    check(mc && mc->paintSlots[2].material == b,
          "a copy of the component carries the slots with it");

    // Choosing a material is a change the undo stack must see, like a stroke.
    MeshComponent other = m;
    other.paintSlots[0].material = fitzel::AssetId::generate();
    nlohmann::json jo;
    other.save(jo);
    check(jo != j, "filling a slot is a change the undo stack can see");
}

} // namespace

int main() {
    std::printf("Mesh paint check\n");
    checkTopology();
    checkBrush();
    checkWeights();
    checkCarry();
    checkSlots();
    std::printf("\n%d check(s), %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
