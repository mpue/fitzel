#include <imgui.h>

#include "SplinePanelParts.hpp"
#include "SplineSystem.hpp"
#include "UiStyle.hpp"

// The bridge designer: the Shape section of the Splines panel for a bridge.
// Grouped by layer -- deck, edges, supports, arch, truss, cables -- because that
// is how the generator builds it and how the presets differ: a suspension bridge
// is the beam bridge with its piers off and the cable layer on.
namespace splineui {
namespace {

// An enum as a combo, bracketed into one undo step like the sliders.
template <typename E>
void choice(const PanelState& s, int path, const char* label, E& v,
            const char* const* names, int count) {
    int cur = static_cast<int>(v);
    if (!ImGui::BeginCombo(label, names[cur])) return;
    for (int k = 0; k < count; ++k) {
        if (ImGui::Selectable(names[k], k == cur) && k != cur) {
            s.beginEdit();
            v = static_cast<E>(k);
            s.endEdit(label);
            s.splines.touch(path);
        }
    }
    ImGui::EndCombo();
}

void checkbox(const PanelState& s, int path, const char* label, bool& v) {
    bool on = v;
    if (!ImGui::Checkbox(label, &on)) return;
    s.beginEdit();
    v = on;
    s.endEdit(label);
    s.splines.touch(path);
}

} // namespace

void bridgeStyle(const PanelState& s, int i, splinegen::Style& st) {
    bridgegen::Style& b = st.bridge;
    ImGui::PushID("bridge");   // "Height", "Spacing"... recur across sections

    ui::sectionText("Deck");
    slider(s, i, "Width", &b.width, 1.0f, 40.0f);
    slider(s, i, "Slab", &b.thick, 0.1f, 3.0f);
    slider(s, i, "Girder depth", &b.girder, 0.0f, 6.0f, "%.2f m (0 = none)");
    slider(s, i, "Camber", &b.camber, -5.0f, 15.0f, "%+.2f m");
    ui::hint("How far the middle rises above the straight line between the ends "
             "-- the hump of an old stone bridge.");

    ui::sectionText("Edges");
    {
        static const char* const names[] = {"None", "Parapet", "Railing"};
        choice(s, i, "Edge", b.rail, names, static_cast<int>(bridgegen::Rail::Count));
    }
    if (b.rail != bridgegen::Rail::None) {
        slider(s, i, "Edge height", &b.railHeight, 0.3f, 3.0f);
        slider(s, i, "Edge thickness", &b.railThick, 0.03f, 1.0f, "%.3f m");
        if (b.rail == bridgegen::Rail::Railing)
            slider(s, i, "Post spacing", &b.postEvery, 0.5f, 6.0f);
    }

    ui::sectionText("Supports");
    slider(s, i, "Pier spacing", &b.pierEvery, 0.0f, 120.0f, "%.1f m (0 = none)");
    ui::hint("Evened out over the length, so every span is the same.");
    if (b.pierEvery > 0.0f) {
        sliderInt(s, i, "Columns", &b.pierCols, 1, 4);
        slider(s, i, "Pier width", &b.pierWidth, 0.2f, 8.0f);
        if (b.pierCols <= 1) slider(s, i, "Pier depth", &b.pierDepth, 0.5f, 30.0f);
    }
    slider(s, i, "Abutments", &b.abutment, 0.0f, 20.0f, "%.1f m (0 = none)");

    ui::sectionText("Arch");
    {
        static const char* const names[] = {"None", "Under the deck", "Over the deck",
                                            "Stone arches between piers"};
        choice(s, i, "Arch", b.arch, names, static_cast<int>(bridgegen::Arch::Count));
    }
    if (b.arch != bridgegen::Arch::None) {
        const char* riseLabel = b.arch == bridgegen::Arch::Below ? "Arch depth"
                              : b.arch == bridgegen::Arch::Above ? "Arch height"
                                                                 : "Arch rise";
        slider(s, i, riseLabel, &b.archRise, 1.0f, 80.0f);
        slider(s, i, b.arch == bridgegen::Arch::Masonry ? "Ring thickness" : "Rib size",
               &b.archRib, 0.2f, 5.0f);
        if (b.arch == bridgegen::Arch::Masonry) {
            if (b.pierEvery <= 0.0f)
                ui::hint("No piers: one arch over the whole length. Set a pier "
                         "spacing for a row of them.");
        } else {
            slider(s, i, "Springing inset", &b.archInset, 0.0f, 60.0f);
            slider(s, i, b.arch == bridgegen::Arch::Below ? "Column spacing"
                                                          : "Hanger spacing",
                   &b.hangerEvery, 1.0f, 20.0f);
            slider(s, i, "Hanger thickness", &b.hangerThick, 0.03f, 2.0f, "%.3f m");
        }
    }

    ui::sectionText("Truss");
    slider(s, i, "Truss height", &b.trussHeight, 0.0f, 20.0f, "%.1f m (0 = none)");
    if (b.trussHeight > 0.0f) {
        slider(s, i, "Panel length", &b.trussPanel, 2.0f, 15.0f);
        slider(s, i, "Member size", &b.trussBar, 0.1f, 1.5f, "%.3f m");
        checkbox(s, i, "Top bracing", b.trussTop);
    }

    ui::sectionText("Cables");
    {
        static const char* const names[] = {"None", "Suspension", "Cable-stayed"};
        choice(s, i, "Cables", b.cable, names, static_cast<int>(bridgegen::Cable::Count));
    }
    if (b.cable != bridgegen::Cable::None) {
        slider(s, i, "Tower height", &b.towerHeight, 5.0f, 150.0f);
        slider(s, i, "Tower position", &b.towerAt, 0.05f, 0.5f, "%.2f of length");
        ui::hint("From each end. 0.50 is a single tower in the middle.");
        slider(s, i, "Tower width", &b.towerWidth, 0.5f, 8.0f);
        slider(s, i, "Cable thickness", &b.cableThick, 0.05f, 1.5f, "%.3f m");
        if (b.cable == bridgegen::Cable::Suspension) {
            slider(s, i, "Cable sag", &b.cableSag, 0.5f, 20.0f);
            ui::hint("Lowest point of the main cable above the deck.");
            slider(s, i, "Hanger spacing", &b.hangerEvery, 1.0f, 20.0f);
            slider(s, i, "Hanger thickness", &b.hangerThick, 0.03f, 1.0f, "%.3f m");
        } else {
            slider(s, i, "Stay spacing", &b.stayEvery, 2.0f, 30.0f);
        }
    }
    ImGui::PopID();
}

} // namespace splineui
