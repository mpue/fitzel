#include "WorldGenPanel.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <imgui.h>
#include <stb_image.h>

#include <fitzel/graphics/Texture.hpp>
#include <fitzel/ui/ImGuiTexture.hpp>

#include "GeoClient.hpp"
#include "GeoPlan.hpp"
#include "GeoTiles.hpp"
#include "HouseGen.hpp"
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
        const char* msg = s.planning ? "Planning..."
                        : s.plan.real ? "Pick a square on the map above and press Load."
                                      : "Move a slider or press Plan.";
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
    // A real place: its streets and, by what is built for each, its buildings.
    const geoplan::Building* hoverB = nullptr;
    if (pl.real) {
        const float pxPerM = side / span;
        for (const geoplan::Road& r : pl.real->roads)
            poly(r.pts, IM_COL32(246, 236, 206, 255), std::max(1.0f, r.width * pxPerM));
        if (s.realMap && s.realMap->showBuildings) {
            const ImVec2 mouse = ImGui::GetMousePos();
            for (const geoplan::Building& b : pl.real->buildings) {
                ImU32 col = IM_COL32(150, 70, 55, 255);
                switch (b.build) {
                case geoplan::Build::Skip:     col = IM_COL32(120, 120, 120, 160); break;
                case geoplan::Build::House:    col = IM_COL32(196, 92, 70, 255); break;
                case geoplan::Build::Terraced: col = IM_COL32(170, 60, 60, 255); break;
                case geoplan::Build::Block:    col = IM_COL32(110, 110, 160, 255); break;
                case geoplan::Build::Tower:    col = IM_COL32(70, 70, 150, 255); break;
                case geoplan::Build::Hall:     col = IM_COL32(150, 130, 80, 255); break;
                case geoplan::Build::Civic:    col = IM_COL32(220, 170, 40, 255); break;
                default: break;
                }
                if (b.poly.size() < 3) continue;
                ImVec2 q[64];
                const std::size_t n = std::min<std::size_t>(b.poly.size(), 64);
                for (std::size_t k = 0; k < n; ++k) q[k] = toScreen(b.poly[k]);
                if (pxPerM * std::sqrt(b.area) < 2.0f) {
                    dl->AddRectFilled(ImVec2(q[0].x - 0.7f, q[0].y - 0.7f), ImVec2(q[0].x + 0.7f, q[0].y + 0.7f), col);
                } else {
                    dl->AddConvexPolyFilled(q, static_cast<int>(n), col);
                }
                if (hovered && !hoverB) {
                    const ImVec2 c = toScreen(b.centre);
                    const float r = std::max(3.0f, 0.5f * pxPerM * std::sqrt(b.area));
                    if ((mouse.x - c.x) * (mouse.x - c.x) + (mouse.y - c.y) * (mouse.y - c.y) < r * r) hoverB = &b;
                }
            }
        }
        for (const geoplan::Town& t : pl.real->towns) {
            if (t.land) continue;
            const ImVec2 c = toScreen(t.centre);
            const ImVec2 ts = ImGui::CalcTextSize(t.name.c_str());
            const ImVec2 tp(c.x - 0.5f * ts.x, c.y - 0.5f * ts.y);
            dl->AddRectFilled(ImVec2(tp.x - 2, tp.y - 1), ImVec2(tp.x + ts.x + 2, tp.y + ts.y + 1), IM_COL32(20, 20, 20, 150), 3.0f);
            dl->AddText(tp, IM_COL32(255, 245, 225, 255), t.name.c_str());
        }
    }
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
        if (hoverB) {
            const char* name = hoverB->name.empty() ? "" : hoverB->name.c_str();
            ImGui::SetTooltip("%s%s%s: %s\n%.0f m^2, %.0f x %.0f m%s\nBuilt as: %s\nClick: fly the camera here.", name,
                              *name ? " -- " : "", geoplan::useName(hoverB->use), hoverB->type.c_str(), hoverB->area,
                              hoverB->boxL, hoverB->boxW, hoverB->levels > 0 ? (", " + std::to_string(hoverB->levels) + " storeys on the map").c_str() : "",
                              hoverB->why.c_str());
        } else {
            ImGui::SetTooltip("%.0f, %.0f -- %.0f m%s%s\nClick: fly the camera here.", w.x, w.y, g.heightAt(w),
                              u > 0 && u < 7 ? ", " : "", u > 0 && u < 7 ? uses[u] : "");
        }
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

// --- A real place: the map it is picked on -------------------------------------------

double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// A map picture, uploaded once it has come (a few a frame: decoding is the cost).
const fitzel::Texture* mapTexture(const PanelState& s, int z, int x, int y, int priority, int& uploads) {
    RealMap& m = *s.realMap;
    const std::string path = "/map/" + std::to_string(z) + "/" + std::to_string(x) + "/" + std::to_string(y) + ".png";
    auto it = m.tex.find(path);
    if (it != m.tex.end()) {
        m.used[path] = nowSec();
        return it->second.get();
    }
    if (priority < 0) return nullptr;   // only if already there
    const geoclient::Client::Result r = s.geo->want(path, priority);
    if (r.state != geoclient::Client::State::Ready || !r.body || uploads >= 6) return nullptr;
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(r.body->data()),
                                              static_cast<int>(r.body->size()), &w, &h, &n, 4);
    if (!px) return nullptr;
    ++uploads;
    auto tex = std::make_shared<fitzel::Texture>(fitzel::Texture::fromPixels(px, w, h, 4));
    stbi_image_free(px);
    m.tex[path] = tex;
    m.used[path] = nowSec();
    // Keep the most recently drawn few hundred.
    if (m.tex.size() > 360) {
        std::vector<std::pair<double, std::string>> old;
        for (const auto& [k, t] : m.used) old.emplace_back(t, k);
        std::sort(old.begin(), old.end());
        for (std::size_t i = 0; i < old.size() && m.tex.size() > 300; ++i) {
            m.tex.erase(old[i].second);
            m.used.erase(old[i].second);
        }
    }
    return tex.get();
}

void realPicker(const PanelState& s) {
    worldgen::Params& p = s.params;
    RealMap& m = *s.realMap;
    geoclient::Client& geo = *s.geo;
    if (!m.placed) {
        m.cx = geo::tileX(p.lon, m.zoom);
        m.cy = geo::tileY(p.lat, m.zoom);
        m.placed = true;
    }
    // --- Search ---------------------------------------------------------------------
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s", m.search.c_str());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70.0f);
        const bool enter = ImGui::InputTextWithHint("##placesearch", "A place: Kassel, Hallstatt, Bergen ...", buf, sizeof(buf),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        m.search = buf;
        ImGui::SameLine();
        if ((ImGui::Button("Find", ImVec2(-1.0f, 0.0f)) || enter) && !m.search.empty()) {
            m.searchPath = "/search?q=" + geo::urlEncode(m.search);
            m.found.clear();
            m.searchNote = "Searching...";
        }
        if (!m.searchPath.empty()) {
            const geoclient::Client::Result r = geo.want(m.searchPath, 60);
            if (r.state == geoclient::Client::State::Ready && r.body) {
                std::vector<geo::Place> places;
                geo::parseSearch(*r.body, places);
                for (const geo::Place& pl : places) {
                    // Zoom to show the place's own extent.
                    const double ext = std::max(pl.box.e - pl.box.w, pl.box.n - pl.box.s);
                    const double z = std::clamp(std::log2(360.0 / std::max(ext, 1e-4)) + 0.5, 4.0, 16.0);
                    m.found.push_back({pl.name, glm::dvec4(pl.at.lon, pl.at.lat, z, 0.0)});
                }
                m.searchNote = places.empty() ? "Nothing found." : "";
                m.searchPath.clear();
            } else if (r.state == geoclient::Client::State::Failed) {
                m.searchNote = "Search failed: " + r.error;
                m.searchPath.clear();
            }
        }
        if (!m.searchNote.empty()) ui::hint("%s", m.searchNote.c_str());
        int pick = -1;
        for (int i = 0; i < static_cast<int>(m.found.size()) && i < 8; ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(m.found[static_cast<std::size_t>(i)].first.c_str())) pick = i;
            ImGui::PopID();
        }
        if (pick >= 0) {
            const glm::dvec4 f = m.found[static_cast<std::size_t>(pick)].second;
            m.zoom = static_cast<int>(f.z);
            m.cx = geo::tileX(f.x, m.zoom);
            m.cy = geo::tileY(f.y, m.zoom);
            p.lon = f.x;
            p.lat = f.y;
            p.placeLabel = m.found[static_cast<std::size_t>(pick)].first;
            const std::size_t comma = p.placeLabel.find(',');
            if (comma != std::string::npos && comma < p.placeLabel.size()) p.placeLabel = p.placeLabel.substr(0, comma);
            m.found.clear();
        }
    }

    // --- The map ------------------------------------------------------------------------
    const float w = std::clamp(ImGui::GetContentRegionAvail().x, 200.0f, 640.0f);
    const float h = std::round(w * 0.78f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + h);
    ImGui::InvisibleButton("##realmap", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(214, 212, 206, 255));
    const int n = 1 << m.zoom;
    const double tlx = m.cx - 0.5 * w / 256.0, tly = m.cy - 0.5 * h / 256.0;
    auto toScreen = [&](double tx, double ty) {
        return ImVec2(p0.x + static_cast<float>((tx - tlx) * 256.0), p0.y + static_cast<float>((ty - tly) * 256.0));
    };
    auto llScreen = [&](double lon, double lat) { return toScreen(geo::tileX(lon, m.zoom), geo::tileY(lat, m.zoom)); };
    dl->PushClipRect(p0, p1, true);
    int uploads = 0;
    const int x0 = static_cast<int>(std::floor(tlx)), x1 = static_cast<int>(std::floor(tlx + w / 256.0));
    const int y0 = std::max(0, static_cast<int>(std::floor(tly))), y1 = std::min(n - 1, static_cast<int>(std::floor(tly + h / 256.0)));
    bool waiting = false;
    for (int ty = y0; ty <= y1; ++ty)
        for (int txr = x0; txr <= x1; ++txr) {
            const int tx = ((txr % n) + n) % n;
            const ImVec2 a = toScreen(txr, ty), b = toScreen(txr + 1, ty + 1);
            const fitzel::Texture* t = mapTexture(s, m.zoom, tx, ty, 5, uploads);
            if (t && t->isValid()) {
                dl->AddImage(fitzel::imTexture(t->id()), a, b);
                continue;
            }
            waiting = true;
            // The coarser picture of the same ground, while this one comes.
            bool filled = false;
            for (int up = 1; up <= 3 && !filled && m.zoom - up >= 0; ++up) {
                const int k = 1 << up;
                const fitzel::Texture* pt = mapTexture(s, m.zoom - up, tx / k, ty / k, -1, uploads);
                if (!pt || !pt->isValid()) continue;
                const float u0 = static_cast<float>(tx % k) / k, v0 = static_cast<float>(ty % k) / k;
                dl->AddImage(fitzel::imTexture(pt->id()), a, b, ImVec2(u0, v0), ImVec2(u0 + 1.0f / k, v0 + 1.0f / k));
                filled = true;
            }
            if (!filled) dl->AddRect(a, b, IM_COL32(190, 188, 182, 255));
        }
    // The square to be generated, and how far its tiles have come.
    {
        const geo::BBox b = geo::squareAround({p.lon, p.lat}, p.size);
        const ImVec2 a = llScreen(b.w, b.n), c = llScreen(b.e, b.s);
        if (s.load && (s.load->active() || s.load->done())) {
            const geo::LonLat lc = s.load->center();
            if (std::fabs(lc.lon - p.lon) < 1e-7 && std::fabs(lc.lat - p.lat) < 1e-7)
                for (std::size_t i = 0; i < s.load->osmTiles().size(); ++i) {
                    const geo::TileId& t = s.load->osmTiles()[i];
                    const double k = std::ldexp(1.0, m.zoom - t.z);
                    const ImVec2 ta = toScreen(t.x * k, t.y * k), tb = toScreen((t.x + 1) * k, (t.y + 1) * k);
                    const int st = s.load->osmState(i);
                    const ImU32 col = st == 1 ? IM_COL32(60, 170, 90, 50) : st == 2 ? IM_COL32(220, 60, 50, 70) : IM_COL32(240, 200, 60, 40);
                    dl->AddRectFilled(ta, tb, col);
                    dl->AddRect(ta, tb, IM_COL32(60, 60, 60, 90));
                }
        }
        dl->AddRectFilled(a, c, IM_COL32(230, 60, 40, 40));
        dl->AddRect(a, c, IM_COL32(230, 60, 40, 255), 0.0f, 0, 2.0f);
        const ImVec2 mid = llScreen(p.lon, p.lat);
        dl->AddLine(ImVec2(mid.x - 5, mid.y), ImVec2(mid.x + 5, mid.y), IM_COL32(230, 60, 40, 255), 1.5f);
        dl->AddLine(ImVec2(mid.x, mid.y - 5), ImVec2(mid.x, mid.y + 5), IM_COL32(230, 60, 40, 255), 1.5f);
    }
    // OpenStreetMap's licence asks for this on every map that shows its tiles.
    {
        const char* attr = "\xC2\xA9 OpenStreetMap contributors";
        const ImVec2 ts = ImGui::CalcTextSize(attr);
        const ImVec2 tp(p1.x - ts.x - 6.0f, p1.y - ts.y - 3.0f);
        dl->AddRectFilled(ImVec2(tp.x - 3, tp.y - 1), ImVec2(p1.x, p1.y), IM_COL32(255, 255, 255, 190));
        dl->AddText(tp, IM_COL32(40, 40, 40, 255), attr);
    }
    if (waiting && geo.server() == geoclient::Client::Server::Down) {
        const std::string note = "No map: " + geo.serverNote();
        dl->AddText(ImVec2(p0.x + 8, p0.y + 8), IM_COL32(150, 30, 20, 255), note.c_str());
    }
    dl->PopClipRect();
    dl->AddRect(p0, p1, IM_COL32(90, 90, 96, 255));

    // --- Moving it ------------------------------------------------------------------------
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) {
        m.cx -= io.MouseDelta.x / 256.0;
        m.cy -= io.MouseDelta.y / 256.0;
        m.dragging = true;
    }
    const ImVec2 mouse = io.MousePos;
    const double mtx = tlx + (mouse.x - p0.x) / 256.0, mty = tly + (mouse.y - p0.y) / 256.0;
    if (ImGui::IsItemDeactivated()) {
        if (!m.dragging && hovered) {
            p.lon = geo::lonOf(mtx, m.zoom);
            p.lat = geo::latOf(mty, m.zoom);
            p.placeLabel.clear();
        }
        m.dragging = false;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        const int z = std::clamp(m.zoom + (io.MouseWheel > 0.0f ? 1 : -1), 3, 18);
        if (z != m.zoom) {
            const double k = std::ldexp(1.0, z - m.zoom);
            m.cx = mtx * k - (mouse.x - p0.x - 0.5 * w) / 256.0;
            m.cy = mty * k - (mouse.y - p0.y - 0.5 * h) / 256.0;
            m.zoom = z;
            s.geo->dropQueuedBelow(6);
        }
    }
    m.cy = std::clamp(m.cy, 0.0, static_cast<double>(n));
    if (hovered)
        ImGui::SetTooltip("%.5f N, %.5f E\nClick: the square's centre here. Drag: move the map. Wheel: zoom.",
                          geo::latOf(mty, m.zoom), geo::lonOf(mtx, m.zoom));

    // --- The square ---------------------------------------------------------------------
    ImGui::SliderFloat("Size", &p.size, 500.0f, 4000.0f, "%.0f m");
    tip("The side of the square that is built. Bigger squares are many more\n"
        "buildings and streets: 2 km of a city is a few thousand of each.");
    ImGui::Text("%s%.5f N, %.5f E", p.placeLabel.empty() ? "" : (p.placeLabel + ": ").c_str(), p.lat, p.lon);
    const bool loaded = p.area && std::fabs(p.area->center.lon - p.lon) < 1e-7 &&
                        std::fabs(p.area->center.lat - p.lat) < 1e-7 && std::fabs(p.area->size - p.size) < 0.5f;
    const bool busy = s.load && s.load->active();
    ImGui::BeginDisabled(busy || !s.loadArea);
    if (ImGui::Button(loaded ? "Load again" : "Load this square", ImVec2(-1.0f, 0.0f)) && s.loadArea) s.loadArea();
    ImGui::EndDisabled();
    if (busy) {
        ImGui::ProgressBar(s.load->progress(), ImVec2(-1.0f, 0.0f));
    }
    if (s.load && !s.load->status().empty()) ui::hint("%s", s.load->status().c_str());
    if (!loaded && !busy && p.area) {
        ImGui::PushStyleColor(ImGuiCol_Text, kNote);
        ImGui::TextWrapped("The plan below is of the square loaded before -- Load this one.");
        ImGui::PopStyleColor();
    }
    {
        const char* st = "?";
        switch (geo.server()) {
        case geoclient::Client::Server::Unknown:  st = "not asked yet"; break;
        case geoclient::Client::Server::Up:       st = "running"; break;
        case geoclient::Client::Server::Starting: st = "starting"; break;
        case geoclient::Client::Server::Down:     st = "not running"; break;
        }
        ui::hint("Map cache (fitzelgeo) at %s:%d: %s%s", geo.host().c_str(), geo.port(), st,
                 geo.pending() ? (", " + std::to_string(geo.pending()) + " requests waiting").c_str() : "");
        if (geo.server() == geoclient::Client::Server::Down) {
            ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
            ImGui::TextWrapped("%s", geo.serverNote().c_str());
            ImGui::PopStyleColor();
            if (ImGui::SmallButton("Try again")) geo.retryServer();
        }
    }
}

void realSummary(const PanelState& s) {
    if (!s.planValid || !s.plan.real) return;
    const geoplan::Real& R = *s.plan.real;
    const geoplan::Summary& S = R.summary;
    ImGui::Text("Region: %s", worldgen::regionName(S.region));
    ui::hint("%s", S.regionWhy.c_str());
    ui::hint("Ground %.0f .. %.0f m above the sea%s \xC2\xB7 %d river%s and brook%s",
             S.lowest, S.highest, S.coast ? ", on the coast" : "", S.rivers, S.rivers == 1 ? "" : "s", S.rivers == 1 ? "" : "s");
    std::string places;
    int shown = 0;
    for (const geoplan::Place& pl : R.places) {
        if (pl.buildings < 12) continue;
        if (shown++ < 6) places += (places.empty() ? "" : ", ") + pl.name + " (" + geoplan::settlementName(pl.kind) + ")";
    }
    if (!places.empty()) ImGui::TextWrapped("Places: %s%s", places.c_str(), shown > 6 ? ", ..." : "");
    ImGui::Text("%d buildings, built as:", S.buildings);
    {
        auto count = [&](geoplan::Build b) { return S.builds[static_cast<std::size_t>(b)]; };
        std::string houses;
        for (int k = 0; k < static_cast<int>(housegen::Preset::Count); ++k)
            if (int c = S.housePresets[static_cast<std::size_t>(k)])
                houses += (houses.empty() ? "" : ", ") + std::string(housegen::presetName(static_cast<housegen::Preset>(k))) + " " + std::to_string(c);
        ImGui::BulletText("%d houses (HouseGen): %s", count(geoplan::Build::House) + count(geoplan::Build::Terraced), houses.c_str());
        ImGui::BulletText("%d apartment and office blocks, %d towers (BuildingGen)", count(geoplan::Build::Block), count(geoplan::Build::Tower));
        ImGui::BulletText("%d halls, %d public buildings (CivicGen)", count(geoplan::Build::Hall), count(geoplan::Build::Civic));
        if (count(geoplan::Build::Skip)) ImGui::BulletText("%d left out (garages, sheds)", count(geoplan::Build::Skip));
    }
    {
        int n = 0;
        float km = 0.0f;
        for (int c = 0; c < static_cast<int>(geoplan::RoadClass::Count); ++c) {
            n += S.roads[static_cast<std::size_t>(c)];
            km += S.roadKm[static_cast<std::size_t>(c)];
        }
        ImGui::Text("%d streets and roads, %.1f km", n, km);
        if (ImGui::IsItemHovered()) {
            std::string t;
            for (int c = 0; c < static_cast<int>(geoplan::RoadClass::Count); ++c)
                if (S.roads[static_cast<std::size_t>(c)]) {
                    char b[96];
                    std::snprintf(b, sizeof(b), "%s: %d, %.1f km\n", geoplan::roadClassName(static_cast<geoplan::RoadClass>(c)),
                                  S.roads[static_cast<std::size_t>(c)], S.roadKm[static_cast<std::size_t>(c)]);
                    t += b;
                }
            ImGui::SetTooltip("%s", t.c_str());
        }
    }
    ui::hint("Woods %.0f %%, fields %.0f %%, built over %.0f %% \xC2\xB7 %d single trees%s",
             S.forestShare * 100.0f, S.fieldShare * 100.0f, S.builtShare * 100.0f, S.trees,
             S.needleShare >= 0.0f ? (" \xC2\xB7 " + std::to_string(static_cast<int>(S.needleShare * 100.0f)) + " % conifers in the woods").c_str() : "");
    for (const std::string& note : S.notes) {
        ImGui::PushStyleColor(ImGuiCol_Text, kNote);
        ImGui::TextWrapped("%s", note.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Checkbox("Buildings on the plan", &s.realMap->showBuildings);
    tip("Each footprint in the colour of what is built for it: houses red, blocks\n"
        "blue, halls ochre, public buildings gold, grey what is left out. Hover one\n"
        "for why.");
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
    // The shade is of the plan the map last drew: a plan with no ground yet (a
    // real place not loaded) has no cells to read it against.
    if (s.map.rev != s.planRev || s.map.shade.size() != s.plan.grid.use.size()) return;
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
    if (s.params.source == worldgen::Source::RealPlace && s.plan.real) {
        const geoplan::Summary& S = s.plan.real->summary;
        int streets = 0, towns = 0;
        for (const geoplan::Town& t : s.plan.real->towns) { streets += static_cast<int>(t.streets.size()); towns += !t.land; }
        ImGui::Spacing();
        ui::sectionText("From the map");
        ImGui::BulletText("%s: the measured ground of %.0f x %.0f m, and the land around it.",
                          s.params.placeLabel.empty() ? "The square" : s.params.placeLabel.c_str(), s.params.size, s.params.size);
        ImGui::BulletText("%d buildings (%d left out) in %d place(s), on %d streets.", S.buildings,
                          S.builds[static_cast<std::size_t>(geoplan::Build::Skip)], towns, streets);
        ImGui::BulletText("%d river(s) and brook(s), the woods as mapped, %d single trees.", S.rivers, S.trees);
        ui::hint("A town of thousands of buildings takes a while to build the first time.");
        ui::hint("Built from OpenStreetMap data: a game made from it credits\n"
                 "\"\xC2\xA9 OpenStreetMap contributors\" (the ODbL asks for that).");
    }
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

    // --- Invented, or a real place ---------------------------------------------------
    const bool canReal = s.geo && s.realMap;
    bool realSrc = p.source == worldgen::Source::RealPlace && canReal;
    {
        int src = realSrc ? 1 : 0;
        ImGui::RadioButton("Invented", &src, 0);
        ImGui::SameLine();
        ImGui::BeginDisabled(!canReal);
        ImGui::RadioButton("A real place", &src, 1);
        ImGui::EndDisabled();
        tip("A square of a real map (OpenStreetMap): its ground, buildings, streets,\n"
            "rivers and woods -- and for each building the generator decides which of\n"
            "the editor's houses, blocks, halls or public buildings stands for it.");
        const worldgen::Source want = src == 1 ? worldgen::Source::RealPlace : worldgen::Source::Generated;
        if (want != p.source) { p.source = want; now = true; }
        realSrc = p.source == worldgen::Source::RealPlace && canReal;
    }
    if (realSrc) {
        ui::sectionText("Place");
        realPicker(s);
    }

    // --- What kind of place ---------------------------------------------------------
    if (!realSrc) {
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
    }   // !realSrc

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
            if (!realSrc) { ImGui::TableNextColumn(); ImGui::Checkbox("Farms", &st.farms); }
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
    if (realSrc) realSummary(s);
    else report(s);

    if (ui::header("Sun", ImGuiTreeNodeFlags_DefaultOpen)) sunSection(s);
    if (ui::header("Trees", ImGuiTreeNodeFlags_DefaultOpen)) trees(s);
    if (realSrc) {
        if (ui::header("Look")) {
            worldgen::Character& c = p.look;
            ImGui::ColorEdit3("Facades", &c.facade.x, ImGuiColorEditFlags_NoInputs);
            ImGui::SameLine();
            ImGui::ColorEdit3("Roofs", &c.roof.x, ImGuiColorEditFlags_NoInputs);
            tip("Read from the map's roof colours where enough buildings have one,\n"
                "else the region's.");
            ImGui::SliderFloat("Rocks", &c.rocks, 0.0f, 1.0f, "%.2f"); slid();
            ImGui::SliderFloat("Crag slope", &c.rockSlope, 0.2f, 1.2f, "%.2f"); slid();
            if (ImGui::Button("Back to what the map says") && s.plan.real) {
                p.look = s.plan.real->look;
                p.region = s.plan.real->region;
                now = true;
            }
        }
    } else if (ui::header("Character")) {
        now |= character(s, slid);
    }

    ImGui::Separator();
    // A real place with nothing loaded yet has no ground to put in: generating
    // it would only clear the scene.
    const bool nothing = realSrc && (!p.area || !s.planValid || s.plan.grid.n <= 1);
    ImGui::BeginDisabled(s.applying || nothing);
    if (ImGui::Button(s.applying ? "Generating..." : "Generate world...", ImVec2(-1.0f, 0.0f)))
        ImGui::OpenPopup("Generate world");
    ImGui::EndDisabled();
    if (nothing && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Load a square of the map first.");
    confirmPopup(s);

    if ((slid.released || now) && s.preview) s.preview();
    ImGui::End();
}

} // namespace worldui
