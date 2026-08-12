#include "ModelingPanel.hpp"

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
        if (bigButton("Subdivide into four") && s.edit)
            s.edit([face](MeshComponent& m) { return editmesh::subdivide(m.mesh, face); },
                   "Subdivide");
        ImGui::Spacing();
        if (bigButton("Delete face") && s.edit)
            s.edit([face](MeshComponent& m) { return editmesh::deleteFace(m.mesh, face); },
                   "Delete face");

        ImGui::EndDisabled();
    }
    ImGui::End();
}

} // namespace modelui
