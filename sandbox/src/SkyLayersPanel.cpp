#include "SkyLayers.hpp"

#include <imgui.h>

#include "UiStyle.hpp"

// The Sky panel's layer sections. Split from SkyLayers.cpp because that file is
// RUNTIME -- the shipped player orders and uploads the same layers -- and this
// one drags ImGui behind it. It is also what lets skycheck link the ordering
// code without linking an editor.
namespace skylayers {
namespace {

// The panel's own table: which slot, what it is called, and what the type IS in
// a sentence. That last one is not decoration -- the difference between a
// stratocumulus and an altocumulus is exactly what an author is guessing at
// while dragging a slider, and a layer nobody can tell from the layer above it
// is a layer nobody will use.
struct Row {
    weather::Sheet weather::Sky::*field;
    Kind        kind;
    const char* label;
    const char* hint;
    // The band the type actually lives in, in metres. Slider bounds, not
    // limits: a fantasy sky can put a mackerel deck at four hundred metres, but
    // the range the slider opens on should be where the real one is.
    float minH, maxH;
};

constexpr Row kRows[] = {
    {&weather::Sky::stratus, kStratus, "Stratus",
     "The low grey lid. No shape to speak of -- what varies across\n"
     "one is its THICKNESS, and at full coverage it simply closes\n"
     "over. This is the layer that makes an overcast day, and the\n"
     "one whose shaded underside you are looking at.",
     100.0f, 2000.0f},
    {&weather::Sky::stratocumulus, kStratocumulus, "Stratocumulus",
     "Rolls. Lumpy like a cumulus but spread flat and lined up in\n"
     "bands -- so this layer's Direction is what the bands run\n"
     "along, and turning it turns the whole deck.",
     400.0f, 3000.0f},
    {&weather::Sky::altocumulus, kAltocumulus, "Altocumulus",
     "A mackerel sky: separate rounded elements with real sky\n"
     "between them, in patches rather than over the whole dome.\n"
     "Coverage sets how big the elements are as well as how many.",
     2000.0f, 7000.0f},
    {&weather::Sky::cirrus, kCirrus, "Cirrus",
     "Ice combed into fibres by a wind that has nothing to do with\n"
     "the one below -- give it its own speed. It is the last thing\n"
     "up there still in daylight, so it takes the sunset first.",
     3000.0f, 12000.0f},
    {&weather::Sky::cirrocumulus, kCirrocumulus, "Cirrocumulus",
     "The same ice, but grained: a field of very fine elements.\n"
     "The grain is what tells it from a flat cirrostratus, and it\n"
     "gets finer as you raise the layer, as the real one does.",
     4000.0f, 12000.0f},
    {&weather::Sky::contrails, kContrails, "Condensation trails",
     "Straightness is the whole read: nothing else in a sky is a\n"
     "perfect line. They come in one at a time as Traffic rises,\n"
     "each older than the last. Direction points the airway --\n"
     "lanes run roughly parallel, never in a fan.",
     4000.0f, 13000.0f},
};

} // namespace

void drawPanel(weather::Sky& sky) {
    for (const Row& r : kRows) {
        weather::Sheet& sh = sky.*(r.field);

        // The checkbox sits on the header's own line rather than inside it, so
        // a layer can be switched on and off without opening it -- which is how
        // this panel is actually used: you are comparing skies, not editing one
        // slider. ImGui needs the id to differ from the header's, hence "##on".
        ImGui::PushID(r.label);
        ImGui::Checkbox("##on", &sh.on);
        ImGui::SameLine();
        // Dimmed while off, so the stack reads at a glance as which layers are
        // in this sky and which are merely available.
        if (!sh.on)
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                ImGui::GetStyle().Alpha * 0.5f);
        const bool open = ui::header(r.label);
        if (!sh.on) ImGui::PopStyleVar();

        if (open) {
            ImGui::Indent();
            // Contrails are COUNTED, not covered: the slider adds aircraft.
            // Calling it Coverage would promise a fraction of the sky it does
            // not deliver -- four lines never cover anything. Its Scale means
            // something else too, so it gets its own name: how far the traffic
            // has spread, which is the one thing that slider needs to say.
            if (r.kind == kContrails) {
                ImGui::SliderFloat("Traffic", &sh.amount, 0.0f, 1.0f);
                ImGui::SliderFloat("Spread",  &sh.scale,  0.0f, 1.5f);
            } else {
                ImGui::SliderFloat("Coverage", &sh.amount, 0.0f, 1.0f);
                ImGui::SliderFloat("Scale",    &sh.scale,  0.2f, 4.0f);
            }
            ImGui::SliderFloat("Height", &sh.height, r.minH, r.maxH, "%.0f m");
            ImGui::SliderFloat("Wind",   &sh.wind,   0.0f, 20.0f);
            // A heading in degrees, because that is the unit a direction is
            // actually thought in. Stored in radians so the shader gets a
            // cos/sin without converting per frame.
            float deg = sh.dir * 57.2957795f;
            if (ImGui::SliderFloat("Direction", &deg, -180.0f, 180.0f, "%.0f deg"))
                sh.dir = deg * 0.0174532925f;
            ui::hint("%s", r.hint);
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
}

} // namespace skylayers
