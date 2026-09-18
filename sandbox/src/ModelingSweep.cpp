// The Modeling panel's rows for the operations that repeat something: Spin (a
// profile of corners or edges turned round an axis), Duplicate (copies in a
// row) and Duplicate along a spline path. The geometry is EditMeshSweep.cpp;
// this is the part that turns the panel's settings -- a world axis, the 3D
// cursor, a path in the scene -- into the mesh-space transforms it takes.
//
// Like everything on the panel: buttons with numbers, a ghost of the result
// while the pointer rests on a button, one undo step per click. Each operation
// leaves the thing it made picked, so pressing it again carries on -- the next
// quarter of a turn, the next copy in the row.

#include "ModelingPanelParts.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include "Component.hpp"
#include "SplineSystem.hpp"
#include "UiStyle.hpp"

namespace modelui::parts {

namespace {

using modeltools::Amounts;
using modeltools::Mode;
using modeltools::Op;
using EdgeSel = std::pair<int, int>;

constexpr float kDegToRad = 3.14159265f / 180.0f;

EdgeSel ordered(int a, int b) { return {std::min(a, b), std::max(a, b)}; }

glm::vec3 worldOf(const glm::mat4& M, const glm::vec3& p) { return glm::vec3(M * glm::vec4(p, 1.0f)); }

// The profile a spin sweeps: edge mode's edges, or in corner mode every edge
// both of whose ends are picked. (A fitzel mesh has no loose corners, so a
// corner on its own has nothing to sweep into.)
std::vector<EdgeSel> profileEdges(const modeltools::Selection& s, const EditMesh& m) {
    if (s.mode == Mode::Edge) return s.edges;
    std::vector<int> vs = s.verts;
    std::sort(vs.begin(), vs.end());
    std::vector<EdgeSel> out;
    for (const editmesh::EdgeInfo& e : editmesh::edges(m))
        if (std::binary_search(vs.begin(), vs.end(), e.a) &&
            std::binary_search(vs.begin(), vs.end(), e.b))
            out.push_back(ordered(e.a, e.b));
    return out;
}

// What a duplicate copies: the picked faces, or in corner and edge mode every
// face all of whose corners are picked -- the same rule Shift+D uses.
std::vector<int> copiedFaces(const modeltools::Selection& s, const EditMesh& m, int faceSel) {
    if (s.mode == Mode::Face) return modeltools::selectedFaces(s, m, faceSel);
    std::vector<int> vs = modeltools::activeVerts(s, m, faceSel);
    std::sort(vs.begin(), vs.end());
    std::vector<int> out;
    for (int f = 0; f < static_cast<int>(m.faces.size()); ++f) {
        if (!m.validFace(f)) continue;
        bool all = true;
        for (int v : m.faces[f]) all = all && std::binary_search(vs.begin(), vs.end(), v);
        if (all) out.push_back(f);
    }
    return out;
}

// The world-space box around these faces' corners.
void worldBox(const EditMesh& m, const glm::mat4& M, const std::vector<int>& faces,
              glm::vec3& mn, glm::vec3& mx) {
    mn = glm::vec3(1e30f);
    mx = glm::vec3(-1e30f);
    for (int f : faces)
        for (int v : m.faces[f]) {
            const glm::vec3 w = worldOf(M, m.verts[v]);
            mn = glm::min(mn, w);
            mx = glm::max(mx, w);
        }
}

// Pick what an operation made, in the current mode: these faces, their corners
// or their edges. Returns the active face (face mode) or -1.
int pickMade(modeltools::Selection& s, const EditMesh& m, const std::vector<int>& faces) {
    const Mode mode = s.mode;
    s.clear();
    if (mode == Mode::Face) {
        s.faces = faces;
        return faces.empty() ? -1 : faces.front();
    }
    for (int f : faces) {
        const std::vector<int>& fv = m.faces[f];
        for (std::size_t i = 0; i < fv.size(); ++i) {
            if (mode == Mode::Vertex) s.verts.push_back(fv[i]);
            else                      s.edges.push_back(ordered(fv[i], fv[(i + 1) % fv.size()]));
        }
    }
    std::sort(s.verts.begin(), s.verts.end());
    s.verts.erase(std::unique(s.verts.begin(), s.verts.end()), s.verts.end());
    std::sort(s.edges.begin(), s.edges.end());
    s.edges.erase(std::unique(s.edges.begin(), s.edges.end()), s.edges.end());
    return -1;
}

// A small square toggle in a row of choices, the chosen one on the accent.
bool choice(const char* id, picto::Icon icon, bool on, const char* tip) {
    return picto::buttonSized(id, icon, tip, true, on, ImVec2(smallH() * 1.25f, smallH()));
}

// The world axis buttons: a letter on the axis's colour, full for the chosen one.
bool axisButton(int k, bool on) {
    static const ImVec4 col[3] = {ImVec4(0.75f, 0.25f, 0.25f, 1.0f),
                                  ImVec4(0.30f, 0.62f, 0.25f, 1.0f),
                                  ImVec4(0.25f, 0.42f, 0.80f, 1.0f)};
    static const char* const name[3] = {"X##spinax", "Y##spinax", "Z##spinax"};
    static const char* const tip[3]  = {
        "Turn about the world X axis", "Turn about the world Y axis (upright: a lathe)",
        "Turn about the world Z axis"};
    const ImVec4& c = col[k];
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(c.x, c.y, c.z, on ? 0.95f : 0.30f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(c.x, c.y, c.z, on ? 1.0f : 0.6f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, c);
    const bool clicked = ImGui::Button(name[k], ImVec2(smallH() * 1.25f, smallH()));
    ImGui::PopStyleColor(3);
    ImGui::SetItemTooltip("%s", tip[k]);
    return clicked;
}

// --- Spin -------------------------------------------------------------------

void spinRow(const PanelState& s, std::function<void()>& pending) {
    Amounts&         am = modeltools::amounts();
    const EditMesh&  m  = s.mesh->mesh;
    const std::vector<EdgeSel> es = profileEdges(s.sel, m);

    // What the axis runs through, in the world.
    glm::vec3 centre = worldOf(s.model, glm::vec3(0.0f));
    if (am.spinPivot == Amounts::Pivot::Cursor) {
        centre = s.cursor;
    } else if (am.spinPivot == Amounts::Pivot::Selection) {
        const std::vector<int> act = modeltools::activeVerts(s.sel, m, s.faceSel);
        if (!act.empty()) {
            glm::vec3 c(0.0f);
            for (int v : act) c += worldOf(s.model, m.verts[v]);
            centre = c / static_cast<float>(act.size());
        }
    }
    glm::vec3 axis(0.0f);
    axis[std::clamp(am.spinAxis, 0, 2)] = 1.0f;
    const int       steps = std::clamp(static_cast<int>(std::lround(am.spinSteps)), 1, 256);
    const float     angle = am.spinAngle * kDegToRad;
    const float     rise  = am.spinRise;
    const bool      close = editmesh::spinCloses(angle, rise);
    const glm::mat4 step  = editmesh::spinStep(s.model, centre, axis, angle, rise, steps);

    Op op = [es, step, steps, close](EditMesh& mm) {
        editmesh::spinEdges(mm, es, step, steps, close);
        return -1;
    };
    if (opColumn("spin", picto::Icon::Spin, !es.empty(),
                 "Spin\nTurn the picked edges round an axis and lay a surface\n"
                 "where they pass -- a lathe: a vase, a column, a wheel rim.\n"
                 "The number below is the whole turn; a full 360 closes up.\n"
                 "Corners on the axis stay one corner (a cone's tip).\n"
                 "In corner mode, the edges between picked corners turn.\n"
                 "What the turn ends on stays picked: press again to go on.",
                 op, &am.spinAngle, 15.0f, -720.0f, 720.0f, "%.0f\xc2\xb0")) {
        const Mode mode = s.sel.mode;
        pending = [&s, es, step, steps, close, mode]() {
            std::vector<EdgeSel> end;
            s.edit([&](MeshComponent& comp) {
                end = editmesh::spinEdges(comp.mesh, es, step, steps, close);
                return -1;
            }, "Spin");
            if (end.empty()) return;
            s.sel.clear();
            if (mode == Mode::Edge) {
                s.sel.edges = end;
            } else {
                for (const EdgeSel& e : end) { s.sel.verts.push_back(e.first); s.sel.verts.push_back(e.second); }
                std::sort(s.sel.verts.begin(), s.sel.verts.end());
                s.sel.verts.erase(std::unique(s.sel.verts.begin(), s.sel.verts.end()), s.sel.verts.end());
            }
        };
    }
    ImGui::SameLine();
    captionColumn("spinsteps", "steps",
                  "Steps\nHow many pieces the turn is laid in: more is rounder.",
                  am.spinSteps, 1.0f, 1.0f, 128.0f, "%.0f");
    ImGui::SameLine();
    captionColumn("spinrise", "rise",
                  "Rise\nHow far the profile climbs along the axis over the whole\n"
                  "turn: 0 is a lathe, more is a screw -- a spiral stair, a\n"
                  "thread, a coil. A turn that rises never closes.",
                  am.spinRise, 0.1f, -100.0f, 100.0f, "%.2f m");

    // Which axis, and through what: two rows of small choices.
    gap();
    ImGui::BeginGroup();
    for (int k = 0; k < 3; ++k) {
        if (k > 0) ImGui::SameLine(0.0f, 3.0f);
        if (axisButton(k, am.spinAxis == k)) am.spinAxis = k;
    }
    using P = Amounts::Pivot;
    if (choice("pvorigin", picto::Icon::PivotOrigin, am.spinPivot == P::Origin,
               "Turn about the object's origin\n(the middle of the object)"))
        am.spinPivot = P::Origin;
    ImGui::SameLine(0.0f, 3.0f);
    if (choice("pvsel", picto::Icon::PivotSelection, am.spinPivot == P::Selection,
               "Turn about the middle of what is picked"))
        am.spinPivot = P::Selection;
    ImGui::SameLine(0.0f, 3.0f);
    if (choice("pvcursor", picto::Icon::PivotCursor, am.spinPivot == P::Cursor,
               "Turn about the 3D cursor\n(Shift+right-click puts it on the ground; the\n"
               "3D Cursor window sets it to a number)"))
        am.spinPivot = P::Cursor;
    ImGui::EndGroup();
}

// --- Duplicate --------------------------------------------------------------

void duplicateRow(const PanelState& s, const std::vector<int>& fs,
                  std::function<void()>& pending) {
    Amounts&        am = modeltools::amounts();
    const EditMesh& m  = s.mesh->mesh;

    // Each copy one of the selection's own widths on from the last, plus the gap:
    // with no gap they sit edge to edge, which is what a row of windows or a
    // stack of storeys wants without anybody measuring anything.
    const int ax  = std::clamp(am.dupDir, 0, 5) / 2;
    const float sgn = (am.dupDir % 2) ? 1.0f : -1.0f;
    float stride = 0.0f;
    if (!fs.empty()) {
        glm::vec3 mn, mx;
        worldBox(m, s.model, fs, mn, mx);
        stride = (mx[ax] - mn[ax]) + am.dupGap;
    }
    const bool can = !fs.empty() && std::fabs(stride) > 1e-4f;
    const int  count = std::clamp(static_cast<int>(std::lround(am.dupCount)), 1, 100);
    const glm::mat4 inv = glm::inverse(s.model);
    std::vector<glm::mat4> xs;
    for (int k = 1; k <= count && can; ++k) {
        glm::vec3 d(0.0f);
        d[ax] = sgn * stride * static_cast<float>(k);
        xs.push_back(inv * glm::translate(glm::mat4(1.0f), d) * s.model);
    }

    Op op = [fs, xs](EditMesh& mm) {
        editmesh::duplicateFacesAt(mm, fs, xs);
        return -1;
    };
    static const char* const kDir[6] = {"-X", "+X", "-Y", "+Y", "-Z", "+Z"};
    char tip[400];
    if (fs.empty())
        std::snprintf(tip, sizeof tip,
                      "Duplicate\nCopy the picked faces in a row. Pick faces first -- or,\n"
                      "in corner and edge mode, all the corners of some.");
    else if (!can)
        std::snprintf(tip, sizeof tip,
                      "Duplicate\nWhat is picked has no width along %s, so the copies would\n"
                      "land on top of it: give them a gap.", kDir[am.dupDir]);
    else
        std::snprintf(tip, sizeof tip,
                      "Duplicate\nCopy the picked faces %d time%s along world %s, each %.2f m\n"
                      "on from the last: their own width plus the gap. The last\n"
                      "copy stays picked, so pressing again carries the row on.\n"
                      "(Shift+D in the modelling mode copies and moves by hand.)",
                      count, count == 1 ? "" : "s", kDir[am.dupDir], std::fabs(stride));
    if (opColumn("duplicate", picto::Icon::Duplicate, can, tip, op, &am.dupCount, 1.0f, 1.0f,
                 100.0f, "%.0f x")) {
        pending = [&s, fs, xs]() {
            s.edit([&](MeshComponent& comp) {
                std::vector<int> made = editmesh::duplicateFacesAt(comp.mesh, fs, xs);
                // The last copy is the one the row goes on from.
                const std::size_t per = xs.empty() ? 0 : made.size() / xs.size();
                made.erase(made.begin(), made.end() - static_cast<std::ptrdiff_t>(per));
                return pickMade(s.sel, comp.mesh, made);
            }, "Duplicate");
        };
    }
    ImGui::SameLine();
    captionColumn("dupgap", "gap",
                  "Gap\nSpace between one copy and the next. 0 lays them edge to\n"
                  "edge; less than 0 lets them overlap.",
                  am.dupGap, 0.05f, -20.0f, 50.0f, "%.2f m");

    // Which way: the nudge arrows' pictures, the chosen one on the accent.
    gap();
    static const picto::Icon kArrow[6] = {picto::Icon::ArrowLeft, picto::Icon::ArrowRight,
                                          picto::Icon::ArrowDown, picto::Icon::ArrowUp,
                                          picto::Icon::ArrowIn,   picto::Icon::ArrowOut};
    // Two rows, one column per axis: +X +Y +Z over -X -Y -Z, so a direction
    // and its opposite are always one above the other.
    ImGui::BeginGroup();
    for (int row = 0; row < 2; ++row)
        for (int a = 0; a < 3; ++a) {
            const int k = a * 2 + (row == 0 ? 1 : 0);
            if (a > 0) ImGui::SameLine(0.0f, 3.0f);
            // Not the bare "+X": the nudge arrows below use those ids.
            char id[16], t[48];
            std::snprintf(id, sizeof id, "dupdir%s", kDir[k]);
            std::snprintf(t, sizeof t, "Copy towards world %s", kDir[k]);
            if (choice(id, kArrow[k], am.dupDir == k, t)) am.dupDir = k;
        }
    ImGui::EndGroup();
}

// --- Duplicate along a spline path -----------------------------------------

void alongRow(const PanelState& s, const std::vector<int>& fs, std::function<void()>& pending) {
    Amounts&        am = modeltools::amounts();
    const EditMesh& m  = s.mesh->mesh;
    const SplineSystem* sp = s.splines;
    const int np = sp ? static_cast<int>(sp->paths.size()) : 0;
    if (np > 0) am.pathIndex = std::clamp(am.pathIndex, 0, np - 1);
    const std::vector<glm::vec3> noLine;
    const std::vector<glm::vec3>& line = np > 0 ? sp->line(am.pathIndex) : noLine;
    const bool closed = np > 0 && sp->paths[am.pathIndex].closed;
    const bool pathOk = line.size() >= 2;
    const int  count  = std::clamp(static_cast<int>(std::lround(am.pathCount)), 1, 500);

    // Each copy: the middle of what is picked put on the path, turned so the
    // object's X runs along it (when aligning). Worked out in the world and
    // brought into the mesh, so a rotated or scaled object copies true.
    std::vector<glm::mat4> xs;
    if (pathOk && !fs.empty()) {
        glm::vec3 mn, mx;
        worldBox(m, s.model, fs, mn, mx);
        const glm::mat4 toOrigin = glm::translate(glm::mat4(1.0f), -0.5f * (mn + mx));
        const glm::mat4 inv      = glm::inverse(s.model);
        for (const glm::mat4& F : editmesh::framesAlong(line, closed, count, am.pathAlign))
            xs.push_back(inv * F * toOrigin * s.model);
    }
    const bool can = !xs.empty();

    Op op = [fs, xs](EditMesh& mm) {
        editmesh::duplicateFacesAt(mm, fs, xs);
        return -1;
    };
    const float len     = pathOk ? editmesh::lineLength(line, closed) : 0.0f;
    const float spacing = closed ? len / count : (count > 1 ? len / (count - 1) : 0.0f);
    char tip[400];
    if (np == 0)
        std::snprintf(tip, sizeof tip,
                      "Duplicate along a path\nThe scene has no spline path yet. Lay one in the\n"
                      "Splines panel (a bare Path is made for this).");
    else if (fs.empty())
        std::snprintf(tip, sizeof tip,
                      "Duplicate along a path\nPick faces first -- or, in corner and edge mode,\n"
                      "all the corners of some.");
    else
        std::snprintf(tip, sizeof tip,
                      "Duplicate along a path\nCopy the picked faces %d times along the path, spread\n"
                      "evenly over its %.1f m (one every %.2f m), the middle of\n"
                      "the pick on the path. Turning with the path, the object's\n"
                      "X runs along it: model the piece lying along X.\n"
                      "The copies stay picked; the original stays where it is.",
                      count, len, spacing);
    if (opColumn("along", picto::Icon::DuplicatePath, can, tip, op, &am.pathCount, 1.0f, 1.0f,
                 500.0f, "%.0f x")) {
        pending = [&s, fs, xs]() {
            s.edit([&](MeshComponent& comp) {
                const std::vector<int> made = editmesh::duplicateFacesAt(comp.mesh, fs, xs);
                return pickMade(s.sel, comp.mesh, made);
            }, "Duplicate along path");
        };
    }

    // Which path, and whether the copies turn with it.
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::BeginDisabled(np == 0);
    ImGui::SetNextItemWidth(em() * 10.0f);
    const char* cur = np > 0 ? sp->paths[am.pathIndex].name.c_str() : "(no spline paths)";
    if (ImGui::BeginCombo("##alongpath", cur)) {
        for (int i = 0; i < np; ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(sp->paths[i].name.c_str(), i == am.pathIndex)) am.pathIndex = i;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("The spline path the copies go along");
    if (choice("alongalign", picto::Icon::AlignPath, am.pathAlign,
               am.pathAlign ? "Copies turn with the path (click: keep them as they are)"
                            : "Copies keep their own turn (click: turn them with the path)"))
        am.pathAlign = !am.pathAlign;
    if (pathOk && !fs.empty()) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("one every %.2f m", spacing);
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();
}

} // namespace

void sweepTools(const PanelState& s, std::function<void()>& pending) {
    if (!s.mesh) return;
    ImGui::Spacing();
    ImGui::Separator();
    if (s.sel.mode != Mode::Face) {
        spinRow(s, pending);
        ImGui::Spacing();
    }
    const std::vector<int> fs = copiedFaces(s.sel, s.mesh->mesh, s.faceSel);
    duplicateRow(s, fs, pending);
    ImGui::Spacing();
    alongRow(s, fs, pending);
}

} // namespace modelui::parts
