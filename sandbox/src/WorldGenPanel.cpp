#include "WorldGenPanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <imgui.h>

#include <fitzel/graphics/Texture.hpp>
#include <fitzel/ui/ImGuiTexture.hpp>

#include "SunPlaceUi.hpp"
#include "TreeGen.hpp"   // treegen::presets(): the ready-made species a row can grow
#include "UiStyle.hpp"

namespace worldui {
namespace {

// A slider that re-plans when it is RELEASED, never while it is dragged: a plan
// is tens of milliseconds and a map that redraws through twenty answers on the
// way to one is a map nobody can read. The level generator's rule, for the same
// reason.
struct Settled {
    bool released = false;
    void operator()() { released |= ImGui::IsItemDeactivatedAfterEdit(); }
};

void tip(const char* text) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

const ImVec4 kWarn(1.00f, 0.55f, 0.30f, 1.0f);
const ImVec4 kNote(0.80f, 0.80f, 0.55f, 1.0f);

// --- The map ---------------------------------------------------------------------

void drawMap(const PanelState& s) {
    const worldgen::Plan& pl = s.plan;
    const worldgen::Grid& g = pl.grid;
    const float side = std::clamp(ImGui::GetContentRegionAvail().x, 160.0f, 560.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + side, p0.y + side);
    ImGui::InvisibleButton("##worldmap", ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(28, 30, 34, 255));
    if (!s.planValid || g.n <= 1) {
        const char* msg = s.planning ? "Planning..." : "Move a slider or press Plan.";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(p0.x + 0.5f * (side - ts.x), p0.y + 0.5f * (side - ts.y)),
                    IM_COL32(200, 200, 200, 255), msg);
        return;
    }
    // Redrawn for a new plan, and for a sun that moved -- once the slider is let
    // go: the shade is a march over every cell, a few tens of milliseconds.
    const glm::vec3 sun = sunpos::at(s.params.look.place, s.params.hour).dir;
    const bool sunMoved = s.map.showShade != s.map.shaded ||
                          (s.map.showShade && glm::length(sun - s.map.sun) > 1e-4f);
    if (s.map.rev != s.planRev || !s.map.tex || (sunMoved && !ImGui::IsAnyItemActive())) {
        if (s.map.showShade) s.map.shade = worldgen::shade(pl, sun);
        else s.map.shade.clear();
        s.map.shaded = s.map.showShade;
        s.map.sun    = sun;
        int w = 0;
        const std::vector<unsigned char> px =
            worldgen::image(pl, 1, w, s.map.shade.empty() ? nullptr : &s.map.shade);
        if (w > 0) s.map.tex = std::make_shared<fitzel::Texture>(fitzel::Texture::fromPixels(px.data(), w, w, 4));
        s.map.rev = s.planRev;
    }
    if (s.map.tex && s.map.tex->isValid()) dl->AddImage(fitzel::imTexture(s.map.tex->id()), p0, p1);

    // The image spans the cells, centre to centre plus half a cell each side.
    const float span = g.cell * static_cast<float>(g.n);
    const glm::vec2 lo = g.origin - glm::vec2(0.5f * g.cell);
    auto toScreen = [&](glm::vec2 w) {
        return ImVec2(p0.x + (w.x - lo.x) / span * side, p0.y + (w.y - lo.y) / span * side);
    };
    auto toWorld = [&](ImVec2 sp) {
        return lo + glm::vec2((sp.x - p0.x) / side, (sp.y - p0.y) / side) * span;
    };
    dl->PushClipRect(p0, p1, true);
    auto poly = [&](const std::vector<glm::vec2>& l, ImU32 col, float thick) {
        for (std::size_t i = 1; i < l.size(); ++i)
            dl->AddLine(toScreen(l[i - 1]), toScreen(l[i]), col, thick);
    };
    for (const worldgen::River& r : pl.rivers)
        poly(r.points, IM_COL32(70, 135, 240, 255), r.brook ? 1.2f : 2.4f);
    for (const worldgen::Road& r : pl.roads) {
        poly(r.points, IM_COL32(40, 36, 30, 200), 4.0f);
        poly(r.points, IM_COL32(246, 236, 206, 255), 2.2f);
        for (const worldgen::Span& b : r.bridges)
            if (b.a >= 0 && b.b < static_cast<int>(r.points.size()))
                dl->AddLine(toScreen(r.points[static_cast<std::size_t>(b.a)]),
                            toScreen(r.points[static_cast<std::size_t>(b.b)]),
                            IM_COL32(235, 95, 225, 255), 3.6f);
    }
    for (const worldgen::Town& t : pl.towns) {
        const float a = glm::radians(t.rotation);
        const glm::vec2 ax(std::cos(a), std::sin(a)), az(-std::sin(a), std::cos(a));
        const glm::vec2 hx = ax * (0.5f * t.sizeX), hz = az * (0.5f * t.sizeZ);
        const ImVec2 q[4] = {toScreen(t.center - hx - hz), toScreen(t.center + hx - hz),
                             toScreen(t.center + hx + hz), toScreen(t.center - hx + hz)};
        dl->AddQuadFilled(q[0], q[1], q[2], q[3], IM_COL32(150, 70, 55, 110));
        dl->AddQuad(q[0], q[1], q[2], q[3], IM_COL32(120, 40, 28, 255), 1.5f);
        const ImVec2 c = toScreen(t.center);
        const ImVec2 ts = ImGui::CalcTextSize(t.name.c_str());
        const ImVec2 tp(c.x - 0.5f * ts.x, c.y - 0.5f * ts.y);
        dl->AddRectFilled(ImVec2(tp.x - 2, tp.y - 1), ImVec2(tp.x + ts.x + 2, tp.y + ts.y + 1),
                          IM_COL32(20, 20, 20, 150), 3.0f);
        dl->AddText(tp, IM_COL32(255, 245, 225, 255), t.name.c_str());
    }
    for (const worldgen::Farm& f : pl.farms) {
        const ImVec2 c = toScreen(f.pos);
        dl->AddRectFilled(ImVec2(c.x - 2.5f, c.y - 2.5f), ImVec2(c.x + 2.5f, c.y + 2.5f), IM_COL32(240, 160, 40, 255));
    }
    for (const worldgen::Rock& r : pl.rocks)
        dl->AddCircleFilled(toScreen({r.pos.x, r.pos.z}), 1.4f, IM_COL32(40, 40, 42, 230));
    // Where the camera is, and which way it looks.
    {
        const ImVec2 c = toScreen(s.camera);
        const ImVec2 f(s.cameraDir.x, s.cameraDir.y), r(f.y, -f.x);
        dl->AddTriangleFilled(ImVec2(c.x + f.x * 8, c.y + f.y * 8),
                              ImVec2(c.x - f.x * 4 + r.x * 5, c.y - f.y * 4 + r.y * 5),
                              ImVec2(c.x - f.x * 4 - r.x * 5, c.y - f.y * 4 - r.y * 5),
                              IM_COL32(255, 255, 255, 230));
    }
    dl->PopClipRect();
    dl->AddRect(p0, p1, IM_COL32(90, 90, 96, 255));

    if (hovered) {
        const glm::vec2 w = toWorld(ImGui::GetMousePos());
        static const char* uses[] = {"", "sea", "river", "town", "road", "farm", "crag"};
        const int ix = std::clamp(static_cast<int>(std::lround((w.x - g.origin.x) / g.cell)), 0, g.n - 1);
        const int iz = std::clamp(static_cast<int>(std::lround((w.y - g.origin.y) / g.cell)), 0, g.n - 1);
        const int u = g.use[static_cast<std::size_t>(g.index(ix, iz))];
        ImGui::SetTooltip("%.0f, %.0f -- %.0f m%s%s\nClick: fly the camera here.", w.x, w.y, g.heightAt(w),
                          u > 0 && u < 7 ? ", " : "", u > 0 && u < 7 ? uses[u] : "");
        if (clicked && s.lookAt) s.lookAt(w);
    }
}

void legend() {
    auto key = [](ImU32 col, const char* label) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 0.3f * h), ImVec2(p.x + 10, p.y + 0.7f * h), col);
        ImGui::Dummy(ImVec2(12, h));
        ImGui::SameLine(0, 2);
        ImGui::TextDisabled("%s", label);
        ImGui::SameLine(0, 10);
    };
    key(IM_COL32(70, 135, 240, 255), "water");
    key(IM_COL32(246, 236, 206, 255), "road");
    key(IM_COL32(235, 95, 225, 255), "bridge");
    key(IM_COL32(150, 70, 55, 255), "town");
    key(IM_COL32(240, 160, 40, 255), "farm");
    key(IM_COL32(40, 40, 42, 255), "rock");
    ImGui::NewLine();
}

void report(const PanelState& s) {
    if (!s.planValid) return;
    const worldgen::Report& r = s.plan.report;
    ui::hint("%d river%s, %d brook%s \xC2\xB7 %d town%s, %d village%s \xC2\xB7 %d farm%s \xC2\xB7 %d rock%s",
             r.rivers, r.rivers == 1 ? "" : "s", r.brooks, r.brooks == 1 ? "" : "s",
             r.towns, r.towns == 1 ? "" : "s", r.villages, r.villages == 1 ? "" : "s",
             r.farms, r.farms == 1 ? "" : "s", r.rocks, r.rocks == 1 ? "" : "s");
    ui::hint("%d road%s, %.1f km, %d bridge%s \xC2\xB7 steepest %.0f%% (limit %.0f%%)",
             r.roads, r.roads == 1 ? "" : "s", r.roadKm, r.bridges, r.bridges == 1 ? "" : "s",
             r.steepestRoad * 100.0f, s.params.look.maxGrade * 100.0f);
    char sea[48] = "no sea";
    if (s.plan.sea) std::snprintf(sea, sizeof(sea), "sea at %.0f m", s.plan.seaLevel);
    char tree[48] = "no tree line";
    if (s.plan.treeLine > 0.0f) std::snprintf(tree, sizeof(tree), "tree line %.0f m", s.plan.treeLine);
    ui::hint("Land %.0f .. %.0f m \xC2\xB7 %s \xC2\xB7 %s", r.lowest, r.highest, sea, tree);
    if (!r.why.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, r.ok ? kNote : kWarn);
        ImGui::TextWrapped("%s", r.why.c_str());
        ImGui::PopStyleColor();
    }
}

// --- The character: every number a region decides ------------------------------------

template <class E>
bool enumCombo(const char* label, E& v, int count, const char* (*name)(E)) {
    bool changed = false;
    if (ImGui::BeginCombo(label, name(v))) {
        for (int i = 0; i < count; ++i) {
            const E e = static_cast<E>(i);
            if (ImGui::Selectable(name(e), e == v) && e != v) { v = e; changed = true; }
        }
        ImGui::EndCombo();
    }
    return changed;
}

const char* rockName(int i) {
    static const char* names[] = {"Granite", "Sandstone", "Limestone", "Basalt", "Slate"};
    return i >= 0 && i < 5 ? names[i] : "?";
}

bool character(const PanelState& s, Settled& slid) {
    worldgen::Character& c = s.params.look;
    bool now = false;   // a combo: re-plan at once
    if (ImGui::Button("Back to the region's own")) { c = worldgen::character(s.params.region); now = true; }
    tip("Every number below as the region sets it.");

    ui::sectionText("Land");
    ImGui::SliderFloat("Height", &c.heightScale, 5.0f, 250.0f, "%.0f m"); slid();
    tip("How high the hills swing -- and, with Continents, the big lowlands and highlands.");
    ImGui::SliderFloat("Continents", &c.continentAmp, 0.0f, 3.0f, "%.2f"); slid();
    ImGui::SliderFloat("Mountains", &c.ridgeScale, 0.0f, 400.0f, "%.0f m"); slid();
    tip("Sharp ridged ranges on the rugged highlands.");
    ImGui::SliderFloat("Peaks", &c.peakSharpness, 0.5f, 2.5f, "%.2f"); slid();
    {
        float wave = 1.0f / std::max(c.frequency, 1e-5f);
        if (ImGui::SliderFloat("Hill spacing", &wave, 150.0f, 2500.0f, "%.0f m")) c.frequency = 1.0f / wave;
        slid();
        tip("How far apart the hills are. The terrain panel's presets are tuned for a\n"
            "scene of a few hundred metres (~80 m); a landscape wants ~600.");
    }
    ImGui::SliderFloat("Valleys", &c.valleyDepth, 0.0f, 150.0f, "%.0f m"); slid();
    tip("The terrain's carved valley network. Deep valleys make canyons and fjords --\n"
        "and hollows a river has to cut its way out of.");
    ImGui::SliderFloat("Terraces", &c.terrace, 0.0f, 1.0f, "%.2f"); slid();
    ImGui::SliderFloat("Relief", &c.reliefGain, 0.3f, 2.5f, "%.2f"); slid();
    ImGui::SliderFloat("Backdrop", &c.backdrop, 0.0f, 2500.0f, "%.0f m"); slid();
    tip("Mountains rising all round the planned area (0 = none).");
    ImGui::SliderFloat("Island", &c.island, 0.0f, 1.0f, "%.2f"); slid();
    tip("A bounded island, its radius this share of the map (0 = open land).");

    ui::sectionText("Water");
    {
        float pct = c.seaShare * 100.0f;
        if (ImGui::SliderFloat("Sea", &pct, 0.0f, 70.0f, "%.0f%% of the map")) c.seaShare = pct * 0.01f;
        slid();
        tip("The sea level is set so this much of the map lies under it.");
    }
    ImGui::SliderInt("Rivers", &c.rivers, 0, 3); slid();
    tip("At most; fewer when the land drains nowhere big enough.");
    ImGui::SliderInt("Brooks", &c.brooks, 0, 8); slid();
    now |= enumCombo("River kind", c.river, static_cast<int>(rivergen::Preset::Count), rivergen::presetName);
    now |= enumCombo("Brook kind", c.brook, static_cast<int>(rivergen::Preset::Count), rivergen::presetName);

    ui::sectionText("Settlement");
    ImGui::SliderInt("Towns", &c.towns, 0, 3); slid();
    now |= enumCombo("Town kind", c.town, static_cast<int>(cityplan::Preset::Count), cityplan::presetName);
    ImGui::SliderInt("Villages", &c.villages, 0, 8); slid();
    now |= enumCombo("Village kind", c.village, static_cast<int>(cityplan::Preset::Count), cityplan::presetName);
    ImGui::SliderFloat("Village size", &c.villageScale, 0.4f, 1.5f, "%.2f"); slid();
    {
        float pct = c.siteSlope * 100.0f;
        if (ImGui::SliderFloat("Steepest site", &pct, 2.0f, 25.0f, "%.0f%%")) c.siteSlope = pct * 0.01f;
        slid();
        tip("How steep a place may be and still be built on. Nothing levels a town's\n"
            "ground: a lot steeper than the town allows is simply left empty.");
    }
    ImGui::SliderInt("Farms", &c.farms, 0, 30); slid();
    now |= enumCombo("Farmhouse", c.farmHouse, static_cast<int>(housegen::Preset::Count), housegen::presetName);
    if (ImGui::ColorEdit3("Facades", &c.facade.x, ImGuiColorEditFlags_NoInputs)) {}
    slid();
    ImGui::SameLine();
    if (ImGui::ColorEdit3("Roofs", &c.roof.x, ImGuiColorEditFlags_NoInputs)) {}
    slid();

    ui::sectionText("Roads");
    ImGui::SliderFloat("Width", &c.roadWidth, 4.0f, 12.0f, "%.1f m"); slid();
    {
        float pct = c.maxGrade * 100.0f;
        if (ImGui::SliderFloat("Grade limit", &pct, 3.0f, 15.0f, "%.0f%%")) c.maxGrade = pct * 0.01f;
        slid();
        tip("What a road should keep to. Steeper costs the router dearly but is not\n"
            "forbidden; across a gorge it builds a viaduct instead.");
    }
    ImGui::SliderFloat("Extra links", &c.extraRoads, 0.0f, 1.0f, "%.2f"); slid();
    tip("0: every place reached once (a tree). Higher: loops where the tree makes\n"
        "the longest detours.");
    ImGui::SliderInt("Roads off the map", &c.exits, 0, 4); slid();

    ui::sectionText("Nature");
    ImGui::SliderFloat("Forest", &c.forest, 0.0f, 1.0f, "%.2f"); slid();
    ImGui::SliderFloat("Tree line", &c.treeLine, 0.0f, 900.0f, c.treeLine > 0.0f ? "%.0f m up" : "none"); slid();
    tip("Metres above the sea (or the lowest ground) where the forest gives out and\n"
        "the rock begins. 0: no tree line.");
    ImGui::SliderFloat("Rocks", &c.rocks, 0.0f, 1.0f, "%.2f"); slid();
    ImGui::SliderFloat("Crag slope", &c.rockSlope, 0.2f, 1.2f, "%.2f"); slid();
    tip("Ground steeper than this (rise over run) counts as crag: that is where the\n"
        "boulders lie.");
    if (ImGui::BeginCombo("Rock", rockName(c.rockType))) {
        for (int i = 0; i < 5; ++i)
            if (ImGui::Selectable(rockName(i), i == c.rockType) && i != c.rockType) { c.rockType = i; now = true; }
        ImGui::EndCombo();
    }
    return now;
}

// --- The forest: what the Tree generator grows for it --------------------------------
//
// One row per kind of tree: a ready-made species the Tree generator grows afresh
// on every Generate, or a tree someone made there and saved in trees/, planted
// as it is. "Edit" shows the row's tree in the Tree generator; the next save
// there becomes the row's tree. Rows do not change the plan, so nothing here
// re-plans the map.
void trees(const PanelState& s) {
    std::vector<worldgen::Character::Tree>& rows = s.params.look.trees;
    ui::hint("Grown with the Tree generator: a ready-made species, or a tree\n"
             "you made there and saved in the project's trees/ folder.");
    const auto& presets = treegen::presets();
    int drop = -1;
    const float w = ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        worldgen::Character::Tree& t = rows[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        const bool own = !t.file.empty();
        std::string label = own ? t.file.substr(0, t.file.rfind('.')) : t.preset;
        if (own) label += "  (yours)";
        ImGui::SetNextItemWidth(0.46f * w);
        if (ImGui::BeginCombo("##kind", label.c_str())) {
            ImGui::TextDisabled("Ready-made, grown afresh");
            for (const treegen::Preset& pr : presets)
                if (ImGui::Selectable(pr.name, !own && t.preset == pr.name)) {
                    t.preset = pr.name;
                    t.file.clear();
                }
            ImGui::Separator();
            ImGui::TextDisabled("Yours, from trees/");
            if (s.projectTrees.empty()) ImGui::TextDisabled("  none saved yet");
            for (const std::string& f : s.projectTrees)
                if (ImGui::Selectable(f.substr(0, f.rfind('.')).c_str(), t.file == f)) t.file = f;
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            if (own) ImGui::SetTooltip("Your tree, planted as you saved it. If the file goes,\n"
                                       "the row grows its ready-made species instead.");
            else     ImGui::SetTooltip("Grown by the Tree generator into trees/World_%s.glb\n"
                                       "on every Generate, with the world's seed.", t.preset.c_str());
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(0.26f * w);
        ImGui::SliderFloat("##share", &t.share, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How much of the forest is this tree, against the others.");
        ImGui::SameLine();
        ImGui::BeginDisabled(!s.haveProject || !s.editTree);
        if (ImGui::SmallButton(s.editingRow == i ? "Editing" : "Edit")) s.editTree(i);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Open this tree in the Tree generator. Change it, Save there,\n"
                              "and this row plants what you saved.");
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) drop = i;
        if (s.editingRow == i)
            ui::hint("   The Tree generator's next save becomes this row's tree.");
        ImGui::PopID();
    }
    if (drop >= 0) rows.erase(rows.begin() + drop);
    if (ImGui::SmallButton("+ Tree")) rows.push_back({"Oak", 0.2f, ""});
    ImGui::SameLine();
    ImGui::BeginDisabled(!s.haveProject || !s.replant || rows.empty() || s.applying);
    if (ImGui::SmallButton("Plant now")) s.replant();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Make these the forest's species now -- the land, the towns and\n"
                          "the roads stay as they are. For trying out a tree.");
    if (!s.haveProject) ui::hint("Open a project: the trees are written to its trees/ folder.");
}

// --- The sun: where on Earth, which day, what time ------------------------------------
//
// The map shows the shade of the land at this time -- the mountains' outside the
// planned square included -- so a valley that would lie in their shadow is seen
// to before it is generated. Generate gives the scene this place, date and time.
void sunSection(const PanelState& s) {
    worldgen::Params& p = s.params;
    {
        char label[16];
        std::snprintf(label, sizeof(label), "%s", sunui::clock(p.hour));
        ImGui::SliderFloat("Time", &p.hour, 0.0f, 24.0f, label);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The clock at the place below (summer time included). Generate\n"
                              "sets the scene to it; the Sky panel's clock takes over from there.");
    }
    sunui::placeEditor(p.look.place, p.hour);
    ImGui::Checkbox("Shade on the map", &s.map.showShade);
    if (!s.planValid || !s.map.shaded || s.map.shade.empty()) return;
    // Which places lie in the shade at this hour.
    const worldgen::Grid& g = s.plan.grid;
    std::string dark;
    int n = 0;
    for (const worldgen::Town& t : s.plan.towns) {
        const int i = std::clamp(static_cast<int>(std::lround((t.center.x - g.origin.x) / g.cell)), 0, g.n - 1);
        const int j = std::clamp(static_cast<int>(std::lround((t.center.y - g.origin.y) / g.cell)), 0, g.n - 1);
        if (s.map.shade[static_cast<std::size_t>(g.index(i, j))]) { dark += (n++ ? ", " : "") + t.name; }
    }
    int land = 0, shadeCells = 0;
    for (std::size_t k = 0; k < s.map.shade.size(); ++k)
        if (g.use[k] != worldgen::use::Sea) { ++land; shadeCells += s.map.shade[k]; }
    ui::hint("At %s %.0f%% of the land lies in shade.", sunui::clock(p.hour),
             land ? 100.0f * static_cast<float>(shadeCells) / static_cast<float>(land) : 0.0f);
    if (n > 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, kNote);
        ImGui::TextWrapped("In the shade then: %s.", dark.c_str());
        ImGui::PopStyleColor();
    } else if (!s.plan.towns.empty()) {
        ui::hint("Every place in the sun.");
    }
}

// --- The confirmation ------------------------------------------------------------

void confirmPopup(const PanelState& s) {
    ImGui::SetNextWindowSize(ImVec2(500.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Generate world", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const worldgen::Stages& st = s.params.stages;
    ImGui::TextWrapped("This puts the planned world into this scene.");
    ImGui::Spacing();
    ui::sectionText("Replaced");
    if (st.terrain) ImGui::BulletText("The terrain's shape and the sea level.");
    if (st.rivers)  ImGui::BulletText("Every river and brook (%d now).", s.sceneRivers);
    if (st.towns)   ImGui::BulletText("Every town (%d now) with its streets.", s.sceneTowns);
    if (st.roads || st.towns)
        ImGui::BulletText("Every road (%d now), circuits included.", s.sceneRoads);
    if (st.farms || st.rocks) ImGui::BulletText("The farms and rocks of an earlier generated world.");
    if (st.forest)  ImGui::BulletText("Forest cover, tree line, snow line and the tree species (the rows under Trees).");
    {
        int month = 1, dom = 1;
        sunpos::monthDay(s.params.look.place.day, month, dom);
        const int city = sunpos::cityAt(s.params.look.place);
        ImGui::BulletText("The place and the day of the sun, and the clock: %s, %d %s, %s.",
                          city >= 0 ? sunpos::cities()[static_cast<std::size_t>(city)].name : "your place",
                          dom, sunpos::monthName(month), sunui::clock(s.params.hour));
    }
    if (st.terrain) ImGui::BulletText("The terrain texture layers' height bands, moved with the sea.");
    if (s.sculptCells > 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
        ImGui::BulletText("Sculpting: %d edited cells, DISCARDED.", s.sculptCells);
        ImGui::PopStyleColor();
        ui::hint("Roads grade their corridors and rivers cut their beds into that layer;\n"
                 "the old ones would stay behind as trenches in the new land.");
    }
    ImGui::Spacing();
    ui::sectionText("Kept");
    ImGui::BulletText("Sun, sky, weather, your materials and everything you placed.");
    if (!st.rivers) ImGui::BulletText("The rivers, cut again into the new ground.");
    if (!st.towns && st.roads) ImGui::BulletText("The towns, their streets laid again.");
    if (s.paintCells > 0) ImGui::Checkbox("Also discard texture painting", &s.discardPaint);
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
    ImGui::TextWrapped("Sculpting and painting are not on the undo stack. Generate clears the undo history.");
    ImGui::PopStyleColor();
    ImGui::Separator();
    if (ImGui::Button("Generate", ImVec2(140.0f, 0.0f))) {
        s.generate();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(140.0f, 0.0f))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    ImGui::SetNextWindowSize(ImVec2(460.0f, 760.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("World generator", &s.show)) { ImGui::End(); return; }

    worldgen::Params& p = s.params;
    Settled slid;
    bool now = false;

    ui::hint("A whole landscape from one region and one seed: the land, where the\n"
             "water drains, where people settle, the roads between them, the farms,\n"
             "the forest and the rock -- every part planned knowing the others, then\n"
             "built by the generators the editor already has.");

    // --- What kind of place ---------------------------------------------------------
    ui::sectionText("Region");
    if (ImGui::BeginCombo("Region", worldgen::regionName(p.region))) {
        for (int i = 0; i < static_cast<int>(worldgen::Region::Count); ++i) {
            const auto r = static_cast<worldgen::Region>(i);
            if (ImGui::Selectable(worldgen::regionName(r), r == p.region) && r != p.region) {
                p.region = r;
                p.look = worldgen::character(r);
                now = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", worldgen::regionBlurb(r));
        }
        ImGui::EndCombo();
    }
    ui::hint("%s", worldgen::regionBlurb(p.region));
    {
        int seed = static_cast<int>(p.seed);
        if (ImGui::DragInt("Seed", &seed, 1.0f, 1, 1000000)) p.seed = static_cast<unsigned>(std::max(seed, 1));
        slid();
        ImGui::SameLine();
        if (ImGui::Button("Reroll")) {
            p.seed = p.seed * 1664525u + 1013904223u;
            if (p.seed == 0) p.seed = 1;
            p.seed %= 1000000u;
            if (p.seed == 0) p.seed = 1;
            now = true;
        }
    }
    ImGui::SliderFloat("Size", &p.size, 1500.0f, 6000.0f, "%.0f m"); slid();
    tip("The side of the square that is planned. The terrain runs on beyond it;\n"
        "rivers and roads leave the map as if the world did.");
    ImGui::Text("Centre %.0f, %.0f", p.center.x, p.center.y);
    ImGui::SameLine();
    if (ImGui::SmallButton("Here")) {
        p.center = glm::round(s.camera / 10.0f) * 10.0f;
        now = true;
    }
    tip("Plan around the camera instead.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Origin")) { p.center = glm::vec2(0.0f); now = true; }

    // --- Which generators take part ---------------------------------------------------
    ui::sectionText("Stages");
    {
        worldgen::Stages& st = p.stages;
        const bool before[7] = {st.terrain, st.rivers, st.towns, st.roads, st.farms, st.forest, st.rocks};
        if (ImGui::BeginTable("##stages", 2)) {
            ImGui::TableNextColumn(); ImGui::Checkbox("Terrain & sea", &st.terrain);
            tip("Off: plan on the ground the scene already has.");
            ImGui::TableNextColumn(); ImGui::Checkbox("Rivers & brooks", &st.rivers);
            ImGui::TableNextColumn(); ImGui::Checkbox("Towns & villages", &st.towns);
            ImGui::TableNextColumn(); ImGui::Checkbox("Roads & bridges", &st.roads);
            ImGui::TableNextColumn(); ImGui::Checkbox("Farms", &st.farms);
            ImGui::TableNextColumn(); ImGui::Checkbox("Forest", &st.forest);
            ImGui::TableNextColumn(); ImGui::Checkbox("Rocks", &st.rocks);
            ImGui::EndTable();
        }
        const bool after[7] = {st.terrain, st.rivers, st.towns, st.roads, st.farms, st.forest, st.rocks};
        now |= !std::equal(std::begin(before), std::end(before), std::begin(after));
    }

    // --- The plan -----------------------------------------------------------------------
    ui::sectionText("Plan");
    drawMap(s);
    legend();
    if (s.planning) ui::hint("Planning...");
    report(s);

    if (ui::header("Sun", ImGuiTreeNodeFlags_DefaultOpen)) sunSection(s);
    if (ui::header("Trees", ImGuiTreeNodeFlags_DefaultOpen)) trees(s);
    if (ui::header("Character")) now |= character(s, slid);

    ImGui::Separator();
    ImGui::BeginDisabled(s.applying);
    if (ImGui::Button(s.applying ? "Generating..." : "Generate world...", ImVec2(-1.0f, 0.0f)))
        ImGui::OpenPopup("Generate world");
    ImGui::EndDisabled();
    confirmPopup(s);

    if ((slid.released || now) && s.preview) s.preview();
    ImGui::End();
}

} // namespace worldui
