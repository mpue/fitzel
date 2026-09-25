#include "HousePanel.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <imgui.h>

#include "UiStyle.hpp"

namespace houseui {

namespace {

using housegen::LevelPlan;
using housegen::Opening;
using housegen::Plan;
using housegen::RoomSpec;
using housegen::RoomType;

constexpr int   kTypeCount = static_cast<int>(RoomType::Count);
constexpr float kPi        = 3.14159265358979f;
#define M2 " m\xC2\xB2"   // " m^2" in UTF-8

// A room-type combo driven by the module's own name function, so a type added
// in HouseGen shows up here with no second list to maintain.
bool typeCombo(const char* id, RoomType& t) {
    const char* names[kTypeCount];
    for (int i = 0; i < kTypeCount; ++i) names[i] = housegen::roomTypeName(static_cast<RoomType>(i));
    int cur = static_cast<int>(t);
    if (!ImGui::Combo(id, &cur, names, kTypeCount)) return false;
    t = static_cast<RoomType>(cur);
    return true;
}

// A measurement: typed, or stepped with the -/+ buttons -- never dragged (see
// the editor's rule in Gui.cpp). Clamped here so a typed 0 cannot reach the
// generator; the generator clamps again anyway.
bool num(const char* label, float& v, float step, float lo, float hi, const char* fmt) {
    const bool changed = ImGui::InputFloat(label, &v, step, step * 4.0f, fmt);
    v = std::clamp(v, lo, hi);
    return changed;
}

// Plan colours per room type: warm for living, cool for wet, pale for sleeping,
// grey for storage -- the convention of every coloured plan in a sales brochure.
ImU32 roomColor(RoomType t, bool hot) {
    ImVec4 c;
    switch (t) {
        case RoomType::Wohnen: case RoomType::Essen: c = {0.96f, 0.85f, 0.62f, 1}; break;
        case RoomType::Kueche:                        c = {0.97f, 0.76f, 0.55f, 1}; break;
        case RoomType::Diele:                         c = {0.86f, 0.86f, 0.83f, 1}; break;
        case RoomType::Bad: case RoomType::GaesteBad: c = {0.66f, 0.83f, 0.95f, 1}; break;
        case RoomType::Schlafen:                      c = {0.80f, 0.76f, 0.95f, 1}; break;
        case RoomType::Kind:                          c = {0.74f, 0.91f, 0.73f, 1}; break;
        case RoomType::Buero:                         c = {0.95f, 0.78f, 0.82f, 1}; break;
        case RoomType::Gast:                          c = {0.82f, 0.83f, 0.96f, 1}; break;
        default:                                      c = {0.78f, 0.78f, 0.75f, 1}; break;
    }
    if (hot) c = {c.x * 0.85f + 0.15f, c.y * 0.85f + 0.15f, c.z * 0.85f + 0.15f, 1};
    return ImGui::ColorConvertFloat4ToU32(c);
}

std::vector<RoomSpec>* programmeOf(housegen::Params& c, int level) {
    if (level < c.storeys) return &c.storeyRooms[level];
    if (level == c.storeys && c.attic) return &c.atticRooms;
    return nullptr;
}

// --- The plan preview ---------------------------------------------------------

void drawPlanView(const Plan& plan, int li) {
    const LevelPlan& lv = plan.levels[li];
    const housegen::Params& p = plan.params;
    const float W = p.width, D = p.depth;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float m  = 26.0f;
    const float sc = std::max(6.0f, (avail - 2.0f * m) / W);
    const ImVec2 size(avail, D * sc + 2.0f * m + 6.0f);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##housePlan", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto P = [&](float x, float y) { return ImVec2(o.x + m + x * sc, o.y + m + y * sc); };
    auto rectOf = [&](const housegen::Rect& r, ImU32 col) {
        dl->AddRectFilled(P(r.x0, r.y0), P(r.x1, r.y1), col);
    };
    const ImU32 ink   = IM_COL32(30, 32, 36, 255);
    const ImU32 wall  = IM_COL32(58, 62, 68, 255);
    const ImU32 glass = IM_COL32(150, 205, 240, 255);
    const ImU32 muted = IM_COL32(150, 156, 164, 255);
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y),
                      ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
    dl->AddRectFilled(P(0, 0), P(W, D), wall);

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int hot = -1;
    for (int i = 0; i < static_cast<int>(lv.rooms.size()); ++i) {
        const auto& r = lv.rooms[i].r;
        const ImVec2 a = P(r.x0, r.y0), b = P(r.x1, r.y1);
        if (hovered && mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y) hot = i;
    }
    for (int i = 0; i < static_cast<int>(lv.rooms.size()); ++i)
        rectOf(lv.rooms[i].r, roomColor(lv.rooms[i].type, i == hot));

    // Attic: where the clear height is under 2 m (counts half), shaded.
    if (lv.attic && !lv.loft) {
        const float z0 = lv.z;
        for (int side = 0; side < 2; ++side) {
            float x = side == 0 ? p.outerWall : W - p.outerWall;
            const float dir = side == 0 ? 1.0f : -1.0f;
            while (std::abs(x - 0.5f * W) > 0.05f &&
                   housegen::roofUnderside(plan, x) - z0 < 2.0f) x += dir * 0.05f;
            const float xa = side == 0 ? p.outerWall : x, xb = side == 0 ? x : W - p.outerWall;
            if (xb - xa < 0.05f) continue;
            dl->AddRectFilled(P(xa, p.outerWall), P(xb, D - p.outerWall), IM_COL32(40, 60, 110, 55));
            dl->AddLine(P(side == 0 ? xb : xa, p.outerWall), P(side == 0 ? xb : xa, D - p.outerWall),
                        IM_COL32(40, 60, 140, 200), 1.0f);
        }
    }

    // Stair: treads, the eye between the flights, the walking line up.
    if (plan.hasStair && (lv.stairUp || lv.stairDown)) {
        const auto& s = plan.stair;
        const ImU32 floorC = roomColor(RoomType::Diele, false);
        rectOf(s.r, floorC);
        dl->AddRect(P(s.r.x0, s.r.y0), P(s.r.x1, s.r.y1), ink, 0.0f, 0, 1.2f);
        for (int i = 1; i <= 8; ++i) {
            const float x = s.r.x0 + static_cast<float>(i) * s.tread;
            dl->AddLine(P(x, s.r.y0), P(x, s.r.y0 + s.flight), ink, 0.8f);
            dl->AddLine(P(x, s.r.y1 - s.flight), P(x, s.r.y1), ink, 0.8f);
        }
        dl->AddRectFilled(P(s.r.x0, s.r.y0 + s.flight), P(s.landingX, s.r.y1 - s.flight), wall);
        const float yS = s.r.y1 - 0.5f * s.flight, yN = s.r.y0 + 0.5f * s.flight;
        const float xl = 0.5f * (s.landingX + s.r.x1);
        const ImVec2 pts[4] = {P(s.r.x0 + 0.12f, yS), P(xl, yS), P(xl, yN), P(s.r.x0 + 0.10f, yN)};
        dl->AddPolyline(pts, 4, ink, ImDrawFlags_None, 1.0f);
        dl->AddCircle(pts[0], 3.0f, ink);
        const ImVec2 tip = pts[3];
        dl->AddTriangleFilled(tip, ImVec2(tip.x + 7, tip.y - 4), ImVec2(tip.x + 7, tip.y + 4), ink);
    }

    // Openings: windows as glass in the wall, doors as a gap with the leaf and
    // its swing, the pass without either.
    for (const Opening& op : lv.openings) {
        const bool glazed = op.kind == Opening::Kind::Window || op.kind == Opening::Kind::FrenchDoor;
        if (glazed) {
            rectOf(op.r, glass);
            if (op.alongX) {
                const float y = 0.5f * (op.r.y0 + op.r.y1);
                dl->AddLine(P(op.r.x0, y), P(op.r.x1, y), ink, 1.0f);
            } else {
                const float x = 0.5f * (op.r.x0 + op.r.x1);
                dl->AddLine(P(x, op.r.y0), P(x, op.r.y1), ink, 1.0f);
            }
            continue;
        }
        rectOf(op.r, roomColor(RoomType::Diele, false));
        if (!op.hasLeaf) continue;
        const ImVec2 h = P(op.hinge.x, op.hinge.y), e = P(op.open.x, op.open.y),
                     c = P(op.close.x, op.close.y);
        dl->AddLine(h, e, ink, 2.0f);
        float a0 = std::atan2(e.y - h.y, e.x - h.x), a1 = std::atan2(c.y - h.y, c.x - h.x);
        if (a1 - a0 > kPi) a1 -= 2.0f * kPi;
        if (a1 - a0 < -kPi) a1 += 2.0f * kPi;
        const float rad = std::hypot(e.x - h.x, e.y - h.y);
        dl->PathArcTo(h, rad, a0, a1, 12);
        dl->PathStroke(ink, ImDrawFlags_None, 0.8f);
    }
    for (const auto& rw : lv.roofWindows)
        dl->AddRect(P(rw.r.x0, rw.r.y0), P(rw.r.x1, rw.r.y1), IM_COL32(40, 90, 170, 255), 0.0f, 0, 1.5f);

    // Room stamps: name, and the living area under it -- whichever fits.
    for (int i = 0; i < static_cast<int>(lv.rooms.size()); ++i) {
        const auto& r = lv.rooms[i];
        const ImVec2 a = P(r.r.x0, r.r.y0), b = P(r.r.x1, r.r.y1);
        ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        if (r.hall && plan.hasStair && (lv.stairUp || lv.stairDown))   // off the stair
            c.y = (P(0, plan.stair.r.y1).y + b.y) * 0.5f;
        char area[32];
        std::snprintf(area, sizeof area, "%.1f" M2, r.livingArea);
        const ImVec2 ts = ImGui::CalcTextSize(r.name.c_str()), as = ImGui::CalcTextSize(area);
        const float room = b.x - a.x - 4.0f;
        const float lh = ImGui::GetTextLineHeight();
        if (ts.x <= room) {
            dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - lh), ink, r.name.c_str());
            if (as.x <= room) dl->AddText(ImVec2(c.x - as.x * 0.5f, c.y), IM_COL32(70, 72, 78, 255), area);
        } else if (as.x <= room) {
            dl->AddText(ImVec2(c.x - as.x * 0.5f, c.y - lh * 0.5f), ink, area);
        }
    }

    // North arrow and the two overall dimensions.
    const ImVec2 na(o.x + size.x - 16.0f, o.y + 20.0f);
    dl->AddTriangleFilled(ImVec2(na.x, na.y - 9), ImVec2(na.x + 5, na.y + 5), ImVec2(na.x - 5, na.y + 5), muted);
    dl->AddText(ImVec2(na.x - 4, na.y + 6), muted, "N");
    char dim[32];
    std::snprintf(dim, sizeof dim, "%.2f m", W);
    const ImVec2 ds = ImGui::CalcTextSize(dim);
    dl->AddText(ImVec2(P(0.5f * W, 0).x - ds.x * 0.5f, o.y + 4.0f), muted, dim);
    std::snprintf(dim, sizeof dim, "%.2f m", D);
    dl->AddText(ImVec2(o.x + 2.0f, P(0, 0.5f * D).y - 7.0f), muted, dim);

    if (hot >= 0) {
        const auto& r = lv.rooms[hot];
        dl->AddRect(P(r.r.x0, r.r.y0), P(r.r.x1, r.r.y1), IM_COL32(255, 255, 255, 255), 0, 0, 2.0f);
        ImGui::BeginTooltip();
        ui::title("%s", r.name.c_str());
        ImGui::Text("%s  \xC2\xB7  %.2f x %.2f m", housegen::roomTypeName(r.type), r.r.w(), r.r.h());
        ImGui::Text("Floor %.2f" M2 ", living area %.2f" M2, r.floorArea, r.livingArea);
        if (r.target > 0.0f) ImGui::Text("Asked for %.1f" M2, r.target);
        if (r.daylight > 0.0f)
            ImGui::Text("Windows %.2f" M2 " (1/%.1f of the floor)", r.daylight,
                        r.floorArea / std::max(0.01f, r.daylight));
        if (r.autoAdded) ImGui::TextDisabled("Added to fill the plan.");
        ImGui::EndTooltip();
    }
}

} // namespace

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    ImGui::SetNextWindowSize(ImVec2(440.0f, 760.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Houses", &s.show)) {
        housegen::Params& c = s.cfg;
        bool changed = false;

        ui::hint("A single-family house from a room programme: list the rooms per "
                 "storey, the plan lays itself out, Generate builds it to walk through.");

        ui::sectionText("Preset");
        {
            // Three to a row: big targets, one click each, a whole new house.
            const int n = static_cast<int>(housegen::Preset::Count);
            const int cols = 3;
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float w = (ImGui::GetContentRegionAvail().x - gap * static_cast<float>(cols - 1)) /
                            static_cast<float>(cols);
            for (int i = 0; i < n; ++i) {
                if (i % cols) ImGui::SameLine();
                const auto pr = static_cast<housegen::Preset>(i);
                if (ImGui::Button(housegen::presetName(pr), ImVec2(w, 28.0f))) {
                    housegen::applyPreset(c, pr);
                    s.level = 0;
                    changed = true;
                }
            }
        }

        if (ui::header("Building", ImGuiTreeNodeFlags_DefaultOpen)) {
            changed |= num("Width (E-W)", c.width, 0.25f, 8.0f, 24.0f, "%.2f m");
            changed |= num("Depth (N-S)", c.depth, 0.25f, 7.0f, 18.0f, "%.2f m");
            if (ImGui::InputInt("Full storeys", &c.storeys)) {
                c.storeys = std::clamp(c.storeys, 1, housegen::kMaxStoreys);
                changed = true;
            }
            changed |= ImGui::Checkbox("Attic fitted out", &c.attic);
            ImGui::SetItemTooltip("Rooms under the roof (DG). Off: an unheated loft.");
            changed |= num("Storey height", c.storeyHeight, 0.05f, 2.55f, 3.40f, "%.2f m");
            changed |= num("Clear height", c.clearHeight, 0.05f, 2.25f, 3.20f, "%.2f m");
            changed |= num("Service band", c.serviceDepth, 0.05f, 3.10f, 6.0f, "%.2f m");
            ImGui::SetItemTooltip("Clear depth of the north band: hall with the stair, "
                                  "kitchen, baths, storage.");
            changed |= num("Roof pitch", c.roofPitch, 1.0f, 20.0f, 55.0f, "%.0f\xC2\xB0");
            changed |= num("Knee wall", c.kneeWall, 0.05f, 0.0f, 2.0f, "%.2f m");
            ImGui::SetItemTooltip("Kniestock: height of the attic wall under the eaves.");
            changed |= num("Eave overhang", c.eaveOverhang, 0.05f, 0.0f, 1.2f, "%.2f m");
            changed |= num("Plinth", c.plinth, 0.05f, 0.0f, 1.0f, "%.2f m");
            changed |= ImGui::Checkbox("Terrace", &c.terrace);
        }

        // One layout per frame: a handful of rectangles, cheap enough to be the
        // live answer to every keystroke above.
        const Plan plan = housegen::layout(c);
        const int nLv = static_cast<int>(plan.levels.size());
        s.level = std::clamp(s.level, 0, nLv - 1);

        ui::sectionText("Storeys");
        if (ImGui::BeginTabBar("##houseLevels")) {
            for (int i = 0; i < nLv; ++i) {
                char lbl[48];
                std::snprintf(lbl, sizeof lbl, "%s%s###lv%d", plan.levels[i].label.c_str(),
                              plan.levels[i].loft ? " (loft)" : "", i);
                if (ImGui::BeginTabItem(lbl)) { s.level = i; ImGui::EndTabItem(); }
            }
            ImGui::EndTabBar();
        }

        // --- Room programme of the selected storey.
        if (std::vector<RoomSpec>* rooms = programmeOf(c, s.level)) {
            int moveFrom = -1, moveTo = -1, remove = -1;
            if (ImGui::BeginTable("##rooms", 4, ImGuiTableFlags_SizingStretchProp |
                                                    ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 1.1f);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                ImGui::TableSetupColumn("Area", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 78.0f);
                ImGui::TableHeadersRow();
                for (int i = 0; i < static_cast<int>(rooms->size()); ++i) {
                    RoomSpec& r = (*rooms)[i];
                    ImGui::PushID(i);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    changed |= typeCombo("##type", r.type);
                    ImGui::TableNextColumn();
                    char buf[64];
                    std::snprintf(buf, sizeof buf, "%s", r.name.c_str());
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputTextWithHint("##name", housegen::roomTypeName(r.type), buf, sizeof buf)) {
                        r.name = buf;
                        changed = true;
                    }
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (r.type == RoomType::Diele) ImGui::TextDisabled("(fills the rest)");
                    else changed |= num("##area", r.area, 0.5f, 1.5f, 150.0f, "%.1f" M2);
                    ImGui::TableNextColumn();
                    if (ImGui::ArrowButton("##up", ImGuiDir_Up) && i > 0) { moveFrom = i; moveTo = i - 1; }
                    ImGui::SameLine(0, 2);
                    if (ImGui::ArrowButton("##dn", ImGuiDir_Down) && i + 1 < static_cast<int>(rooms->size())) {
                        moveFrom = i; moveTo = i + 1;
                    }
                    ImGui::SameLine(0, 2);
                    if (ImGui::Button("x")) remove = i;
                    ImGui::SetItemTooltip("Remove this room");
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (moveFrom >= 0) { std::swap((*rooms)[moveFrom], (*rooms)[moveTo]); changed = true; }
            if (remove >= 0) { rooms->erase(rooms->begin() + remove); changed = true; }
            if (ImGui::Button("+ Add room", ImVec2(-FLT_MIN, 0.0f))) ImGui::OpenPopup("##addRoom");
            if (ImGui::BeginPopup("##addRoom")) {
                for (int t = 0; t < kTypeCount; ++t) {
                    const auto rt = static_cast<RoomType>(t);
                    if (ImGui::Selectable(housegen::roomTypeName(rt))) {
                        rooms->push_back({rt, "", housegen::defaultArea(rt)});
                        changed = true;
                    }
                }
                ImGui::EndPopup();
            }
            ui::hint("Main rooms line the south facade west to east in this order; "
                     "kitchen, baths and storage go to the north band beside the stair. "
                     "Areas are weights -- the plan shares out what the house has.");
        } else {
            ImGui::TextDisabled("Loft -- tick \"Attic fitted out\" to plan rooms here.");
        }

        // --- Plan preview + figures of the selected storey.
        ui::sectionText("Plan");
        drawPlanView(plan, s.level);
        {
            const LevelPlan& lv = plan.levels[s.level];
            if (ImGui::BeginTable("##areas", 4, ImGuiTableFlags_SizingStretchProp |
                                                    ImGuiTableFlags_BordersInnerH)) {
                ImGui::TableSetupColumn("Room");
                ImGui::TableSetupColumn("Floor");
                ImGui::TableSetupColumn("Living");
                ImGui::TableSetupColumn("Windows");
                ImGui::TableHeadersRow();
                for (const auto& r : lv.rooms) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (r.autoAdded) ImGui::TextDisabled("%s", r.name.c_str());
                    else             ImGui::TextUnformatted(r.name.c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%.2f", r.floorArea);
                    ImGui::TableNextColumn(); ImGui::Text("%.2f", r.livingArea);
                    ImGui::TableNextColumn();
                    if (housegen::isHabitable(r.type)) {
                        const bool ok = r.daylight >= r.floorArea / 8.0f - 0.01f;
                        ImGui::TextColored(ok ? ImVec4(0.55f, 0.85f, 0.55f, 1) : ImVec4(1.0f, 0.65f, 0.3f, 1),
                                           "1/%.1f", r.floorArea / std::max(0.01f, r.daylight));
                    } else {
                        ImGui::TextDisabled("-");
                    }
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ui::title("%s", lv.label.c_str());
                ImGui::TableNextColumn();
                ImGui::TableNextColumn(); ui::title("%.2f", lv.living);
                ImGui::EndTable();
            }
            ui::hint("Living area per WoFlV: stairs deducted, attic floor under 2 m "
                     "clear height at half. Windows: opening per floor area, 1/8 required.");
        }
        ui::title("Living area %.1f" M2, plan.living);
        ui::hint("Gross floor %.0f" M2 "  \xC2\xB7  ridge %.2f m, eaves %.2f m above ground",
                 plan.grossFloor, plan.ridgeZ, plan.eaveZ);
        for (int i = 0; i < static_cast<int>(plan.notes.size()); ++i) {
            if (i < plan.warnings)
                ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.30f, 1.0f), "! %s", plan.notes[i].c_str());
            else
                ui::hint("%s", plan.notes[i].c_str());
        }

        if (ui::header("View")) {
            changed |= ImGui::Checkbox("Roof", &c.roof);
            const char* upTo[housegen::kMaxStoreys + 1];
            for (int i = 0; i < nLv; ++i) upTo[i] = plan.levels[i].label.c_str();
            int cut = std::min(c.cutLevel, nLv - 1);
            if (ImGui::Combo("Build up to", &cut, upTo, nLv)) {
                c.cutLevel = cut >= nLv - 1 ? housegen::kMaxStoreys : cut;
                changed = true;
            }
            ImGui::SetItemTooltip("Leave the storeys above out to look into the house "
                                  "from above.");
            changed |= ImGui::Checkbox("Collider", &c.collider);
        }
        if (ui::header("Look")) {
            changed |= ImGui::ColorEdit3("Facade", &c.facadeColor.x);
            changed |= ImGui::ColorEdit3("Plinth", &c.plinthColor.x);
            changed |= ImGui::ColorEdit3("Roof tiles", &c.roofColor.x);
            changed |= ImGui::ColorEdit3("Frames", &c.frameColor.x);
            changed |= ImGui::ColorEdit3("Floors", &c.floorColor.x);
            ui::hint("Shared by every generated house in the project.");
        }

        // --- Actions ------------------------------------------------------
        ImGui::Separator();
        if (ImGui::Button("Generate house", ImVec2(-FLT_MIN, 34.0f))) s.generate();
        ImGui::SetItemTooltip("Places a new house on the ground in front of the camera "
                              "(one undo step).");
        ImGui::BeginDisabled(!s.hasLive);
        if (ImGui::Button("Rebuild in place", ImVec2(-FLT_MIN, 0.0f))) s.rebuild();
        ImGui::EndDisabled();
        ImGui::Checkbox("Live update", &s.autoRebuild);
        ImGui::SetItemTooltip("Re-generate the last house whenever a setting changes.");
        ImGui::BeginDisabled(!s.selectionIsHouse);
        if (ImGui::Button("Edit selected house", ImVec2(-FLT_MIN, 0.0f))) {
            s.loadSelected();
            s.level = 0;
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Load the parameters of the selected house into this panel "
                              "and make it the one Rebuild acts on.");

        ui::sectionText("Save as prefab");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##houseName", "Name", s.nameBuf, s.nameBufSize);
        ImGui::BeginDisabled(!s.hasLive || !s.hasProject || s.nameBuf[0] == '\0');
        if (ImGui::Button("Save as prefab", ImVec2(-FLT_MIN, 0.0f))) s.saveAsPrefab();
        ImGui::EndDisabled();
        if (!s.hasProject)
            ImGui::TextDisabled("Open a project first (prefabs live in it).");
        if (!s.status.empty()) ImGui::TextWrapped("%s", s.status.c_str());

        // An edit is applied once the widget is released: typing a width would
        // otherwise rebuild (and push an undo step) on every keystroke.
        if (changed) s.pendingRebuild = true;
        if (s.pendingRebuild && !ImGui::IsAnyItemActive()) {
            s.pendingRebuild = false;
            if (s.autoRebuild && s.hasLive) s.rebuild();
        }
    }
    ImGui::End();
}

} // namespace houseui
