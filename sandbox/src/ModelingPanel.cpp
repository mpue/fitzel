#include "ModelingPanel.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include "Component.hpp"
#include "ModelingPanelParts.hpp"
#include "Pictogram.hpp"
#include "UiStyle.hpp"

namespace modelui {

// --- Metrics ------------------------------------------------------------------
// Sizes follow the UI font, never fixed pixels: the editor runs at whatever font
// size and display scale the user picked, and a 40 px button clips its label at
// 150 %.
float parts::em() { return ImGui::GetFontSize(); }
// Tall enough for a two-line label ("Extrude / 0.50 m"), which every operation
// button gets -- so the rows line up and the targets stay large.
float parts::btnH() { return em() * 2.0f + ImGui::GetStyle().FramePadding.y * 2.0f + 4.0f; }
float parts::smallH() { return ImGui::GetFrameHeight() + em() * 0.35f; }
void  parts::gap() { ImGui::SameLine(0.0f, 14.0f); }

// The amount controls are ui::stepper (UiStyle.hpp): the same number field the
// rest of the editor uses, and the same rule -- clicks, never a drag.
//
// One operation: a picture of what it does, and under it the amount it
// applies. Both the same width, so a row of operations reads as columns rather
// than as a heap. `amount` may be null for an operation that has no number
// (Subdivide, Delete). `badge`, when given, is a small number in the corner --
// how many faces a loop cut would split, which is worth seeing before pressing.
bool parts::opColumn(const char* id, picto::Icon icon, bool enabled, const char* tip,
                     const modeltools::Op& previewOp, float* amount, float step, float lo,
                     float hi, const char* fmt, const char* badge) {
    const float w = std::max(btnH(), amount ? ui::stepperWidth(fmt) : 0.0f);
    ImGui::BeginGroup();
    const bool clicked = picto::buttonSized(id, icon, tip, enabled, false, ImVec2(w, btnH()));
    if (enabled && previewOp && ImGui::IsItemHovered()) modeltools::preview(previewOp);
    if (badge && *badge) {
        const ImVec2 mx = ImGui::GetItemRectMax();
        const ImVec2 ts = ImGui::CalcTextSize(badge);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(mx.x - ts.x - 4.0f, mx.y - ts.y - 2.0f),
            ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled), badge);
    }
    if (amount) ui::stepper(id, *amount, step, lo, hi, fmt, w);
    ImGui::EndGroup();
    return clicked;
}

void parts::captionColumn(const char* id, const char* caption, const char* tip, float& value,
                          float step, float lo, float hi, const char* fmt) {
    const float w = ui::stepperWidth(fmt);
    ImGui::BeginGroup();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, btnH()));
    const ImVec2 ts = ImGui::CalcTextSize(caption);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (btnH() - ts.y) * 0.5f),
        ImGui::GetColorU32(ImGuiCol_TextDisabled), caption);
    if (tip) ImGui::SetItemTooltip("%s", tip);
    ui::stepper(id, value, step, lo, hi, fmt, w);
    ImGui::EndGroup();
}

namespace {

using modeltools::Mode;
using modeltools::Op;
using namespace parts;

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

// A mode switch, drawn as what it picks: the corners of a box, one of its
// edges, one of its faces. The one that is on sits on the accent. (It used to
// be a word on a button -- "Vertex" -- which is jargon to anyone who has not
// modelled before, where three pictures of the same box are not.)
bool modeButton(const char* id, picto::Icon icon, bool on, const char* tip) {
    return picto::button(id, icon, tip, true, on, btnH());
}

using ui::stepper;
using ui::stepperWidth;

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
    // A floor under the width. The window sizes itself to its content, and with
    // pictures instead of words the content can be one small button -- which
    // left the explaining text beside it wrapping after every word.
    ImGui::SetNextWindowSizeConstraints(ImVec2(em() * 22.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
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
                ui::hint("Select an object first. A box, ramp, cylinder, sphere or\n"
                         "plane becomes editable in one click, and from there you\n"
                         "shape it corner by corner.");
            } else if (s.canConvert) {
                ui::hint("This shape can become an editable mesh: the same shape,\n"
                         "but with corners, edges and faces you can shape.\n"
                         "Nothing else about it changes.");
                ImGui::Spacing();
                if (picto::button("makeedit", picto::Icon::MakeEditable,
                                  "Make editable\nThe same shape, with corners, edges\n"
                                  "and faces you can shape",
                                  true, false, btnH()) &&
                    s.convert)
                    pending = s.convert;
            } else {
                ui::hint("Only the built-in shapes become meshes. Imported models\n"
                         "are left as their author made them.");
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
        if (modeButton("vertex", picto::Icon::ModeVertex, sel.mode == Mode::Vertex,
                       "Corners\nPick corners. Move them with the gizmo (Q/W/E) or the\n"
                       "arrows below; merge or delete them."))
            pending = [&s, mc] { modeltools::setMode(s.sel, Mode::Vertex, mc->mesh, s.faceSel); };
        ImGui::SameLine();
        if (modeButton("edge", picto::Icon::ModeEdge, sel.mode == Mode::Edge,
                       "Edges\nPick edges. Split, collapse or dissolve them, or run a\n"
                       "loop cut across one."))
            pending = [&s, mc] { modeltools::setMode(s.sel, Mode::Edge, mc->mesh, s.faceSel); };
        ImGui::SameLine();
        if (modeButton("face", picto::Icon::ModeFace, sel.mode == Mode::Face,
                       "Faces\nPick a face. Extrude, inset, scale, cut or delete it,\n"
                       "and give it a material of its own."))
            pending = [&s, mc] { modeltools::setMode(s.sel, Mode::Face, mc->mesh, s.faceSel); };

        gap();
        if (modeButton("add", picto::Icon::SelectAdd, sel.additive,
                       "Add to the selection\nWhile this is on, a click ADDS to what is\n"
                       "picked (a second click on the same element takes it out\n"
                       "again) instead of replacing it. Shift+click does the same once."))
            sel.additive = !sel.additive;
        ImGui::SameLine();
        if (picto::button("all", picto::Icon::SelectAll,
                          "Pick everything\nEvery corner, every edge or every face.",
                          true, false, btnH()))
            pending = [&s, mc] { modeltools::selectAll(s.sel, mc->mesh, s.faceSel); };
        ImGui::SameLine();
        if (picto::button("grow", picto::Icon::Grow,
                          "Grow the selection\nAdd the ring around what is picked: the\n"
                          "neighbouring faces, corners or edges. A few presses\n"
                          "pick a whole side without aiming at every piece.",
                          sel.any() || face >= 0, false, btnH()))
            pending = [&s, mc] { modeltools::grow(s.sel, mc->mesh, s.faceSel); };
        ImGui::SameLine();
        if (picto::button("none", picto::Icon::SelectNone,
                          "Let go of the selection (Esc does too)", true, false, btnH())) {
            sel.clear();
            s.faceSel = -1;
        }

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
            else if (modeltools::selectedFaces(sel, m, face).size() > 1)
                std::snprintf(what, sizeof what, "%d faces picked",
                              static_cast<int>(modeltools::selectedFaces(sel, m, face).size()));
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
            // Every operation here takes all the picked faces; the active one
            // (the last clicked) goes first and is the one that stays active.
            const std::vector<int> fs = modeltools::selectedFaces(sel, m, face);
            const bool have = !fs.empty();
            const bool quad = m.validFace(face) && m.faces[face].size() == 4;
            bool anyQuad = false;
            for (int f : fs) anyQuad = anyQuad || m.faces[f].size() == 4;

            const float ex = am.extrude;
            Op opEx = [fs, ex](EditMesh& mm) { return editmesh::extrudeFaces(mm, fs, ex); };
            if (opColumn("extrude", picto::Icon::Extrude, have,
                         "Extrude\nPull the face out by the amount below and wall in the\n"
                         "gap. The new face keeps the selection, so pressing it\n"
                         "again builds another step. A negative amount pushes in.\n"
                         "Several faces that touch go out together, as one piece.",
                         opEx, &am.extrude, 0.05f, -20.0f, 20.0f, "%.2f m"))
                run("Extrude", opEx, nullptr);
            ImGui::SameLine();

            const float mv = am.move;
            Op opMv = [fs, mv](EditMesh& mm) { return editmesh::moveFaces(mm, fs, mv); };
            if (opColumn("move", picto::Icon::MoveNormal, have,
                         "Move\nSlide the face along its normal without adding anything:\n"
                         "the neighbours stretch to follow, so the whole shape\n"
                         "grows instead.",
                         opMv, &am.move, 0.05f, -20.0f, 20.0f, "%.2f m"))
                run("Move face", opMv, nullptr);
            ImGui::SameLine();

            const float in = am.inset;
            Op opIn = [fs, in](EditMesh& mm) { return editmesh::insetFaces(mm, fs, in); };
            if (opColumn("inset", picto::Icon::Inset, have,
                         "Inset\nLay a border of that width inside the face and select\n"
                         "the middle. Inset, then extrude inwards: a window.\n"
                         "Several faces get a border each.",
                         opIn, &am.inset, 0.05f, 0.0f, 10.0f, "%.2f m"))
                run("Inset", opIn, nullptr);
            ImGui::SameLine();

            const float sc = am.scale;
            Op opSc = [fs, sc](EditMesh& mm) { return editmesh::scaleFaces(mm, fs, sc); };
            if (opColumn("scale", picto::Icon::ScaleFace, have,
                         "Scale\nScale the face about its own centre. Shrinking the top\n"
                         "of a box gives a frustum -- the corners are shared, so\n"
                         "the sides come along. Faces that do not touch each\n"
                         "scale about their own centre.",
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
            std::snprintf(lbl, sizeof lbl, "%d", n);
            Op opLc = [face, dir, at](EditMesh& mm) {
                return editmesh::loopCut(mm, face, dir, at);
            };
            if (opColumn("loopcut", dir ? picto::Icon::LoopCutV : picto::Icon::LoopCutH,
                         quad && n > 0,
                         "Loop cut (the number: how many faces it splits)\n"
                         "Run a new edge right round the band this face lies in\n"
                         "and split every quad it crosses -- a storey line on a\n"
                         "tower, a joint to bend a wall at. Only quads.\n"
                         "The number below is where along the band it falls.",
                         opLc, &am.loopAt, 0.05f, 0.05f, 0.95f, "cut at %.2f", lbl))
                run("Loop cut", opLc, nullptr);
            ImGui::SameLine();
            Op opLt = [face, dir, at](EditMesh& mm) {
                return editmesh::loopCut(mm, face, dir ^ 1, at);
            };
            // Drawn as the OTHER direction: what pressing it would switch to.
            if (opColumn("turncut", dir ? picto::Icon::LoopCutH : picto::Icon::LoopCutV, quad,
                         "Turn the cut\nSwap which of the two bands through this face the cut\n"
                         "runs in. The preview shows the other one.",
                         opLt, nullptr, 0.0f, 0.0f, 0.0f, nullptr))
                am.loopDir ^= 1;
            ImGui::SameLine();
            Op opSd = [fs](EditMesh& mm) { return editmesh::subdivideFaces(mm, fs); };
            if (opColumn("subdivide", picto::Icon::Subdivide, anyQuad,
                         "Subdivide\nSplit each picked quad into four.",
                         opSd, nullptr, 0.0f, 0.0f, 0.0f, nullptr))
                run("Subdivide", opSd, nullptr);
            ImGui::SameLine();
            Op opFl = [fs](EditMesh& mm) { return editmesh::flipFaces(mm, fs); };
            if (opColumn("flip", picto::Icon::Flip, have,
                         "Flip\nTurn the face inside out: its front becomes its back.\n"
                         "The cure for a face that is dark, or invisible, from\n"
                         "the side you look at it from.",
                         opFl, nullptr, 0.0f, 0.0f, 0.0f, nullptr))
                run("Flip faces", opFl, nullptr);
            ImGui::SameLine();
            Op opDf = [fs](EditMesh& mm) { editmesh::deleteFaces(mm, fs); return -1; };
            if (opColumn("delface", picto::Icon::Trash, have,
                         "Delete\nRemove the picked faces, leaving holes (Del does too).\n"
                         "Fill a hole again from the edge mode.",
                         opDf, nullptr, 0.0f, 0.0f, 0.0f, nullptr))
                run("Delete faces", opDf, [&s](int) { s.sel.clear(); });
        } else if (sel.mode == Mode::Vertex) {
            const std::vector<int> vs = sel.verts;
            Op opMg = [vs](EditMesh& mm) { return editmesh::mergeVerts(mm, vs); };
            if (opColumn("merge", picto::Icon::Merge, vs.size() >= 2,
                         "Merge corners\nCollapse the picked corners into one, at their centre.\n"
                         "Faces that shrink to nothing go with them.",
                         opMg, nullptr, 0.0f, 0.0f, 0.0f, nullptr))
                run("Merge corners", opMg, [&s](int r) {
                    s.sel.verts.clear();
                    if (r >= 0) s.sel.verts.push_back(r);
                });
            ImGui::SameLine();
            const float wd = am.weld;
            Op opWd = [vs, wd](EditMesh& mm) { editmesh::weldVerts(mm, vs, wd); return -1; };
            if (opColumn("weld", picto::Icon::Weld, vs.size() >= 2,
                         "Weld\nMerge those of the picked corners that lie closer\n"
                         "together than the distance below -- the tidy-up after\n"
                         "moving corners onto each other. Pick all first to\n"
                         "weld the whole mesh.",
                         opWd, &am.weld, 0.005f, 0.0f, 1.0f, "%.3f m"))
                run("Weld corners", opWd, [&s](int) { s.sel.clear(); });
            ImGui::SameLine();
            Op opCn = [vs](EditMesh& mm) {
                return vs.size() == 2 ? editmesh::connectVerts(mm, vs[0], vs[1]) : -1;
            };
            // Only when some face has both corners and they are not already
            // neighbours on it -- otherwise there is nothing to cut.
            bool canConnect = false;
            if (vs.size() == 2)
                for (const std::vector<int>& fv : m.faces) {
                    const auto pa = std::find(fv.begin(), fv.end(), vs[0]);
                    const auto pb = std::find(fv.begin(), fv.end(), vs[1]);
                    if (pa == fv.end() || pb == fv.end()) continue;
                    const int n = static_cast<int>(fv.size());
                    const int g = (static_cast<int>(pb - pa) + n) % n;
                    if (g != 1 && g != n - 1) canConnect = true;
                }
            if (opColumn("connect", picto::Icon::Connect, canConnect,
                         "Connect\nDraw a new edge between two corners of the same face,\n"
                         "cutting it in two. Pick exactly two corners.",
                         opCn, nullptr, 0.0f, 0.0f, 0.0f, nullptr))
                run("Connect corners", opCn, nullptr);
            ImGui::SameLine();
            Op opMf = [vs](EditMesh& mm) { editmesh::makeFace(mm, vs); return -1; };
            if (opColumn("makeface", picto::Icon::MakeFace, vs.size() >= 3,
                         "Make a face\nClose the picked corners with a new face. The order\n"
                         "you picked them in does not matter; the face turns to\n"
                         "match the faces around it.",
                         opMf, nullptr, 0.0f, 0.0f, 0.0f, nullptr))
                run("Make face", opMf, nullptr);
            ImGui::SameLine();
            Op opDv = [vs](EditMesh& mm) { editmesh::deleteVerts(mm, vs); return -1; };
            if (opColumn("delcorners", picto::Icon::Trash, !vs.empty(),
                         "Delete corners\nRemove the corners and every face that uses them.",
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
            if (opColumn("split", picto::Icon::SplitEdge, !es.empty(),
                         "Split\nPut a new corner in the middle of each picked edge.\n"
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
            if (opColumn("collapse", picto::Icon::Collapse, !es.empty(),
                         "Collapse\nShrink the picked edges to a single corner.",
                         opCo, nullptr, 0.0f, 0.0f, 0.0f, nullptr))
                run("Collapse edge", opCo, [&s](int) { s.sel.clear(); });
            ImGui::SameLine();
            Op opDs = [ea, eb](EditMesh& mm) { return editmesh::dissolveEdge(mm, ea, eb); };
            if (opColumn("dissolve", picto::Icon::Dissolve, one,
                         "Dissolve\nRemove the edge by joining the two faces either side of\n"
                         "it into one. One edge at a time.",
                         opDs, nullptr, 0.0f, 0.0f, 0.0f, nullptr))
                run("Dissolve edge", opDs, [&s](int) { s.sel.clear(); });
            ImGui::SameLine();
            const int n = one ? editmesh::loopLengthEdge(m, ea, eb) : 0;
            std::snprintf(lbl, sizeof lbl, "%d", n);
            const float at = am.loopAt;
            Op opLc = [ea, eb, at](EditMesh& mm) {
                return editmesh::loopCutEdge(mm, ea, eb, at);
            };
            if (opColumn("edgeloop", picto::Icon::LoopCutV, n > 0,
                         "Loop cut (the number: how many faces it splits)\n"
                         "Run a new edge right round the mesh, crossing this one.\n"
                         "The number below says where along it the cut falls.",
                         opLc, &am.loopAt, 0.05f, 0.05f, 0.95f, "cut at %.2f", lbl))
                run("Loop cut", opLc, [&s](int) { s.sel.clear(); });

            ImGui::Spacing();
            const float bw   = am.bevel;
            const int   segs = static_cast<int>(std::lround(am.bevelSegs));
            Op opBv = [es, bw, segs](EditMesh& mm) {
                editmesh::bevelEdges(mm, es, bw, segs);
                return -1;
            };
            if (opColumn("bevel", picto::Icon::Bevel, !es.empty(),
                         "Bevel\nCut the picked edges back into a strip of the width\n"
                         "below: flat with one segment, rounded with more. The\n"
                         "width is held under half the shortest edge beside it.\n"
                         "Pick all edges first to round off a whole box.",
                         opBv, &am.bevel, 0.02f, 0.01f, 5.0f, "%.2f m"))
                run("Bevel", opBv, [&s](int) { s.sel.clear(); });
            ImGui::SameLine();
            // How round: a stepper under a caption rather than a second button --
            // it is the bevel's setting, not an operation.
            captionColumn("bevelsegs", segs > 1 ? "rounded" : "flat",
                          "Bevel segments\n1 cuts a flat chamfer, more round it off.",
                          am.bevelSegs, 1.0f, 1.0f, 8.0f, "%.0f seg");
            ImGui::SameLine();
            std::vector<int> endsMf;
            for (const auto& e : es) { endsMf.push_back(e.first); endsMf.push_back(e.second); }
            Op opMf = [endsMf](EditMesh& mm) { editmesh::makeFace(mm, endsMf); return -1; };
            if (opColumn("makefaceedge", picto::Icon::MakeFace, es.size() >= 2,
                         "Make a face\nClose the picked edges with a new face -- pick the two\n"
                         "opposite edges of a gap to bridge it.",
                         opMf, nullptr, 0.0f, 0.0f, 0.0f, nullptr))
                run("Make face", opMf, [&s](int) { s.sel.clear(); });
            ImGui::SameLine();
            Op opFh = [ea, eb](EditMesh& mm) { editmesh::fillHole(mm, ea, eb); return -1; };
            bool border = false;
            if (one)
                for (const editmesh::EdgeInfo& e : editmesh::edges(m))
                    if (e.f1 < 0 && std::min(e.a, e.b) == ea && std::max(e.a, e.b) == eb)
                        border = true;
            if (opColumn("fillhole", picto::Icon::FillHole, border,
                         "Fill the hole\nClose the hole this edge runs along with one face. One\n"
                         "edge of its rim is enough; the whole rim is followed.",
                         opFh, nullptr, 0.0f, 0.0f, 0.0f, nullptr))
                run("Fill hole", opFh, [&s](int) { s.sel.clear(); });
        }

        // --- Spin, duplicate, duplicate along a path (ModelingSweep.cpp) -----
        sweepTools(s, pending);

        // --- Nudge: exact steps along the world axes -------------------------
        // A click moves what is picked by exactly the step. This is the drag-free
        // way to put a corner where it belongs, and it is why the gizmo is the
        // convenience on top rather than the only way in.
        ImGui::Spacing();
        const std::vector<int> act = modeltools::activeVerts(sel, m, face);
        ImGui::BeginDisabled(act.empty());
        {
            // Not a word but the four-way arrow: "move it by a step".
            const float  ps = smallH();
            const ImVec2 p  = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(ps, ps));
            picto::draw(ImGui::GetWindowDrawList(), picto::Icon::Nudge,
                        ImVec2(p.x + ps * 0.5f, p.y + ps * 0.5f), ps * 0.36f,
                        ImGui::GetColorU32(act.empty() ? ImGuiCol_TextDisabled : ImGuiCol_Text));
            ImGui::SetItemTooltip("Nudge: move what is picked by exactly the step");
        }
        ImGui::SameLine();
        stepper("nudge", am.nudge, 0.05f, 0.01f, 5.0f, "%.2f m", stepperWidth("%.2f m"));
        ImGui::SameLine(0.0f, 12.0f);

        static const char*  kAxis[6]    = {"-X", "+X", "-Y", "+Y", "-Z", "+Z"};
        // An arrow per direction, in the axis's colour. X and Y are the screen's
        // own left/right and down/up; Z has no direction on a flat screen, so it
        // is drawn the way depth is drawn on the box icons: towards you is down
        // and to the left, away is up and to the right.
        static const picto::Icon kArrow[6] = {picto::Icon::ArrowLeft, picto::Icon::ArrowRight,
                                              picto::Icon::ArrowDown, picto::Icon::ArrowUp,
                                              picto::Icon::ArrowIn,   picto::Icon::ArrowOut};
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
            char tip[64];
            std::snprintf(tip, sizeof tip, "%s %.2f m\nMove what is picked along world %s",
                          kAxis[k], am.nudge, kAxis[k]);
            const bool clicked = picto::buttonSized(kAxis[k], kArrow[k], tip, !act.empty(),
                                                    false, ImVec2(smallH() * 1.25f, smallH()));
            ImGui::PopStyleColor(2);
            if (!act.empty() && ImGui::IsItemHovered()) modeltools::preview(op);
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
                if (picto::button("editmat", picto::Icon::Pencil,
                                  "Edit this material\n(opens it in the Materials panel)",
                                  true, false, ImGui::GetFrameHeight()) &&
                    s.editMaterial)
                    s.editMaterial(cur);
                ImGui::SameLine();
                if (picto::button("backmat", picto::Icon::Undo,
                                  "Back to the object's own material", true, false,
                                  ImGui::GetFrameHeight())) {
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
                     ? "Click a face in the viewport (Shift+click, or the pointer\n"
                       "with the plus, adds more). The gizmo (Q/W/E) drags them;\n"
                       "Del removes them; Esc hands it back to the whole object."
                     : sel.mode == Mode::Vertex
                           ? "Click corners in the viewport. The gizmo (Q/W/E) moves\n"
                             "every one that is picked; Del removes them."
                           : "Click edges in the viewport. The gizmo (Q/W/E) moves\n"
                             "both their ends; Del dissolves a single edge.");

        // Outside every read of the mesh: applying this runs host code that
        // re-centres the geometry the widgets above were drawing from.
        if (wantSet && s.edit) {
            const fitzel::AssetId  id = wantId;
            const std::vector<int> fs = modeltools::selectedFaces(sel, m, face);
            const int              f  = face;
            pending = [&s, fs, f, id]() {
                s.edit([fs, f, id](MeshComponent& comp) {
                    for (int k : fs) comp.mesh.setFaceMaterial(k, id);
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
