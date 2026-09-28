#include "MeshPaintPanel.hpp"
#include "UiStyle.hpp"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

#include "Command.hpp"
#include "Component.hpp"
#include "EditorContext.hpp"
#include "MeshPaint.hpp"

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

// The editable mesh on the selected object, if it has one.
MeshComponent* selectedMesh(EditorContext& ed) {
    if (!ed.sel.valid()) return nullptr;
    return ed.entities[ed.sel.index()].components.get<MeshComponent>();
}

// Where a stroke stops splitting. A brush held down over a wall would otherwise
// quarter its faces until the editor stops.
constexpr int kMaxFaces = 4000;

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
                ui::hint("This shape can become an editable mesh -- the same shape,\n"
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

void panel(EditorContext& ed, Brush& brush, const Host& h) {
    MeshComponent* mc  = selectedMesh(ed);
    const bool haveSel = ed.sel.valid();
    int painted = 0;
    if (mc)
        for (const glm::vec4& w : mc->mesh.paint)
            if (w.x > 0.0f || w.y > 0.0f || w.z > 0.0f || w.w > 0.0f) ++painted;
    brush.slot = glm::clamp(brush.slot, 0, 3);
    // The panel does not touch the document: it says what it wants and this
    // does it below, once the panel has stopped reading from the component. An
    // undo push assigns the entity's snapshot over it and replaces its
    // components, so a panel that edited in place would spend the rest of its
    // frame drawing from freed memory -- which is exactly what it used to do.
    SlotEdit slotEdit;
    bool     wantClearPaint = false;
    drawPanel({
        h.show, h.paintMode,
        h.terrainPaintMode, h.grassPaintMode, h.roadEditMode, h.treePaintMode,
        h.flowerPaintMode, h.sculptMode, h.scatterMode,
        ed.materials, brush.slot, brush.radius, brush.strength,
        brush.detail, brush.erase,
        mc, haveSel,
        haveSel && !mc && isSolidPrimitive(ed.entities[ed.sel.index()].type),
        mc ? static_cast<int>(mc->mesh.faces.size()) : 0, painted,
        slotEdit,
        h.convert,
        [&]{ wantClearPaint = true; },
        // "Edit" on a slot: the texture itself is a material, and the place to
        // change a material is the Materials panel. Reads only, so it is safe
        // from inside the panel.
        [&](int k) {
            MeshComponent* m = selectedMesh(ed);
            if (!m || k < 0 || k >= static_cast<int>(m->paintSlots.size())) return;
            const fitzel::AssetId id = m->paintSlots[k].material;
            if (!id.valid()) return;
            ed.matSel       = ed.document.materialIndex(id);
            h.showMaterials = true;
        },
    });

    // What the panel asked for, applied as one undo step each. Filling a slot
    // needs no touch(): the geometry did not move, only what its weights are
    // drawn with.
    if (MeshComponent* m = selectedMesh(ed)) {
        Entity&      e      = ed.entities[ed.sel.index()];
        const Entity before = e;
        bool         did    = false;
        if (slotEdit.slot >= 0 && slotEdit.slot < static_cast<int>(m->paintSlots.size())) {
            MeshPaintSlot& sl = m->paintSlots[slotEdit.slot];
            if (slotEdit.setMaterial) { sl.material = slotEdit.material; did = true; }
            if (slotEdit.setScale)    { sl.scale    = slotEdit.scale;    did = true; }
        }
        if (wantClearPaint && meshpaint::clear(m->mesh)) { m->touch(); did = true; }
        if (did) {
            auto cmd = std::make_unique<ModifyEntityCmd>(before, e);
            if (!cmd->trivial()) ed.history.pushApplied(std::move(cmd));
        }
    }
}

void brushViewport(EditorContext& ed, const ViewportFrame& view, Brush& brush,
                   bool& paintMode, bool othersActive, float dt) {
    MeshComponent* mc = selectedMesh(ed);
    // Bank a stroke that is in progress, whichever way this is left. Without
    // it, dropping the brush mid-stroke keeps the snapshot around and the NEXT
    // stroke undoes back past it -- one Ctrl+Z throwing away work the user
    // never joined up. Looks the entity up by id: the push replaces components,
    // so no pointer taken before it survives, `mc` included.
    auto bankStroke = [&]{
        if (!brush.stroking) return;
        brush.stroking = false;
        Entity* e = ed.document.find(brush.before.id);
        if (!e) return;
        auto cmd = std::make_unique<ModifyEntityCmd>(brush.before, *e);
        if (!cmd->trivial()) ed.history.pushApplied(std::move(cmd));
    };
    // Only one tool may own the left button. The older panels each switch
    // their rivals off from their own list; rather than add this one to six of
    // them, the newcomer yields -- the same deal the spline editor takes.
    if (othersActive) {
        bankStroke();
        paintMode = false;
        return;
    }
    if (!mc) {
        bankStroke();
        // The selection moved off the mesh -- there is nothing to paint on, so
        // let go of the left button rather than sit on it invisibly.
        paintMode = false;
        return;
    }

    Entity& me = ed.entities[ed.sel.index()];
    // The matrix the mesh is DRAWN through, so the brush measures metres where
    // the user sees them even on an object somebody scaled.
    const glm::mat4 mm = meshModelOf(me, *mc);

    glm::vec3 ro, rd;
    view.mouseRay(ro, rd);

    meshpaint::Hit hit;
    const bool onMesh  = view.hovered && meshpaint::pick(mc->mesh, mm, ro, rd, hit);
    const bool erasing = brush.erase || ImGui::GetIO().KeyAlt;

    // An empty slot has nothing to lay down, so the brush does not lay it down:
    // weights in a slot the shader will skip are invisible work, and the panel
    // says so where the slot is chosen. Erasing stays available -- taking paint
    // off needs no slot at all.
    const bool slotReady = brush.slot >= 0 &&
                           brush.slot < static_cast<int>(mc->paintSlots.size()) &&
                           mc->paintSlots[brush.slot].material.valid();
    if (onMesh && (slotReady || erasing) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (!brush.stroking) {
            brush.stroking = true;
            brush.before   = me; // the whole stroke undoes as one
        }
        const float rate = glm::clamp(brush.strength * 4.0f * dt, 0.0f, 1.0f);
        // Split first, then paint: the dab lands on the corners the split just
        // made rather than on the four the face started with. Erasing never
        // splits -- taking paint off needs no more corners than putting it on did.
        const int split =
            erasing ? 0
                    : meshpaint::refine(mc->mesh, mm, hit.world, brush.radius, brush.detail,
                                        kMaxFaces);
        const bool dabbed =
            meshpaint::dab(mc->mesh, mm, hit.world, brush.radius, brush.slot, rate, erasing);
        if (split > 0 || dabbed) mc->touch();
        // A split leaves the selected index pointing at a QUARTER of the face it
        // was picked on. Rather than hand the modelling panel a face nobody
        // chose, drop the selection.
        if (split > 0) ed.meshFaceSel = -1;
    }
    // The stroke ended -- but the undo push is deferred to the BOTTOM of this
    // function on purpose. Pushing runs the command's redo, which assigns the
    // "after" snapshot over the entity and so replaces its components with
    // fresh clones: every MeshComponent* taken above is dead the instant it
    // happens, `mc` included, and the brush cursor below still wants it. Bank
    // the stroke once nothing needs the pointer any more.
    const bool endStroke = brush.stroking && !ImGui::IsMouseDown(ImGuiMouseButton_Left);

    // Brush cursor: a ring lying in the surface it would paint, lifted a hair
    // off it so it is not swallowed by the face.
    if (onMesh && mc->mesh.validFace(hit.face)) {
        std::vector<glm::vec3> w;
        for (int i : mc->mesh.faces[hit.face])
            w.push_back(glm::vec3(mm * glm::vec4(mc->mesh.verts[i], 1.0f)));
        glm::vec3 fn(0.0f, 1.0f, 0.0f);
        if (w.size() >= 3) {
            const glm::vec3 c = glm::cross(w[1] - w[0], w[2] - w[0]);
            if (glm::dot(c, c) > 1e-12f) fn = glm::normalize(c);
        }
        // Any two axes in the face's plane will do for a circle.
        const glm::vec3 ref = (std::abs(fn.y) > 0.9f) ? glm::vec3(1.0f, 0.0f, 0.0f)
                                                      : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 t1 = glm::normalize(glm::cross(ref, fn));
        const glm::vec3 t2 = glm::cross(fn, t1);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 col = erasing ? IM_COL32(205, 205, 215, 225)
                                  : IM_COL32(90, 230, 210, 225);
        const int SEG = 48;
        ImVec2 prev; bool have = false;
        for (int i = 0; i <= SEG; ++i) {
            const float a = static_cast<float>(i) / SEG * 6.2831853f;
            const glm::vec3 p = hit.world + fn * 0.01f +
                                (t1 * std::cos(a) + t2 * std::sin(a)) * brush.radius;
            ImVec2 sp;
            if (!view.toScreen(p, sp)) { have = false; continue; }
            if (have) dl->AddLine(prev, sp, col, 2.0f);
            prev = sp; have = true;
        }
    }

    // Last thing here: see the comment at `endStroke`. `mc` must be treated as
    // dangling from here on.
    if (endStroke) bankStroke();
}

} // namespace meshpaintui
