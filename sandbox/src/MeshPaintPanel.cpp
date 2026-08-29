#include "MeshPaintPanel.hpp"
#include "UiStyle.hpp"

#include <string>

#include <imgui.h>

#include "Component.hpp"

namespace meshpaintui {

namespace {

// Full-width and tall enough to be aimed at rather than hit precisely, the same
// as the Modeling panel's buttons and for the same reason.
bool bigButton(const char* label) {
    return ImGui::Button(label, ImVec2(-1.0f, 30.0f));
}

// One shared filter buffer across the four slot pickers -- only one popup is
// open at a time, and it starts empty each time so a picker never opens already
// hiding most of the library.
char g_pickFilter[64] = {};

// The library entry a slot points at, or nullptr for an empty slot. Not
// Document::materialIndex(), which answers 0 -- a real material -- for a GUID it
// does not know, and an empty slot has to stay visibly empty.
const MaterialDef* slotMaterial(const PanelState& s, const MeshPaintSlot& sl) {
    if (!sl.material.valid()) return nullptr;
    for (const MaterialDef& md : s.materials)
        if (md.assetId == sl.material) return &md;
    return nullptr;
}

} // namespace

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    if (ImGui::Begin("Mesh Paint", &s.show)) {
        if (!s.mesh) {
            s.paintMode = false; // nothing to paint on -> don't sit on the left button
            ui::sectionText("No editable mesh");
            if (!s.haveSelection) {
                ui::hint("Select an object first. Paint goes onto the object's own\n"
                         "corners, so it belongs to that object and moves with it.");
            } else if (s.canConvert) {
                ui::hint("This box can become an editable mesh -- the same shape,\n"
                         "with faces the brush can split and paint.");
                ImGui::Spacing();
                if (bigButton("Make editable") && s.convert) s.convert();
            } else {
                ui::hint("Only modelled meshes take paint. An imported model's\n"
                         "geometry is shared by every copy of it in the scene, so\n"
                         "painting one would paint all of them.");
            }
            ImGui::End();
            return;
        }

        if (ImGui::Checkbox("Paint mode", &s.paintMode) && s.paintMode)
            s.terrainPaintMode = s.grassPaintMode = s.roadEditMode = s.treePaintMode =
                s.flowerPaintMode = s.sculptMode = s.scatterMode = false; // owns the LMB
        if (s.paintMode)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.85f, 1.0f),
                "Hold LMB on the object | Alt (or Erase) takes the paint back off");
        else
            ImGui::TextDisabled("Enable to paint textures onto the selected mesh");

        ui::sectionText("This object's paint");
        ui::hint("Four slots, filled from the material library. They belong to\n"
                 "this object: what you choose here paints this mesh and nothing\n"
                 "else in the scene.");

        const auto& slots = s.mesh->paintSlots;
        for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
            // Read-only: a slot is changed through s.edit, after this
            // panel has stopped looking at the component. See SlotEdit.
            const MeshPaintSlot& sl = slots[i];
            const MaterialDef* md = slotMaterial(s, sl);
            ImGui::PushID(i);

            // The radio IS the slot: picking one to paint with and saying what it
            // holds are the same decision, so they sit on the same row.
            if (ImGui::RadioButton("##use", s.slot == i)) s.slot = i;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            const char* label = md ? md->name.c_str() : "(empty)";
            if (ImGui::BeginCombo("##mat", label)) {
                if (ImGui::IsWindowAppearing()) {
                    g_pickFilter[0] = 0;
                    ImGui::SetKeyboardFocusHere();
                }
                ui::searchBox("##slotf", g_pickFilter, sizeof(g_pickFilter));
                if (ImGui::Selectable("(empty)", md == nullptr))
                    s.edit = {i, true, fitzel::AssetId{}, false, 0.0f};
                for (const MaterialDef& cand : s.materials) {
                    if (!ui::icontains(cand.name.c_str(), g_pickFilter)) continue;
                    // A material with no base-colour texture has nothing to paint
                    // WITH -- the brush lays down a texture, not a flat colour.
                    if (!cand.tex) continue;
                    const bool sel = (md && md->assetId == cand.assetId);
                    if (ImGui::Selectable(cand.name.c_str(), sel)) {
                        s.edit = {i, true, cand.assetId, false, 0.0f};
                        s.slot = i;   // choosing one is also choosing to paint it
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            if (md) {
                ImGui::SetNextItemWidth(-60.0f);
                float sc = sl.scale;
                if (ImGui::SliderFloat("##scale", &sc, 0.02f, 4.0f, "%.2f /m"))
                    s.edit = {i, false, fitzel::AssetId{}, true, sc};
                ImGui::SameLine();
                if (ImGui::SmallButton("Edit") && s.editMaterial) s.editMaterial(i);
            }
            ImGui::PopID();
        }

        bool anyFilled = false;
        for (const MeshPaintSlot& sl : slots) anyFilled = anyFilled || slotMaterial(s, sl);
        if (!anyFilled)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f),
                "Fill a slot with a textured material to paint with.");
        else if (!slotMaterial(s, slots[s.slot]))
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f),
                "The chosen slot is empty -- paint would land on nothing.");

        ui::sectionText("Brush");
        ImGui::SliderFloat("Radius", &s.radius, 0.05f, 10.0f, "%.2f m");
        ImGui::SliderFloat("Strength", &s.strength, 0.05f, 1.0f);
        ImGui::Checkbox("Erase (back to the material)", &s.erase);

        ui::sectionText("Detail");
        ImGui::SliderFloat("Face size", &s.detail, 0.05f, 2.0f, "%.2f m");
        ui::hint("The brush splits the faces it crosses until their edges are\n"
                 "this short -- paint lives on corners, and a wall with four of\n"
                 "them can only be painted whole. Smaller means a finer stroke\n"
                 "and more faces; the split stops at a few thousand.");

        ImGui::Text("%d faces, %d painted corners", s.faceCount, s.paintedCount);
        ui::hint("Paint changes the object's colour, not its relief: a slot's\n"
                 "normal map is not part of the stroke.");
        ImGui::BeginDisabled(s.paintedCount == 0);
        if (ImGui::Button("Clear paint") && s.clearPaint) s.clearPaint();
        ImGui::EndDisabled();
    }
    ImGui::End();
}

} // namespace meshpaintui
