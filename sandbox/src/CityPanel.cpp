#include "CityPanel.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

#include <imgui.h>

#include "CityGen.hpp"
#include "RoadSystem.hpp"
#include "UiStyle.hpp"

namespace cityui {
namespace {

const char* kStyleNames[city::kStyleCount] = {
    "Skyscraper", "Neo-Tokyo", "Arcology", "Needle", "Slab", "Habitat",
};

// One biome's editor. Returns true when something changed that the district has
// to be re-derived for. Every widget is a DragFloat rather than a slider: click
// to type an exact number, drag to sweep -- a slider demands a precise grab, and
// this editor is built for hands that can't reliably give one.
bool biomeEditor(const PanelState& s, city::Biome& B, int index) {
    bool changed = false;
    ImGui::PushID(index);

    auto drag = [&](const char* label, float* v, float speed, float lo, float hi,
                    const char* fmt, const char* undoLabel) {
        if (ImGui::DragFloat(label, v, speed, lo, hi, fmt)) changed = true;
        if (ImGui::IsItemActivated())            s.beginEdit();
        if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit(undoLabel);
    };
    auto dragInt = [&](const char* label, int* v, float speed, int lo, int hi,
                       const char* undoLabel) {
        if (ImGui::DragInt(label, v, speed, lo, hi)) changed = true;
        if (ImGui::IsItemActivated())            s.beginEdit();
        if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit(undoLabel);
    };
    // A checkbox reports its value already flipped, so snapshot by flipping back
    // over the bracket -- the same trick roadui uses.
    auto check = [&](const char* label, bool* v, const char* undoLabel) {
        if (ImGui::Checkbox(label, v)) {
            *v = !*v; s.beginEdit(); *v = !*v; s.endEdit(undoLabel);
            changed = true;
        }
    };
    auto color = [&](const char* label, glm::vec3* v, const char* undoLabel) {
        if (ImGui::ColorEdit3(label, &v->x, ImGuiColorEditFlags_NoInputs)) changed = true;
        if (ImGui::IsItemActivated())            s.beginEdit();
        if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit(undoLabel);
    };

    ui::sectionText("Extent");
    drag("From", &B.from, 1.0f, 0.0f, 100000.0f, "%.0f m", "Biome extent");
    drag("To",   &B.to,   1.0f, 0.0f, 100000.0f, "%.0f m", "Biome extent");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Metres along the road from its first control point.\n"
                          "Set To <= From for \"all the way to the end\".");
    drag("Blend", &B.blend, 0.5f, 0.0f, 400.0f, "%.0f m", "Biome blend");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Metres at each end over which the district thins out\n"
                          "and loses height, so it ends ragged instead of\n"
                          "stopping in a wall.");
    {
        const int old = B.side;
        if (ImGui::Combo("Side", &B.side, "Left\0Right\0Both\0")) {
            const int nw = B.side;
            B.side = old; s.beginEdit(); B.side = nw; s.endEdit("Biome side");
            changed = true;
        }
    }

    ui::sectionText("Parcels");
    drag("Setback", &B.setback, 0.05f, 0.0f, 120.0f, "%.1f m", "Biome setback");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Distance from the road EDGE to the facade.\n"
                          "Small setback + tall towers = a canyon.");
    drag("Setback spread", &B.setbackVar, 0.05f, 0.0f, 40.0f, "%.1f m", "Biome setback");
    drag("Frontage", &B.frontage, 0.1f, 4.0f, 200.0f, "%.1f m", "Biome frontage");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Average plot width ALONG the road -- the building's\n"
                          "street face.");
    drag("Frontage spread", &B.frontageVar, 0.1f, 0.0f, 80.0f, "%.1f m", "Biome frontage");
    drag("Gap", &B.gap, 0.05f, 0.0f, 120.0f, "%.1f m", "Biome gap");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Daylight between neighbours. 0 = a party wall, which is\n"
                          "what makes a continuous canyon face.");
    drag("Depth", &B.depth, 0.1f, 5.0f, 200.0f, "%.1f m", "Biome depth");
    drag("Depth spread", &B.depthVar, 0.1f, 0.0f, 80.0f, "%.1f m", "Biome depth");
    drag("Fill", &B.fill, 0.005f, 0.0f, 1.0f, "%.2f", "Biome fill");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Chance a plot is actually built. Below 1 it leaves\n"
                          "vacant lots in the frontage.");
    drag("Block length", &B.blockLength, 1.0f, 0.0f, 2000.0f, "%.0f m", "Biome blocks");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Metres between cross streets (0 = one endless block).");
    drag("Cross street", &B.blockGap, 0.2f, 1.0f, 120.0f, "%.0f m", "Biome blocks");

    ui::sectionText("Height");
    dragInt("Floors min", &B.floorsMin, 0.2f, 1, 250, "Biome floors");
    dragInt("Floors max", &B.floorsMax, 0.2f, 1, 250, "Biome floors");
    drag("Floor height", &B.floorHeight, 0.01f, 2.0f, 12.0f, "%.2f m", "Biome floors");
    drag("Skyline scale", &B.skylineScale, 1.0f, 10.0f, 2000.0f, "%.0f m", "Biome skyline");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Wavelength of the tall/short swell along the road.\n"
                          "Short = jagged, long = broad peaks and troughs.");
    drag("Skyline bias", &B.skylineBias, 0.005f, 0.0f, 1.0f, "%.2f", "Biome skyline");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("0 = mostly at Floors min, 1 = mostly at Floors max.");
    drag("Twist", &B.twist, 0.5f, 0.0f, 360.0f, "%.0f deg", "Biome twist");

    ui::sectionText("Style mix");
    ui::hint("Relative weights. All zero falls back to Skyscraper.");
    for (int i = 0; i < city::kStyleCount; ++i) {
        ImGui::PushID(i);
        if (ImGui::DragFloat(kStyleNames[i], &B.styleMix[i], 0.02f, 0.0f, 10.0f, "%.2f"))
            changed = true;
        if (ImGui::IsItemActivated())            s.beginEdit();
        if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit("Biome style mix");
        ImGui::PopID();
    }

    ui::sectionText("Street");
    drag("Skyway every", &B.skywayEvery, 1.0f, 0.0f, 2000.0f, "%.0f m", "Biome skyways");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Overhead decks spanning the street, facade to facade.\n"
                          "0 = none. Needs Side = Both -- a skyway wants two sides\n"
                          "to land on.");
    if (B.skywayEvery >= 5.0f) {
        drag("Skyway height", &B.skywayHeight, 0.2f, 5.0f, 300.0f, "%.0f m", "Biome skyways");
        drag("Skyway width",  &B.skywayWidth,  0.1f, 1.5f, 40.0f,  "%.1f m", "Biome skyways");
        if (B.side != city::Side::Both)
            ui::hint("Side is not Both -- no skyways will be built.");
    }
    drag("Signs", &B.signChance, 0.005f, 0.0f, 1.0f, "%.2f", "Biome signs");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Chance a building carries a projecting neon sign over\n"
                          "the street. Needs Neon on.");

    ui::sectionText("Wear");
    drag("Weathering", &B.weathering, 0.005f, 0.0f, 1.0f, "%.2f", "Biome wear");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("0 = new curtain wall, 1 = derelict concrete.\n"
                          "Kills the mirror finish (glass drops from a chromed\n"
                          "0.70 reflectivity to almost nothing), soils the\n"
                          "colours and roughens the massing. District-wide --\n"
                          "the palette is shared.");
    drag("Bare concrete", &B.grime, 0.005f, 0.0f, 1.0f, "%.2f", "Biome wear");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Chance a tower section is raw structural concrete\n"
                          "instead of glazing. Varies building to building at no\n"
                          "extra material cost -- a glass tower between two\n"
                          "concrete hulks is what neglect looks like.");
    dragInt("Roof clutter", &B.clutter, 0.1f, 0, 60, "Biome wear");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Tanks, vents and ducts dropped on the setback roofs.\n"
                          "Junk on the silhouette -- the cheapest thing that\n"
                          "makes an extrusion look unmaintained.");
    drag("Clutter spread", &B.clutterVar, 0.005f, 0.0f, 1.0f, "%.2f", "Biome wear");
    drag("Dead signage", &B.deadNeon, 0.005f, 0.0f, 1.0f, "%.2f", "Biome wear");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Fraction of buildings whose neon is unlit. The dark\n"
                          "ones are what make the lit ones read as lit.");

    ui::sectionText("Look");
    dragInt("Palette slot", &B.palette, 0.05f, 0, 7, "Biome palette");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Buildings on the same slot share four materials\n"
                          "(\"Building A Glass\" and friends), so a whole city\n"
                          "costs four materials per slot -- not four per tower.\n"
                          "Two biomes on one slot are recoloured together.");
    color("Glass",  &B.glassTint,   "Biome colour");
    color("Frame",  &B.frameTint,   "Biome colour");
    color("Base",   &B.baseTint,    "Biome colour");
    color("Accent", &B.accentColor, "Biome colour");
    drag("Accent glow", &B.accentStrength, 0.02f, 0.0f, 12.0f, "%.2f", "Biome colour");
    drag("Lit windows", &B.windowLit, 0.005f, 0.0f, 1.0f, "%.2f", "Biome colour");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Fraction of the glazing that is lit from inside.\n"
                          "Costs no geometry and no texture -- the shader\n"
                          "hashes the windows out of the world position --\n"
                          "and it decides whether a district reads as\n"
                          "working, half-empty or evacuated.");
    color("Window light", &B.windowColor, "Biome colour");
    check("Neon", &B.neon, "Biome neon");
    check("Solid in Play", &B.collider, "Biome collider");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Give the masses a static collider so a vehicle crashes\n"
                          "into the facade instead of driving through it.");

    ui::sectionText("Ground");
    drag("Max slope", &B.maxSlope, 0.002f, 0.01f, 2.0f, "%.2f", "Biome ground");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Plots on ground steeper than this (metres of fall per\n"
                          "metre of run across the footprint) are left empty\n"
                          "rather than built floating or buried.");
    drag("Max drop", &B.maxDrop, 0.1f, 0.0f, 80.0f, "%.1f m", "Biome ground");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How far below the carriageway a plot may sit before it\n"
                          "is propped back up to it -- keeps ground floors\n"
                          "addressing the street across a dip.");

    {
        int seed = static_cast<int>(B.seed);
        if (ImGui::DragInt("Seed", &seed, 1.0f, 0, 1000000)) {
            B.seed = static_cast<unsigned>(std::max(seed, 0));
            changed = true;
        }
        if (ImGui::IsItemActivated())            s.beginEdit();
        if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit("Biome seed");
        ImGui::SameLine();
        if (ImGui::Button("Reroll")) {
            s.beginEdit();
            B.seed = B.seed * 1664525u + 1013904223u;
            s.endEdit("Reroll biome");
            changed = true;
        }
    }

    ImGui::PopID();
    return changed;
}

} // namespace

void drawPanel(const PanelState& s) {
    if (!ImGui::Begin("City", &s.show)) { ImGui::End(); return; }

    ui::hint("Buildings along the road, generated from the biome rules below.\n"
             "The rules are saved -- the buildings are rebuilt from them, so\n"
             "moving the road moves the whole skyline with it.");

    RoadSystem& road = s.road;
    bool changed = false;

    if (ImGui::Checkbox("Enabled", &road.cityEnabled)) changed = true;
    ImGui::SameLine();
    if (ImGui::Button("Rebuild")) { if (s.rebuild) s.rebuild(); }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Re-derive the city. It also rebuilds by itself\n"
                          "whenever the road is built or a rule changes.");

    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::DragInt("Budget", &road.cityBudget, 2.0f, 0, 4000)) changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hard ceiling on the number of buildings. Generation\n"
                          "stops there rather than filling a kilometre of road\n"
                          "with towers after a mistyped frontage.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::DragFloat("Draw range", &road.cityRange, 5.0f, 100.0f, 8000.0f, "%.0f m");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Buildings beyond this are not drawn at all, and the\n"
                          "camera's far plane follows it so a tower is never\n"
                          "sliced in half as it crosses. Raise it with the\n"
                          "detail cull beside it, not on its own -- alone it\n"
                          "buys depth by paying for every shed out there too.");
    ImGui::SetNextItemWidth(140.0f);
    ImGui::DragFloat("Detail cull", &road.cityMinPixels, 0.1f, 0.0f, 40.0f, "%.0f px");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Drop a chunk once it is shorter than this on screen.\n"
                          "What a long draw range is FOR is the tower still a\n"
                          "hundred pixels tall a kilometre out; what it costs is\n"
                          "the row of blocks four pixels tall behind it -- and a\n"
                          "draw costs the same either way. 0 = distance only\n"
                          "(and what a scene saved before this loads as).");

    // What actually came out, so "why is my street empty" has an answer.
    const city::District& d = road.district();
    if (!s.hasRoad) {
        ui::hint("Build the road first -- the city is laid out along its centreline.");
    } else if (road.biomes.empty()) {
        ui::hint("No biomes yet. Add one below.");
    } else {
        // Batches, not parts, are what the frame time is made of: each one is a
        // draw the renderer repeats per shadow cascade, per probe face, per pass.
        ImGui::Text("%d buildings, %d parts -> %d draws", d.placed, d.parts,
                    static_cast<int>(d.batches.size()));
        // Vertices are the cost the merge cannot remove -- worth showing, because
        // it is what the Budget slider actually trades against.
        ui::hint("%.0fk vertices, about %.0f MB on the GPU.", d.verts / 1000.0f,
                 d.verts * 52.0 / (1024.0 * 1024.0));
        if (d.skippedSlope > 0 || d.skippedTight > 0)
            ui::hint("Skipped: %d too steep, %d on a bend tighter than the plot.",
                     d.skippedSlope, d.skippedTight);
        if (d.budgetHit)
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                               "Budget reached -- the run stops short.");
    }

    ImGui::BeginDisabled(d.buildings.empty());
    if (ImGui::Button("Bake nearest to entities")) { if (s.bakeNearest) s.bakeNearest(); }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Rebuild the building nearest the camera as a real,\n"
                          "editable subtree in the scene -- to open it up, redress\n"
                          "it, or save it as a prefab. The derived copy stays where\n"
                          "it is; exclude it with a biome boundary if you want it\n"
                          "gone.");

    // --- The biome list ----------------------------------------------------
    ImGui::Separator();
    ui::sectionText("Biomes");
    ui::hint("Matched by distance along the road, first match wins.");

    static int addPreset = static_cast<int>(city::Preset::Canyon);
    ImGui::SetNextItemWidth(180.0f);
    {
        std::string items;
        for (int i = 0; i < static_cast<int>(city::Preset::Count); ++i) {
            items += city::presetName(static_cast<city::Preset>(i));
            items.push_back('\0');
        }
        items.push_back('\0');
        ImGui::Combo("##preset", &addPreset, items.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Add biome")) {
        s.beginEdit();
        city::Biome b = city::preset(static_cast<city::Preset>(addPreset));
        // A new biome starts where the last one ended and runs to the end of the
        // road, so adding two in a row gives two districts back to back instead
        // of two rulesets fighting over the same metres.
        float from = 0.0f;
        for (const city::Biome& e : road.biomes) from = std::max(from, std::max(e.from, e.to));
        b.from = from;
        b.to   = 0.0f;
        b.seed = 7u + static_cast<unsigned>(road.biomes.size()) * 7919u;
        road.biomes.push_back(std::move(b));
        s.endEdit("Add biome");
        changed = true;
    }

    int remove = -1, moveUp = -1;
    for (int i = 0; i < static_cast<int>(road.biomes.size()); ++i) {
        city::Biome& B = road.biomes[i];
        ImGui::PushID(i);

        const float to = (B.to > B.from) ? B.to : 0.0f;
        char title[192];
        if (to > B.from)
            std::snprintf(title, sizeof(title), "%s  (%.0f - %.0f m)###b",
                          B.name.c_str(), B.from, to);
        else
            std::snprintf(title, sizeof(title), "%s  (%.0f m - end)###b",
                          B.name.c_str(), B.from);

        const bool open = ui::header(title, i == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        // Row of per-biome actions, on the header's line so the list stays compact.
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 92.0f);
        if (ImGui::SmallButton("On##t")) {
            s.beginEdit(); B.enabled = !B.enabled; s.endEdit("Toggle biome");
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(B.enabled ? "Enabled -- click to mute" : "Muted");
        ImGui::SameLine();
        if (ImGui::SmallButton("Up##u")) moveUp = i;
        ImGui::SameLine();
        if (ImGui::SmallButton("X##x")) remove = i;

        if (open) {
            ImGui::Indent();
            if (!B.enabled) ui::hint("Muted -- nothing is built here.");
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s", B.name.c_str());
            if (ImGui::InputText("Name", buf, sizeof(buf))) B.name = buf;
            if (ImGui::IsItemActivated())            s.beginEdit();
            if (ImGui::IsItemDeactivatedAfterEdit()) s.endEdit("Rename biome");

            // Re-preset in place: keeps the extent (which the user placed by hand)
            // and replaces the character, which is what "try Slums here" means.
            ImGui::SetNextItemWidth(180.0f);
            static int rePreset = 0;
            {
                std::string items;
                for (int k = 0; k < static_cast<int>(city::Preset::Count); ++k) {
                    items += city::presetName(static_cast<city::Preset>(k));
                    items.push_back('\0');
                }
                items.push_back('\0');
                ImGui::Combo("##re", &rePreset, items.c_str());
            }
            ImGui::SameLine();
            if (ImGui::Button("Apply preset")) {
                s.beginEdit();
                const float from = B.from, toKeep = B.to, blend = B.blend;
                const unsigned seed = B.seed;
                B = city::preset(static_cast<city::Preset>(rePreset));
                B.from = from; B.to = toKeep; B.blend = blend; B.seed = seed;
                s.endEdit("Biome preset");
                changed = true;
            }

            changed |= biomeEditor(s, B, i);
            ImGui::Unindent();
        }
        ImGui::PopID();
    }

    if (moveUp > 0) {
        s.beginEdit();
        std::swap(road.biomes[moveUp], road.biomes[moveUp - 1]);
        s.endEdit("Reorder biomes");
        changed = true;
    }
    if (remove >= 0) {
        s.beginEdit();
        road.biomes.erase(road.biomes.begin() + remove);
        s.endEdit("Remove biome");
        changed = true;
    }

    // A small district follows the number under your finger; a big one waits for
    // you to let go. Rebuilding means re-running the tower generator, merging a
    // couple of million vertices and re-uploading them -- about 35 ms and 80 MB
    // for a full 400-building canyon, which is not something to do sixty times a
    // second, but nothing at all for the seventy-odd buildings most edits start
    // from. IsAnyItemActive() is true exactly while a widget is held, so the
    // split needs no timer.
    static bool pending = false;
    if (changed) pending = true;
    const bool cheap = road.district().parts <= 4000;
    if (pending && (cheap || !ImGui::IsAnyItemActive())) {
        pending = false;
        if (s.rebuild) s.rebuild();
    }

    if (!s.status.empty()) {
        ImGui::Separator();
        ui::hint("%s", s.status.c_str());
    }
    ImGui::End();
}

} // namespace cityui
