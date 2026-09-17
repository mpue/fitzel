#include "ModelingPanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include "Component.hpp"
#include "UiStyle.hpp"

namespace modelui {

namespace {

using modeltools::Mode;
using modeltools::Op;

// --- Metrics ------------------------------------------------------------------
// Sizes follow the UI font, never fixed pixels: the editor runs at whatever font
// size and display scale the user picked, and a 40 px button clips its label at
// 150 %.
float em() { return ImGui::GetFontSize(); }
// Tall enough for a two-line label ("Extrude / 0.50 m"), which every operation
// button gets -- so the rows line up and the targets stay large.
float btnH() { return em() * 2.0f + ImGui::GetStyle().FramePadding.y * 2.0f + 4.0f; }
float smallH() { return ImGui::GetFrameHeight() + em() * 0.35f; }
// Wide enough for the label's longest line, never narrower than `minEm` ems.
ImVec2 fit(const char* label, float minEm, float h) {
    const char*  end = std::strstr(label, "##");
    const ImVec2 ts  = ImGui::CalcTextSize(label, end);
    return ImVec2(std::max(ts.x + ImGui::GetStyle().FramePadding.x * 2.0f + em() * 0.6f,
                           em() * minEm),
                  h);
}

// The filter inside the face-material picker. One buffer: only one popup is open
// at a time, and it starts empty each time so the picker never opens already
// hiding most of the library.
char g_matFilter[64] = {};

// How long the two bands through the selected face are, and what that answer was
// worked out for. Walking a band means deriving the mesh's edges first, and doing
// that twice per frame for a read-out is a cost that grows with the mesh while
// the answer only changes when the selection or the geometry does.
std::uint64_t g_ringRev    = 0;
int           g_ringFace   = -1;
int           g_ringLen[2] = {0, 0};

// The library entry `id` names, or nullptr. Not Document::materialIndex(), which
// answers 0 -- a real material -- for a GUID it does not know, and a face wearing
// the object's material has to stay visibly undressed.
const MaterialDef* findMaterial(const PanelState& s, const fitzel::AssetId& id) {
    if (!id.valid()) return nullptr;
    for (const MaterialDef& md : s.materials)
        if (md.assetId == id) return &md;
    return nullptr;
}

// --- Controls -----------------------------------------------------------------

// A mode switch: filled when it is the current one.
bool modeButton(const char* label, bool on, const char* tip) {
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.85f, 0.52f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.60f, 0.16f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.00f, 0.66f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.08f, 0.06f, 0.03f, 1.0f));
    }
    const bool clicked = ImGui::Button(label, fit(label, 3.6f, btnH()));
    if (on) ImGui::PopStyleColor(4);
    ImGui::SetItemTooltip("%s", tip);
    return clicked;
}

// An amount, changed by a pair of big buttons rather than by dragging: a value
// you can only approach with a steady hand is a value this editor does not ask
// for (see the panel header). The steps are the round numbers one actually
// builds with, so "half a metre" is one click from "a quarter", not a slide.
void stepper(const char* id, float& v, float step, float lo, float hi, const char* fmt,
             float width) {
    ImGui::PushID(id);
    ImGui::BeginGroup();
    const ImVec2 bs(std::max(em() * 1.6f, 26.0f), smallH());
    if (ImGui::Button("-", bs)) v = std::max(lo, v - step);
    ImGui::SetItemTooltip("%.2f less", step);
    ImGui::SameLine(0.0f, 3.0f);
    // The value between them reads as a field, not as a third target: it is
    // drawn in the frame colour and does nothing when pressed.
    char buf[48];
    std::snprintf(buf, sizeof buf, fmt, v);
    const ImVec4 frame = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    ImGui::PushStyleColor(ImGuiCol_Button, frame);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, frame);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, frame);
    ImGui::Button(buf, ImVec2(std::max(width - 2.0f * bs.x - 6.0f, em() * 2.5f), bs.y));
    ImGui::PopStyleColor(3);
    ImGui::SameLine(0.0f, 3.0f);
    if (ImGui::Button("+", bs)) v = std::min(hi, v + step);
    ImGui::SetItemTooltip("%.2f more", step);
    ImGui::EndGroup();
    ImGui::PopID();
}

// How wide a stepper has to be for its number to fit whatever the format puts
// in it -- "0.50 m" clipped to "0.50 n" is the kind of read-out that sends you
// looking for a bug in the geometry.
float stepperWidth(const char* fmt) {
    char wide[48];
    std::snprintf(wide, sizeof wide, fmt, -8.88f);
    return 2.0f * std::max(em() * 1.6f, 26.0f) + 6.0f +
           ImGui::CalcTextSize(wide).x + ImGui::GetStyle().FramePadding.x * 2.0f +
           em() * 0.4f;
}

// One operation: the button, and under it the amount it applies. Both the same
// width, so a row of operations reads as columns rather than as a heap. `amount`
// may be null for an operation that has no number (Subdivide, Delete).
bool opColumn(const char* label, bool enabled, const char* tip, const Op& previewOp,
              float* amount, float step, float lo, float hi, const char* fmt) {
    const float w = std::max(fit(label, 4.6f, 0.0f).x,
                             amount ? stepperWidth(fmt) : 0.0f);
    ImGui::BeginGroup();
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::Button(label, ImVec2(w, btnH()));
    ImGui::EndDisabled();
    if (enabled && previewOp && ImGui::IsItemHovered()) modeltools::preview(previewOp);
    ImGui::SetItemTooltip("%s", tip);
    if (amount) stepper(label, *amount, step, lo, hi, fmt, w);
    ImGui::EndGroup();
    return clicked && enabled;
}

void gap() { ImGui::SameLine(0.0f, 14.0f); }

} // namespace

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    modeltools::Amounts& am = modeltools::amounts();

    // Floats over the viewport, near the mesh. Docking is off on purpose: docked
    // at the edge of the screen it would be a trip away from the thing it shapes,
    // which is the whole reason it moved out here.
    ImGui::SetNextWindowPos(ImVec2(s.viewMin.x + 16.0f, s.viewMin.y + 56.0f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(5.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

    // What a click asked for, run after the last widget: an edit re-centres the
    // mesh and banks an undo step, and nothing below should still be reading it.
    std::function<void()> pending;

    // "##floating" so it is a NEW window to ImGui: an editor that has the old,
    // docked Modeling panel in its imgui.ini would otherwise reopen this one
    // inside that dock node, which is the one place it must not be.
    if (ImGui::Begin("Modeling##floating", &s.show,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize)) {
        // Where the window is, so the overlay can write the name of the last edit
        // just under it -- next to the button that was pressed. Last frame's size
        // on an auto-resizing window, which is close enough for a caption.
        {
            const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
            modeltools::panelRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y));
        }

        if (!s.mesh) {
            if (!s.haveSelection) {
                ui::hint("Select an object first. A box becomes editable in one\n"
                         "click, and from there you shape it corner by corner.");
            } else if (s.canConvert) {
                ui::hint("This box can become an editable mesh: the same shape,\n"
                         "but with corners, edges and faces you can shape.\n"
                         "Nothing else about it changes.");
                ImGui::Spacing();
                if (ImGui::Button("Make editable", fit("Make editable", 8.0f, btnH())) &&
                    s.convert)
                    pending = s.convert;
            } else {
                ui::hint("Only boxes become meshes. Imported models are left as\n"
                         "their author made them.");
            }
            ImGui::End();
            ImGui::PopStyleVar(3);
            // The same as at the bottom: what was pressed runs once the panel has
            // stopped reading the entity it would change.
            if (pending) pending();
            return;
        }

        const EditMesh&        m    = s.mesh->mesh;
        modeltools::Selection& sel  = s.sel;
        const int              face = s.faceSel;
        MeshComponent*         mc   = s.mesh;

        // Run `op` as one undo step, then let `after` see what it returned.
        auto run = [&](const char* label, Op op, std::function<void(int)> after) {
            pending = [label, op, after, &s]() {
                int        res      = -1;
                const bool faceMode = s.sel.mode == Mode::Face;
                s.edit([&](MeshComponent& comp) {
                    res = op(comp.mesh);
                    return faceMode ? res : -1;
                }, label);
                if (after) after(res);
            };
        };

        // --- What is being picked ------------------------------------------
        if (modeButton("Vertex", sel.mode == Mode::Vertex,
                       "Pick corners. Move them with the gizmo (Q/W/E) or the\n"
                       "arrows below; merge or delete them."))
            pending = [&s, mc] { modeltools::setMode(s.sel, Mode::Vertex, mc->mesh, s.faceSel); };
        ImGui::SameLine();
        if (modeButton("Edge", sel.mode == Mode::Edge,
                       "Pick edges. Split, collapse or dissolve them, or run a\n"
                       "loop cut across one."))
            pending = [&s, mc] { modeltools::setMode(s.sel, Mode::Edge, mc->mesh, s.faceSel); };
        ImGui::SameLine();
        if (modeButton("Face", sel.mode == Mode::Face,
                       "Pick a face. Extrude, inset, scale, cut or delete it,\n"
                       "and give it a material of its own."))
            pending = [&s, mc] { modeltools::setMode(s.sel, Mode::Face, mc->mesh, s.faceSel); };

        gap();
        if (modeButton("Add", sel.additive,
                       "While this is on, a click ADDS to the selection (and a\n"
                       "second click on the same element takes it out again)\n"
                       "instead of replacing it. Shift+click does the same once."))
            sel.additive = !sel.additive;
        ImGui::SameLine();
        ImGui::BeginDisabled(sel.mode == Mode::Face);
        if (ImGui::Button("All", fit("All", 2.8f, btnH()))) {
            pending = [&s, mc] {
                modeltools::Selection& ss = s.sel;
                ss.clear();
                if (ss.mode == Mode::Vertex)
                    for (int i = 0; i < static_cast<int>(mc->mesh.verts.size()); ++i)
                        ss.verts.push_back(i);
                else
                    for (const editmesh::EdgeInfo& e : editmesh::edges(mc->mesh))
                        ss.edges.push_back({std::min(e.a, e.b), std::max(e.a, e.b)});
            };
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Select every corner / every edge.");
        ImGui::SameLine();
        if (ImGui::Button("None", fit("None", 2.8f, btnH()))) {
            sel.clear();
            s.faceSel = -1;
        }
        ImGui::SetItemTooltip("Let go of the selection (Esc does too).");

        // What is picked, and what the mesh is made of.
        {
            char what[64];
            if (sel.mode == Mode::Vertex)
                std::snprintf(what, sizeof what, "%d corner%s picked",
                              static_cast<int>(sel.verts.size()),
                              sel.verts.size() == 1 ? "" : "s");
            else if (sel.mode == Mode::Edge)
                std::snprintf(what, sizeof what, "%d edge%s picked",
                              static_cast<int>(sel.edges.size()),
                              sel.edges.size() == 1 ? "" : "s");
            else if (m.validFace(face))
                std::snprintf(what, sizeof what, "face %d picked (%d corners)", face,
                              static_cast<int>(m.faces[face].size()));
            else
                std::snprintf(what, sizeof what, "nothing picked -- click the mesh");
            ImGui::TextDisabled("%s  |  %d faces, %d corners", what, s.faceCount,
                                s.vertCount);
        }
        ImGui::Separator();

        // --- What can be done with it ---------------------------------------
        char lbl[64];
        if (sel.mode == Mode::Face) {
            const bool have = m.validFace(face);
            const bool quad = have && m.faces[face].size() == 4;

            const float ex = am.extrude;
            Op opEx = [face, ex](EditMesh& mm) { return editmesh::extrude(mm, face, ex); };
            if (opColumn("Extrude", have,
                         "Pull the face out by the amount below and wall in the\n"
                         "gap. The new face keeps the selection, so pressing it\n"
                         "again builds another step. A negative amount pushes in.",
                         opEx, &am.extrude, 0.05f, -20.0f, 20.0f, "%.2f m"))
                run("Extrude", opEx, nullptr);
            ImGui::SameLine();

            const float mv = am.move;
            Op opMv = [face, mv](EditMesh& mm) { return editmesh::moveFace(mm, face, mv); };
            if (opColumn("Move", have,
                         "Slide the face along its normal without adding anything:\n"
                         "the neighbours stretch to follow, so the whole shape\n"
                         "grows instead.",
                         opMv, &am.move, 0.05f, -20.0f, 20.0f, "%.2f m"))
                run("Move face", opMv, nullptr);
            ImGui::SameLine();

            const float in = am.inset;
            Op opIn = [face, in](EditMesh& mm) { return editmesh::inset(mm, face, in); };
            if (opColumn("Inset", have,
                         "Lay a border of that width inside the face and select\n"
                         "the middle. Inset, then extrude inwards: a window.",
                         opIn, &am.inset, 0.05f, 0.0f, 10.0f, "%.2f m"))
                run("Inset", opIn, nullptr);
            ImGui::SameLine();

            const float sc = am.scale;
            Op opSc = [face, sc](EditMesh& mm) { return editmesh::scaleFace(mm, face, sc); };
            if (opColumn("Scale", have,
                         "Scale the face about its own centre. Shrinking the top\n"
                         "of a box gives a frustum -- the corners are shared, so\n"
                         "the sides come along.",
                         opSc, &am.scale, 0.05f, 0.0f, 4.0f, "%.2fx"))
                run("Scale face", opSc, nullptr);

            ImGui::Spacing();
            // Loop cut: the cut that goes all the way round. Which of the two
            // bands is not something a click can say -- the face lies in both at
            // once -- so it is a button that swaps, with the count of faces each
            // one would cut written on it.
            if (g_ringRev != s.mesh->revision || g_ringFace != face) {
                g_ringRev    = s.mesh->revision;
                g_ringFace   = face;
                g_ringLen[0] = have ? editmesh::loopLength(m, face, 0) : 0;
                g_ringLen[1] = have ? editmesh::loopLength(m, face, 1) : 0;
            }
            const int   dir = am.loopDir;
            const float at  = am.loopAt;
            const int   n   = g_ringLen[dir & 1];
            std::snprintf(lbl, sizeof lbl, "Loop cut\n%d face%s", n, n == 1 ? "" : "s");
            Op opLc = [face, dir, at](EditMesh& mm) {
                return editmesh::loopCut(mm, face, dir, at);
            };
            if (opColumn(lbl, quad && n > 0,
                         "Run a new edge right round the band this face lies in\n"
                         "and split every quad it crosses -- a storey line on a\n"
                         "tower, a joint to bend a wall at. Only quads.\n"
                         "The number below is where along the band it falls.",
                         opLc, &am.loopAt, 0.05f, 0.05f, 0.95f, "cut at %.2f"))
                run("Loop cut", opLc, nullptr);
            ImGui::SameLine();
            std::snprintf(lbl, sizeof lbl, "Turn cut\n%s", dir ? "across" : "along");
            Op opLt = [face, dir, at](EditMesh& mm) {
                return editmesh::loopCut(mm, face, dir ^ 1, at);
            };
            if (opColumn(lbl, quad,
                         "Swap which of the two bands through this face the cut\n"
                         "runs in. The preview shows the other one.",
                         opLt, nullptr, 0.0f, 0.0f, 0.0f, nullptr))
                am.loopDir ^= 1;
            ImGui::SameLine();
            Op opSd = [face](EditMesh& mm) { return editmesh::subdivide(mm, face); };
            if (opColumn("Subdivide\ninto four", quad, "Split the quad into four.",
                         opSd, nullptr, 0.0f, 0.0f, 0.0f, nullptr))
                run("Subdivide", opSd, nullptr);
            ImGui::SameLine();
            Op opDf = [face](EditMesh& mm) { return editmesh::deleteFace(mm, face); };
            if (opColumn("Delete\nface", have, "Remove the face, leaving a hole.",
                         opDf, nullptr, 0.0f, 0.0f, 0.0f, nullptr))
                run("Delete face", opDf, nullptr);
        } else if (sel.mode == Mode::Vertex) {
            const std::vector<int> vs = sel.verts;
            Op opMg = [vs](EditMesh& mm) { return editmesh::mergeVerts(mm, vs); };
            if (opColumn("Merge\ncorners", vs.size() >= 2,
                         "Collapse the picked corners into one, at their centre.\n"
                         "Faces that shrink to nothing go with them.",
                         opMg, nullptr, 0.0f, 0.0f, 0.0f, nullptr))
                run("Merge corners", opMg, [&s](int r) {
                    s.sel.verts.clear();
                    if (r >= 0) s.sel.verts.push_back(r);
                });
            ImGui::SameLine();
            Op opDv = [vs](EditMesh& mm) { editmesh::deleteVerts(mm, vs); return -1; };
            if (opColumn("Delete\ncorners", !vs.empty(),
                         "Remove the corners and every face that uses them.",
                         opDv, nullptr, 0.0f, 0.0f, 0.0f, nullptr))
                run("Delete corners", opDv, [&s](int) { s.sel.clear(); });
        } else {
            const std::vector<std::pair<int, int>> es = sel.edges;
            const bool one = es.size() == 1;
            const int  ea  = one ? es[0].first : -1;
            const int  eb  = one ? es[0].second : -1;

            Op opSp = [es](EditMesh& mm) {
                for (const auto& e : es) editmesh::splitEdge(mm, e.first, e.second, 0.5f);
                return -1;
            };
            if (opColumn("Split", !es.empty(),
                         "Put a new corner in the middle of each picked edge.\n"
                         "The two halves stay picked, ready to be moved.",
                         opSp, nullptr, 0.0f, 0.0f, 0.0f, nullptr)) {
                pending = [&s, es]() {
                    std::vector<std::pair<int, int>> halves;
                    s.edit([&](MeshComponent& comp) {
                        for (const auto& e : es) {
                            const int v = editmesh::splitEdge(comp.mesh, e.first, e.second, 0.5f);
                            if (v < 0) continue;
                            halves.push_back({std::min(e.first, v), std::max(e.first, v)});
                            halves.push_back({std::min(e.second, v), std::max(e.second, v)});
                        }
                        return -1;
                    }, "Split edge");
                    s.sel.edges = halves;
                };
            }
            ImGui::SameLine();
            std::vector<int> ends;
            for (const auto& e : es) { ends.push_back(e.first); ends.push_back(e.second); }
            Op opCo = [ends](EditMesh& mm) { return editmesh::mergeVerts(mm, ends); };
            if (opColumn("Collapse", !es.empty(),
                         "Shrink the picked edges to a single corner.",
                         opCo, nullptr, 0.0f, 0.0f, 0.0f, nullptr))
                run("Collapse edge", opCo, [&s](int) { s.sel.clear(); });
            ImGui::SameLine();
            Op opDs = [ea, eb](EditMesh& mm) { return editmesh::dissolveEdge(mm, ea, eb); };
            if (opColumn("Dissolve", one,
                         "Remove the edge by joining the two faces either side of\n"
                         "it into one. One edge at a time.",
                         opDs, nullptr, 0.0f, 0.0f, 0.0f, nullptr))
                run("Dissolve edge", opDs, [&s](int) { s.sel.clear(); });
            ImGui::SameLine();
            const int n = one ? editmesh::loopLengthEdge(m, ea, eb) : 0;
            std::snprintf(lbl, sizeof lbl, "Loop cut\n%d face%s", n, n == 1 ? "" : "s");
            const float at = am.loopAt;
            Op opLc = [ea, eb, at](EditMesh& mm) {
                return editmesh::loopCutEdge(mm, ea, eb, at);
            };
            if (opColumn(lbl, n > 0,
                         "Run a new edge right round the mesh, crossing this one.\n"
                         "The number below says where along it the cut falls.",
                         opLc, &am.loopAt, 0.05f, 0.05f, 0.95f, "cut at %.2f"))
                run("Loop cut", opLc, [&s](int) { s.sel.clear(); });
        }

        // --- Nudge: exact steps along the world axes -------------------------
        // A click moves what is picked by exactly the step. This is the drag-free
        // way to put a corner where it belongs, and it is why the gizmo is the
        // convenience on top rather than the only way in.
        ImGui::Spacing();
        const std::vector<int> act = modeltools::activeVerts(sel, m, face);
        ImGui::BeginDisabled(act.empty());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Nudge");
        ImGui::SameLine();
        stepper("nudge", am.nudge, 0.05f, 0.01f, 5.0f, "%.2f m", stepperWidth("%.2f m"));
        ImGui::SameLine(0.0f, 12.0f);

        static const char*  kAxis[6]    = {"-X", "+X", "-Y", "+Y", "-Z", "+Z"};
        static const ImVec4 kAxisCol[3] = {ImVec4(0.75f, 0.25f, 0.25f, 1.0f),
                                           ImVec4(0.30f, 0.62f, 0.25f, 1.0f),
                                           ImVec4(0.25f, 0.42f, 0.80f, 1.0f)};
        for (int k = 0; k < 6; ++k) {
            if (k > 0) ImGui::SameLine(0.0f, k % 2 == 0 ? 10.0f : 3.0f);
            glm::vec3 d(0.0f);
            d[k / 2] = (k % 2 ? 1.0f : -1.0f) * am.nudge;
            const glm::mat4 L = glm::inverse(s.model) *
                                glm::translate(glm::mat4(1.0f), d) * s.model;
            const bool faceMode = sel.mode == Mode::Face;
            Op op = [act, L, faceMode, face](EditMesh& mm) {
                editmesh::transformVerts(mm, act, L);
                return faceMode ? face : -1;
            };
            const ImVec4& ac = kAxisCol[k / 2];
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(ac.x, ac.y, ac.z, 0.55f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ac);
            const bool clicked = ImGui::Button(kAxis[k], fit("+X", 2.8f, smallH()));
            ImGui::PopStyleColor(2);
            if (!act.empty() && ImGui::IsItemHovered()) modeltools::preview(op);
            ImGui::SetItemTooltip("Move what is picked %.2f m along world %s.",
                                  am.nudge, kAxis[k]);
            if (clicked && !act.empty()) run("Nudge", op, nullptr);
        }
        ImGui::EndDisabled();

        // --- The face's own material -----------------------------------------
        // A face can wear a material of its own instead of the object's: brick on
        // one side, plaster on the other, one object. Picking it here is the
        // no-drag way in; dropping a material from the Assets browser straight
        // onto the face in the viewport does the same thing.
        bool            wantSet = false;
        fitzel::AssetId wantId;
        if (sel.mode == Mode::Face) {
            ImGui::Separator();
            const fitzel::AssetId cur = m.faceMaterial(face);
            const MaterialDef*    md  = findMaterial(s, cur);
            ImGui::BeginDisabled(!m.validFace(face));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Material");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(em() * 14.0f);
            if (ImGui::BeginCombo("##facemat",
                                  md ? md->name.c_str() : "(the object's material)")) {
                if (ImGui::IsWindowAppearing()) {
                    g_matFilter[0] = 0;
                    ImGui::SetKeyboardFocusHere();
                }
                ui::searchBox("##facematf", g_matFilter, sizeof(g_matFilter));
                if (ImGui::Selectable("(the object's material)", md == nullptr)) {
                    wantSet = true;
                    wantId  = fitzel::AssetId{};
                }
                for (const MaterialDef& cand : s.materials) {
                    // Model-owned materials are not the library's to hand out:
                    // they are re-created by the next import, and a face pointing
                    // at one would wear a GUID nobody answers to after a reload.
                    if (cand.fromModel) continue;
                    if (!ui::icontains(cand.name.c_str(), g_matFilter)) continue;
                    const bool on = (md && md->assetId == cand.assetId);
                    if (ImGui::Selectable(cand.name.c_str(), on)) {
                        wantSet = true;
                        wantId  = cand.assetId;
                    }
                    if (on) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("Only this face changes. You can also drag a material\n"
                                  "from the Assets browser onto a face in the viewport.");
            if (md) {
                ImGui::SameLine();
                if (ImGui::Button("Edit") && s.editMaterial) s.editMaterial(cur);
                ImGui::SetItemTooltip("Open this material in the Materials panel.");
                ImGui::SameLine();
                if (ImGui::Button("Back to the object's")) {
                    wantSet = true;
                    wantId  = fitzel::AssetId{};
                }
            }
            // How far the dressing has spread. Worth saying: a face wearing its own
            // material is a second draw call for the object, and this is the only
            // place that shows how many of those an afternoon of clicking made.
            int dressed = 0;
            for (int f = 0; f < static_cast<int>(m.faces.size()); ++f)
                if (m.faceMaterial(f).valid()) ++dressed;
            if (dressed > 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%d of %d faces dressed)", dressed,
                                    static_cast<int>(m.faces.size()));
            }
            ImGui::EndDisabled();
        }

        // The one line that is worth having on screen rather than in a tooltip.
        ui::hint(sel.mode == Mode::Face
                     ? "Click a face in the viewport. The gizmo (Q/W/E) drags its\n"
                       "corners; Esc hands it back to the whole object."
                     : sel.mode == Mode::Vertex
                           ? "Click corners in the viewport. The gizmo (Q/W/E) moves\n"
                             "every one that is picked; Del removes them."
                           : "Click edges in the viewport. The gizmo (Q/W/E) moves\n"
                             "both their ends; Del dissolves a single edge.");

        // Outside every read of the mesh: applying this runs host code that
        // re-centres the geometry the widgets above were drawing from.
        if (wantSet && s.edit) {
            const fitzel::AssetId id = wantId;
            const int             f  = face;
            pending = [&s, f, id]() {
                s.edit([f, id](MeshComponent& comp) {
                    comp.mesh.setFaceMaterial(f, id);
                    return f;
                }, "Face material");
            };
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);

    if (pending) pending();
}

} // namespace modelui
