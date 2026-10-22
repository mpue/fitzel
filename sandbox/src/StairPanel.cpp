#include "StairPanel.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include <imgui.h>

#include "UiStyle.hpp"

namespace stairui {

namespace {

using stairgen::Kind;
using stairgen::Plan;
using stairgen::RailStyle;
using stairgen::Sides;
using stairgen::Structure;
using stairgen::Tread;

#define DEG "\xC2\xB0"

// A measurement: typed, or stepped with the -/+ buttons -- never dragged (the
// editor's rule, see Gui.cpp). Clamped here so a typed 0 cannot reach the
// generator; the generator clamps again anyway.
bool num(const char* label, float& v, float step, float lo, float hi, const char* fmt) {
    const bool changed = ImGui::InputFloat(label, &v, step, step * 4.0f, fmt);
    v = std::clamp(v, lo, hi);
    return changed;
}

bool count(const char* label, int& v, int lo, int hi) {
    const bool changed = ImGui::InputInt(label, &v);
    v = std::clamp(v, lo, hi);
    return changed;
}

// A row of buttons, one per choice, the chosen one lit: every option in view
// and one click away, where a combo would hide them behind a second click.
bool choice(const char* id, int& cur, const char* const* labels, int n, int perRow,
            const char* const* tips = nullptr) {
    bool changed = false;
    ImGui::PushID(id);
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const int cols = std::min(n, perRow);
    const float w = (ImGui::GetContentRegionAvail().x - gap * static_cast<float>(cols - 1)) /
                    static_cast<float>(cols);
    for (int i = 0; i < n; ++i) {
        if (i % perRow) ImGui::SameLine();
        const bool on = i == cur;
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        ImGui::PushID(i);
        if (ImGui::Button(labels[i], ImVec2(w, 0.0f)) && !on) {
            cur = i;
            changed = true;
        }
        ImGui::PopID();
        if (on) ImGui::PopStyleColor(2);
        if (tips && tips[i] && tips[i][0]) ImGui::SetItemTooltip("%s", tips[i]);
    }
    ImGui::PopID();
    return changed;
}

template <class E>
bool enumChoice(const char* id, E& v, const char* (*name)(E), int perRow) {
    const int n = static_cast<int>(E::Count);
    const char* labels[16];
    for (int i = 0; i < n && i < 16; ++i) labels[i] = name(static_cast<E>(i));
    int cur = static_cast<int>(v);
    if (!choice(id, cur, labels, n, perRow)) return false;
    v = static_cast<E>(cur);
    return true;
}

bool sidesChoice(const char* id, Sides& v, bool turning) {
    // A turning stair names its sides by the turn: the inner one is at the eye.
    const char* flat[]  = {"None", "Left", "Right", "Both"};
    const char* round[] = {"None", "Inner", "Outer", "Both"};
    int cur = static_cast<int>(v);
    if (!choice(id, cur, turning ? round : flat, 4, 4)) return false;
    v = static_cast<Sides>(cur);
    return true;
}

ImVec4 okColor(bool ok) {
    return ok ? ImVec4(0.55f, 0.85f, 0.55f, 1.0f) : ImVec4(1.0f, 0.65f, 0.30f, 1.0f);
}

glm::vec2 centroid(const std::vector<glm::vec2>& poly) {
    glm::vec2 c(0.0f);
    for (const glm::vec2& q : poly) c += q;
    return c / static_cast<float>(std::max<std::size_t>(poly.size(), 1));
}

bool insideConvex(const std::vector<glm::vec2>& poly, const glm::vec2& p) {
    if (poly.size() < 3) return false;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const glm::vec2 a = poly[i], b = poly[(i + 1) % poly.size()];
        if ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x) < 0.0f) return false;
    }
    return true;
}

ImU32 treadColor(const Tread& t, float height) {
    if (t.arrival) return IM_COL32(196, 214, 190, 255);
    if (t.landing) return IM_COL32(214, 214, 206, 255);
    const float f = std::clamp(t.top / std::max(height, 0.01f), 0.0f, 1.0f);
    const ImVec4 lo(0.95f, 0.91f, 0.83f, 1.0f), hi(0.74f, 0.63f, 0.50f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(lo.x + (hi.x - lo.x) * f, lo.y + (hi.y - lo.y) * f,
                                                 lo.z + (hi.z - lo.z) * f, 1.0f));
}

// --- The plan view --------------------------------------------------------------
//
// Seen from above, the first flight going up the screen: treads shaded by
// height, numbered, the walking line with its arrow, railings blue, walls
// dark, and a tooltip for the tread under the mouse.
void drawPlanView(const Plan& plan) {
    const stairgen::Params& p = plan.params;
    const float pad = std::max(p.wallThickness, 0.1f) + 0.15f;
    glm::vec2 mn = plan.bbMin - glm::vec2(pad), mx = plan.bbMax + glm::vec2(pad);
    const float bw = std::max(mx.x - mn.x, 0.5f), bh = std::max(mx.y - mn.y, 0.5f);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float m = 18.0f;
    const float sc = std::min((avail - 2.0f * m) / bw, 300.0f / bh);
    const ImVec2 size(avail, bh * sc + 2.0f * m);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##stairPlan", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x0 = o.x + 0.5f * (size.x - bw * sc);
    auto S = [&](const glm::vec2& q) {
        return ImVec2(x0 + (q.x - mn.x) * sc, o.y + m + (mx.y - q.y) * sc);
    };
    auto fromS = [&](const ImVec2& s) {
        return glm::vec2((s.x - x0) / sc + mn.x, mx.y - (s.y - o.y - m) / sc);
    };
    const ImU32 ink   = IM_COL32(40, 42, 46, 255);
    const ImU32 muted = IM_COL32(150, 156, 164, 255);
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), ImGui::GetColorU32(ImGuiCol_FrameBg),
                      4.0f);
    dl->PushClipRect(o, ImVec2(o.x + size.x, o.y + size.y), true);

    const glm::vec2 mouse = fromS(ImGui::GetIO().MousePos);
    int hot = -1;
    const int nT = static_cast<int>(plan.treads.size());
    std::vector<std::vector<glm::vec2>> polys(nT);
    for (int i = 0; i < nT; ++i) {
        polys[i] = plan.treads[i].polygon();
        if (hovered && insideConvex(polys[i], mouse)) hot = i;
    }

    // Walls under everything, as thick bands outside the edge.
    auto sideLines = [&](bool left, float off, float thick, ImU32 col) {
        for (const Tread& t : plan.treads) {
            const auto& pl = left ? t.left : t.right;
            for (std::size_t k = 0; k + 1 < pl.size(); ++k) {
                glm::vec2 d = pl[k + 1] - pl[k];
                if (glm::length(d) < 1e-4f) continue;
                d = glm::normalize(d);
                const glm::vec2 out = left ? glm::vec2(-d.y, d.x) : glm::vec2(d.y, -d.x);
                dl->AddLine(S(pl[k] + out * off), S(pl[k + 1] + out * off), col, thick);
            }
        }
    };
    const float wt = p.wallThickness;
    if (plan.wallLeft)  sideLines(true, 0.5f * wt, std::max(2.0f, wt * sc), IM_COL32(88, 92, 100, 255));
    if (plan.wallRight) sideLines(false, 0.5f * wt, std::max(2.0f, wt * sc), IM_COL32(88, 92, 100, 255));

    for (int i = 0; i < nT; ++i) {
        std::vector<ImVec2> pts;
        for (const glm::vec2& q : polys[i]) pts.push_back(S(q));
        if (pts.size() < 3) continue;
        ImU32 col = treadColor(plan.treads[i], p.height);
        if (i == hot) col = IM_COL32(255, 236, 170, 255);
        dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), col);
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), ink, ImDrawFlags_Closed, 1.0f);
    }
    if (stairgen::isRound(p.kind) && p.column)
        dl->AddCircleFilled(S(plan.centre), p.innerRadius * sc, IM_COL32(70, 74, 80, 255), 24);

    // Railings: a blue line just inside the edge (or on a wall: the handrail).
    const ImU32 railC = IM_COL32(50, 110, 220, 255);
    if (plan.railLeft)  sideLines(true, -0.05f, 2.5f, railC);
    if (plan.railRight) sideLines(false, -0.05f, 2.5f, railC);

    // Riser numbers, where they fit.
    for (int i = 0; i < nT; ++i) {
        const Tread& t = plan.treads[i];
        char lbl[16];
        if (t.arrival)      std::snprintf(lbl, sizeof lbl, "top");
        else if (t.landing) std::snprintf(lbl, sizeof lbl, "L");
        else                std::snprintf(lbl, sizeof lbl, "%d", i + 1);
        // A spiral past one turn passes over itself: number the tread on top.
        const glm::vec2 mid = centroid(polys[i]);
        bool covered = false;
        for (int j = i + 1; j < nT && !covered; ++j) covered = insideConvex(polys[j], mid);
        if (covered) continue;
        const ImVec2 ts = ImGui::CalcTextSize(lbl);
        const ImVec2 c = S(mid);
        const float room = std::min(p.width, std::max(plan.going, 0.2f)) * sc;
        if (ts.y > room * 0.9f || ts.x > p.width * sc * 0.6f) continue;
        dl->AddText(ImVec2(c.x - 0.5f * ts.x, c.y - 0.5f * ts.y), IM_COL32(90, 80, 70, 255), lbl);
    }

    // Walking line: a dot at the foot, an arrow at the top.
    if (plan.walkLine.size() >= 2) {
        std::vector<ImVec2> pts;
        for (const glm::vec2& q : plan.walkLine) pts.push_back(S(q));
        const ImU32 wl = IM_COL32(205, 60, 40, 255);
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), wl, ImDrawFlags_None, 1.5f);
        dl->AddCircleFilled(pts.front(), 4.0f, wl);
        const ImVec2 a = pts[pts.size() - 2], b = pts.back();
        float dx = b.x - a.x, dy = b.y - a.y;
        const float l = std::sqrt(dx * dx + dy * dy);
        if (l > 1e-3f) {
            dx /= l; dy /= l;
            dl->AddTriangleFilled(ImVec2(b.x + dx * 6, b.y + dy * 6),
                                  ImVec2(b.x - dx * 4 - dy * 5, b.y - dy * 4 + dx * 5),
                                  ImVec2(b.x - dx * 4 + dy * 5, b.y - dy * 4 - dx * 5), wl);
        }
        dl->AddText(ImVec2(pts.front().x + 6, pts.front().y - 4), wl, "UP");
    }
    dl->PopClipRect();

    char dim[48];
    std::snprintf(dim, sizeof dim, "%.2f x %.2f m", plan.bbMax.x - plan.bbMin.x,
                  plan.bbMax.y - plan.bbMin.y);
    dl->AddText(ImVec2(o.x + 6.0f, o.y + 4.0f), muted, dim);

    if (hot >= 0) {
        const Tread& t = plan.treads[hot];
        ImGui::BeginTooltip();
        if (t.arrival)      ui::title("Top platform");
        else if (t.landing) ui::title("Landing");
        else                ui::title("Step %d of %d", hot + 1, plan.risers - 1);
        ImGui::Text("Walked on at %.3f m", t.top);
        ImGui::Text("%.2f m along the walking line", t.walkFront);
        if (!t.landing) ImGui::Text("Going %.1f cm here", (t.walkBack - t.walkFront) * 100.0f);
        ImGui::EndTooltip();
    }
}

// --- The side view ----------------------------------------------------------------
//
// The stair unrolled along its walking line: every tread at its height, the
// nosing line, the handrail, and a 1.80 m figure at the foot for scale.
void drawProfileView(const Plan& plan) {
    const stairgen::Params& p = plan.params;
    float sEnd = 0.0f;
    for (const Tread& t : plan.treads) sEnd = std::max(sEnd, t.walkBack);
    const float s0 = -0.8f, s1 = std::max(sEnd, 0.5f) + 0.3f;
    const bool rails = p.railSides != Sides::None;
    const float zTop = std::max(p.height + (rails ? p.railHeight : 0.0f), 1.9f) + 0.15f;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float m = 14.0f;
    const float sc = std::min((avail - 2.0f * m - 40.0f) / (s1 - s0), 170.0f / zTop);
    const ImVec2 size(avail, zTop * sc + 2.0f * m);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##stairProfile", size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto S = [&](float s, float z) {
        return ImVec2(o.x + m + (s - s0) * sc, o.y + size.y - m - z * sc);
    };
    const ImU32 ink   = IM_COL32(40, 42, 46, 255);
    const ImU32 muted = IM_COL32(150, 156, 164, 255);
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), ImGui::GetColorU32(ImGuiCol_FrameBg),
                      4.0f);
    dl->AddLine(S(s0, 0.0f), S(s1, 0.0f), muted, 1.0f);

    // Body of the stair: what is under each tread, by structure.
    const bool solid = p.structure == Structure::Solid;
    const float under = p.structure == Structure::Slab ? p.stringerDepth : p.treadThickness;
    for (const Tread& t : plan.treads) {
        const float zb = solid ? 0.0f : std::max(0.0f, t.top - (t.landing ? under : under + 0.5f * plan.rise));
        dl->AddRectFilled(S(t.walkFront, zb), S(t.walkBack, t.top), treadColor(t, p.height));
    }
    // Outline of the steps.
    std::vector<ImVec2> pts;
    pts.push_back(S(0.0f, 0.0f));
    for (const Tread& t : plan.treads) {
        pts.push_back(S(t.walkFront, t.top));
        pts.push_back(S(t.walkBack, t.top));
    }
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), ink, ImDrawFlags_None, 1.5f);
    // Nosing line and handrail.
    std::vector<ImVec2> nose, rail;
    for (const Tread& t : plan.treads) {
        nose.push_back(S(t.walkFront, t.top));
        rail.push_back(S(t.walkFront, t.top + p.railHeight));
        if (t.landing) {
            // Level across, climbing into the next flight on the last stretch --
            // the way the stringers and the handrail are built.
            const float lift = t.arrival ? 0.0f : plan.rise;
            const float s1 = std::max(t.walkFront, t.walkBack - plan.going);
            nose.push_back(S(s1, t.top));
            rail.push_back(S(s1, t.top + p.railHeight));
            nose.push_back(S(t.walkBack, t.top + lift));
            rail.push_back(S(t.walkBack, t.top + lift + p.railHeight));
        }
    }
    if (!plan.treads.empty() && !plan.treads.back().arrival) {
        nose.push_back(S(plan.treads.back().walkBack, p.height));
        rail.push_back(S(plan.treads.back().walkBack, p.height + p.railHeight));
    }
    // A landing's last point is the next step's first: drop the repeat, which
    // a polyline would draw as a stray mitre.
    auto unique = [](std::vector<ImVec2>& v) {
        v.erase(std::unique(v.begin(), v.end(),
                            [](const ImVec2& a, const ImVec2& b) {
                                return std::abs(a.x - b.x) < 0.5f && std::abs(a.y - b.y) < 0.5f;
                            }),
                v.end());
    };
    unique(nose);
    unique(rail);
    if (nose.size() >= 2)
        dl->AddPolyline(nose.data(), static_cast<int>(nose.size()), IM_COL32(205, 60, 40, 160),
                        ImDrawFlags_None, 1.0f);
    if (rails && rail.size() >= 2)
        dl->AddPolyline(rail.data(), static_cast<int>(rail.size()), IM_COL32(50, 110, 220, 255),
                        ImDrawFlags_None, 2.0f);

    // A person for scale, 1.80 m, at the foot.
    {
        const float s = -0.45f;
        const ImU32 fc = IM_COL32(110, 116, 126, 255);
        dl->AddCircle(S(s, 1.68f), 0.11f * sc, fc, 12, 1.5f);
        dl->AddLine(S(s, 1.56f), S(s, 0.88f), fc, 1.5f);
        dl->AddLine(S(s, 0.88f), S(s - 0.12f, 0.0f), fc, 1.5f);
        dl->AddLine(S(s, 0.88f), S(s + 0.12f, 0.0f), fc, 1.5f);
        dl->AddLine(S(s, 1.42f), S(s - 0.16f, 0.92f), fc, 1.5f);
        dl->AddLine(S(s, 1.42f), S(s + 0.16f, 0.92f), fc, 1.5f);
    }
    // Height dimension at the right.
    {
        const float s = s1 - 0.05f;
        dl->AddLine(S(s, 0.0f), S(s, p.height), muted, 1.0f);
        dl->AddLine(S(s - 0.08f, p.height), S(s + 0.08f, p.height), muted, 1.0f);
        char dim[32];
        std::snprintf(dim, sizeof dim, "%.2f m", p.height);
        const ImVec2 q = S(s, 0.5f * p.height);
        dl->AddText(ImVec2(q.x + 4.0f, q.y - 7.0f), muted, dim);
    }
}

// Where the turn (or the mid landing) comes: half way, or after a number of
// treads counted from the foot. 0 in the parameters means half way.
bool splitControl(stairgen::Params& c, const Plan& plan, const char* halfLabel) {
    bool half = c.splitStep == 0;
    bool changed = false;
    if (ImGui::Checkbox(halfLabel, &half)) {
        if (half) {
            c.splitStep = 0;
        } else {
            // Start from where it is now: the treads before the first landing
            // or winder.
            int k = 0;
            for (const Tread& t : plan.treads) {
                if (t.landing || t.left.size() == 1 || t.right.size() > 2) break;
                ++k;
            }
            c.splitStep = std::max(1, k);
        }
        changed = true;
    }
    if (!half) {
        changed |= count("Treads before", c.splitStep, 1, 79);
        ImGui::SetItemTooltip("Treads from the foot to the turn. Fewer puts the turn low.");
    }
    return changed;
}

void figures(const Plan& plan) {
    const stairgen::Params& p = plan.params;
    if (!ImGui::BeginTable("##stairFigures", 4, ImGuiTableFlags_SizingStretchProp)) return;
    auto cell = [](const char* k) {
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", k);
        ImGui::TableNextColumn();
    };
    cell("Risers");
    ImGui::Text("%d x %.1f cm", plan.risers, plan.rise * 100.0f);
    cell("Going");
    ImGui::TextColored(okColor(plan.going >= 0.23f), "%.1f cm", plan.going * 100.0f);
    cell("Pitch");
    ImGui::TextColored(okColor(plan.pitch <= 42.0f), "%.1f" DEG, plan.pitch);
    cell("2R + G");
    ImGui::TextColored(okColor(plan.stepRule >= 0.59f && plan.stepRule <= 0.65f), "%.1f cm",
                       plan.stepRule * 100.0f);
    ImGui::SetItemTooltip("The step rule: two risers and a going make one stride, 59-65 cm.");
    cell("Run");
    ImGui::Text("%.2f m", plan.run);
    cell("Footprint");
    ImGui::Text("%.2f x %.2f", plan.bbMax.x - plan.bbMin.x, plan.bbMax.y - plan.bbMin.y);
    if (stairgen::isRound(p.kind)) {
        cell("Turn");
        ImGui::Text("%.0f" DEG, plan.turnDeg);
        cell("Diameter");
        ImGui::Text("%.2f m", 2.0f * plan.outerRadius);
    }
    ImGui::EndTable();
}

} // namespace

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    ImGui::SetNextWindowSize(ImVec2(460.0f, 820.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Stairs", &s.show)) {
        stairgen::Params& c = s.cfg;
        bool changed = false;

        ui::hint("Straight, turning, spiral: set the height and the type, the steps work "
                 "themselves out. Generate places it at the 3D cursor, climbing away from you.");

        ui::sectionText("Preset");
        {
            const int n = static_cast<int>(stairgen::Preset::Count);
            const int cols = 4;
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float w = (ImGui::GetContentRegionAvail().x - gap * static_cast<float>(cols - 1)) /
                            static_cast<float>(cols);
            for (int i = 0; i < n; ++i) {
                if (i % cols) ImGui::SameLine();
                const auto pr = static_cast<stairgen::Preset>(i);
                if (ImGui::Button(stairgen::presetName(pr), ImVec2(w, 28.0f))) {
                    stairgen::applyPreset(c, pr);
                    changed = true;
                }
                ImGui::SetItemTooltip("%s", stairgen::presetHint(pr));
            }
        }

        ui::sectionText("Type");
        {
            const char* tips[] = {
                "One flight, optionally with a landing part-way.",
                "Quarter turn on a square landing.",
                "Quarter turn on winders -- no landing, less floor.",
                "Half turn on a landing across both flights.",
                "Half turn on winders -- the most compact turning stair.",
                "Round a central column.",
                "Round an open eye: a sweeping curve.",
            };
            const char* labels[static_cast<int>(Kind::Count)];
            for (int i = 0; i < static_cast<int>(Kind::Count); ++i)
                labels[i] = stairgen::kindName(static_cast<Kind>(i));
            int cur = static_cast<int>(c.kind);
            if (choice("##kind", cur, labels, static_cast<int>(Kind::Count), 4, tips)) {
                const Kind was = c.kind;
                c.kind = static_cast<Kind>(cur);
                // Round stairs want a narrower flight and a column to start from.
                if (stairgen::isRound(c.kind) && !stairgen::isRound(was)) {
                    c.column = c.kind == Kind::Spiral;
                    c.innerRadius = c.kind == Kind::Spiral ? 0.12f : 1.2f;
                }
                if (c.kind == Kind::Spiral && !stairgen::isRound(was)) c.width = std::min(c.width, 0.8f);
                changed = true;
            }
            if (stairgen::isTurning(c.kind)) {
                const char* hands[] = {"Turns left", "Turns right"};
                int hand = c.turnRight ? 1 : 0;
                if (choice("##hand", hand, hands, 2, 2)) { c.turnRight = hand == 1; changed = true; }
            }
        }

        // One layout per frame: a few dozen polygons, cheap enough to be the live
        // answer to every click above.
        const Plan plan = stairgen::layout(c);
        ui::sectionText("Plan");
        drawPlanView(plan);
        drawProfileView(plan);
        figures(plan);
        for (int i = 0; i < static_cast<int>(plan.notes.size()); ++i) {
            if (i < plan.warnings) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.30f, 1.0f), "! %s", plan.notes[i].c_str());
                ImGui::PopTextWrapPos();
            } else {
                ui::hint("%s", plan.notes[i].c_str());
            }
        }

        if (ui::header("Size", ImGuiTreeNodeFlags_DefaultOpen)) {
            changed |= num("Total height", c.height, 0.05f, 0.30f, 12.0f, "%.2f m");
            ImGui::SetItemTooltip("Floor to floor: what the stair climbs.");
            changed |= num("Width", c.width, 0.05f, 0.50f, 4.0f, "%.2f m");
            ImGui::SetItemTooltip("Clear width of the flight.");
            changed |= ImGui::Checkbox("Risers from the height", &c.autoRisers);
            if (c.autoRisers) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%d)", plan.risers);
                changed |= num("Aim for a rise of", c.targetRise, 0.005f, 0.12f, 0.22f, "%.3f m");
                ImGui::SetItemTooltip("16-18 cm is a house stair, 14-16 cm a grand or an outdoor "
                                      "one, 20 cm steep.");
            } else {
                changed |= count("Risers", c.risers, 2, 80);
            }
            changed |= ImGui::Checkbox("Going from the step rule", &c.autoGoing);
            ImGui::SetItemTooltip("Tread depth from 2 x rise + going = 63 cm, the stride "
                                  "of an adult.");
            if (c.autoGoing) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%.1f cm)", plan.going * 100.0f);
            } else {
                changed |= num("Going", c.going, 0.01f, 0.18f, 0.45f, "%.2f m");
            }
            changed |= num("Top platform", c.topPlatform, 0.10f, 0.0f, 4.0f, "%.2f m");
            ImGui::SetItemTooltip("Depth of a platform at the top. 0: the stair arrives at the "
                                  "edge of a floor.");
        }

        if (ui::header("Layout", ImGuiTreeNodeFlags_DefaultOpen)) {
            const bool turning = stairgen::isTurning(c.kind) && !stairgen::isRound(c.kind);
            if (c.kind == Kind::Straight) {
                changed |= ImGui::Checkbox("Landing part-way", &c.midLanding);
                if (c.midLanding) {
                    changed |= num("Landing depth", c.landingDepth, 0.05f, 0.6f, 4.0f, "%.2f m");
                    changed |= splitControl(c, plan, "Landing half way");
                }
            }
            if (turning) changed |= splitControl(c, plan, "Turn half way");
            if (c.kind == Kind::ULanding) {
                changed |= num("Landing depth", c.landingDepth, 0.05f, 0.6f, 4.0f, "%.2f m");
            }
            if (c.kind == Kind::ULanding || c.kind == Kind::UWinder) {
                changed |= num("Eye", c.eye, 0.05f, 0.0f, 1.5f, "%.2f m");
                ImGui::SetItemTooltip("Gap between the two flights.");
            }
            if (c.kind == Kind::LWinder || c.kind == Kind::UWinder) {
                changed |= count(c.kind == Kind::UWinder ? "Winders per quarter" : "Winders",
                                 c.winders, 2, 6);
                ImGui::SetItemTooltip("Wedge-shaped treads in the turn. More: a gentler turn, "
                                      "narrower wedges.");
            }
            if (stairgen::isRound(c.kind)) {
                changed |= num(c.kind == Kind::Spiral ? "Column radius" : "Eye radius",
                               c.innerRadius, 0.05f, 0.05f, 4.0f, "%.2f m");
                changed |= ImGui::Checkbox("Column", &c.column);
                changed |= ImGui::Checkbox("Turn from the going", &c.autoTurn);
                ImGui::SetItemTooltip("Each tread turns as far as the going on the walking line "
                                      "asks. Off: share out a total turn.");
                if (c.autoTurn) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%.0f" DEG ")", plan.turnDeg);
                } else {
                    changed |= num("Total turn", c.turn, 15.0f, 30.0f, 1080.0f, "%.0f" DEG);
                }
            }
            if (c.kind == Kind::Straight && !c.midLanding)
                ImGui::TextDisabled("A single flight -- nothing more to lay out.");
        }

        if (ui::header("Construction", ImGuiTreeNodeFlags_DefaultOpen)) {
            const char* labels[] = {"Solid", "Slab", "Stringers", "Spine", "Floating"};
            const char* tips[] = {
                "Every step a block down to the ground: outdoor, cellar, stone.",
                "Steps on an inclined concrete slab.",
                "Board treads between two side stringers.",
                "Board treads on one beam down the middle.",
                "Board treads only, held by the wall or the column.",
            };
            int cur = static_cast<int>(c.structure);
            if (choice("##structure", cur, labels, 5, 5, tips)) {
                c.structure = static_cast<Structure>(cur);
                changed = true;
            }
            const bool boards = c.structure == Structure::Stringers ||
                                c.structure == Structure::Spine ||
                                c.structure == Structure::Floating;
            if (boards) {
                changed |= num("Tread thickness", c.treadThickness, 0.01f, 0.02f, 0.25f, "%.2f m");
                changed |= num("Nosing", c.nosing, 0.005f, 0.0f, 0.06f, "%.3f m");
                ImGui::SetItemTooltip("How far a tread reaches over the riser below.");
                changed |= ImGui::Checkbox("Closed risers", &c.closedRisers);
            }
            if (c.structure == Structure::Slab)
                changed |= num("Slab thickness", c.stringerDepth, 0.02f, 0.10f, 0.60f, "%.2f m");
            if (c.structure == Structure::Stringers || c.structure == Structure::Spine)
                changed |= num(c.structure == Structure::Spine ? "Spine depth" : "Stringer depth",
                               c.stringerDepth, 0.02f, 0.10f, 0.60f, "%.2f m");
            if (c.structure == Structure::Stringers || c.structure == Structure::Spine)
                changed |= num(c.structure == Structure::Spine ? "Spine half-width" : "Stringer width",
                               c.stringerWidth, 0.01f, 0.03f, 0.25f, "%.2f m");
        }

        const bool turningSides = stairgen::isTurning(c.kind);
        if (ui::header("Railing & walls", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextUnformatted("Railing");
            changed |= sidesChoice("##rail", c.railSides, turningSides);
            if (c.railSides != Sides::None) {
                const char* tips[] = {"Posts, a handrail and two rails between them.",
                                      "Vertical bars under the handrail.",
                                      "Glass panels under the handrail.",
                                      "A solid wall at railing height."};
                const char* labels[] = {"Posts", "Balusters", "Glass", "Parapet"};
                int cur = static_cast<int>(c.railStyle);
                if (choice("##railStyle", cur, labels, 4, 4, tips)) {
                    c.railStyle = static_cast<RailStyle>(cur);
                    changed = true;
                }
                changed |= num("Railing height", c.railHeight, 0.05f, 0.60f, 1.30f, "%.2f m");
                ImGui::SetItemTooltip("Above the nosings. 0.90 m in a house, 1.00-1.10 m with a "
                                      "long drop.");
                if (c.railStyle == RailStyle::Balusters)
                    changed |= num("Gap between bars", c.balusterGap, 0.01f, 0.04f, 0.30f, "%.2f m");
            }
            ImGui::TextUnformatted("Walls");
            changed |= sidesChoice("##wall", c.wallSides, turningSides);
            if (c.wallSides != Sides::None) {
                changed |= num("Wall height", c.wallHeight, 0.10f, 0.30f, 5.0f, "%.2f m");
                ImGui::SetItemTooltip("Above the nosings: the wall follows the stair up.");
                changed |= num("Wall thickness", c.wallThickness, 0.01f, 0.06f, 0.50f, "%.2f m");
            }
            ui::hint("A railing on a side with a wall becomes a handrail on the wall.");
        }

        if (ui::header("Look")) {
            const char* labels[static_cast<int>(stairgen::Look::Count)];
            for (int i = 0; i < static_cast<int>(stairgen::Look::Count); ++i)
                labels[i] = stairgen::lookName(static_cast<stairgen::Look>(i));
            int cur = static_cast<int>(c.look);
            if (choice("##look", cur, labels, static_cast<int>(stairgen::Look::Count), 4)) {
                stairgen::applyLook(c, static_cast<stairgen::Look>(cur));
                changed = true;
            }
            changed |= ImGui::ColorEdit3("Treads", &c.treadColor.x);
            changed |= ImGui::ColorEdit3("Structure", &c.structureColor.x);
            changed |= ImGui::ColorEdit3("Railing", &c.railColor.x);
            changed |= ImGui::ColorEdit3("Walls", &c.wallColor.x);
            ui::hint("Stairs of one look share its materials (\"Stair %s ...\"): a colour "
                     "changed here changes them all.", stairgen::lookName(c.look));
        }
        if (ui::header("Physics")) {
            changed |= ImGui::Checkbox("Collider", &c.collider);
            ImGui::SetItemTooltip("Static colliders on every part, so it can be walked up.");
        }

        // --- Actions ------------------------------------------------------
        ImGui::Separator();
        if (ImGui::Button("Generate stair", ImVec2(-FLT_MIN, 34.0f))) s.generate();
        ImGui::SetItemTooltip("Places a new stair at the 3D cursor (or on the ground in front "
                              "of the camera), climbing away from the view. One undo step.");
        ImGui::BeginDisabled(!s.hasLive);
        if (ImGui::Button("Rebuild in place", ImVec2(-FLT_MIN, 0.0f))) s.rebuild();
        ImGui::EndDisabled();
        ImGui::Checkbox("Live update", &s.autoRebuild);
        ImGui::SetItemTooltip("Re-generate the last stair whenever a setting changes.");
        ImGui::BeginDisabled(!s.selectionIsStair);
        if (ImGui::Button("Edit selected stair", ImVec2(-FLT_MIN, 0.0f))) s.loadSelected();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Load the parameters of the selected stair into this panel "
                              "and make it the one Rebuild acts on.");

        ui::sectionText("Save as prefab");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##stairName", "Name", s.nameBuf, s.nameBufSize);
        ImGui::BeginDisabled(!s.hasLive || !s.hasProject || s.nameBuf[0] == '\0');
        if (ImGui::Button("Save as prefab", ImVec2(-FLT_MIN, 0.0f))) s.saveAsPrefab();
        ImGui::EndDisabled();
        if (!s.hasProject) ImGui::TextDisabled("Open a project first (prefabs live in it).");
        {
            // Building the meshes is the expensive half: count only on change.
            static stairgen::Params counted;
            static int faces = -1;
            if (faces < 0 || !(counted == c)) { counted = c; faces = stairgen::faceCount(c); }
            ui::hint("%d faces", faces);
        }
        if (!s.status.empty()) ImGui::TextWrapped("%s", s.status.c_str());

        // An edit is applied once the widget is released: typing a height would
        // otherwise rebuild (and push an undo step) on every keystroke.
        if (changed) s.pendingRebuild = true;
        if (s.pendingRebuild && !ImGui::IsAnyItemActive()) {
            s.pendingRebuild = false;
            if (s.autoRebuild && s.hasLive) s.rebuild();
        }
    }
    ImGui::End();
}

} // namespace stairui
