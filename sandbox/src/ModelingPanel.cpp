#include "ModelingPanel.hpp"

#include <cstdint>
#include <string>

#include <imgui.h>

#include "Component.hpp"
#include "UiStyle.hpp"

namespace modelui {

namespace {

// The amounts the operations apply, in metres (a factor, for scale). They live
// between invocations on purpose: pressing the same button five times with the
// same step is a legitimate way to build a staircase here, and re-typing the
// number each time would be the tax on doing it that way.
float g_extrude = 0.5f;
float g_move    = 0.25f;
float g_inset   = 0.1f;
float g_scale   = 0.75f;

// Where a loop cut falls along the band (0..1 across the faces it crosses) and
// which of the two bands through the selected face it runs in. A quad belongs to
// two of them, at right angles, and no amount of pointing says which one is
// meant -- so it is a button that swaps, with the count of faces each would cut
// written next to it.
float g_loopAt  = 0.5f;
int   g_loopDir = 0;

// How long the two bands through the selected face are, and what that answer was
// worked out for. Walking a band means deriving the mesh's edges first, and doing
// that twice per frame for a read-out is a cost that grows with the mesh while
// the answer only changes when the selection or the geometry does.
std::uint64_t g_ringRev  = 0;
int           g_ringFace = -1;
int           g_ringLen[2] = {0, 0};

// The filter inside the face-material picker. One buffer: only one popup is open
// at a time, and it starts empty each time so the picker never opens already
// hiding most of the library.
char g_matFilter[64] = {};

// The library entry `id` names, or nullptr. Not Document::materialIndex(), which
// answers 0 -- a real material -- for a GUID it does not know, and a face wearing
// the object's material has to stay visibly undressed.
const MaterialDef* findMaterial(const PanelState& s, const fitzel::AssetId& id) {
    if (!id.valid()) return nullptr;
    for (const MaterialDef& md : s.materials)
        if (md.assetId == id) return &md;
    return nullptr;
}

// Full-width, tall enough to be aimed at rather than hit precisely. Every one of
// these is a target for a hand that may not land exactly where it meant to.
bool bigButton(const char* label) {
    return ImGui::Button(label, ImVec2(-1.0f, 30.0f));
}

} // namespace

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    if (ImGui::Begin("Modeling", &s.show)) {
        if (!s.mesh) {
            ui::sectionText("No editable mesh");
            if (!s.haveSelection) {
                ui::hint("Select an object first. A box becomes editable in one\n"
                         "click, and from there you shape it face by face.");
            } else if (s.canConvert) {
                ui::hint("This box can become an editable mesh: the same shape,\n"
                         "but with faces you can pull out, push in and scale.\n"
                         "Nothing else about it changes.");
                ImGui::Spacing();
                if (bigButton("Make editable") && s.convert) s.convert();
            } else {
                ui::hint("Only boxes become meshes. Imported models are left as\n"
                         "their author made them.");
            }
            ImGui::End();
            return;
        }

        ImGui::Text("%d faces, %d corners", s.faceCount, s.vertCount);
        ImGui::TextDisabled("Click a face in the viewport to select it.");
        if (s.faceSel >= 0)
            ui::hint("The gizmo works on the selected face: Move, Rotate and\n"
                     "Scale (Q/W/E) drag its corners instead of the whole\n"
                     "object. Click past the mesh to hand it back.");
        ImGui::Separator();

        const int  face = s.faceSel;
        const bool have = face >= 0;
        if (!have)
            ui::hint("No face selected -- everything below acts on one face.");
        ImGui::BeginDisabled(!have);

        ui::sectionText("Grow");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat("##ext", &g_extrude, 0.01f, -20.0f, 20.0f, "Extrude %.2f m");
        if (bigButton("Extrude") && s.edit) {
            const float d = g_extrude;
            s.edit([face, d](MeshComponent& m) { return editmesh::extrude(m.mesh, face, d); },
                   "Extrude");
        }
        ui::hint("Pulls the face out and walls in the gap. The new face keeps\n"
                 "the selection, so pressing it again builds another step.");

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat("##mov", &g_move, 0.01f, -20.0f, 20.0f, "Move %.2f m");
        if (bigButton("Move along normal") && s.edit) {
            const float d = g_move;
            s.edit([face, d](MeshComponent& m) { return editmesh::moveFace(m.mesh, face, d); },
                   "Move face");
        }
        ui::hint("Slides the face without adding anything: the neighbours\n"
                 "stretch to follow, so the whole shape grows instead.");

        ui::sectionText("Shape");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat("##scl", &g_scale, 0.005f, 0.0f, 4.0f, "Scale %.2fx");
        if (bigButton("Scale face") && s.edit) {
            const float k = g_scale;
            s.edit([face, k](MeshComponent& m) { return editmesh::scaleFace(m.mesh, face, k); },
                   "Scale face");
        }
        ui::hint("Scales the face about its own centre. Shrinking the top of a\n"
                 "box gives a frustum -- the corners are shared, so the sides\n"
                 "come along.");

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat("##ins", &g_inset, 0.005f, 0.0f, 10.0f, "Inset %.2f m");
        if (bigButton("Inset") && s.edit) {
            const float a = g_inset;
            s.edit([face, a](MeshComponent& m) { return editmesh::inset(m.mesh, face, a); },
                   "Inset");
        }
        ui::hint("Lays a border inside the face and selects the middle. Inset,\n"
                 "then extrude inwards, is a window.");

        ui::sectionText("Detail");
        // Loop cut: the cut that goes all the way round. `dir` is not something
        // the click can say -- the face lies in two bands at once -- so it is a
        // button that swaps between them, and both counts are written out so the
        // choice is made before the cut rather than judged after it.
        {
            if (g_ringRev != s.mesh->revision || g_ringFace != face) {
                g_ringRev   = s.mesh->revision;
                g_ringFace  = face;
                g_ringLen[0] = have ? editmesh::loopLength(s.mesh->mesh, face, 0) : 0;
                g_ringLen[1] = have ? editmesh::loopLength(s.mesh->mesh, face, 1) : 0;
            }
            const int nA = g_ringLen[0], nB = g_ringLen[1];
            const int n  = (g_loopDir & 1) ? nB : nA;
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::SliderFloat("##loopat", &g_loopAt, 0.05f, 0.95f, "Cut at %.2f");
            if (ImGui::Button(g_loopDir ? "Direction: across##loopdir"
                                        : "Direction: along##loopdir",
                              ImVec2(-1.0f, 26.0f)))
                g_loopDir ^= 1;
            ImGui::TextDisabled("along: %d face%s | across: %d face%s",
                                nA, nA == 1 ? "" : "s", nB, nB == 1 ? "" : "s");
            ImGui::BeginDisabled(n <= 0);
            if (bigButton(n > 0 ? (std::string("Loop cut (") + std::to_string(n) +
                                   " faces)").c_str()
                                : "Loop cut") &&
                s.edit) {
                const float t = g_loopAt;
                const int   d = g_loopDir;
                s.edit([face, d, t](MeshComponent& m) {
                    return editmesh::loopCut(m.mesh, face, d, t);
                }, "Loop cut");
            }
            ImGui::EndDisabled();
            ui::hint("Runs a new edge right around the band the face lies in and\n"
                     "splits every quad it crosses -- a storey line on a tower,\n"
                     "a joint to bend a wall at. Only quads: the cut stops\n"
                     "where the mesh does.");
        }

        ImGui::Spacing();
        if (bigButton("Subdivide into four") && s.edit)
            s.edit([face](MeshComponent& m) { return editmesh::subdivide(m.mesh, face); },
                   "Subdivide");
        ImGui::Spacing();
        if (bigButton("Delete face") && s.edit)
            s.edit([face](MeshComponent& m) { return editmesh::deleteFace(m.mesh, face); },
                   "Delete face");

        // --- The face's own material ----------------------------------------
        // A face can wear a material of its own instead of the object's: brick on
        // one side, plaster on the other, one object. Picking it here is the
        // no-drag way in; dropping a material from the Assets browser straight
        // onto the face in the viewport does the same thing.
        ui::sectionText("Material");
        const fitzel::AssetId cur = s.mesh->mesh.faceMaterial(face);
        const MaterialDef*    md  = findMaterial(s, cur);
        // What the panel WANTS, applied once it has stopped reading the mesh: the
        // edit runs host code that re-centres the geometry and banks an undo step,
        // and a picker still drawing from the component afterwards would be
        // reading through it as it went.
        bool            wantSet = false;
        fitzel::AssetId wantId;

        ImGui::SetNextItemWidth(-1.0f);
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
                // Model-owned materials are not the library's to hand out: they
                // are re-created by the next import, and a face pointing at one
                // would be wearing a GUID nobody answers to after a reload.
                if (cand.fromModel) continue;
                if (!ui::icontains(cand.name.c_str(), g_matFilter)) continue;
                const bool sel = (md && md->assetId == cand.assetId);
                if (ImGui::Selectable(cand.name.c_str(), sel)) {
                    wantSet = true;
                    wantId  = cand.assetId;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (md) {
            if (ImGui::SmallButton("Edit this material") && s.editMaterial)
                s.editMaterial(cur);
            ImGui::SameLine();
            if (ImGui::SmallButton("Back to the object's")) {
                wantSet = true;
                wantId  = fitzel::AssetId{};
            }
        }
        // How far the dressing has spread. Worth saying: a face wearing its own
        // material is a second draw call for the object, and the count is the only
        // place that shows how many of those an afternoon of clicking has made.
        int dressed = 0;
        for (int f = 0; f < static_cast<int>(s.mesh->mesh.faces.size()); ++f)
            if (s.mesh->mesh.faceMaterial(f).valid()) ++dressed;
        if (dressed > 0)
            ImGui::TextDisabled("%d of %d faces wear their own material", dressed,
                                static_cast<int>(s.mesh->mesh.faces.size()));
        ui::hint("Only this face changes. You can also drag a material from the\n"
                 "Assets browser straight onto a face in the viewport.");

        ImGui::EndDisabled();

        // Outside the disabled block and after every read of the mesh: applying
        // this replaces the entity the panel is drawing from.
        if (wantSet && s.edit) {
            const fitzel::AssetId id = wantId;
            s.edit([face, id](MeshComponent& m) {
                m.mesh.setFaceMaterial(face, id);
                return face;
            }, "Face material");
        }
    }
    ImGui::End();
}

} // namespace modelui
