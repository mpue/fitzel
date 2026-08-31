#include "RiverPanel.hpp"

#include <algorithm>
#include <cstdio>

#include <imgui.h>

#include "RiverSystem.hpp"
#include "UiStyle.hpp"

namespace riverui {
namespace {

using rivergen::Kind;
using rivergen::Preset;

constexpr int kPresetCount = static_cast<int>(Preset::Count);

// A slider that brackets itself into one undo step and marks the watercourse for
// re-solving. Every control in this panel goes through it, so "did I remember to
// rebuild, and did I remember not to re-cut the terrain mid-drag" is not a
// question the rest of the file has to keep answering: touch() while the slider
// moves, endEdit() -- and with it the cut -- only when it is let go.
bool slider(const PanelState& s, int path, const char* label, float* v,
            float lo, float hi, const char* fmt = "%.2f m") {
    const bool changed = ImGui::SliderFloat(label, v, lo, hi, fmt);
    if (ImGui::IsItemActivated())            s.beginEdit();
    if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit(label);
    if (changed) s.rivers.touch(path);
    return changed;
}

bool sliderInt(const PanelState& s, int path, const char* label, int* v,
               int lo, int hi) {
    const bool changed = ImGui::SliderInt(label, v, lo, hi);
    if (ImGui::IsItemActivated())            s.beginEdit();
    if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit(label);
    if (changed) s.rivers.touch(path);
    return changed;
}

bool colorEdit(const PanelState& s, int path, const char* label, glm::vec3& c) {
    const bool changed = ImGui::ColorEdit3(label, &c.x);
    if (ImGui::IsItemActivated())            s.beginEdit();
    if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit(label);
    if (changed) s.rivers.touch(path);
    return changed;
}

// A checkbox that brackets the value it HAD, the same dance every checkbox in
// RoadPanel does: ImGui has already flipped it by the time we are asked.
bool checkbox(const PanelState& s, int path, const char* label, bool& v,
              const char* undoLabel) {
    if (!ImGui::Checkbox(label, &v)) return false;
    v = !v;
    s.beginEdit();
    v = !v;
    s.endEdit(undoLabel);
    s.rivers.touch(path);
    return true;
}

// The preset list for one kind, as menu items. Shared by the add buttons and the
// selected course's picker so the two can never offer different sets.
int presetMenu(Kind kind, int currentPreset) {
    int picked = -1;
    for (int p = 0; p < kPresetCount; ++p) {
        const auto pr = static_cast<Preset>(p);
        if (rivergen::presetKind(pr) != kind) continue;
        if (ImGui::Selectable(rivergen::presetName(pr), p == currentPreset))
            picked = p;
    }
    return picked;
}

} // namespace

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    ImGui::SetNextWindowSize(ImVec2(360.0f, 640.0f), ImGuiCond_FirstUseEver);
    // "Rivers", not "Water": the lake's own settings panel already has that
    // name, and two ImGui windows with one title are one window.
    if (!ImGui::Begin("Rivers", &s.show)) { ImGui::End(); return; }

    RiverSystem& rv = s.rivers;

    // --- Edit mode -----------------------------------------------------------
    // First and full width: drawing a course is what this panel is for, and the
    // toggle is the one control that has to be findable without reading.
    {
        const ImVec4 on(0.55f, 0.82f, 1.00f, 1.0f);
        if (s.editMode)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.30f, 0.45f, 1.0f));
        if (ImGui::Button(s.editMode ? "Drawing water (click to stop)"
                                     : "Draw water in viewport",
                          ImVec2(-1.0f, 0.0f))) {
            s.editMode = !s.editMode;
            if (s.editMode && s.grabLMB) s.grabLMB();
        }
        if (s.editMode) ImGui::PopStyleColor();
        if (s.editMode) {
            ImGui::TextColored(on, "Click the ground to add a point");
            ui::hint("Drag a handle to move it, Ctrl+drag to raise or lower the "
                     "water there, Delete to remove it. Arrow keys nudge. The bed "
                     "is cut when you let go.");
        }
    }

    // --- Adding --------------------------------------------------------------
    ui::sectionText("Add");
    {
        const struct { Kind k; const char* label; } kinds[] = {
            {Kind::Brook, "Brook"}, {Kind::River, "River"}, {Kind::Canal, "Canal"}};
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
                    s.sel   = rv.addPath(static_cast<Preset>(picked));
                    s.ptSel = -1;
                    s.endEdit("Add watercourse");
                    if (!s.editMode) { s.editMode = true; if (s.grabLMB) s.grabLMB(); }
                }
                ImGui::EndPopup();
            }
        }
    }

    if (rv.paths.empty()) {
        ui::hint("No water yet. Pick a channel above, then click the ground from "
                 "the source downhill -- the descent, the rapids and the falls "
                 "are worked out from the terrain you drew across.");
        ImGui::End();
        return;
    }

    // --- The list ------------------------------------------------------------
    ui::sectionText("Watercourses");
    if (ImGui::BeginChild("courses", ImVec2(0.0f, 108.0f), true)) {
        for (int i = 0; i < static_cast<int>(rv.paths.size()); ++i) {
            RiverSystem::Path& p = rv.paths[i];
            ImGui::PushID(i);
            if (ImGui::Checkbox("##on", &p.enabled)) {
                p.enabled = !p.enabled;
                s.beginEdit();
                p.enabled = !p.enabled;
                s.endEdit("Toggle watercourse");
                rv.touch(i);
            }
            ImGui::SameLine();
            char row[160];
            std::snprintf(row, sizeof(row), "%s  (%s, %d pts)", p.name.c_str(),
                          rivergen::presetName(p.preset),
                          static_cast<int>(p.points.size()));
            if (ImGui::Selectable(row, s.sel == i)) { s.sel = i; s.ptSel = -1; }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if (s.sel < 0 || s.sel >= static_cast<int>(rv.paths.size())) {
        ui::hint("Select a watercourse to edit it.");
        ImGui::End();
        return;
    }

    const int i = s.sel;
    RiverSystem::Path& p  = rv.paths[i];
    rivergen::Style&   st = p.style;

    // --- The selected course -------------------------------------------------
    ui::sectionText("Selected");
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s", p.name.c_str());
        if (ImGui::InputText("Name", buf, sizeof(buf))) p.name = buf;
        if (ImGui::IsItemActivated())            s.beginEdit();
        if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit("Rename watercourse");
    }
    if (ImGui::BeginCombo("Type", rivergen::presetName(p.preset))) {
        const struct { Kind k; const char* label; } groups[] = {
            {Kind::Brook, "Brooks"}, {Kind::River, "Rivers"}, {Kind::Canal, "Cut channels"}};
        for (const auto& g : groups) {
            ui::sectionText(g.label);
            const int picked = presetMenu(g.k, static_cast<int>(p.preset));
            if (picked >= 0) {
                s.beginEdit();
                rv.applyPreset(i, static_cast<Preset>(picked));
                s.endEdit("Change channel");
            }
        }
        ImGui::EndCombo();
    }
    ui::hint("Picking one re-seeds every number below.");

    ImGui::BeginDisabled(p.points.empty());
    if (ImGui::Button("Clear points")) {
        s.beginEdit();
        p.points.clear();
        p.bias.clear();
        s.ptSel = -1;
        s.endEdit("Clear points");
        rv.touch(i);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Delete watercourse")) {
        s.beginEdit();
        rv.removePath(i);
        s.sel = -1; s.ptSel = -1;
        s.endEdit("Delete watercourse");
        ImGui::End();
        return;
    }

    if (s.ptSel >= 0 && s.ptSel < static_cast<int>(p.points.size())) {
        float bias = rv.biasOf(i, s.ptSel);
        if (ImGui::SliderFloat("Water here", &bias, -10.0f, 10.0f, "%+.2f m"))
            rv.setBias(i, s.ptSel, bias);
        if (ImGui::IsItemActivated())            s.beginEdit();
        if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit("Water level at point");
        ui::hint("Point #%d of %d. Above 0 dams a pool, below 0 digs the channel "
                 "in. The descent still wins -- this cannot make the water climb.",
                 s.ptSel, static_cast<int>(p.points.size()));
    }

    // --- Channel -------------------------------------------------------------
    if (ui::header("Channel", ImGuiTreeNodeFlags_DefaultOpen)) {
        slider(s, i, "Width", &st.width, 0.4f, 120.0f);
        slider(s, i, "Widens to", &st.widen, 0.5f, 4.0f, "%.2fx at the mouth");
        ui::hint("A river gathers water on the way down and gets wider for it. "
                 "1 keeps one section the whole way.");
        slider(s, i, "Depth", &st.depth, 0.05f, 12.0f);
        slider(s, i, "Flat bed", &st.bedFlat, 0.0f, 0.95f, "%.2f of half-width");
        ui::hint("How much of the section is floor before it starts climbing to "
                 "the waterline. Low is a bowl, high is a trough.");

        ui::sectionText("Banks");
        slider(s, i, "Bank width", &st.bankWidth, 0.2f, 60.0f);
        ui::hint("How far past the water the cut eases back to the natural "
                 "ground. Most of the carve's cost is here.");
        slider(s, i, "Bank rise", &st.bankRise, 0.0f, 4.0f);
        ui::hint("Metres the lip stands above the water. In a valley the ground "
                 "is already higher and this does nothing.");

        // The gravel. Above the numbers that shape the bank, because it is the
        // one that decides whether the channel reads as a watercourse at all: a
        // cut alone leaves meadow grass running to the waterline, and grass to
        // the waterline is what stops a stream looking like a stream.
        ui::sectionText("Bed material");
        const char* layers[] = {"None", "Layer 1", "Layer 2", "Layer 3", "Layer 4"};
        int li = glm::clamp(st.bankLayer + 1, 0, 4);
        if (ImGui::BeginCombo("Paint", layers[li])) {
            for (int k = 0; k < 5; ++k)
                if (ImGui::Selectable(layers[k], k == li)) {
                    s.beginEdit();
                    st.bankLayer = k - 1;
                    s.endEdit("Bed material");
                    rv.touch(i);
                }
            ImGui::EndCombo();
        }
        ui::hint("Which terrain paint layer the channel lays over its bed and "
                 "margins -- the gravel, sand or mud. Set the layers themselves "
                 "in the Terrain panel.");
        if (st.bankLayer >= 0) {
            slider(s, i, "Paint margin", &st.bankPaint, 0.0f, 30.0f);
            slider(s, i, "Paint strength", &st.bankBlend, 0.0f, 1.0f, "%.2f");
        }
    }

    // --- Meander -------------------------------------------------------------
    // Its own section and above Flow, because it is the one thing that decides
    // whether the result reads as a river or as a road with water in it.
    if (ui::header("Meander", ImGuiTreeNodeFlags_DefaultOpen)) {
        ui::hint("The line you drew is the valley. The water wanders inside it "
                 "-- and where it wanders it also deepens on the outside of each "
                 "bend and spreads out shallow at the crossings between. All "
                 "three come off one wave, so they cannot fall out of step.");
        slider(s, i, "Wander", &st.meander, 0.0f, 120.0f, "%.1f m (0 = straight)");
        if (st.meander > 0.0f)
            slider(s, i, "Wavelength", &st.meanderLength, 8.0f, 800.0f, "%.0f m");
        slider(s, i, "Ease bends", &st.bendEase, 0.0f, 3.0f, "%.2f x width");
        ui::hint("A drawn line is a polygon and water has no corners. Measured "
                 "in channel widths, so a brook still follows every wiggle you "
                 "drew and a wide river rounds the same drawing into something "
                 "water could take. 0 follows the line exactly -- which is what "
                 "a dug channel wants.");
        ui::hint("Faded out where the ground steepens: water with a gradient "
                 "under it goes straight down the fall line. The gradient it "
                 "gives up by is the same one that says where the surface breaks "
                 "(Rapids and falls > Breaks at).");

        ui::sectionText("Pools and riffles");
        slider(s, i, "Width swing", &st.widthVary, 0.0f, 0.9f, "%.2f");
        slider(s, i, "Depth swing", &st.depthVary, 0.0f, 0.9f, "%.2f");
        ui::hint("A pool at each bend, a riffle at each crossing -- narrow and "
                 "deep, then wide and shallow. The two swing opposite ways "
                 "because it is the same water.");
        slider(s, i, "Bend scour", &st.bendScour, 0.0f, 0.9f, "%.2f of half-width");
        ui::hint("How far the deep line moves off the middle towards the outer "
                 "bank. The inside becomes a point bar, which is where the "
                 "gravel and the reeds then land on their own.");

        const rivergen::Course& mc = rv.course(i);
        if (st.meander > 0.0f && !mc.empty()) {
            // How much of the wander actually survived the terrain. Worth saying:
            // a straight-looking river with the slider up is not a broken slider,
            // it is a river on a slope, and there is no other way to tell.
            float amp = 0.0f;
            for (std::size_t k = 0; k + 1 < mc.line.size(); ++k) {
                const glm::vec2 a(mc.line[k].x, mc.line[k].z);
                const glm::vec2 b(mc.line[k + 1].x, mc.line[k + 1].z);
                amp = std::max(amp, glm::length(b - a));
            }
            ui::hint("Sinuosity %.2f (1.00 = dead straight).",
                     mc.length > 1.0f
                         ? mc.length / std::max(glm::distance(
                               glm::vec2(mc.line.front().x, mc.line.front().z),
                               glm::vec2(mc.line.back().x,  mc.line.back().z)), 1.0f)
                         : 1.0f);
        }
    }

    // --- Flow ----------------------------------------------------------------
    if (ui::header("Flow", ImGuiTreeNodeFlags_DefaultOpen)) {
        checkbox(s, i, "Find the downhill end", st.autoFlow, "Flow direction");
        ui::hint("On, the terrain decides which end is the source.");
        ImGui::SameLine();
        checkbox(s, i, "Flip", st.flip, "Flip flow");

        slider(s, i, "Minimum fall", &st.minSlope, 0.0f, 0.08f, "%.4f m per m");
        ui::hint("The floor on how flat the water may run. 0 lets a stretch "
                 "stand level; anything above it guarantees the whole course "
                 "falls from source to mouth.");
        slider(s, i, "Deepest cut", &st.maxCut, 0.5f, 40.0f);
        ui::hint("A safety rail, not a look: it stops a line drawn up a mountain "
                 "from cutting a canyon. Where it bites, the water has to climb "
                 "-- and it says so below.");
        sliderInt(s, i, "Smoothing", &st.smooth, 0, 40);
        slider(s, i, "Current", &st.current, 0.0f, 12.0f, "%.2f m/s");
        ui::hint("What the water pushes anything floating in it with, in Play.");
    }

    // --- Whitewater ----------------------------------------------------------
    if (ui::header("Rapids and falls")) {
        slider(s, i, "Breaks at", &st.rapidSlope, 0.0f, 0.5f, "%.3f m per m");
        slider(s, i, "Falls at", &st.fallSlope, 0.02f, 1.5f, "%.3f m per m");
        ui::hint("The two gradients: where the surface starts to break, and "
                 "where it stops being a slope and becomes a step.");
        slider(s, i, "Shortest fall", &st.fallMin, 0.1f, 12.0f);
        ui::hint("A shorter drop stays a rapid however steep -- otherwise every "
                 "bump in the ground becomes a waterfall.");
        slider(s, i, "Pool length", &st.poolLength, 0.0f, 60.0f);
        ui::hint("Level water held above a lip and below the drop. This is most "
                 "of what makes a fall read as an edge rather than a slope.");
        slider(s, i, "Plunge pool", &st.plunge, 0.0f, 6.0f);
    }

    // --- Stones and reeds ----------------------------------------------------
    if (ui::header("Stones and reeds")) {
        ui::hint("Derived instances along the line, like the roadside rails -- "
                 "never entities, so a kilometre of bank costs no hierarchy rows "
                 "and no undo snapshots.");
        ui::sectionText("Stones");
        slider(s, i, "Stones", &st.stones, 0.0f, 200.0f, "%.0f per 100 m");
        if (st.stones > 0.0f) {
            slider(s, i, "Stone size", &st.stoneSize, 0.03f, 3.0f);
            slider(s, i, "Scatter past bank", &st.stoneSpread, 0.0f, 12.0f);
            colorEdit(s, i, "Stone colour", st.stoneColor);
        }
        ui::sectionText("Reeds");
        slider(s, i, "Clumps", &st.reeds, 0.0f, 120.0f, "%.0f per 100 m");
        if (st.reeds > 0.0f) {
            slider(s, i, "Reed height", &st.reedHeight, 0.1f, 4.0f);
            slider(s, i, "Only shallower than", &st.reedDepth, 0.05f, 3.0f);
            ui::hint("Reeds stand in the margins. Deeper than this, or in broken "
                     "water, nothing grows.");
            colorEdit(s, i, "Reed colour", st.reedColor);
        }
    }

    // --- Look ----------------------------------------------------------------
    if (ui::header("Look")) {
        colorEdit(s, i, "Shallow", st.shallow);
        colorEdit(s, i, "Deep", st.deep);
        slider(s, i, "Clarity", &st.clarity, 0.05f, 6.0f, "%.2f");
        ui::hint("How far you see into it before the deep colour takes over.");
        slider(s, i, "Reflection", &st.reflect, 0.0f, 1.0f, "%.2f");
        slider(s, i, "Surface speed", &st.flowSpeed, 0.0f, 6.0f, "%.2f m/s");
        ui::hint("How fast the ripple pattern travels. Independent of the "
                 "current above, so still-looking water can still carry a boat.");
        slider(s, i, "Ripple size", &st.rippleScale, 0.05f, 4.0f, "%.2f per m");
        slider(s, i, "Ripple depth", &st.ripple, 0.0f, 0.3f, "%.3f");
        slider(s, i, "Bank foam", &st.foamWidth, 0.0f, 4.0f);
        slider(s, i, "Sparkle", &st.sparkle, 0.0f, 3.0f, "%.2f");
    }

    // --- What it came out as -------------------------------------------------
    // The report is the whole feedback loop for a tool that decides the heights
    // itself: the author drew a line, and this is what the ground made of it.
    const rivergen::Course& c = rv.course(i);
    ImGui::Separator();
    if (c.empty()) {
        ui::hint("Fewer than two points -- nothing to solve yet.");
    } else {
        float wLo = 1e9f, wHi = 0.0f, dLo = 1e9f, dHi = 0.0f;
        for (std::size_t k = 0; k < c.half.size(); ++k) {
            wLo = std::min(wLo, c.half[k] * 2.0f);
            wHi = std::max(wHi, c.half[k] * 2.0f);
        }
        for (float dv : c.deep) { dLo = std::min(dLo, dv); dHi = std::max(dHi, dv); }
        ui::hint("%.0f m long, %.1f m of fall (%.2f%%), %d fall%s, cut %.1f m "
                 "deep at most, %d verts", c.length, c.drop,
                 c.length > 1.0f ? 100.0f * c.drop / c.length : 0.0f,
                 c.falls, c.falls == 1 ? "" : "s", c.maxCut,
                 i < static_cast<int>(rv.runs().size()) ? rv.runs()[i].verts : 0);
        if (wHi > 0.0f)
            ui::hint("%.1f--%.1f m wide, %.2f--%.2f m deep.", wLo, wHi, dLo, dHi);
        if (c.reversed)
            ui::hint("Flowing from the LAST point you placed to the first -- that "
                     "end is downhill.");
        if (c.uphill)
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.30f, 1.0f),
                               "The line climbs: the water has to run uphill "
                               "somewhere. Move it into a valley, or raise "
                               "'Deepest cut'.");
    }

    ImGui::End();
}

} // namespace riverui
