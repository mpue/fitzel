#include "CityPlanPanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <imgui.h>

#include "CityCommand.hpp"
#include "RoadSet.hpp"
#include <fitzel/asset/AssetDatabase.hpp>
#include "UiStyle.hpp"

namespace citygenui {

namespace {

using cityplan::Grid;
using cityplan::Rule;
using cityplan::Zone;

// A whole number on the same big -/+ buttons as every other amount.
bool stepInt(const char* id, int& v, int step, int lo, int hi, const char* fmt) {
    float f = static_cast<float>(v);
    if (!ui::stepper(id, f, static_cast<float>(step), static_cast<float>(lo),
                     static_cast<float>(hi), fmt))
        return false;
    v = static_cast<int>(std::lround(f));
    return true;
}

// Label on the left, stepper on the right: one row per amount, so a column of
// them lines up and every target sits in the same place.
bool row(const char* label, float& v, float step, float lo, float hi, const char* fmt,
         const char* tip = nullptr) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    ImGui::SameLine(ImGui::GetFontSize() * 10.0f);
    return ui::stepper(label, v, step, lo, hi, fmt, ImGui::GetFontSize() * 12.0f);
}
bool rowInt(const char* label, int& v, int step, int lo, int hi, const char* fmt,
            const char* tip = nullptr) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    ImGui::SameLine(ImGui::GetFontSize() * 10.0f);
    float f = static_cast<float>(v);
    if (!ui::stepper(label, f, static_cast<float>(step), static_cast<float>(lo),
                     static_cast<float>(hi), fmt, ImGui::GetFontSize() * 12.0f))
        return false;
    v = static_cast<int>(std::lround(f));
    return true;
}

ImU32 zoneColor(Zone z) {
    switch (z) {
        case Zone::Towers: return IM_COL32(70, 96, 140, 255);
        case Zone::Blocks: return IM_COL32(196, 146, 92, 255);
        case Zone::Rows:   return IM_COL32(188, 92, 76, 255);
        case Zone::Houses: return IM_COL32(222, 205, 170, 255);
        case Zone::Industry: return IM_COL32(128, 116, 150, 255);
        case Zone::Park:   return IM_COL32(96, 160, 88, 255);
        default:           return IM_COL32(128, 128, 128, 255);
    }
}

// The town from above: blocks in their zone's colour, the draft street grid on
// top, the streets actually laid (when they differ) as a grey ghost under it,
// and a dot per building that stands. North (-Z) is up.
void drawMap(const Rule& r, const CitySystem::Built* built, glm::vec3 cursor) {
    const cityplan::Layout draft = cityplan::layout(r, r.grid);
    const bool ghost = r.hasLaid && r.laid != r.grid;
    const cityplan::Layout laid = ghost ? cityplan::layout(r, r.laid) : cityplan::Layout{};

    glm::vec2 lo(1e9f), hi(-1e9f);
    auto grow = [&](const cityplan::Layout& L) {
        for (const cityplan::Street& s : L.streets)
            for (const glm::vec2& p : s.pts) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
    };
    grow(draft);
    if (ghost) grow(laid);
    if (hi.x <= lo.x) return;
    const glm::vec2 ext = hi - lo;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float m  = 8.0f;
    const float sc = std::min((avail - 2.0f * m) / ext.x, 340.0f / ext.y);
    const ImVec2 size(avail, ext.y * sc + 2.0f * m);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##townMap", size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto P = [&](glm::vec2 w) {
        return ImVec2(o.x + m + (w.x - lo.x) * sc, o.y + m + (w.y - lo.y) * sc);
    };
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y),
                      ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);

    for (const cityplan::Block& b : draft.blocks)
        dl->AddQuadFilled(P(b.corner[0]), P(b.corner[1]), P(b.corner[2]), P(b.corner[3]),
                          zoneColor(b.zone));
    if (ghost)
        for (const cityplan::Street& s : laid.streets)
            for (std::size_t i = 0; i + 1 < s.pts.size(); ++i)
                dl->AddLine(P(s.pts[i]), P(s.pts[i + 1]), IM_COL32(150, 150, 150, 150),
                            std::max(1.0f, s.width * sc));
    for (const cityplan::Street& s : draft.streets)
        for (std::size_t i = 0; i + 1 < s.pts.size(); ++i)
            dl->AddLine(P(s.pts[i]), P(s.pts[i + 1]), IM_COL32(52, 54, 58, 255),
                        std::max(s.avenue ? 2.5f : 1.5f, s.width * sc));
    if (built)
        for (const cityplan::Placed& p : built->town.placed)
            dl->AddCircleFilled(P(p.pos), std::max(1.2f, std::min(p.radius * sc * 0.5f, 4.0f)),
                                IM_COL32(20, 20, 24, 200), 8);
    // The public buildings, one letter on their block.
    for (const cityplan::Block& b : draft.blocks) {
        if (b.civic < 0) continue;
        // One letter per kind (civic::Kind order; Industry is never a block's).
        static const char* kLetter[] = {"C", "P", "F", "H", "I", "T", "S", "K", "L", "M", "Th", "Sw",
                                        "G", "E", "D", "B"};
        static const ImU32 kCol[] = {
            IM_COL32(245, 235, 200, 255), IM_COL32(40, 90, 210, 255), IM_COL32(215, 40, 30, 255),
            IM_COL32(255, 255, 255, 255), IM_COL32(150, 150, 150, 255), IM_COL32(235, 215, 170, 255),
            IM_COL32(200, 120, 90, 255),  IM_COL32(240, 190, 60, 255),  IM_COL32(240, 240, 240, 255),
            IM_COL32(210, 200, 180, 255), IM_COL32(170, 40, 40, 255),   IM_COL32(60, 160, 210, 255),
            IM_COL32(20, 150, 130, 255),  IM_COL32(200, 200, 60, 255),  IM_COL32(120, 95, 70, 255),
            IM_COL32(180, 60, 60, 255)};
        static_assert(sizeof(kLetter) / sizeof(kLetter[0]) == civic::kKinds, "a letter per kind");
        const int k = glm::clamp(b.civic, 0, civic::kKinds - 1);
        const ImVec2 c = P(0.25f * (b.corner[0] + b.corner[1] + b.corner[2] + b.corner[3]));
        const float rad = std::max(7.0f, ImGui::GetFontSize() * 0.6f);
        dl->AddCircleFilled(c, rad, kCol[k], 16);
        dl->AddCircle(c, rad, IM_COL32(20, 20, 24, 255), 16, 1.5f);
        const ImVec2 ts = ImGui::CalcTextSize(kLetter[k]);
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                    k == 3 ? IM_COL32(210, 20, 20, 255) : IM_COL32(20, 20, 24, 255), kLetter[k]);
    }
    // Where the 3D cursor is, if it is on the map at all.
    const glm::vec2 c(cursor.x, cursor.z);
    if (c.x >= lo.x && c.x <= hi.x && c.y >= lo.y && c.y <= hi.y) {
        const ImVec2 q = P(c);
        dl->AddCircle(q, 6.0f, IM_COL32(255, 80, 60, 255), 16, 2.0f);
        dl->AddLine(ImVec2(q.x - 9, q.y), ImVec2(q.x + 9, q.y), IM_COL32(255, 80, 60, 255), 1.5f);
        dl->AddLine(ImVec2(q.x, q.y - 9), ImVec2(q.x, q.y + 9), IM_COL32(255, 80, 60, 255), 1.5f);
    }
    // Legend.
    ImGui::Spacing();
    for (int z = 0; z < static_cast<int>(Zone::Count); ++z) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + h, p.y + h),
                                                  zoneColor(static_cast<Zone>(z)), 2.0f);
        ImGui::Dummy(ImVec2(h, h));
        ImGui::SameLine();
        ImGui::TextUnformatted(cityplan::zoneName(static_cast<Zone>(z)));
        if (z + 1 < static_cast<int>(Zone::Count)) ImGui::SameLine(0.0f, 14.0f);
    }
}

} // namespace

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    ImGui::SetNextWindowSize(ImVec2(520, 760), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Town generator", &s.show)) { ImGui::End(); return; }

    CitySystem& cs = s.cities;
    // While nothing is being edited, keep the undo bracket on the towns as they
    // are -- an undo or a scene load changes them behind the panel's back.
    if (!s.editing) s.undoBefore = cs.snapshot();

    // Finish an edit that is in flight before anything that pushes its own step.
    auto flush = [&] {
        if (!s.editing) return;
        s.editing = false;
        auto cmd = std::make_unique<CityCmd>(cs, s.roads, s.undoBefore, cs.snapshot(),
                                             std::vector<int>{}, std::vector<int>{},
                                             "Edit town");
        if (!cmd->trivial()) s.pushApplied(std::move(cmd));
        s.undoBefore = cs.snapshot();
    };

    ui::hint("A whole town from a handful of numbers. The streets become\n"
             "ordinary roads (graded, with junctions and bridges); the\n"
             "buildings are derived and follow every change live.");

    // --- The list ------------------------------------------------------------
    if (cs.count() == 0) s.sel = -1;
    if (s.sel >= cs.count()) s.sel = cs.count() - 1;
    for (int i = 0; i < cs.count(); ++i) {
        const Rule& r = cs.towns[static_cast<std::size_t>(i)];
        char label[160];
        std::snprintf(label, sizeof label, "%s%s##town%d", r.name.c_str(),
                      r.enabled ? "" : "  (off)", r.id);
        if (ImGui::Selectable(label, s.sel == i, 0, ImVec2(0, ImGui::GetFrameHeight())))
            s.sel = i;
    }
    const ImVec2 big(ImGui::GetFontSize() * 9.0f, ImGui::GetFrameHeight() * 1.3f);
    if (ImGui::Button("New town at cursor", big)) {
        flush();
        const CitySystem::Snapshot before = cs.snapshot();
        Rule r;
        cityplan::applyPreset(r, cityplan::Preset::SmallTown);
        r.grid.center = {s.cursor.x, s.cursor.z};
        r.name = "Town " + std::to_string(cs.count() + 1);
        // A town of its own: another seed means other street names and other
        // houses, where the defaults would give every new town the same ones.
        {
            std::uint32_t h = static_cast<std::uint32_t>(s.cursor.x * 131.0f) * 2654435761U ^
                              static_cast<std::uint32_t>(s.cursor.z * 137.0f) * 40503U ^
                              static_cast<std::uint32_t>(cs.count() + 1) * 97U;
            h ^= h >> 15;
            r.seed      = h % 100000U;
            r.grid.seed = (h >> 7) % 100000U;
        }
        s.sel = cs.add(r);
        s.pushApplied(std::make_unique<CityCmd>(cs, s.roads, before, cs.snapshot(),
                                                std::vector<int>{}, std::vector<int>{},
                                                "New town"));
        s.status = "New town at the 3D cursor. Pick a preset, then Lay streets.";
    }
    ImGui::SetItemTooltip("A small town centred on the 3D cursor. Nothing is\n"
                          "built into the terrain until you press Lay streets.");
    if (s.sel >= 0) {
        ImGui::SameLine();
        if (ImGui::Button("Delete town", big)) {
            flush();
            const CitySystem::Snapshot before = cs.snapshot();
            std::vector<int> added;
            const std::vector<int> killed =
                cs.removeStreets(cs.towns[static_cast<std::size_t>(s.sel)].id, s.roads, &added);
            cs.erase(s.sel);
            if (!killed.empty()) cs.requestBuild();
            s.pushApplied(std::make_unique<CityCmd>(cs, s.roads, before, cs.snapshot(),
                                                    killed, added, "Delete town"));
            s.sel = std::min(s.sel, cs.count() - 1);
            s.status = "Town deleted, with its streets (Ctrl+Z brings both back).";
        }
    }
    if (s.sel < 0) {
        if (!s.status.empty()) ui::hint("%s", s.status.c_str());
        ImGui::End();
        return;
    }

    Rule& r = cs.towns[static_cast<std::size_t>(s.sel)];
    Grid& g = r.grid;
    bool changed = false;

    ImGui::Separator();
    {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s", r.name.c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        if (ImGui::InputText("Name", buf, sizeof buf)) { r.name = buf; changed = true; }
        ImGui::SameLine();
        if (ImGui::Checkbox("Enabled", &r.enabled)) changed = true;
    }

    // --- Presets ---------------------------------------------------------------
    ui::sectionText("Character");
    for (int p = 0; p < static_cast<int>(cityplan::Preset::Count); ++p) {
        if (p) ImGui::SameLine();
        if (ImGui::Button(cityplan::presetName(static_cast<cityplan::Preset>(p)),
                          ImVec2(0, ImGui::GetFrameHeight() * 1.3f))) {
            cityplan::applyPreset(r, static_cast<cityplan::Preset>(p));
            changed = true;
        }
    }
    ImGui::SetItemTooltip("Sets size, grid, zoning and density; place and seed stay.");

    // --- Map ---------------------------------------------------------------------
    const CitySystem::Built* built =
        s.sel < static_cast<int>(cs.built().size()) ? &cs.built()[static_cast<std::size_t>(s.sel)]
                                                    : nullptr;
    drawMap(r, built, s.cursor);

    // --- Streets -----------------------------------------------------------------
    const int laidCount = cs.streetCount(r.id, s.roads);
    if (!r.hasLaid || laidCount == 0)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "No streets yet -- the buildings are a preview.");
    else if (r.streetsStale())
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "The street plan changed -- Lay streets to move the roads.");
    else
        ImGui::TextDisabled("%d streets laid, up to date.", laidCount);
    if (ImGui::Button("Lay streets", ImVec2(ImGui::GetFontSize() * 12.0f,
                                            ImGui::GetFrameHeight() * 1.6f))) {
        flush();
        const CitySystem::Snapshot before = cs.snapshot();
        const CitySystem::Laid laid = cs.layStreets(s.sel, s.roads);
        s.pushApplied(std::make_unique<CityCmd>(cs, s.roads, before, cs.snapshot(),
                                                laid.removed, laid.added, "Lay streets"));
        char buf[160];
        std::snprintf(buf, sizeof buf, "Laid %d streets as %zu roads, %d bridge%s%s.",
                      laid.streets, laid.added.size(), laid.bridges,
                      laid.bridges == 1 ? "" : "s",
                      laid.breaks ? ", some broken at wide water" : "");
        s.status = buf;
    }
    ImGui::SetItemTooltip("Replace this town's roads with the street plan above and\n"
                          "build them into the terrain. Roads you drew yourself are\n"
                          "left alone. One undo step.");
    ImGui::SameLine();
    if (ImGui::Button("Place at cursor", ImVec2(0, ImGui::GetFrameHeight() * 1.6f))) {
        g.center = {s.cursor.x, s.cursor.z};
        changed = true;
    }
    ImGui::SetItemTooltip("Move the town's centre to the 3D cursor (then Lay streets).");

    if (ui::header("Where and streets", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= row("Size X", g.sizeX, 20.0f, 80.0f, 6000.0f, "%.0f m");
        changed |= row("Size Z", g.sizeZ, 20.0f, 80.0f, 6000.0f, "%.0f m");
        changed |= row("Rotation", g.rotation, 5.0f, -180.0f, 180.0f, "%.0f\xC2\xB0");
        changed |= row("Block X", g.blockX, 5.0f, 30.0f, 400.0f, "%.0f m",
                       "Street spacing along the town's X, centre to centre.");
        changed |= row("Block Z", g.blockZ, 5.0f, 30.0f, 400.0f, "%.0f m");
        changed |= row("Organic", g.organic, 0.05f, 0.0f, 1.0f, "%.2f",
                       "0 = chessboard, 1 = crooked old town.");
        changed |= row("Street width", g.streetWidth, 0.5f, 3.0f, 30.0f, "%.1f m");
        changed |= row("Avenue width", g.avenueWidth, 0.5f, 3.0f, 40.0f, "%.1f m");
        changed |= rowInt("Avenue every", g.avenueEvery, 1, 0, 20, "%.0f",
                          "Every Nth street through the centre is an avenue (0 = none).");
        changed |= row("Roads out", g.stub, 5.0f, 10.0f, 400.0f, "%.0f m",
                       "How far each street runs on past the town edge.");
        if (ImGui::Checkbox("Bridge water", &g.bridges)) changed = true;
        ImGui::SetItemTooltip("Streets crossing a river get a bridge. Off: they stop\n"
                              "at the banks.");
        changed |= row("Longest bridge", g.maxBridge, 10.0f, 20.0f, 1000.0f, "%.0f m",
                       "Wider water breaks the street instead of bridging it.");
        float gs = static_cast<float>(g.seed);
        if (row("Street seed", gs, 1.0f, 0.0f, 99999.0f, "%.0f")) {
            g.seed = static_cast<unsigned>(gs);
            changed = true;
        }
        static const char* kNameLists[] = {"Invented", "Frankfurt am Main"};
        if (ImGui::Combo("Street names", &g.names, kNameLists, 2)) changed = true;
        ImGui::SetItemTooltip("Invented: German names from a pool of parts.\n"
                              "Frankfurt am Main: drawn from the city's real street\n"
                              "directory. The street seed picks which.");
    }

    // --- Zoning ------------------------------------------------------------------
    if (ui::header("Zoning", ImGuiTreeNodeFlags_DefaultOpen)) {
        ui::hint("Rings from the centre (0) to the edge (1).");
        changed |= row("Towers up to", r.towerRing, 0.05f, 0.0f, 2.0f, "%.2f");
        changed |= row("Blocks up to", r.blockRing, 0.05f, 0.0f, 2.0f, "%.2f");
        changed |= row("Terraces up to", r.rowRing, 0.05f, 0.0f, 2.0f, "%.2f",
                       "Beyond this: detached family houses.");
        changed |= row("Ragged rings", r.zoneNoise, 0.02f, 0.0f, 0.6f, "%.2f");
        changed |= row("Parks", r.parkChance, 0.02f, 0.0f, 1.0f, "%.2f",
                       "Chance any block is left green (the trees grow there).");
        if (ImGui::Checkbox("Square in the centre", &r.centrePark)) changed = true;
        float seed = static_cast<float>(r.seed);
        if (row("Seed", seed, 1.0f, 0.0f, 99999.0f, "%.0f")) {
            r.seed = static_cast<unsigned>(seed);
            changed = true;
        }
    }

    // --- Public buildings and industry ---------------------------------------------
    if (ui::header("Public buildings & industry", ImGuiTreeNodeFlags_DefaultOpen)) {
        ui::hint("Each takes a whole block. Map: C church, P police, F fire,\n"
                 "H hospital, T town hall, S school, K kindergarten, L library,\n"
                 "M museum, Th theatre, Sw swimming pool, G petrol station,\n"
                 "E power station, D landfill, B main station.");
        using K = civic::Kind;
        static const K kKinds[] = {K::Church, K::Police, K::FireStation, K::Hospital, K::TownHall,
                                   K::School, K::Kindergarten, K::Library, K::Museum, K::Theatre,
                                   K::Pool, K::Station, K::PetrolStation, K::PowerPlant,
                                   K::Landfill};
        for (K kind : kKinds) {
            cityplan::CivicSlot& c = r.civic[static_cast<std::size_t>(kind)];
            ImGui::PushID(static_cast<int>(kind));
            changed |= rowInt(civic::kindName(kind), c.count, 1, 0, 12, "%.0f");
            if (c.count > 0) {
                // Generated by the town, or an imported model stood on the block.
                ImGui::Indent();
                int src = c.useModel ? 1 : 0;
                if (ImGui::RadioButton("Generated", &src, 0)) { c.useModel = false; changed = true; }
                ImGui::SameLine();
                if (ImGui::RadioButton("Model", &src, 1)) { c.useModel = true; changed = true; }
                ImGui::SetItemTooltip("Stand an imported model on the block instead of the\n"
                                      "building the town generates.");
                if (c.useModel) {
                    const std::string preview = c.model.empty() ? "(pick a model)" : c.model;
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::BeginCombo("##model", preview.c_str())) {
                        fitzel::AssetDatabase& db = s.roads.assetDb();
                        int shown = 0;
                        for (const fitzel::AssetId& id : db.allAssets()) {
                            const auto* e = db.entry(id);
                            if (!e || e->type != fitzel::AssetType::Model) continue;
                            ++shown;
                            const std::string label = e->absPath.filename().string();
                            if (ImGui::Selectable((label + "##" + id.toString()).c_str(),
                                                  e->relPath == c.model)) {
                                c.model = e->relPath;
                                changed = true;
                            }
                            if (ImGui::IsItemHovered() && e->relPath != label)
                                ImGui::SetTooltip("%s", e->relPath.c_str());
                        }
                        if (shown == 0)
                            ImGui::TextDisabled("No model assets found (drop a .glb in Assets)");
                        ImGui::EndCombo();
                    }
                    if (ImGui::Checkbox("Fit the block", &c.fitPlot)) changed = true;
                    ImGui::SetItemTooltip("Scale the model to fill the block. Off: as imported.");
                    ImGui::SameLine();
                    float deg = 90.0f * static_cast<float>(c.turn);
                    if (ui::stepper("##turn", deg, 90.0f, 0.0f, 270.0f, "%.0f\xC2\xB0 turn")) {
                        c.turn = static_cast<int>(std::lround(deg / 90.0f));
                        changed = true;
                    }
                    ImGui::SetItemTooltip("Turn the model to face its front to the street.");
                }
                ImGui::Unindent();
            }
            ImGui::PopID();
        }
        if (ImGui::Checkbox("Power lines", &r.powerLines)) changed = true;
        ImGui::SetItemTooltip("A high-voltage line on lattice pylons from every power\n"
                              "station out of town.");
        if (r.powerLines)
            changed |= row("Line length", r.powerLineLength, 100.0f, 100.0f, 10000.0f, "%.0f m");
        changed |= row("Industry", r.industryShare, 0.02f, 0.0f, 0.6f, "%.2f",
                       "Share of all blocks that become the industrial estate,\n"
                       "taken from the outer blocks on the side below.");
        changed |= row("Industry side", r.industryAngle, 45.0f, -180.0f, 180.0f,
                       "%.0f\xC2\xB0", "Which side of town the estate is on\n"
                       "(0 = the town's +X, 90 = its +Z).");
    }

    // --- Buildings ---------------------------------------------------------------
    if (ui::header("Buildings")) {
        changed |= row("Pavement", r.sidewalk, 0.5f, 0.0f, 20.0f, "%.1f m",
                       "Kerb to the building line.");
        changed |= rowInt("Tower floors min", r.towerFloorsMin, 1, 1, 200, "%.0f");
        changed |= rowInt("Tower floors max", r.towerFloorsMax, 1, 1, 200, "%.0f");
        changed |= rowInt("Block floors min", r.blockFloorsMin, 1, 1, 30, "%.0f");
        changed |= rowInt("Block floors max", r.blockFloorsMax, 1, 1, 30, "%.0f");
        changed |= row("House plot", r.houseLot, 1.0f, 12.0f, 60.0f, "%.0f m",
                       "Frontage of a family house's plot.");
        changed |= row("Front garden", r.houseSetback, 0.5f, 0.0f, 20.0f, "%.1f m");
        changed |= row("Built plots", r.fill, 0.02f, 0.0f, 1.0f, "%.2f",
                       "The rest stay empty plots.");
        changed |= row("Max slope", r.maxSlope, 0.02f, 0.02f, 1.0f, "%.2f",
                       "Steeper ground than this (drop per metre) stays unbuilt.");
        changed |= rowInt("Budget", r.budget, 100, 0, 10000, "%.0f",
                          "Hard cap on buildings.");
        if (ImGui::Checkbox("Solid in Play", &r.collider)) changed = true;
    }

    // --- Look --------------------------------------------------------------------
    if (ui::header("Look")) {
        auto col = [&](const char* label, glm::vec3& c) {
            if (ImGui::ColorEdit3(label, &c.x, ImGuiColorEditFlags_NoInputs)) changed = true;
        };
        col("House walls", r.facadeColor);
        ImGui::SameLine();
        col("Roofs", r.roofColor);
        col("Block walls", r.blockColor);
        ImGui::SameLine();
        col("Block base", r.blockBase);
        col("Tower glass", r.glassTint);
        ImGui::SameLine();
        col("Tower base", r.towerBase);
        changed |= row("Colour variety", r.colourVariety, 0.1f, 0.0f, 1.0f, "%.1f",
                       "How many houses and blocks wear their own plaster and\n"
                       "roof colour instead of the two above (0 = all alike).");
        changed |= row("Lit windows", r.windowLit, 0.05f, 0.0f, 1.0f, "%.2f");
        changed |= row("Weathering", r.weathering, 0.05f, 0.0f, 1.0f, "%.2f");
        if (ImGui::Checkbox("Street signs", &r.signs)) changed = true;
        ImGui::SetItemTooltip("A name sign on a corner of every crossing. The names are\n"
                              "the roads' own -- rename a street in the Roads panel and\n"
                              "its signs follow.");
        if (r.signs) {
            const auto& ps = streetsign::presets();
            const int n = static_cast<int>(ps.size());
            r.signStyle = std::clamp(r.signStyle, 0, n - 1);
            if (ImGui::BeginCombo("Sign style", ps[static_cast<std::size_t>(r.signStyle)].name)) {
                for (int i = 0; i < n; ++i)
                    if (ImGui::Selectable(ps[static_cast<std::size_t>(i)].name, i == r.signStyle)) {
                        r.signStyle = i;
                        changed = true;
                    }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("How the signs are lettered (see View > Track > Street signs).");
        }
        if (ImGui::Checkbox("Pavements", &r.pavements)) changed = true;
        ImGui::SetItemTooltip("The pavement band (Buildings > Pavement) paved, on a 12 cm\n"
                              "kerb that follows the laid streets and stops at the crossings.");
        ImGui::SameLine();
        if (ImGui::Checkbox("Traffic lights", &r.trafficLights)) changed = true;
        ImGui::SetItemTooltip("At crossings with an avenue, and in the tower and\n"
                              "apartment-block zones: one per approach, on the right.");
        changed |= row("Bus stop every", r.busStopEvery, 50.0f, 0.0f, 2000.0f, "%.0f m",
                       "A stop in each direction, named after the next cross street.\n"
                       "With a shelter where the pavement is 2.4 m or wider. 0 = none.");
        changed |= row("Traffic", r.traffic, 1.0f, 0.0f, 60.0f, "%.0f / km",
                       "Vehicles per kilometre of street, driving on the right,\n"
                       "stopping at red lights. 0 = none.");
        changed |= row("Buses", r.busShare, 0.05f, 0.0f, 1.0f, "%.2f",
                       "The share of the traffic that is buses (they call at the stops).");
        changed |= row("Lorries", r.truckShare, 0.05f, 0.0f, 1.0f, "%.2f");
        changed |= row("People", r.people, 0.5f, 0.0f, 20.0f, "%.1f / 100 m",
                       "Pedestrians per 100 m of pavement, walking round the blocks.");
        // Prefabs the traffic drives instead of its placeholder blocks.
        if (ImGui::TreeNode("Vehicle prefabs")) {
            ui::hint("Each vehicle of a kind is one of these or a placeholder,\n"
                     "by weight. A scene object can also join the traffic in\n"
                     "Play: give it a Traffic driver component.");
            changed |= row("Placeholders", r.placeholderWeight, 0.5f, 0.0f, 50.0f, "%.1f",
                           "How much the built-in blocks weigh against the prefabs\n"
                           "(0 = only prefabs, for the kinds that have one).");
            const std::vector<std::string> names =
                s.prefabNames ? s.prefabNames() : std::vector<std::string>{};
            static const char* kKinds[] = {"Car", "Bus", "Lorry"};
            static const char* kNose[]  = {"Nose +Z", "Nose -Z", "Nose +X", "Nose -X"};
            int remove = -1;
            for (int i = 0; i < static_cast<int>(r.vehiclePrefabs.size()); ++i) {
                cityplan::VehiclePrefab& vp = r.vehiclePrefabs[static_cast<std::size_t>(i)];
                ImGui::PushID(i);
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 40.0f);
                if (ImGui::BeginCombo("##prefab", vp.prefab.empty() ? "(pick a prefab)" : vp.prefab.c_str())) {
                    for (const std::string& n : names)
                        if (ImGui::Selectable(n.c_str(), n == vp.prefab)) { vp.prefab = n; changed = true; }
                    if (names.empty()) ImGui::TextDisabled("No prefabs in this project (save one first)");
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::Button("X", ImVec2(32.0f, 0.0f))) remove = i;
                ImGui::SetItemTooltip("Remove this prefab from the traffic.");
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
                if (ImGui::Combo("##kind", &vp.kind, kKinds, 3)) changed = true;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
                if (ImGui::Combo("##nose", &vp.forward, kNose, 4)) changed = true;
                ImGui::SetItemTooltip("Which way the prefab's front faces in its own frame.");
                changed |= row("Weight", vp.weight, 0.5f, 0.0f, 50.0f, "%.1f");
                ImGui::Separator();
                ImGui::PopID();
            }
            if (remove >= 0) {
                r.vehiclePrefabs.erase(r.vehiclePrefabs.begin() + remove);
                changed = true;
            }
            if (ImGui::Button("Add vehicle prefab")) {
                r.vehiclePrefabs.push_back({});
                changed = true;
            }
            ImGui::TreePop();
        }
        // Prefabs the people walk as instead of the built-in figures.
        if (ImGui::TreeNode("People prefabs")) {
            ui::hint("Each person is one of these or a figure, by weight.\n"
                     "An animated model walks its Animation clip (the\n"
                     "prefab's own clip and range), in step with the ground.\n"
                     "The nearest people show as their prefab, the rest\n"
                     "as figures.");
            changed |= row("Figures", r.personPlaceholderWeight, 0.5f, 0.0f, 50.0f, "%.1f",
                           "How much the built-in figures weigh against the prefabs\n"
                           "(0 = only prefabs).");
            const std::vector<std::string> names =
                s.prefabNames ? s.prefabNames() : std::vector<std::string>{};
            static const char* kFaces[] = {"Faces +Z", "Faces -Z", "Faces +X", "Faces -X"};
            int remove = -1;
            for (int i = 0; i < static_cast<int>(r.personPrefabs.size()); ++i) {
                cityplan::PersonPrefab& pp = r.personPrefabs[static_cast<std::size_t>(i)];
                ImGui::PushID(1000 + i);
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 40.0f);
                if (ImGui::BeginCombo("##prefab", pp.prefab.empty() ? "(pick a prefab)" : pp.prefab.c_str())) {
                    for (const std::string& n : names)
                        if (ImGui::Selectable(n.c_str(), n == pp.prefab)) { pp.prefab = n; changed = true; }
                    if (names.empty()) ImGui::TextDisabled("No prefabs in this project (save one first)");
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::Button("X", ImVec2(32.0f, 0.0f))) remove = i;
                ImGui::SetItemTooltip("Remove this prefab from the people.");
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
                if (ImGui::Combo("##faces", &pp.forward, kFaces, 4)) changed = true;
                ImGui::SetItemTooltip("Which way the person faces in the prefab's own frame.");
                changed |= row("Weight", pp.weight, 0.5f, 0.0f, 50.0f, "%.1f");
                ImGui::Separator();
                ImGui::PopID();
            }
            if (remove >= 0) {
                r.personPrefabs.erase(r.personPrefabs.begin() + remove);
                changed = true;
            }
            if (ImGui::Button("Add person prefab")) {
                r.personPrefabs.push_back({});
                changed = true;
            }
            ImGui::TreePop();
        }
        // Street lamps: prefabs along the pavements, lit at dusk.
        if (ImGui::TreeNode("Street lamps")) {
            ui::hint("Lamps stand on the pavement beside the kerb, facing the\n"
                     "street: residential streets lit from one side, avenues\n"
                     "from both. Each lamp is one of these prefabs, by weight.\n"
                     "Its root's pivot is the foot; give it a Light component\n"
                     "at the head -- light and glowing glass come on at dusk.\n"
                     "Needs pavements.");
            changed |= row("Lamp every", r.lampEvery, 2.0f, 10.0f, 120.0f, "%.0f m",
                           "Spacing along each side of a block.");
            changed |= row("From the kerb", r.lampInset, 0.1f, 0.2f, 3.0f, "%.1f m",
                           "How far in from the kerb the prefab's pivot stands.");
            if (ImGui::Checkbox("Both sides of every street", &r.lampBothSides)) changed = true;
            ImGui::SetItemTooltip("Off: one side of an ordinary street, both of an avenue.\n"
                                  "On: both sides everywhere, staggered.");
            const std::vector<std::string> names =
                s.prefabNames ? s.prefabNames() : std::vector<std::string>{};
            static const char* kHead[] = {"Head +Z", "Head -Z", "Head +X", "Head -X"};
            int remove = -1;
            for (int i = 0; i < static_cast<int>(r.lampPrefabs.size()); ++i) {
                cityplan::LampPrefab& lp = r.lampPrefabs[static_cast<std::size_t>(i)];
                ImGui::PushID(2000 + i);
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 40.0f);
                if (ImGui::BeginCombo("##prefab", lp.prefab.empty() ? "(pick a prefab)" : lp.prefab.c_str())) {
                    for (const std::string& n : names)
                        if (ImGui::Selectable(n.c_str(), n == lp.prefab)) { lp.prefab = n; changed = true; }
                    if (names.empty()) ImGui::TextDisabled("No prefabs in this project (save one first)");
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::Button("X", ImVec2(32.0f, 0.0f))) remove = i;
                ImGui::SetItemTooltip("Remove this prefab from the lamps.");
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
                if (ImGui::Combo("##head", &lp.forward, kHead, 4)) changed = true;
                ImGui::SetItemTooltip("Which way the lamp's head (its arm, the lantern's\n"
                                      "front) points in the prefab's own frame. The town\n"
                                      "turns that side to the street.");
                changed |= row("Weight", lp.weight, 0.5f, 0.0f, 50.0f, "%.1f");
                ImGui::Separator();
                ImGui::PopID();
            }
            if (remove >= 0) {
                r.lampPrefabs.erase(r.lampPrefabs.begin() + remove);
                changed = true;
            }
            if (ImGui::Button("Add lamp prefab")) {
                r.lampPrefabs.push_back({});
                changed = true;
            }
            if (!r.pavements)
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                                   "Pavements are off: no lamps without them.");
            ImGui::TreePop();
        }
        changed |= rowInt("Tower palette", r.towerPalette, 1, 0, 7, "%.0f",
                          "Building material slot (0 = A .. 7 = H). Keep it apart\n"
                          "from the roadside city's slots to colour them separately.");
        changed |= rowInt("Block palette", r.blockPalette, 1, 0, 7, "%.0f");
    }

    // --- What came out -----------------------------------------------------------
    if (built) {
        const cityplan::Stats& st = built->town.stats;
        ImGui::Separator();
        ImGui::Text("%d buildings: %d towers, %d blocks, %d terraced, %d houses; %d parks",
                    st.built, st.towers, st.blocks, st.rows, st.houses, st.parks);
        std::string pub;
        for (int k = 0; k < civic::kKinds; ++k) {
            const int n = st.civic[static_cast<std::size_t>(k)];
            if (n <= 0) continue;
            if (!pub.empty()) pub += ", ";
            pub += std::to_string(n) + " " + civic::kindName(static_cast<civic::Kind>(k));
        }
        ImGui::TextWrapped("Public: %s", pub.empty() ? "none" : pub.c_str());
        ImGui::Text("%d street signs, %d crossings with traffic lights, %d bus stops, %d lamps",
                    st.signs, st.lights, st.busStops, st.lamps);
        ImGui::TextDisabled("Skipped: %d road, %d water, %d slope, %d empty plots%s",
                            st.skippedRoad, st.skippedWater, st.skippedSlope, st.skippedEmpty,
                            st.budgetHit ? "  -- BUDGET HIT" : "");
        ImGui::TextDisabled("%zu draws, %.1f MB, %.0f ms", built->town.district.batches.size(),
                            built->town.district.verts * 64.0 / 1048576.0, built->ms);
    }
    if (!s.status.empty()) ui::hint("%s", s.status.c_str());

    if (changed) {
        s.editing = true;
        cs.markDirty(s.sel);
    }
    // One undo step per gesture: committed when the hand comes off.
    if (s.editing && !ImGui::IsAnyItemActive()) flush();
    ImGui::End();
}

} // namespace citygenui
