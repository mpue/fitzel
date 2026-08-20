#include "SplinePanel.hpp"

#include <cstdio>
#include <string>

#include <imgui.h>

#include "SplineSystem.hpp"
#include "UiStyle.hpp"

namespace splineui {
namespace {

using splinegen::Kind;
using splinegen::Preset;

constexpr int kPresetCount = static_cast<int>(Preset::Count);

// A slider that brackets itself into one undo step and marks the path for
// regeneration. Every control in this panel goes through it, so "did I remember
// to rebuild" is not a question the rest of the file has to keep answering.
bool slider(const PanelState& s, int path, const char* label, float* v,
            float lo, float hi, const char* fmt = "%.2f m") {
    const bool changed = ImGui::SliderFloat(label, v, lo, hi, fmt);
    if (ImGui::IsItemActivated())            s.beginEdit();
    if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit(label);
    if (changed) s.splines.touch(path);
    return changed;
}

bool sliderInt(const PanelState& s, int path, const char* label, int* v,
               int lo, int hi) {
    const bool changed = ImGui::SliderInt(label, v, lo, hi);
    if (ImGui::IsItemActivated())            s.beginEdit();
    if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit(label);
    if (changed) s.splines.touch(path);
    return changed;
}

bool colorEdit(const PanelState& s, int path, const char* label, glm::vec3& c) {
    const bool changed = ImGui::ColorEdit3(label, &c.x);
    if (ImGui::IsItemActivated())            s.beginEdit();
    if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit(label);
    if (changed) s.splines.touch(path);
    return changed;
}

// --- Style sections ----------------------------------------------------------
// Split by kind so switching a path from a fence to a wall changes which numbers
// are on offer and nothing else -- the fence's own numbers stay where they were,
// untouched, for when it is switched back.

void fenceStyle(const PanelState& s, int i, splinegen::Style& st) {
    ui::sectionText("Posts");
    slider(s, i, "Spacing", &st.postSpacing, 0.3f, 12.0f);
    slider(s, i, "Height", &st.postHeight, 0.2f, 6.0f);
    slider(s, i, "Thickness", &st.postWidth, 0.02f, 0.8f, "%.3f m");
    slider(s, i, "Height variation", &st.postJitter, 0.0f, 0.6f, "%.2f");
    slider(s, i, "Cap height", &st.postCap, 0.0f, 0.4f, "%.3f m (0 = none)");
    if (st.postCap > 0.0f)
        slider(s, i, "Cap oversail", &st.postCapOver, 0.0f, 0.2f, "%.3f m");

    ui::sectionText("Bars");
    sliderInt(s, i, "Count", &st.rails, 0, 8);
    if (st.rails > 0) {
        slider(s, i, "Bar thickness", &st.railThick, 0.01f, 0.4f, "%.3f m");
        slider(s, i, "Top bar", &st.railTop, 0.1f, 1.2f, "%.2f of height");
        slider(s, i, "Bottom bar", &st.railBottom, 0.0f, 1.0f, "%.2f of height");
    }

    ui::sectionText("Pickets");
    slider(s, i, "Picket spacing", &st.picketEvery, 0.0f, 1.2f, "%.3f m (0 = none)");
    ui::hint("Vertical bars between the posts: pickets, palings, balusters, "
             "railings.");
    if (st.picketEvery > 0.0f) {
        slider(s, i, "Picket width", &st.picketWidth, 0.01f, 0.5f, "%.3f m");
        slider(s, i, "Picket depth", &st.picketDepth, 0.005f, 0.5f, "%.3f m");
        slider(s, i, "Picket top", &st.picketTop, 0.1f, 1.2f, "%.2f of height");
        slider(s, i, "Picket bottom", &st.picketBottom, 0.0f, 1.0f, "%.2f of height");
    }

    ui::sectionText("Infill");
    slider(s, i, "Panel thickness", &st.infill, 0.0f, 0.5f, "%.3f m");
    ui::hint("0 leaves the bays open. Give the Panel material a cutout texture "
             "for mesh or chain-link.");
    if (st.infill > 0.0f) {
        slider(s, i, "Panel top", &st.infillTop, 0.1f, 1.2f, "%.2f of height");
        slider(s, i, "Panel bottom", &st.infillBottom, 0.0f, 1.0f, "%.2f of height");
    }
}

void wallStyle(const PanelState& s, int i, splinegen::Style& st) {
    ui::sectionText("Wall");
    slider(s, i, "Height", &st.wallHeight, 0.2f, 20.0f);
    slider(s, i, "Thickness", &st.wallThick, 0.05f, 4.0f);
    slider(s, i, "Taper", &st.wallTaper, 0.2f, 1.5f, "%.2f of base");
    ui::hint("Below 1 the wall batters inwards -- a retaining wall; above 1 it "
             "corbels out.");

    ui::sectionText("Base course");
    slider(s, i, "Toe height", &st.toeHeight, 0.0f, 2.0f, "%.3f m (0 = none)");
    if (st.toeHeight > 0.0f) {
        slider(s, i, "Toe splay", &st.toeOver, 0.0f, 1.0f, "%.3f m");
        ui::hint("No splay is a vertical foot under a battered face -- a Jersey "
                 "barrier. Splayed, it is a plinth.");
    }

    ui::sectionText("Coping");
    slider(s, i, "Cap height", &st.copingHeight, 0.0f, 1.0f, "%.3f m");
    if (st.copingHeight > 0.0f)
        slider(s, i, "Cap oversail", &st.copingOver, 0.0f, 0.6f, "%.3f m");

    ui::sectionText("Battlements");
    slider(s, i, "Merlon spacing", &st.merlonEvery, 0.0f, 8.0f, "%.2f m (0 = none)");
    if (st.merlonEvery > 0.0f) {
        slider(s, i, "Merlon width", &st.merlonWidth, 0.1f, 6.0f);
        slider(s, i, "Merlon rise", &st.merlonRise, 0.0f, 3.0f);
        slider(s, i, "Merlon inset", &st.merlonInset, 0.0f, 1.0f, "%.3f m");
    }

    ui::sectionText("Piers");
    slider(s, i, "Every", &st.pillarEvery, 0.0f, 40.0f, "%.1f m (0 = none)");
    if (st.pillarEvery > 0.0f) {
        slider(s, i, "Pier width", &st.pillarWidth, 0.1f, 3.0f);
        slider(s, i, "Pier rise", &st.pillarRise, 0.0f, 2.0f);
    }
}

void railStyle(const PanelState& s, int i, splinegen::Style& st) {
    ui::sectionText("Track");
    slider(s, i, "Gauge", &st.gauge, 0.4f, 4.0f, "%.3f m");
    ui::hint("1.435 m is standard gauge; 0.760 is a narrow-gauge line.");
    slider(s, i, "Rail height", &st.railHeight, 0.05f, 0.4f, "%.3f m");
    slider(s, i, "Rail width", &st.railWidth, 0.02f, 0.2f, "%.3f m");

    ui::sectionText("Sleepers");
    slider(s, i, "Spacing", &st.sleeperSpacing, 0.2f, 3.0f);
    slider(s, i, "Length", &st.sleeperLength, 0.5f, 5.0f);
    slider(s, i, "Width", &st.sleeperWidth, 0.05f, 0.8f);
    slider(s, i, "Depth", &st.sleeperHeight, 0.05f, 0.5f);

    ui::sectionText("Ballast");
    slider(s, i, "Bed width", &st.ballastWidth, 0.0f, 12.0f, "%.2f m (0 = none)");
    if (st.ballastWidth > 0.0f) {
        slider(s, i, "Bed depth", &st.ballastHeight, 0.0f, 2.0f);
        slider(s, i, "Bed batter", &st.ballastSlope, 0.0f, 3.0f, "%.2f m per m");
    }
}

// What the three material slots are CALLED for this kind. The generator's slots
// are fixed (primary/secondary/tertiary); only their meaning moves.
const char* slotName(Kind k, int slot) {
    switch (k) {
        case Kind::Wall:  return slot == 0 ? "Face"  : slot == 1 ? "Coping"   : "Piers";
        case Kind::Rail:  return slot == 0 ? "Steel" : slot == 1 ? "Sleepers" : "Ballast";
        case Kind::Fence:
        case Kind::Count: break;
    }
    return slot == 0 ? "Posts" : slot == 1 ? "Bars" : "Pickets / panel";
}

// One element's material: either the shared palette slot (the default, coloured
// by the swatch beside it) or any material in the project library, textures and
// all. Returns true when the assignment changed.
bool materialSlot(const PanelState& s, int path, Kind kind, int slot,
                  fitzel::AssetId& id, glm::vec3& color) {
    bool changed = false;
    ImGui::PushID(slot);

    const char* label = slotName(kind, slot);
    const char* current = "Palette (colour below)";
    for (const MaterialDef& m : s.materials)
        if (id.valid() && m.assetId == id) current = m.name.c_str();

    if (ImGui::BeginCombo(label, current)) {
        if (ImGui::Selectable("Palette (colour below)", !id.valid())) {
            s.beginEdit();
            id = fitzel::AssetId{};
            s.endEdit("Element material");
            s.splines.touch(path);
            changed = true;
        }
        ImGui::Separator();
        for (const MaterialDef& m : s.materials) {
            const bool on = id.valid() && m.assetId == id;
            ImGui::PushID(m.assetId.toString().c_str());
            if (ImGui::Selectable(m.name.c_str(), on)) {
                s.beginEdit();
                id = m.assetId;
                s.endEdit("Element material");
                s.splines.touch(path);
                changed = true;
            }
            if (on) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    if (id.valid()) {
        // Straight to the material, so adding a texture to the brick you just
        // picked is one click rather than a hunt through a list of names.
        ImGui::SameLine();
        if (ImGui::SmallButton("Edit") && s.editMaterial) s.editMaterial(id);
    } else {
        // No override: the palette colour IS this element's look, so it belongs
        // on the same row rather than in a separate colour section.
        ImGui::SameLine();
        ImGui::PushID("col");
        changed |= colorEdit(s, path, "##color", color);
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}

// The preset list for one kind, as menu items. Shared by the add buttons and the
// selected path's picker so the two can never offer different sets.
int presetMenu(Kind kind, int currentPreset) {
    int picked = -1;
    for (int p = 0; p < kPresetCount; ++p) {
        const auto pr = static_cast<Preset>(p);
        if (splinegen::presetKind(pr) != kind) continue;
        if (ImGui::Selectable(splinegen::presetName(pr), p == currentPreset))
            picked = p;
    }
    return picked;
}

} // namespace

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    ImGui::SetNextWindowSize(ImVec2(360.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Splines", &s.show)) { ImGui::End(); return; }

    SplineSystem& sp = s.splines;

    // --- Edit mode -----------------------------------------------------------
    // First, and full width: laying a path is what this panel is for, and the
    // toggle is the only control that has to be findable without reading.
    {
        const ImVec4 on(0.95f, 0.72f, 0.20f, 1.0f);
        if (s.editMode) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.33f, 0.06f, 1.0f));
        if (ImGui::Button(s.editMode ? "Editing paths (click to stop)"
                                     : "Edit paths in viewport", ImVec2(-1.0f, 0.0f))) {
            s.editMode = !s.editMode;
            if (s.editMode && s.grabLMB) s.grabLMB();
        }
        if (s.editMode) ImGui::PopStyleColor();
        if (s.editMode) {
            ImGui::TextColored(on, "Click the ground to add a point");
            ui::hint("Drag a handle to move it, Ctrl+drag to raise it, Delete to "
                     "remove it. Arrow keys nudge the selected point.");
        }
    }

    // --- Adding: pick the structure, not the category ------------------------
    ui::sectionText("Add");
    {
        const struct { Kind k; const char* label; } kinds[] = {
            {Kind::Fence, "Fence"}, {Kind::Wall, "Wall"}, {Kind::Rail, "Track"}};
        const float w = (ImGui::GetContentRegionAvail().x -
                         ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine();
            char lbl[32];
            std::snprintf(lbl, sizeof(lbl), "%s...", kinds[i].label);
            if (ImGui::Button(lbl, ImVec2(w, 0.0f))) ImGui::OpenPopup(kinds[i].label);
            if (ImGui::BeginPopup(kinds[i].label)) {
                const int picked = presetMenu(kinds[i].k, -1);
                if (picked >= 0) {
                    s.beginEdit();
                    s.sel   = sp.addPath(static_cast<Preset>(picked));
                    s.ptSel = -1;
                    s.endEdit("Add path");
                    // A brand new path has no points yet, so put the author
                    // straight into the mode that gives it some.
                    if (!s.editMode) { s.editMode = true; if (s.grabLMB) s.grabLMB(); }
                }
                ImGui::EndPopup();
            }
        }
    }

    if (sp.paths.empty()) {
        ui::hint("No paths yet. Pick a structure above, then click the ground to "
                 "lay it out point by point.");
        ImGui::End();
        return;
    }

    // --- The path list -------------------------------------------------------
    ui::sectionText("Paths");
    if (ImGui::BeginChild("paths", ImVec2(0.0f, 108.0f), true)) {
        for (int i = 0; i < static_cast<int>(sp.paths.size()); ++i) {
            SplineSystem::Path& p = sp.paths[i];
            ImGui::PushID(i);
            if (ImGui::Checkbox("##on", &p.enabled)) {
                // Toggled already; bracket around the value it had before, the
                // same dance every checkbox in RoadPanel does.
                p.enabled = !p.enabled;
                s.beginEdit();
                p.enabled = !p.enabled;
                s.endEdit("Toggle path");
                sp.touch(i);
            }
            ImGui::SameLine();
            char row[160];
            std::snprintf(row, sizeof(row), "%s  (%s, %d pts)", p.name.c_str(),
                          splinegen::presetName(p.preset),
                          static_cast<int>(p.points.size()));
            if (ImGui::Selectable(row, s.sel == i)) { s.sel = i; s.ptSel = -1; }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if (s.sel < 0 || s.sel >= static_cast<int>(sp.paths.size())) {
        ui::hint("Select a path to edit it.");
        ImGui::End();
        return;
    }

    const int i = s.sel;
    SplineSystem::Path& p = sp.paths[i];
    splinegen::Style&   st = p.style;

    // --- The selected path ---------------------------------------------------
    ui::sectionText("Selected");
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s", p.name.c_str());
        if (ImGui::InputText("Name", buf, sizeof(buf))) p.name = buf;
        if (ImGui::IsItemActivated())            s.beginEdit();
        if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit("Rename path");
    }
    // Type and preset in one control: every preset names its own kind, so asking
    // for the category first and the structure second would be two questions
    // where the author only has one answer.
    if (ImGui::BeginCombo("Type", splinegen::presetName(p.preset))) {
        const struct { Kind k; const char* label; } groups[] = {
            {Kind::Fence, "Fences"}, {Kind::Wall, "Walls"}, {Kind::Rail, "Track"}};
        for (const auto& g : groups) {
            ui::sectionText(g.label);
            const int picked = presetMenu(g.k, static_cast<int>(p.preset));
            if (picked >= 0) {
                s.beginEdit();
                sp.applyPreset(i, static_cast<Preset>(picked));
                s.endEdit("Change structure");
            }
        }
        ImGui::EndCombo();
    }
    ui::hint("Picking one re-seeds every number below. Your material choices "
             "survive it.");

    if (ImGui::Checkbox("Closed loop", &p.closed)) {
        p.closed = !p.closed;
        s.beginEdit();
        p.closed = !p.closed;
        s.endEdit("Closed loop");
        sp.touch(i);
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Solid", &st.collide)) {
        st.collide = !st.collide;
        s.beginEdit();
        st.collide = !st.collide;
        s.endEdit("Toggle collision");
        sp.touch(i);
    }
    ImGui::SameLine();
    ui::hint("(in Play)");

    ImGui::BeginDisabled(p.points.empty());
    if (ImGui::Button("Clear points")) {
        s.beginEdit();
        p.points.clear();
        p.lifts.clear();
        s.ptSel = -1;
        s.endEdit("Clear points");
        sp.touch(i);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Delete path")) {
        s.beginEdit();
        sp.removePath(i);
        s.sel = -1; s.ptSel = -1;
        s.endEdit("Delete path");
        ImGui::End();
        return;
    }

    if (s.ptSel >= 0 && s.ptSel < static_cast<int>(p.points.size())) {
        float lift = sp.liftOf(i, s.ptSel);
        if (ImGui::SliderFloat("Point height", &lift, -20.0f, 20.0f, "%.2f m"))
            sp.setLift(i, s.ptSel, lift);
        if (ImGui::IsItemActivated())            s.beginEdit();
        if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit("Point height");
        ui::hint("Point #%d of %d. 0 follows the ground.", s.ptSel,
                 static_cast<int>(p.points.size()));
    }

    // --- Materials -----------------------------------------------------------
    // Above the shape controls on purpose: which surface an element wears is the
    // question an author comes back to, and the numbers below it are the one they
    // set once.
    if (ui::header("Materials", ImGuiTreeNodeFlags_DefaultOpen)) {
        materialSlot(s, i, p.kind, 0, st.matA, st.colorA);
        materialSlot(s, i, p.kind, 1, st.matB, st.colorB);
        materialSlot(s, i, p.kind, 2, st.matC, st.colorC);
        slider(s, i, "Texture tile", &st.texTile, 0.1f, 20.0f, "%.2f m");
        ui::hint("Metres per texture repeat, on the swept parts and the posts "
                 "alike.");
        int pal = st.palette;
        if (ImGui::SliderInt("Palette set", &pal, 0, 3)) {
            s.beginEdit();
            st.palette = pal;
            s.endEdit("Palette set");
            sp.touch(i);
        }
        ui::hint("Paths sharing a set share their palette materials -- recolour "
                 "one and every one of them follows. An element pointed at a "
                 "library material ignores this.");
    }

    if (ui::header("Shape", ImGuiTreeNodeFlags_DefaultOpen)) {
        switch (p.kind) {
            case Kind::Wall: wallStyle(s, i, st); break;
            case Kind::Rail: railStyle(s, i, st); break;
            case Kind::Fence:
            case Kind::Count: fenceStyle(s, i, st); break;
        }
        ui::sectionText("Ground");
        slider(s, i, "Sink", &st.sink, 0.0f, 2.0f);
        ui::hint("Metres buried, so no daylight shows under the run on rough "
                 "ground.");
        slider(s, i, "Raise", &st.lift, -10.0f, 10.0f);
    }

    // --- What it came out as -------------------------------------------------
    // Run holds GPU meshes and is move-only, so this is a pointer rather than a
    // reference with a temporary fallback.
    const SplineSystem::Run* run = i < static_cast<int>(sp.runs().size())
                                 ? &sp.runs()[i] : nullptr;
    ImGui::Separator();
    if (run)
        ui::hint("%.0f m, %d pieces, %d verts, %d draws", run->geo.length,
                 run->geo.pieces, run->geo.verts,
                 static_cast<int>(run->geo.batches.size()));
    if (run && run->geo.budgetHit)
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                           "Piece budget reached -- widen the spacing.");

    ImGui::End();
}

} // namespace splineui
