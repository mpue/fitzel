// The UV check: does an unwrapped mesh keep its texture coordinates through
// every edit, and do the UV editor's operations do what Blender's do?
//
// Both fail quietly. A coordinate list beside a corner list that an edit forgot
// to carry is a texture that is right until the third loop cut and smeared
// after it; an unwrap that is a little wrong is still a picture, just one with
// bent bricks. So this measures: after an edit, every unwrapped face's
// coordinates still the affine image of its corners they started as; after an
// unwrap, every triangle's angles what they are in 3D; after a pack, every
// island inside the tile and none on top of another.
//
// Console program, like modelcheck.
//   build/linux/bin/uvcheck
// Exits non-zero if any check fails.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../src/Component.hpp"
#include "../src/EditMeshModifiers.hpp"
#include "../src/UvOps.hpp"

namespace {

int failures = 0;
int checks   = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

std::vector<int> allFaces(const EditMesh& m) {
    std::vector<int> f(m.faces.size());
    for (std::size_t i = 0; i < f.size(); ++i) f[i] = static_cast<int>(i);
    return f;
}

// Every unwrapped face's coordinates against its planar projection now: on a
// flat face that started as its projection, an edit that carried them right
// leaves them equal to it. Returns the worst difference.
float driftFromProjection(const EditMesh& m) {
    float worst = 0.0f;
    for (int f = 0; f < static_cast<int>(m.faces.size()); ++f) {
        if (!m.hasCornerUvs(f)) continue;
        EditMesh::FaceUV plain = m.faceUv(f);
        plain.corners.clear();
        const std::vector<glm::vec2> p = editmesh::faceUvs(m, f, plain);
        for (std::size_t k = 0; k < p.size(); ++k)
            worst = std::max(worst, glm::length(p[k] - m.faceUV[static_cast<std::size_t>(f)].corners[k].uv));
    }
    return worst;
}

int cornered(const EditMesh& m) {
    int n = 0;
    for (int f = 0; f < static_cast<int>(m.faces.size()); ++f) n += m.hasCornerUvs(f);
    return n;
}

// The worst difference, in radians, between a triangle's angles in 3D and in
// UV, over the fan triangles of these faces.
float angleError(const EditMesh& m, const std::vector<int>& faces) {
    auto angles = [](glm::vec3 a, glm::vec3 b, glm::vec3 c, float out[3]) {
        auto ang = [](glm::vec3 p, glm::vec3 q, glm::vec3 r) {
            const glm::vec3 u = glm::normalize(q - p), v = glm::normalize(r - p);
            return std::acos(std::clamp(glm::dot(u, v), -1.0f, 1.0f));
        };
        out[0] = ang(a, b, c); out[1] = ang(b, c, a); out[2] = ang(c, a, b);
    };
    float worst = 0.0f;
    for (int f : faces) {
        const std::vector<glm::vec2> uv = editmesh::faceUvs(m, f);
        const std::vector<int>& fv = m.faces[static_cast<std::size_t>(f)];
        for (std::size_t t = 1; t + 1 < fv.size(); ++t) {
            float a3[3], a2[3];
            angles(m.verts[fv[0]], m.verts[fv[t]], m.verts[fv[t + 1]], a3);
            angles(glm::vec3(uv[0], 0), glm::vec3(uv[t], 0), glm::vec3(uv[t + 1], 0), a2);
            for (int j = 0; j < 3; ++j) worst = std::max(worst, std::abs(a3[j] - a2[j]));
        }
    }
    return worst;
}

// UV area per square metre of every face, as the spread max / min.
float densitySpread(const EditMesh& m, const std::vector<int>& faces) {
    float lo = 1e30f, hi = 0.0f;
    for (int f : faces) {
        const std::vector<glm::vec2> uv = editmesh::faceUvs(m, f);
        float a = 0.0f;
        for (std::size_t i = 0; i < uv.size(); ++i) {
            const glm::vec2 p = uv[i], q = uv[(i + 1) % uv.size()];
            a += p.x * q.y - q.x * p.y;
        }
        const float d = std::abs(0.5f * a) / std::max(m.faceArea(f), 1e-9f);
        lo = std::min(lo, d);
        hi = std::max(hi, d);
    }
    return lo > 0.0f ? hi / lo : 1e30f;
}

// Inside the tile, and no two islands' boxes overlapping.
bool packedCleanly(const EditMesh& m, const std::vector<int>& faces) {
    const uvops::View v = uvops::view(m, faces);
    std::vector<std::pair<glm::vec2, glm::vec2>> boxes;
    for (const std::vector<int>& isl : uvops::islands(v)) {
        glm::vec2 lo(1e30f), hi(-1e30f);
        for (int f : isl)
            for (const glm::vec2& p : v.uv[static_cast<std::size_t>(v.slot[static_cast<std::size_t>(f)])]) {
                lo = glm::min(lo, p);
                hi = glm::max(hi, p);
            }
        if (lo.x < -1e-4f || lo.y < -1e-4f || hi.x > 1.0001f || hi.y > 1.0001f) return false;
        boxes.push_back({lo, hi});
    }
    for (std::size_t i = 0; i < boxes.size(); ++i)
        for (std::size_t j = i + 1; j < boxes.size(); ++j) {
            const auto& a = boxes[i];
            const auto& b = boxes[j];
            const bool apart = a.second.x <= b.first.x + 1e-5f || b.second.x <= a.first.x + 1e-5f ||
                               a.second.y <= b.first.y + 1e-5f || b.second.y <= a.first.y + 1e-5f;
            if (!apart) return false;
        }
    return true;
}

// A flat grid of n x n quads in the XZ plane, `size` metres across.
EditMesh grid(int n, float size) {
    EditMesh m;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            m.verts.push_back({size * (float(x) / n - 0.5f), 0.0f, size * (float(z) / n - 0.5f)});
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const int a = z * (n + 1) + x;
            m.faces.push_back({a, a + n + 1, a + n + 2, a + 1});   // +Y, counter-clockwise from above
        }
    m.syncPaint();
    return m;
}

} // namespace

int main() {
    std::printf("[uvcheck] corner coordinates through the edits\n");
    {
        EditMesh m = EditMesh::box(glm::vec3(1.0f, 0.5f, 0.75f));
        uvops::ensureCorners(m, allFaces(m));
        check(cornered(m) == 6 && driftFromProjection(m) < 1e-5f, "a box unwrapped by its projection");
        editmesh::subdivide(m, 0);
        check(cornered(m) == 9 && driftFromProjection(m) < 1e-5f, "subdivide splits the coordinates with the face");
        editmesh::loopCut(m, 2, 0, 0.3f);
        check(driftFromProjection(m) < 1e-5f, "a loop cut cuts them at the same fraction");
        const int a = m.faces[3][0], b = m.faces[3][1];
        editmesh::splitEdge(m, a, b, 0.25f);
        check(driftFromProjection(m) < 1e-5f, "splitting an edge puts the new corner's coordinate between");
        const int ex = editmesh::extrude(m, 4, 0.6f);
        check(m.hasCornerUvs(ex) && driftFromProjection(m) < 1e-5f, "extrude: the cap keeps its coordinates, the walls are projected");
        const int before = cornered(m);
        editmesh::inset(m, 5, 0.1f);
        check(cornered(m) == before && driftFromProjection(m) < 1e-5f, "inset: the middle keeps the part of the texture it covers");
        const std::vector<glm::vec2> pre = editmesh::faceUvs(m, 1);
        const std::vector<int> preLoop = m.faces[1];
        editmesh::flipFaces(m, {1});
        const std::vector<glm::vec2> post = editmesh::faceUvs(m, 1);
        bool same = true;
        for (std::size_t k = 0; k < preLoop.size(); ++k) {
            const std::size_t j = static_cast<std::size_t>(std::find(m.faces[1].begin(), m.faces[1].end(), preLoop[k]) - m.faces[1].begin());
            same = same && glm::length(pre[k] - post[j]) < 1e-6f;
        }
        check(same, "flipping a face keeps every corner's coordinate on that corner");
    }
    {
        // Two coplanar halves joined again: the coordinates join with them.
        EditMesh m = grid(2, 2.0f);
        uvops::ensureCorners(m, allFaces(m));
        const std::vector<int>& f0 = m.faces[0];
        int ea = -1, eb = -1;
        for (std::size_t k = 0; k < f0.size(); ++k)
            for (int v : m.faces[1])
                if (v == f0[k] && std::count(m.faces[1].begin(), m.faces[1].end(), f0[(k + 1) % f0.size()])) {
                    ea = f0[k];
                    eb = f0[(k + 1) % f0.size()];
                }
        const int joined = editmesh::dissolveEdge(m, ea, eb);
        check(joined >= 0 && m.hasCornerUvs(joined) && driftFromProjection(m) < 1e-5f, "dissolving an edge joins the coordinates too");
        std::vector<int> two{m.faces[0][0], m.faces[0][1]};
        const std::size_t n0 = m.faces[0].size();
        const glm::vec2 kept = m.faceUV[0].corners[0].uv;
        editmesh::mergeVerts(m, two);
        // The corner that stays keeps its coordinate (the texture stretches to
        // the moved corner, as in Blender); every face still one per corner.
        check(cornered(m) == static_cast<int>(m.faces.size()) && m.faces[0].size() == n0 - 1 &&
                  glm::length(m.faceUV[0].corners[0].uv - kept) < 1e-6f,
              "merging corners squeezes the coordinates with the loop");
    }
    {
        // Seams follow their corners.
        EditMesh m = EditMesh::box(glm::vec3(0.5f));
        const int a = m.faces[0][0], b = m.faces[0][1];
        m.setSeam(a, b, true);
        check(m.isSeam(b, a) && m.seams.size() == 1, "a seam, either way round");
        const int v = editmesh::splitEdge(m, a, b, 0.5f);
        check(!m.isSeam(a, b) && m.isSeam(a, v) && m.isSeam(v, b), "splitting a seam edge leaves both halves seams");
        editmesh::deleteFace(m, 5);
        check(m.seams.size() >= 1, "deleting a face keeps the seams that still have their corners");
        const int keep = editmesh::mergeVerts(m, {a, v});
        check(!m.isSeam(keep, keep) && std::all_of(m.seams.begin(), m.seams.end(), [&](auto e) {
                  return e.first < static_cast<int>(m.verts.size()) && e.second < static_cast<int>(m.verts.size()) && e.first != e.second;
              }),
              "merging corners renumbers the seams and drops the one in between");
    }
    {
        // Through the scene file.
        MeshComponent mc;
        mc.mesh = grid(2, 2.0f);
        uvops::ensureCorners(mc.mesh, allFaces(mc.mesh));
        mc.mesh.faceUV[1].corners[2].uv = {7.25f, -3.5f};
        mc.mesh.faceUV[1].corners[2].pin = true;
        mc.mesh.setSeam(0, 1, true);
        nlohmann::json j;
        mc.save(j);
        MeshComponent back;
        back.load(nlohmann::json::parse(j.dump()));
        check(back.mesh.hasCornerUvs(1) && glm::length(back.mesh.faceUV[1].corners[2].uv - glm::vec2(7.25f, -3.5f)) < 1e-5f &&
                  back.mesh.faceUV[1].corners[2].pin && cornered(back.mesh) == 4,
              "corner coordinates and pins come back from the scene file");
        check(back.mesh.isSeam(0, 1) && back.mesh.seams.size() == 1, "so do the seams");
        MeshComponent old;
        nlohmann::json legacy = j;
        legacy.erase("faceCornerUVs");
        legacy.erase("seams");
        old.load(legacy);
        check(cornered(old.mesh) == 0 && old.mesh.seams.empty(), "an older file loads projected, without seams");
    }
    {
        // A modifier's copy carries the coordinates; the back of a shell turns them round.
        EditMesh m = grid(1, 1.0f);
        uvops::ensureCorners(m, allFaces(m));
        editmesh::solidify(m, 0.1f, 0.0f, false, false);
        bool ok = m.faces.size() == 2 && m.hasCornerUvs(0) && m.hasCornerUvs(1);
        for (std::size_t k = 0; ok && k < 4; ++k) {
            const int vf = m.faces[0][k];
            // The back corner under the front one.
            for (std::size_t j = 0; j < 4; ++j)
                if (glm::length(glm::vec2(m.verts[m.faces[1][j]].x, m.verts[m.faces[1][j]].z) -
                                glm::vec2(m.verts[vf].x, m.verts[vf].z)) < 1e-5f)
                    ok = ok && glm::length(m.faceUV[1].corners[j].uv - m.faceUV[0].corners[k].uv) < 1e-6f;
        }
        check(ok, "solidify: the back face wears the front's coordinates, corner for corner");
    }

    std::printf("[uvcheck] selecting\n");
    {
        EditMesh m = grid(2, 2.0f);
        uvops::ensureCorners(m, allFaces(m));
        uvops::View v = uvops::view(m, allFaces(m));
        check(uvops::islands(v).size() == 1, "a grid unwrapped by its projection is one island");
        // The middle corner of the grid: four loops in one place.
        const glm::vec2 mid = v.at({0, 2});
        check(uvops::coincident(v, {0, 2}).size() == 4, "sticky: the middle corner is four loops welded");
        uvops::Selection s;
        uvops::click(v, s, uvops::SelectMode::Vertex, mid, 0.05f, false);
        check(s.size() == 4, "clicking it selects all four");
        uvops::click(v, s, uvops::SelectMode::Vertex, mid, 0.05f, true);
        check(s.empty(), "a second toggling click takes them away");
        uvops::click(v, s, uvops::SelectMode::Face, mid + glm::vec2(0.3f, 0.3f), 0.05f, false);
        const std::vector<int> fs = uvops::facesOf(s);
        int whole = 0;
        for (int f : fs) whole += uvops::faceSelected(s, m, f);
        check(whole == 1 && s.size() > 4, "a face click takes the face, and its welded neighbours' corners come along");
        // Cut one face free: move only its own loops.
        uvops::Selection one;
        for (int k = 0; k < 4; ++k) one.insert({3, k});
        uvops::transform(m, one, glm::mat2(1.0f), glm::vec2(0.0f), glm::vec2(1.5f, 0.0f));
        v = uvops::view(m, allFaces(m));
        check(uvops::islands(v).size() == 2, "moving one face's own loops cuts it free: two islands");
        uvops::Selection isl;
        glm::vec2 c0(0.0f);
        for (const glm::vec2& q : v.uv[static_cast<std::size_t>(v.slot[0])]) c0 += q;
        uvops::click(v, isl, uvops::SelectMode::Island, c0 * 0.25f, 0.05f, false);
        check(uvops::facesOf(isl).size() == 3, "an island click takes the three faces still joined");
        uvops::Selection box;
        uvops::selectBox(v, box, uvops::SelectMode::Vertex, glm::vec2(-10.0f), glm::vec2(10.0f), false);
        check(box.size() == 16, "a box round everything takes every loop");
        uvops::invert(v, box);
        check(box.empty(), "inverted, nothing");
        uvops::Selection lk;
        lk.insert({3, 0});
        uvops::selectLinked(v, lk);
        check(lk.size() == 4, "linked from one loop of the free face: that face");
    }

    std::printf("[uvcheck] moving\n");
    {
        EditMesh m = grid(1, 1.0f);
        uvops::ensureCorners(m, allFaces(m));
        uvops::View v = uvops::view(m, allFaces(m));
        uvops::Selection s;
        uvops::selectAll(v, s);
        glm::vec2 lo, hi;
        uvops::bounds(v, s, lo, hi);
        const glm::vec2 c = 0.5f * (lo + hi);
        const float a = glm::radians(90.0f);
        uvops::transform(m, s, glm::mat2(std::cos(a), std::sin(a), -std::sin(a), std::cos(a)), c, glm::vec2(0.0f));
        v = uvops::view(m, allFaces(m));
        glm::vec2 lo2, hi2;
        uvops::bounds(v, s, lo2, hi2);
        check(glm::length(lo2 - lo) < 1e-5f && glm::length(hi2 - hi) < 1e-5f, "a quarter turn about the middle keeps the square's box");
        uvops::transform(m, s, glm::mat2(-1.0f, 0.0f, 0.0f, 1.0f), c, glm::vec2(0.0f));
        check(angleError(m, {0}) < 1e-4f, "mirrored, the angles are the same");
        uvops::Selection two{{0, 0}, {0, 1}};
        v = uvops::view(m, allFaces(m));
        uvops::align(m, v, two, 1);
        check(std::abs(m.faceUV[0].corners[0].uv.y - m.faceUV[0].corners[1].uv.y) < 1e-6f, "align V: the two at one height");
        v = uvops::view(m, allFaces(m));
        uvops::mergeAtCenter(m, v, two);
        check(glm::length(m.faceUV[0].corners[0].uv - m.faceUV[0].corners[1].uv) < 1e-6f, "merged at their middle");
        m.faceUV[0].corners[2].uv = {0.1234f, 0.5678f};
        uvops::snapToPixels(m, {{0, 2}}, 64, 32);
        check(std::abs(m.faceUV[0].corners[2].uv.x * 64.0f - std::round(m.faceUV[0].corners[2].uv.x * 64.0f)) < 1e-4f &&
                  std::abs(m.faceUV[0].corners[2].uv.y * 32.0f - std::round(m.faceUV[0].corners[2].uv.y * 32.0f)) < 1e-4f,
              "snapped onto the texel corners of 64 x 32");
        uvops::setPin(m, {{0, 3}}, true);
        uvops::Selection pins;
        uvops::selectPinned(uvops::view(m, allFaces(m)), pins);
        check(pins.size() == 1 && pins.count({0, 3}), "pinned, and found as pinned");
    }

    std::printf("[uvcheck] unwrap (conformal)\n");
    {
        // A flat grid: a conformal map of a flat thing is the thing itself.
        EditMesh m = grid(3, 3.0f);
        uvops::Selection none;
        int closed = -1;
        const int charts = uvops::unwrap(m, allFaces(m), 0.02f, &closed);
        check(charts == 1 && closed == 0, "a flat grid is one open chart");
        check(angleError(m, allFaces(m)) < 2e-3f, "...unwrapped with every angle kept");
        check(densitySpread(m, allFaces(m)) < 1.01f, "...at one density");
        check(packedCleanly(m, allFaces(m)), "...inside the tile");
    }
    {
        // A cylinder cut open down one side and round both rims: a rectangle
        // and two discs, all at one density.
        EditMesh m = EditMesh::cylinder(glm::vec3(0.5f, 1.0f, 0.5f), 16);
        std::vector<int> side, caps;
        for (int f = 0; f < static_cast<int>(m.faces.size()); ++f)
            (std::abs(m.faceNormal(f).y) < 0.5f ? side : caps).push_back(f);
        for (int f : caps) {
            const std::vector<int>& fv = m.faces[static_cast<std::size_t>(f)];
            for (std::size_t k = 0; k < fv.size(); ++k) m.setSeam(fv[k], fv[(k + 1) % fv.size()], true);
        }
        // One upright edge of the side: shared by two side faces, both ends on the rims.
        int ua = -1, ub = -1;
        {
            const std::vector<int>& fv = m.faces[static_cast<std::size_t>(side[0])];
            for (std::size_t k = 0; k < fv.size() && ua < 0; ++k) {
                const glm::vec3 d = m.verts[fv[(k + 1) % fv.size()]] - m.verts[fv[k]];
                if (std::abs(d.y) > 0.5f) { ua = fv[k]; ub = fv[(k + 1) % fv.size()]; }
            }
        }
        m.setSeam(ua, ub, true);
        int closed = -1;
        const int charts = uvops::unwrap(m, allFaces(m), 0.02f, &closed);
        check(charts == 3 && closed == 0, "a cylinder cut open: three charts, none closed");
        check(angleError(m, side) < 0.02f, "the side unrolls with its angles kept");
        check(densitySpread(m, allFaces(m)) < 1.15f, "side and caps at one density");
        check(packedCleanly(m, allFaces(m)), "packed into the tile, nothing on top of anything");
    }
    {
        EditMesh m = EditMesh::box(glm::vec3(0.5f));
        int closed = -1;
        uvops::unwrap(m, allFaces(m), 0.02f, &closed);
        check(closed == 1, "a whole box without a seam is reported closed");
    }
    {
        // Pins hold: the chart is solved round them and stays where they are.
        EditMesh m = grid(2, 2.0f);
        uvops::ensureCorners(m, allFaces(m));
        m.faceUV[0].corners[0] = {{0.0f, 0.0f}, true};
        m.faceUV[3].corners[2] = {{2.0f, 2.0f}, true};
        uvops::unwrap(m, allFaces(m), 0.02f);
        check(glm::length(m.faceUV[0].corners[0].uv - glm::vec2(0.0f)) < 1e-6f &&
                  glm::length(m.faceUV[3].corners[2].uv - glm::vec2(2.0f)) < 1e-6f && m.faceUV[0].corners[0].pin,
              "pinned corners stay where they were pinned");
        check(angleError(m, allFaces(m)) < 2e-3f, "...and the rest is conformal round them");
    }

    std::printf("[uvcheck] projections and islands\n");
    {
        EditMesh m = EditMesh::box(glm::vec3(1.0f, 0.5f, 0.5f));
        const int n = uvops::smartProject(m, allFaces(m), 66.0f, 0.02f);
        check(n == 6, "smart UV project: a box is six islands");
        check(packedCleanly(m, allFaces(m)), "...packed into the tile");
        check(densitySpread(m, allFaces(m)) < 1.01f, "...at one density");
        check(uvops::seamsFromIslands(m, allFaces(m)) == 12, "seams from islands: the box's twelve edges");
        EditMesh b = EditMesh::box(glm::vec3(0.5f));
        check(uvops::seamsFromSharp(b, allFaces(b), 30.0f) == 12, "seams from sharp edges: twelve too");
        uvops::unwrap(b, allFaces(b), 0.02f);
        check(angleError(b, allFaces(b)) < 1e-3f && packedCleanly(b, allFaces(b)), "unwrapped along them: six squares, packed");
    }
    {
        EditMesh m = EditMesh::box(glm::vec3(1.0f, 0.5f, 0.5f));
        uvops::cubeProject(m, allFaces(m), 2.0f);
        check(cornered(m) == 6 && densitySpread(m, allFaces(m)) < 1.001f, "cube projection: every face at 2 m to the tile");
        uvops::resetUvs(m, {0});
        const auto& c = m.faceUV[0].corners;
        check(c.size() == 4 && glm::length(c[0].uv - glm::vec2(0, 0)) < 1e-6f && glm::length(c[2].uv - glm::vec2(1, 1)) < 1e-6f,
              "reset: the face fills the tile");
        EditMesh::FaceUV place;
        place.size = glm::vec2(0.5f);
        uvops::projectWorld(m, {0}, place);
        check(!m.hasCornerUvs(0) && m.faceUv(0).size.x == 0.5f, "projected in metres again: no coordinates of its own");
    }
    {
        EditMesh m = EditMesh::cylinder(glm::vec3(0.5f, 1.0f, 0.5f), 16);
        std::vector<int> side;
        for (int f = 0; f < static_cast<int>(m.faces.size()); ++f)
            if (std::abs(m.faceNormal(f).y) < 0.5f) side.push_back(f);
        uvops::cylinderProject(m, side);
        float spanMax = 0.0f, umin = 1e30f, umax = -1e30f;
        for (int f : side) {
            float lo = 1e30f, hi = -1e30f;
            for (const auto& k : m.faceUV[static_cast<std::size_t>(f)].corners) {
                lo = std::min(lo, k.uv.x);
                hi = std::max(hi, k.uv.x);
            }
            spanMax = std::max(spanMax, hi - lo);
            umin = std::min(umin, lo);
            umax = std::max(umax, hi);
        }
        check(spanMax < 0.1f && umax - umin > 0.95f, "cylinder projection: once round, no face stretched back across");
        uvops::sphereProject(m, allFaces(m));
        check(cornered(m) == static_cast<int>(m.faces.size()), "sphere projection gives every face coordinates");
    }
    {
        // Pack and average: three squares of different sizes made one density.
        EditMesh m = grid(2, 2.0f);
        uvops::ensureCorners(m, allFaces(m));
        for (int f = 0; f < 4; ++f) {
            uvops::Selection s;
            for (int k = 0; k < 4; ++k) s.insert({f, k});
            uvops::transform(m, s, glm::mat2(0.3f + 0.4f * float(f)), glm::vec2(0.0f), glm::vec2(3.0f * float(f), 0.0f));
        }
        check(uvops::islands(uvops::view(m, allFaces(m))).size() == 4, "four faces cut apart at four sizes");
        uvops::averageScale(m, allFaces(m));
        check(densitySpread(m, allFaces(m)) < 1.01f, "average island scale: one density");
        uvops::pack(m, allFaces(m), 0.02f, true);
        check(packedCleanly(m, allFaces(m)), "packed into the tile, apart");
        const std::vector<float> st = uvops::stretch(uvops::view(m, allFaces(m)));
        check(std::all_of(st.begin(), st.end(), [](float x) { return std::abs(x) < 0.02f; }), "no face stretched against the others");
    }

    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
