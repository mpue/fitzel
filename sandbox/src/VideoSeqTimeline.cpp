// The Video Sequencer's timeline (VideoSeqPanelState.hpp): the ruler, the track
// headers and the lanes with their clips -- drawn, and pressed, dragged and
// dropped on.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include <fitzel/ui/ImGuiTexture.hpp>

#include "UiStyle.hpp"
#include "VideoSeqPanelState.hpp"

namespace vsequi {

namespace fs = std::filesystem;

namespace {

constexpr ImU32 kPlayhead = IM_COL32(236, 72, 60, 255);
constexpr ImU32 kSnapLine = IM_COL32(250, 210, 80, 220);
constexpr ImU32 kLaneBg   = IM_COL32(30, 32, 37, 255);
constexpr ImU32 kLaneAlt  = IM_COL32(34, 36, 42, 255);

struct Geo {
    ImVec2 a, b;            // the whole timeline
    float  hw = 0, rh = 0;  // header width, ruler height
    float  lx0 = 0, lx1 = 0, ly0 = 0, ly1 = 0;   // the lanes
    float  sbH = 0;
    std::vector<float> rowY, rowH;               // per track (scrolled)
    float  contentH = 0;
    double pps = 1, view0 = 0;
    float  xOf(double t) const { return lx0 + static_cast<float>((t - view0) * pps); }
    double tOf(float x) const { return view0 + (x - lx0) / pps; }
    int    rowAt(float y) const {
        for (std::size_t i = 0; i < rowY.size(); ++i)
            if (y >= rowY[i] && y < rowY[i] + rowH[i]) return static_cast<int>(i);
        return -1;
    }
};

float rowHeight(const vs::Track& t) {
    const float em = ImGui::GetFontSize();
    return std::round(t.kind == vs::TrackKind::Video ? em * 3.4f : em * 2.6f);
}

// Rows of the same kind as `row`, in order: a clip moves among those.
std::vector<int> rowsOfKind(const vs::Film& f, vs::TrackKind k) {
    std::vector<int> r;
    for (std::size_t i = 0; i < f.tracks.size(); ++i)
        if (f.tracks[i].kind == k) r.push_back(static_cast<int>(i));
    return r;
}

bool fits(vs::ClipKind c, vs::TrackKind t) { return (c == vs::ClipKind::Audio) == (t == vs::TrackKind::Audio); }

// A nice step for the ruler's ticks: the smallest that leaves `minPx` between them.
double tickStep(double pps, double fps, double minPx) {
    static const double steps[] = {1.0 / 120, 1.0 / 60, 1.0 / 30, 1.0 / 15, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600};
    const double frame = 1.0 / std::max(1.0, fps);
    for (double s : steps) {
        if (s < frame * 0.999) continue;
        if (s * pps >= minPx) return s;
    }
    return 3600.0;
}

std::string rulerLabel(double t, double step, double fps) {
    char b[48];
    const int whole = static_cast<int>(std::floor(t + 1e-6));
    const int m = whole / 60, s = whole % 60;
    if (step < 1.0) {
        const int fr = static_cast<int>(std::llround((t - whole) * fps));
        if (fr == 0) std::snprintf(b, sizeof b, "%d:%02d", m, s);
        else         std::snprintf(b, sizeof b, "%d", fr);
    } else if (whole >= 3600) {
        std::snprintf(b, sizeof b, "%d:%02d:%02d", whole / 3600, m % 60, s);
    } else {
        std::snprintf(b, sizeof b, "%d:%02d", m, s);
    }
    return b;
}

// --- Drawing a clip -------------------------------------------------------------------------

void drawWave(ImDrawList* dl, const vs::Clip& c, const vs::Sound& snd, const Geo& g, ImVec2 a, ImVec2 b, ImU32 col) {
    if (snd.wave.empty()) return;
    const float mid = (a.y + b.y) * 0.5f, half = (b.y - a.y) * 0.5f - 1.0f;
    const float x0 = std::max(a.x, g.lx0), x1 = std::min(b.x, g.lx1);
    const ImU32 body = (col & 0x00FFFFFF) | 0xB0000000;
    const ImU32 edge = (col & 0x00FFFFFF) | 0xFF000000;
    for (float x = std::floor(x0); x < x1; x += 1.0f) {
        const double t0 = g.tOf(x), t1 = g.tOf(x + 1.0f);
        const float s0 = static_cast<float>(c.sourceAt(t0)), s1 = static_cast<float>(c.sourceAt(t1));
        if (s1 < 0.0f || s0 > snd.wave.seconds) continue;
        // On a square-root scale: a lane is a few dozen pixels, and a sound
        // mixed at a tenth of full scale would be a line on a straight one.
        const float pk = std::sqrt(std::min(1.0f, snd.wave.peakOver(std::max(0.0f, s0), s1) * c.volume));
        const float rm = std::sqrt(std::min(1.0f, snd.wave.rmsOver(std::max(0.0f, s0), s1) * c.volume));
        const float hp = std::min(half, pk * half), hr = std::min(half, rm * half);
        dl->AddRectFilled(ImVec2(x, mid - hp), ImVec2(x + 1.0f, mid + hp), edge);
        dl->AddRectFilled(ImVec2(x, mid - hr), ImVec2(x + 1.0f, mid + hr), body);
    }
}

void drawStrip(ImDrawList* dl, const vs::Clip& c, ThumbTex& tt, const Geo& g, ImVec2 a, ImVec2 b) {
    const vs::ThumbStrip& s = *tt.strip;
    if (!s.ready || !tt.tex.isValid() || s.count <= 0 || s.w <= 0 || s.h <= 0) return;
    const float h = b.y - a.y;
    if (h < 6.0f) return;
    const float tw = std::max(8.0f, h * static_cast<float>(s.w) / s.h);
    const ImTextureID id = fitzel::imTexture(tt.tex.id());
    const float aw = static_cast<float>(s.strip.w), ah = static_cast<float>(s.strip.h);
    // Tiles from the clip's own left edge, so they stay put as the view scrolls.
    const float start = a.x + std::floor(std::max(0.0f, g.lx0 - a.x) / tw) * tw;
    for (float x = start; x < std::min(b.x, g.lx1); x += tw) {
        int i = 0;
        if (c.kind == vs::ClipKind::Video && s.count > 1) {
            const double src = c.sourceAt(g.tOf(x + tw * 0.5f));
            i = std::clamp(static_cast<int>(std::floor(src / s.interval)), 0, s.count - 1);
        }
        const float u0 = (i % s.perRow) * s.w / aw, v0 = (i / s.perRow) * s.h / ah;
        const float u1 = u0 + s.w / aw, v1 = v0 + s.h / ah;
        const float xe = std::min(x + tw, b.x);
        const float uf = u0 + (u1 - u0) * ((xe - x) / tw);
        dl->AddImage(id, ImVec2(x, a.y), ImVec2(xe, b.y), ImVec2(u0, v0), ImVec2(uf, v1), IM_COL32(255, 255, 255, 210));
    }
}

void drawClip(Panel::State& st, ImDrawList* dl, const vs::Track& tr, std::size_t ci, const Geo& g, ImVec2 a, ImVec2 b,
              bool hovered) {
    const vs::Clip& c = tr.clips[ci];
    const bool sel = st.isSel(c.uid);
    const float em = ImGui::GetFontSize();
    const ImU32 col = clipColor(c);
    const ImVec4 cf = ImGui::ColorConvertU32ToFloat4(col);
    const ImU32 dark = ImGui::ColorConvertFloat4ToU32(ImVec4(cf.x * 0.42f, cf.y * 0.42f, cf.z * 0.42f, 1.0f));
    const ImU32 mid = ImGui::ColorConvertFloat4ToU32(ImVec4(cf.x * 0.7f, cf.y * 0.7f, cf.z * 0.7f, 1.0f));
    dl->AddRectFilled(a, b, dark, 3.0f);
    const float headH = std::min(b.y - a.y, std::round(em * 1.05f));
    const ImVec2 bodyA(a.x, a.y + headH);
    dl->AddRectFilled(a, ImVec2(b.x, a.y + headH), mid, 3.0f, ImDrawFlags_RoundCornersTop);

    std::shared_ptr<const vs::MediaInfo> info;
    if (!c.file.empty()) info = st.lib.info(c.file);
    const bool missing = info && !info->ok;
    dl->PushClipRect(ImVec2(std::max(a.x, g.lx0), std::max(a.y, g.ly0)), ImVec2(std::min(b.x, g.lx1), std::min(b.y, g.ly1)), true);
    switch (c.kind) {
    case vs::ClipKind::Video:
    case vs::ClipKind::Image:
        if (ThumbTex* tt = st.thumbsOf(c.file)) {
            const bool sound = c.kind == vs::ClipKind::Video && info && info->audio;
            const float wy = sound ? b.y - (b.y - bodyA.y) * 0.32f : b.y;
            drawStrip(dl, c, *tt, g, bodyA, ImVec2(b.x, wy));
            if (sound && !c.muted)
                if (auto snd = st.lib.sound(c.file); snd && snd->ready)
                    drawWave(dl, c, *snd, g, ImVec2(a.x, wy), b, IM_COL32(140, 200, 150, 255));
        }
        break;
    case vs::ClipKind::Audio:
        if (auto snd = st.lib.sound(c.file); snd && snd->ready)
            drawWave(dl, c, *snd, g, bodyA, b, vs::isSongFile(c.file) ? IM_COL32(250, 200, 120, 255) : IM_COL32(150, 230, 160, 255));
        break;
    case vs::ClipKind::Color:
        dl->AddRectFilled(ImVec2(a.x + 2, bodyA.y + 2), ImVec2(b.x - 2, b.y - 2),
                          ImGui::ColorConvertFloat4ToU32(ImVec4(c.color.r, c.color.g, c.color.b, 1.0f)), 2.0f);
        break;
    case vs::ClipKind::Text: {
        std::string first = c.text.text.substr(0, c.text.text.find('\n'));
        dl->AddText(ImVec2(a.x + 6.0f, bodyA.y + (b.y - bodyA.y - em) * 0.5f), IM_COL32(235, 225, 250, 255),
                    ("\"" + first + "\"").c_str());
        break;
    }
    }
    // Fades: the part of the clip that comes up out of nothing, shaded.
    auto fadeShade = [&](float x0, float x1, bool in) {
        if (x1 - x0 < 1.0f) return;
        dl->AddTriangleFilled(in ? ImVec2(x0, a.y) : ImVec2(x1, a.y), ImVec2(in ? x1 : x0, a.y),
                              in ? ImVec2(x0, b.y) : ImVec2(x1, b.y), IM_COL32(0, 0, 0, 120));
        dl->AddLine(in ? ImVec2(x0, b.y) : ImVec2(x1, b.y), ImVec2(in ? x1 : x0, a.y), IM_COL32(255, 255, 255, 150), 1.2f);
    };
    if (c.fadeIn > 0.0f) fadeShade(a.x, g.xOf(c.start + c.fadeIn), true);
    if (c.fadeOut > 0.0f) fadeShade(g.xOf(c.end() - c.fadeOut), b.x, false);
    // The overlap with the clip before it: the dissolve.
    if (ci > 0 && tr.clips[ci - 1].end() > c.start + 1e-6) {
        const float x0 = a.x, x1 = std::min(b.x, g.xOf(tr.clips[ci - 1].end()));
        dl->AddRectFilled(ImVec2(x0, a.y), ImVec2(x1, b.y), IM_COL32(255, 255, 255, 36));
        dl->AddLine(ImVec2(x0, a.y), ImVec2(x1, b.y), IM_COL32(255, 255, 255, 120), 1.0f);
        dl->AddLine(ImVec2(x0, b.y), ImVec2(x1, a.y), IM_COL32(255, 255, 255, 120), 1.0f);
        if (x1 - x0 > em * 4.5f) dl->AddText(ImVec2(x0 + 4.0f, b.y - em - 2.0f), IM_COL32(255, 255, 255, 200), "dissolve");
    }
    // The name, and what is unusual about the clip.
    std::string label = c.name.empty() ? fs::path(c.file).stem().string() : c.name;
    if (label.empty()) label = vs::kindName(c.kind);
    if (std::abs(c.speed - 1.0) > 1e-3) {
        char sp[24];
        std::snprintf(sp, sizeof sp, "  %.2gx", c.speed);
        label += sp;
    }
    if (c.kind == vs::ClipKind::Video && c.muted) label += "  (no sound)";
    if (c.animate) label += "  ~";
    if (missing) label = "missing: " + label;
    dl->AddText(ImVec2(std::max(a.x, g.lx0) + 4.0f, a.y + (headH - em) * 0.5f),
                missing ? IM_COL32(255, 150, 140, 255) : IM_COL32(240, 240, 245, 255), label.c_str());
    dl->PopClipRect();
    if (missing) dl->AddRect(a, b, IM_COL32(230, 80, 70, 255), 3.0f, 0, 1.5f);
    if (sel) dl->AddRect(a, b, IM_COL32(255, 255, 255, 240), 3.0f, 0, 2.0f);
    else if (hovered) dl->AddRect(a, b, IM_COL32(255, 255, 255, 90), 3.0f);
    // The fade handles, on the top corners of a clip under the pointer.
    if (hovered || sel) {
        const float hs = std::round(em * 0.32f);
        const float fx0 = g.xOf(c.start + c.fadeIn), fx1 = g.xOf(c.end() - c.fadeOut);
        dl->AddRectFilled(ImVec2(fx0 - hs + (c.fadeIn > 0 ? 0 : hs), a.y), ImVec2(fx0 + hs + (c.fadeIn > 0 ? 0 : hs), a.y + hs * 2),
                          IM_COL32(255, 255, 255, 220), 1.0f);
        dl->AddRectFilled(ImVec2(fx1 - hs - (c.fadeOut > 0 ? 0 : hs), a.y), ImVec2(fx1 + hs - (c.fadeOut > 0 ? 0 : hs), a.y + hs * 2),
                          IM_COL32(255, 255, 255, 220), 1.0f);
    }
}

// --- What is under the pointer ------------------------------------------------------------

struct Hit {
    enum Part { Nothing, Body, EdgeL, EdgeR, FadeIn, FadeOut };
    Part part = Nothing;
    int  uid = -1;
    int  row = -1;
};

Hit hitTest(Panel::State& st, const Geo& g, ImVec2 m) {
    Hit h;
    h.row = g.rowAt(m.y);
    if (h.row < 0 || m.x < g.lx0 || m.x > g.lx1) return h;
    const vs::Track& tr = st.film.tracks[static_cast<std::size_t>(h.row)];
    const float em = ImGui::GetFontSize();
    const float edge = std::max(5.0f, em * 0.4f), hs = std::round(em * 0.32f) * 2.0f + 2.0f;
    // Later clips are drawn on top: test them first.
    for (std::size_t i = tr.clips.size(); i-- > 0;) {
        const vs::Clip& c = tr.clips[i];
        const float x0 = g.xOf(c.start), x1 = g.xOf(c.end());
        if (m.x < x0 - 2.0f || m.x > x1 + 2.0f) continue;
        h.uid = c.uid;
        const float y0 = g.rowY[static_cast<std::size_t>(h.row)] + 1.0f;
        const float fx0 = g.xOf(c.start + c.fadeIn), fx1 = g.xOf(c.end() - c.fadeOut);
        const bool top = m.y < y0 + hs;
        const bool roomy = x1 - x0 > edge * 4.0f;
        if (top && roomy && std::abs(m.x - (c.fadeIn > 0 ? fx0 : x0 + hs * 0.5f)) < hs * 0.6f) h.part = Hit::FadeIn;
        else if (top && roomy && std::abs(m.x - (c.fadeOut > 0 ? fx1 : x1 - hs * 0.5f)) < hs * 0.6f) h.part = Hit::FadeOut;
        else if (m.x < x0 + edge) h.part = Hit::EdgeL;
        else if (m.x > x1 - edge) h.part = Hit::EdgeR;
        else h.part = Hit::Body;
        return h;
    }
    return h;
}

// --- Snapping ------------------------------------------------------------------------------------

// The cut points a drag can land on: every clip's ends but the dragged ones',
// the playhead, the range's ends.
std::vector<double> snapPoints(const Panel::State& st, const vs::Film& base, const std::vector<int>& except) {
    std::vector<double> p{0.0, st.playhead};
    if (base.markIn >= 0) p.push_back(base.markIn);
    if (base.markOut >= 0) p.push_back(base.markOut);
    for (const vs::Track& t : base.tracks)
        for (const vs::Clip& c : t.clips) {
            if (std::find(except.begin(), except.end(), c.uid) != except.end()) continue;
            p.push_back(c.start);
            p.push_back(c.end());
        }
    return p;
}

// `t` onto the nearest snap point within `px` pixels (and a frame anyway).
// `snapped`: where it landed, -1 when nowhere.
double snapped(const Panel::State& st, const std::vector<double>& pts, double t, double pps, double& at, bool on) {
    at = -1.0;
    if (on && !ImGui::GetIO().KeyCtrl) {
        const double tol = 9.0 / std::max(1e-6, pps);
        double best = tol;
        for (double p : pts)
            if (std::abs(p - t) < best) {
                best = std::abs(p - t);
                at = p;
            }
        if (at >= 0.0) return at;
    }
    return st.film.snap(t);
}

// --- The parts -------------------------------------------------------------------------------------

void ruler(Panel::State& st, const Geo& g) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(g.lx0, g.a.y), b(g.lx1, g.a.y + g.rh);
    dl->AddRectFilled(ImVec2(g.a.x, g.a.y), ImVec2(g.b.x, b.y), IM_COL32(24, 26, 30, 255));
    const float em = ImGui::GetFontSize();
    // The export range.
    if (st.film.markIn >= 0 || st.film.markOut >= 0) {
        const double i0 = std::max(0.0, st.film.markIn), o1 = st.film.markOut >= 0 ? st.film.markOut : st.end();
        const float x0 = std::max(a.x, g.xOf(i0)), x1 = std::min(b.x, g.xOf(o1));
        if (x1 > x0) {
            dl->AddRectFilled(ImVec2(x0, a.y), ImVec2(x1, b.y), IM_COL32(70, 130, 220, 70));
            dl->AddRectFilled(ImVec2(x0, b.y - 4.0f), ImVec2(x1, b.y), IM_COL32(90, 150, 240, 220));
        }
    }
    dl->PushClipRect(a, b, true);
    const double step = tickStep(g.pps, st.film.fps, em * 5.0);
    const double minor = tickStep(g.pps, st.film.fps, em * 0.7);
    const double t0 = std::floor(g.tOf(a.x) / minor) * minor, t1 = g.tOf(b.x);
    for (double t = std::max(0.0, t0); t <= t1; t += minor) {
        const bool major = std::abs(std::remainder(t, step)) < minor * 0.25;
        const float x = std::round(g.xOf(t)) + 0.5f;
        dl->AddLine(ImVec2(x, major ? a.y + em * 0.9f : b.y - em * 0.35f), ImVec2(x, b.y), IM_COL32(150, 155, 165, major ? 200 : 110));
        if (major) dl->AddText(ImVec2(x + 3.0f, a.y + 1.0f), IM_COL32(200, 205, 215, 255), rulerLabel(t, step, st.film.fps).c_str());
    }
    dl->PopClipRect();

    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##ruler", ImVec2(std::max(1.0f, b.x - a.x), g.rh));
    if (st.host && st.host->probe) st.host->probe("ruler", a, b);
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActivated()) {
        st.drag = {};
        st.drag.mode = Drag::Scrub;
        if (st.playing) st.stop();
    }
    if (st.drag.mode == Drag::Scrub && ImGui::IsItemActive()) {
        st.seek(std::clamp(st.film.snap(g.tOf(io.MousePos.x)), 0.0, st.end() + 60.0));
    }
    if (st.drag.mode == Drag::Scrub && !ImGui::IsItemActive()) st.drag.mode = Drag::None;
    if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("Click or drag: move the playhead.  I / O: the export range");
    if (ImGui::BeginPopupContextItem("##rulerMenu")) {
        const double t = st.film.snap(g.tOf(ImGui::GetMousePosOnOpeningCurrentPopup().x));
        if (ImGui::MenuItem("Range starts here", "I")) { st.checkpoint(); st.film.markIn = t; if (st.film.markOut >= 0 && st.film.markOut <= t) st.film.markOut = -1; st.edited(); }
        if (ImGui::MenuItem("Range ends here", "O")) { st.checkpoint(); st.film.markOut = t; if (st.film.markIn >= t) st.film.markIn = -1; st.edited(); }
        if (ImGui::MenuItem("No range", "Alt+X", false, st.film.markIn >= 0 || st.film.markOut >= 0)) { st.checkpoint(); st.film.markIn = st.film.markOut = -1; st.edited(); }
        ImGui::EndPopup();
    }
}

void headers(Panel::State& st, const Host& h, const Geo& g) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float em = ImGui::GetFontSize();
    dl->PushClipRect(ImVec2(g.a.x, g.ly0), ImVec2(g.a.x + g.hw, g.ly1), true);
    int deleteRow = -1, addKind = -1;
    for (std::size_t r = 0; r < st.film.tracks.size(); ++r) {
        vs::Track& t = st.film.tracks[r];
        const float y0 = g.rowY[r], y1 = y0 + g.rowH[r];
        if (y1 < g.ly0 || y0 > g.ly1) continue;
        const ImVec2 a(g.a.x, y0), b(g.a.x + g.hw - 2.0f, y1 - 1.0f);
        const bool selT = st.selTrack == t.uid;
        dl->AddRectFilled(a, b, selT ? IM_COL32(52, 58, 70, 255) : IM_COL32(40, 43, 50, 255));
        dl->AddRectFilled(a, ImVec2(a.x + 4.0f, b.y), t.kind == vs::TrackKind::Video ? IM_COL32(70, 130, 220, 255) : IM_COL32(80, 180, 100, 255));
        ImGui::PushID(t.uid);
        dl->AddText(ImVec2(a.x + 10.0f, y0 + 4.0f), IM_COL32(230, 232, 238, 255), t.name.c_str());
        // The toggles: what is seen, what is heard, what may move.
        const float bs = std::round(em * 1.35f);
        float x = b.x - bs - 4.0f;
        // Each a little drawing: an eye, a speaker, a padlock -- struck
        // through (or closed) when it is on.
        enum Icon { Eye, Speaker, Lock };
        auto toggle = [&](const char* id, bool& v, Icon icon, const char* tip) {
            ImGui::SetCursorScreenPos(ImVec2(x, y0 + 3.0f));
            ImGui::PushStyleColor(ImGuiCol_Button, v ? ImVec4(0.62f, 0.27f, 0.22f, 1.0f) : ImVec4(0.22f, 0.24f, 0.28f, 1.0f));
            if (ImGui::Button(id, ImVec2(bs, bs))) {
                st.checkpoint();
                v = !v;
                st.edited();
            }
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", tip);
            const ImVec2 c((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                           (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
            const float r = bs * 0.3f;
            const ImU32 ink = v ? IM_COL32(255, 235, 230, 255) : IM_COL32(200, 205, 215, 255);
            switch (icon) {
            case Eye:
                dl->AddBezierQuadratic(ImVec2(c.x - r * 1.2f, c.y), ImVec2(c.x, c.y - r * 1.3f), ImVec2(c.x + r * 1.2f, c.y), ink, 1.4f);
                dl->AddBezierQuadratic(ImVec2(c.x - r * 1.2f, c.y), ImVec2(c.x, c.y + r * 1.3f), ImVec2(c.x + r * 1.2f, c.y), ink, 1.4f);
                dl->AddCircleFilled(c, r * 0.38f, ink);
                break;
            case Speaker:
                dl->AddRectFilled(ImVec2(c.x - r, c.y - r * 0.35f), ImVec2(c.x - r * 0.45f, c.y + r * 0.35f), ink);
                dl->AddTriangleFilled(ImVec2(c.x - r * 0.5f, c.y), ImVec2(c.x + r * 0.15f, c.y - r * 0.85f), ImVec2(c.x + r * 0.15f, c.y + r * 0.85f), ink);
                if (!v) dl->PathArcTo(ImVec2(c.x + r * 0.15f, c.y), r * 0.75f, -0.8f, 0.8f), dl->PathStroke(ink, 0, 1.3f);
                break;
            case Lock:
                dl->AddRectFilled(ImVec2(c.x - r * 0.75f, c.y - r * 0.1f), ImVec2(c.x + r * 0.75f, c.y + r * 0.85f), ink, 1.5f);
                dl->PathArcTo(ImVec2(c.x, c.y - r * 0.1f), r * 0.5f, 3.14159f, v ? 6.2832f : 5.4f);
                dl->PathStroke(ink, 0, 1.4f);
                break;
            }
            if (v && icon != Lock) dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), ink, 1.6f);
            x -= bs + 3.0f;
        };
        toggle("##lock", t.locked, Lock, t.locked ? "Locked: nothing on it moves (click to unlock)" : "Lock the track");
        toggle("##mute", t.muted, Speaker, t.muted ? "Silent (click to hear it)" : "Mute the track's sound");
        if (t.kind == vs::TrackKind::Video)
            toggle("##hide", t.hidden, Eye, t.hidden ? "Hidden (click to show it)" : "Hide the track's pictures");
        // The rest of the header selects the track (where paste and new clips go).
        ImGui::SetCursorScreenPos(ImVec2(a.x + 5.0f, y0 + bs + 5.0f));
        ImGui::InvisibleButton("##hdr", ImVec2(std::max(1.0f, g.hw - 12.0f), std::max(1.0f, g.rowH[r] - bs - 7.0f)));
        if (h.probe) h.probe("header:" + std::to_string(t.uid), a, b);
        if (ImGui::IsItemClicked()) st.selTrack = t.uid;
        if (t.volume != 1.0f) {
            char v[24];
            std::snprintf(v, sizeof v, "%.0f%%", t.volume * 100.0f);
            dl->AddText(ImVec2(a.x + 10.0f, y0 + em * 1.4f), IM_COL32(170, 175, 185, 255), v);
        }
        if (ImGui::BeginPopupContextItem("##trackMenu")) {
            static char nameBuf[64];
            if (ImGui::IsWindowAppearing()) std::snprintf(nameBuf, sizeof nameBuf, "%s", t.name.c_str());
            ImGui::SetNextItemWidth(em * 10.0f);
            if (ImGui::InputText("Name", nameBuf, sizeof nameBuf, ImGuiInputTextFlags_EnterReturnsTrue)) {
                st.checkpoint();
                t.name = nameBuf;
                st.edited();
            }
            float vol = t.volume * 100.0f;
            if (ui::stepper("vol", vol, 5.0f, 0.0f, 400.0f, "%.0f %%")) {
                st.checkpoint();
                t.volume = vol / 100.0f;
                st.edited();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("Volume");
            ImGui::Separator();
            if (ImGui::MenuItem("Add a video track")) addKind = 0;
            if (ImGui::MenuItem("Add an audio track")) addKind = 1;
            if (ImGui::MenuItem(t.clips.empty() ? "Delete the track" : "Delete the track and its clips")) deleteRow = static_cast<int>(r);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    dl->PopClipRect();
    if (addKind >= 0) {
        st.checkpoint();
        st.selTrack = st.film.addTrack(addKind == 0 ? vs::TrackKind::Video : vs::TrackKind::Audio);
        st.edited();
    }
    if (deleteRow >= 0) {
        st.checkpoint();
        st.film.tracks.erase(st.film.tracks.begin() + deleteRow);
        if (st.film.tracks.empty()) st.film.addTrack(vs::TrackKind::Video);
        st.edited();
    }
}

// The film as it would be with the drag applied to its base.
void applyDrag(Panel::State& st, const Geo& g, ImVec2 m) {
    Drag& d = st.drag;
    vs::Film f = d.base;
    const double t = g.tOf(m.x);
    double snapAt = -1.0;
    const double frame = f.frame();
    if (d.mode == Drag::Move) {
        const vs::Clip* anchor = d.base.clip(d.anchor);
        if (!anchor) return;
        const std::vector<double> pts = snapPoints(st, d.base, d.items);
        // Snap whichever end of the pressed clip lands nearer a cut.
        const double raw = anchor->start + (t - d.pressT);
        double s1 = -1, s2 = -1;
        const double a1 = snapped(st, pts, raw, g.pps, s1, st.snap);
        const double a2 = snapped(st, pts, raw + anchor->length, g.pps, s2, st.snap) - anchor->length;
        double newStart = a1;
        if (s2 >= 0 && (s1 < 0 || std::abs(a2 - raw) < std::abs(a1 - raw))) { newStart = a2; snapAt = s2; }
        else snapAt = s1;
        double dt = newStart - anchor->start;
        // Nothing before zero.
        for (int u : d.items)
            if (const vs::Clip* c = d.base.clip(u)) dt = std::max(dt, -c->start);
        dt = f.snap(dt);
        // Rows: among tracks of the clip's own kind, the same step for all.
        const int under = g.rowAt(std::clamp(m.y, g.ly0, g.ly1 - 1.0f));
        int step = 0;
        if (under >= 0 && d.row >= 0) {
            const vs::TrackKind k = d.base.tracks[static_cast<std::size_t>(d.row)].kind;
            const std::vector<int> rows = rowsOfKind(d.base, k);
            const auto from = std::find(rows.begin(), rows.end(), d.row);
            auto to = std::find(rows.begin(), rows.end(), under);
            if (to == rows.end()) to = under < d.row ? rows.begin() : rows.end() - 1;   // pulled into the other kind's tracks
            step = static_cast<int>(to - from);
            for (int u : d.items) {
                const vs::Track* tr = d.base.trackOf(u);
                if (!tr) continue;
                const std::vector<int> rk = rowsOfKind(d.base, tr->kind);
                const int idx = static_cast<int>(std::find(rk.begin(), rk.end(), d.base.rowOf(tr->uid)) - rk.begin());
                step = std::clamp(step, -idx, static_cast<int>(rk.size()) - 1 - idx);
            }
        }
        // The clips picked when the press began (with Alt, the selection
        // becomes the copies; the originals are still what is dragged).
        std::vector<int> newSel;
        for (int u : d.items) {
            const vs::Track* tr = d.base.trackOf(u);
            const vs::Clip* c = d.base.clip(u);
            if (!tr || !c || tr->locked) { newSel.push_back(u); continue; }
            const std::vector<int> rk = rowsOfKind(d.base, tr->kind);
            const int idx = static_cast<int>(std::find(rk.begin(), rk.end(), d.base.rowOf(tr->uid)) - rk.begin());
            const int toRow = rk[static_cast<std::size_t>(std::clamp(idx + step, 0, static_cast<int>(rk.size()) - 1))];
            vs::Clip moved = *c;
            moved.start = f.snap(c->start + dt);
            if (d.copy) moved.uid = f.newUid();
            else f.removeClip(u);
            if (f.tracks[static_cast<std::size_t>(toRow)].locked) {
                // A locked track takes nothing: back where it was.
                if (!d.copy) f.addClip(tr->uid, *c);
                continue;
            }
            newSel.push_back(f.addClip(f.tracks[static_cast<std::size_t>(toRow)].uid, moved));
        }
        if (d.copy) st.sel = newSel;
        st.snapShown = snapAt;
        st.film = std::move(f);
        st.edited();
        return;
    }
    vs::Clip* c = f.clip(d.anchor);
    const vs::Clip* b0 = d.base.clip(d.anchor);
    if (!c || !b0) return;
    std::vector<double> pts = snapPoints(st, d.base, {d.anchor});
    const double tt = snapped(st, pts, t, g.pps, snapAt, st.snap);
    const double srcLen = st.sourceSeconds(*b0);
    if (d.mode == Drag::TrimL) {
        double lo = 0.0;
        if (srcLen >= 0.0) lo = std::max(lo, b0->start - b0->in / b0->speed);
        const double ns = std::clamp(tt, lo, b0->end() - frame);
        c->start  = ns;
        c->length = b0->end() - ns;
        c->in     = std::max(0.0, b0->in + (ns - b0->start) * b0->speed);
    } else if (d.mode == Drag::TrimR) {
        double hi = 1e9;
        if (srcLen >= 0.0) hi = b0->start + std::max(frame, (srcLen - b0->in) / b0->speed);
        const double ne = std::clamp(tt, b0->start + frame, hi);
        c->length = ne - b0->start;
    } else if (d.mode == Drag::FadeIn) {
        c->fadeIn = static_cast<float>(std::clamp(tt - b0->start, 0.0, b0->length - b0->fadeOut));
    } else if (d.mode == Drag::FadeOut) {
        c->fadeOut = static_cast<float>(std::clamp(b0->end() - tt, 0.0, b0->length - b0->fadeIn));
    }
    c->fadeIn  = std::min(c->fadeIn, static_cast<float>(c->length));
    c->fadeOut = std::min(c->fadeOut, static_cast<float>(c->length));
    f.sortClips();
    st.snapShown = snapAt;
    st.film = std::move(f);
    st.edited();
}

void laneMenu(Panel::State& st, int row, double t) {
    if (row >= 0 && row < static_cast<int>(st.film.tracks.size())) {
        const vs::Track& tr = st.film.tracks[static_cast<std::size_t>(row)];
        if (ImGui::MenuItem("Paste here", "Ctrl+V", false, !st.board.empty())) {
            st.selTrack = tr.uid;
            pasteClips(st, t);
        }
        if (tr.kind == vs::TrackKind::Video) {
            if (ImGui::MenuItem("Add a title here")) addMadeClip(st, vs::ClipKind::Text, tr.uid, t);
            if (ImGui::MenuItem("Add a colour here")) addMadeClip(st, vs::ClipKind::Color, tr.uid, t);
        }
        ImGui::Separator();
    }
    if (ImGui::MenuItem("Add a video track")) { st.checkpoint(); st.film.addTrack(vs::TrackKind::Video); st.edited(); }
    if (ImGui::MenuItem("Add an audio track")) { st.checkpoint(); st.film.addTrack(vs::TrackKind::Audio); st.edited(); }
}

void clipMenu(Panel::State& st, const Host& h, int uid) {
    vs::Clip* c = st.film.clip(uid);
    if (!c) return;
    if (!st.isSel(uid)) st.select(uid, false);
    const bool inside = st.playhead > c->start && st.playhead < c->end();
    if (ImGui::MenuItem("Cut at the playhead", "S", false, inside)) splitAt(st, st.playhead, {uid});
    if (ImGui::MenuItem("Copy", "Ctrl+C")) copyClips(st);
    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicateClips(st);
    if (ImGui::MenuItem("Delete", "Del")) deleteClips(st, st.sel, false);
    if (ImGui::MenuItem("Delete and close the gap", "Shift+Del")) deleteClips(st, st.sel, true);
    ImGui::Separator();
    if (c->kind == vs::ClipKind::Video || c->kind == vs::ClipKind::Audio) {
        if (ImGui::BeginMenu("Speed")) {
            for (double s : {0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 4.0, 8.0}) {
                char lbl[16];
                std::snprintf(lbl, sizeof lbl, "%gx", s);
                if (ImGui::MenuItem(lbl, nullptr, std::abs(c->speed - s) < 1e-6)) {
                    st.checkpoint();
                    // The same piece of the source, played faster or slower: the
                    // clip gets shorter or longer.
                    const double src = c->length * c->speed;
                    c->speed = s;
                    c->length = st.film.snap(std::max(st.film.frame(), src / s));
                    st.film.sortClips();
                    st.edited();
                }
            }
            ImGui::EndMenu();
        }
    }
    if (c->kind == vs::ClipKind::Video) {
        if (ImGui::MenuItem("Sound off", nullptr, c->muted)) { st.checkpoint(); c->muted = !c->muted; st.edited(); }
        auto mi = st.lib.info(c->file);
        if (ImGui::MenuItem("Sound onto its own track", nullptr, false, mi && mi->audio && !c->muted)) detachSound(st, uid);
    }
    if (ImGui::MenuItem("Fade in and out (half a second)")) {
        st.checkpoint();
        for (int u : st.sel)
            if (vs::Clip* s = st.film.clip(u)) {
                s->fadeIn = s->fadeOut = static_cast<float>(std::min(0.5, s->length / 2));
            }
        st.edited();
    }
    if (ImGui::MenuItem("No fades")) {
        st.checkpoint();
        for (int u : st.sel)
            if (vs::Clip* s = st.film.clip(u)) s->fadeIn = s->fadeOut = 0.0f;
        st.edited();
    }
    if (vs::isSongFile(c->file)) {
        ImGui::Separator();
        if (ImGui::MenuItem("Open in the Song Sequencer", nullptr, false, static_cast<bool>(h.openSong)))
            h.openSong(fs::path(c->file).stem().string());
    }
}

} // namespace

// --- The timeline --------------------------------------------------------------------------------

void timeline(Panel::State& st, const Host& h, ImVec2 size) {
    const float em = ImGui::GetFontSize();
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Geo g;
    g.a = ImGui::GetCursorScreenPos();
    g.b = ImVec2(g.a.x + size.x, g.a.y + size.y);
    g.hw = std::round(em * 10.5f);
    g.rh = std::round(em * 1.8f);
    g.sbH = std::round(em * 0.75f);
    g.lx0 = g.a.x + g.hw;
    g.lx1 = g.b.x - 2.0f;
    g.ly0 = g.a.y + g.rh;
    g.ly1 = g.b.y - g.sbH - 2.0f;
    const float laneW = std::max(40.0f, g.lx1 - g.lx0);
    st.laneW = laneW;
    const double span = std::max(st.end(), 5.0);
    if (st.pps <= 0.0) {
        st.pps = laneW / (span * 1.05);
        st.view0 = 0.0;
    }
    st.pps = std::clamp(st.pps, laneW / (span * 4.0 + 60.0), st.film.fps * 60.0);
    st.view0 = std::clamp(st.view0, 0.0, std::max(0.0, span * 1.5 - laneW / st.pps));
    g.pps = st.pps;
    g.view0 = st.view0;
    float y = g.ly0 - st.scrollY;
    for (const vs::Track& t : st.film.tracks) {
        g.rowY.push_back(y);
        g.rowH.push_back(rowHeight(t));
        y += rowHeight(t);
    }
    g.contentH = y + st.scrollY - g.ly0;
    st.scrollY = std::clamp(st.scrollY, 0.0f, std::max(0.0f, g.contentH - (g.ly1 - g.ly0)));

    dl->AddRectFilled(g.a, g.b, IM_COL32(22, 24, 28, 255));
    // The lanes' floor, then the clips.
    dl->PushClipRect(ImVec2(g.lx0, g.ly0), ImVec2(g.lx1, g.ly1), true);
    for (std::size_t r = 0; r < st.film.tracks.size(); ++r) {
        const vs::Track& t = st.film.tracks[r];
        dl->AddRectFilled(ImVec2(g.lx0, g.rowY[r]), ImVec2(g.lx1, g.rowY[r] + g.rowH[r] - 1.0f), r % 2 ? kLaneAlt : kLaneBg);
        if (t.locked) {
            for (float x = g.lx0 - std::fmod(g.lx0, 14.0f); x < g.lx1; x += 14.0f)
                dl->AddLine(ImVec2(x, g.rowY[r] + g.rowH[r] - 1.0f), ImVec2(x + g.rowH[r], g.rowY[r]), IM_COL32(255, 255, 255, 10));
        }
        if (h.probe) h.probe("lane:" + std::to_string(t.uid), ImVec2(g.lx0, g.rowY[r]), ImVec2(g.lx1, g.rowY[r] + g.rowH[r]));
    }
    // The range, through the lanes.
    if (st.film.markIn >= 0 || st.film.markOut >= 0) {
        const double i0 = std::max(0.0, st.film.markIn), o1 = st.film.markOut >= 0 ? st.film.markOut : st.end();
        dl->AddRectFilled(ImVec2(g.xOf(i0), g.ly0), ImVec2(g.xOf(o1), g.ly1), IM_COL32(70, 130, 220, 18));
    }
    const ImVec2 m = io.MousePos;
    const bool overLanes = m.x >= g.lx0 && m.x < g.lx1 && m.y >= g.ly0 && m.y < g.ly1 && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    const Hit hover = overLanes && st.drag.mode == Drag::None ? hitTest(st, g, m) : Hit{};
    for (std::size_t r = 0; r < st.film.tracks.size(); ++r) {
        const vs::Track& t = st.film.tracks[r];
        const float y0 = g.rowY[r] + 1.0f, y1 = g.rowY[r] + g.rowH[r] - 2.0f;
        if (y1 < g.ly0 || y0 > g.ly1) continue;
        for (std::size_t ci = 0; ci < t.clips.size(); ++ci) {
            const vs::Clip& c = t.clips[ci];
            const float x0 = g.xOf(c.start), x1 = std::max(x0 + 2.0f, g.xOf(c.end()));
            if (x1 < g.lx0 || x0 > g.lx1) continue;
            drawClip(st, dl, t, ci, g, ImVec2(x0, y0), ImVec2(x1, y1), hover.uid == c.uid);
            if (h.probe) h.probe("clip:" + std::to_string(c.uid), ImVec2(x0, y0), ImVec2(x1, y1));
        }
    }
    dl->PopClipRect();

    // Presses on the lanes.
    ImGui::SetCursorScreenPos(ImVec2(g.lx0, g.ly0));
    ImGui::InvisibleButton("##lanes", ImVec2(laneW, std::max(1.0f, g.ly1 - g.ly0)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    if (h.probe) h.probe("lanes", ImVec2(g.lx0, g.ly0), ImVec2(g.lx1, g.ly1));
    const bool lanesHovered = ImGui::IsItemHovered();
    st.lanesA = ImVec2(g.lx0, g.ly0);
    st.lanesB = ImVec2(g.lx1, g.ly1);
    if (lanesHovered && st.drag.mode == Drag::None) {
        if (hover.part == Hit::EdgeL || hover.part == Hit::EdgeR) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        else if (hover.part == Hit::FadeIn || hover.part == Hit::FadeOut) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const Hit hit = hitTest(st, g, m);
        st.drag = {};
        st.drag.base   = st.film;
        st.drag.press  = m;
        st.drag.pressT = g.tOf(m.x);
        st.drag.row    = hit.row;
        st.drag.anchor = hit.uid;
        const vs::Track* tr = hit.uid >= 0 ? st.film.trackOf(hit.uid) : nullptr;
        if (hit.row >= 0) st.selTrack = st.film.tracks[static_cast<std::size_t>(hit.row)].uid;
        if (hit.uid >= 0 && tr && !tr->locked) {
            const bool add = io.KeyCtrl || io.KeyShift;
            if (!st.isSel(hit.uid)) st.select(hit.uid, add);
            switch (hit.part) {
            case Hit::EdgeL:   st.drag.mode = Drag::TrimL; break;
            case Hit::EdgeR:   st.drag.mode = Drag::TrimR; break;
            case Hit::FadeIn:  st.drag.mode = Drag::FadeIn; break;
            case Hit::FadeOut: st.drag.mode = Drag::FadeOut; break;
            default:           st.drag.mode = Drag::Move; st.drag.copy = io.KeyAlt; break;
            }
            if (st.drag.mode != Drag::Move) st.select(hit.uid, false);
            st.drag.items = st.sel;
        } else if (hit.uid >= 0) {
            st.select(hit.uid, io.KeyCtrl || io.KeyShift);   // on a locked track: picked, not moved
        } else {
            st.drag.mode = Drag::Rubber;
            st.drag.before = (io.KeyCtrl || io.KeyShift) ? st.sel : std::vector<int>{};
            if (!io.KeyCtrl && !io.KeyShift) st.sel.clear();
        }
    }
    if (ImGui::IsItemActive() && st.drag.mode != Drag::None && st.drag.mode != Drag::Scrub) {
        const float dx = m.x - st.drag.press.x, dy = m.y - st.drag.press.y;
        if (!st.drag.moved && dx * dx + dy * dy > 9.0f) st.drag.moved = true;
        if (st.drag.moved) {
            if (st.drag.mode == Drag::Rubber) {
                const ImVec2 ra(std::min(st.drag.press.x, m.x), std::min(st.drag.press.y, m.y));
                const ImVec2 rb(std::max(st.drag.press.x, m.x), std::max(st.drag.press.y, m.y));
                dl->AddRectFilled(ra, rb, IM_COL32(110, 160, 240, 40));
                dl->AddRect(ra, rb, IM_COL32(110, 160, 240, 200));
                st.sel = st.drag.before;
                for (std::size_t r = 0; r < st.film.tracks.size(); ++r) {
                    const float y0 = g.rowY[r], y1 = y0 + g.rowH[r];
                    if (y1 < ra.y || y0 > rb.y || st.film.tracks[r].locked) continue;
                    for (const vs::Clip& c : st.film.tracks[r].clips)
                        if (g.xOf(c.end()) >= ra.x && g.xOf(c.start) <= rb.x && !st.isSel(c.uid)) st.sel.push_back(c.uid);
                }
            } else {
                applyDrag(st, g, m);
            }
            // Pulled past an edge: the view follows.
            if (m.x > g.lx1 - em) st.view0 += (m.x - (g.lx1 - em)) * 0.05 / st.pps * 10.0;
            if (m.x < g.lx0 + em) st.view0 = std::max(0.0, st.view0 - ((g.lx0 + em) - m.x) * 0.05 / st.pps * 10.0);
        }
    }
    if (ImGui::IsItemDeactivated() && st.drag.mode != Drag::None && st.drag.mode != Drag::Scrub) {
        if (st.drag.moved && st.drag.mode != Drag::Rubber) {
            st.checkpoint(st.drag.base);
        } else if (!st.drag.moved && st.drag.mode == Drag::Move && !io.KeyCtrl && !io.KeyShift) {
            st.select(st.drag.anchor, false);   // a click on one of several: just that one
        } else if (!st.drag.moved && st.drag.mode == Drag::Rubber) {
            // A click on an empty lane: the playhead goes there.
            if (!io.KeyCtrl && !io.KeyShift) st.seek(std::max(0.0, st.film.snap(st.drag.pressT)));
        }
        st.drag.mode = Drag::None;
        st.snapShown = -1.0;
    }
    // What a drag is doing, by the pointer: where the clip starts (and by
    // how much), how long it is, how long the fade.
    if (st.drag.moved && st.drag.mode != Drag::Rubber && st.drag.mode != Drag::None && st.drag.mode != Drag::Scrub) {
        const vs::Clip* c = st.film.clip(st.drag.mode == Drag::Move && st.drag.copy && !st.sel.empty() ? st.sel.front() : st.drag.anchor);
        const vs::Clip* b0 = st.drag.base.clip(st.drag.anchor);
        if (c && b0) {
            ImGui::BeginTooltip();
            const double fps = st.film.fps;
            switch (st.drag.mode) {
            case Drag::Move:
                ImGui::Text("%s  (%+.2f s)%s", timecode(c->start, fps).c_str(), c->start - b0->start, st.drag.copy ? "  copy" : "");
                break;
            case Drag::TrimL:
            case Drag::TrimR:
                ImGui::Text("%s - %s, %.2f s", timecode(c->start, fps).c_str(), timecode(c->end(), fps).c_str(), c->length);
                break;
            case Drag::FadeIn:  ImGui::Text("fade in %.2f s", c->fadeIn); break;
            case Drag::FadeOut: ImGui::Text("fade out %.2f s", c->fadeOut); break;
            default: break;
            }
            ImGui::EndTooltip();
        }
    } else if (lanesHovered && st.drag.mode == Drag::None && hover.uid >= 0 && ImGui::GetIO().MouseDelta.x == 0.0f &&
               ImGui::GetIO().MouseDelta.y == 0.0f) {
        if (const vs::Clip* c = st.film.clip(hover.uid); c && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(c->name.empty() ? vs::kindName(c->kind) : c->name.c_str());
            ImGui::TextDisabled("%s - %s  (%.2f s)", timecode(c->start, st.film.fps).c_str(), timecode(c->end(), st.film.fps).c_str(), c->length);
            if (!c->file.empty()) ImGui::TextDisabled("%s, from %.2f s%s", c->file.c_str(), c->in,
                                                      std::abs(c->speed - 1.0) > 1e-3 ? (", at " + std::to_string(c->speed).substr(0, 4) + "x").c_str() : "");
            ImGui::TextDisabled("Drag: move   edges: trim   top corners: fade   Alt: copy   right: menu");
            ImGui::EndTooltip();
        }
    }
    // Right button: the clip's menu, or the lane's.
    static int menuUid = -1, menuRow = -1;
    static double menuT = 0.0;
    if (lanesHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && st.drag.mode == Drag::None) {
        const Hit hit = hitTest(st, g, m);
        menuUid = hit.uid;
        menuRow = hit.row;
        menuT = st.film.snap(std::max(0.0, g.tOf(m.x)));
        ImGui::OpenPopup(hit.uid >= 0 ? "##clipMenu" : "##laneMenu");
    }
    if (ImGui::BeginPopup("##clipMenu")) {
        clipMenu(st, h, menuUid);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##laneMenu")) {
        laneMenu(st, menuRow, menuT);
        ImGui::EndPopup();
    }
    // Media dropped on a lane: a clip where it fell.
    if (ImGui::BeginDragDropTarget()) {
        const int row = g.rowAt(m.y);
        const double at = st.film.snap(std::max(0.0, g.tOf(m.x)));
        std::string file;
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("VSEQ_MEDIA", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
            const int i = *static_cast<const int*>(p->Data);
            if (i >= 0 && i < static_cast<int>(st.film.media.size())) file = st.film.media[static_cast<std::size_t>(i)];
            if (p->IsDelivery() && !file.empty())
                addMediaClip(st, file, row >= 0 ? st.film.tracks[static_cast<std::size_t>(row)].uid : -1, at);
        }
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_GUID", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
            const std::string guid(static_cast<const char*>(p->Data), static_cast<std::size_t>(p->DataSize));
            file = h.fileOfAsset ? h.fileOfAsset(guid) : std::string();
            if (p->IsDelivery() && !file.empty()) {
                bringIn(st, file);
                addMediaClip(st, file, row >= 0 ? st.film.tracks[static_cast<std::size_t>(row)].uid : -1, at);
            }
        }
        if (!file.empty() && row >= 0) {
            // Where it would land, as long as it is.
            const float x = g.xOf(at);
            double len = 5.0;
            if (auto mi = st.lib.info(file); mi && mi->ok && !mi->still && mi->duration > 0.0) len = mi->duration;
            dl->AddRectFilled(ImVec2(x, g.rowY[static_cast<std::size_t>(row)]),
                              ImVec2(std::max(x + 4.0f, g.xOf(at + len)), g.rowY[static_cast<std::size_t>(row)] + g.rowH[static_cast<std::size_t>(row)]),
                              IM_COL32(255, 255, 255, 50), 3.0f);
            dl->AddLine(ImVec2(x, g.ly0), ImVec2(x, g.ly1), kSnapLine, 2.0f);
        }
        ImGui::EndDragDropTarget();
    }
    // Wheel: along the film; Ctrl zooms about the pointer; Shift goes up and down.
    if ((lanesHovered || (m.x >= g.a.x && m.x < g.b.x && m.y >= g.a.y && m.y < g.b.y && ImGui::IsWindowHovered())) && io.MouseWheel != 0.0f) {
        if (io.KeyCtrl) {
            const double at = g.tOf(m.x);
            st.pps *= std::pow(1.2, io.MouseWheel);
            st.pps = std::clamp(st.pps, laneW / (span * 4.0 + 60.0), st.film.fps * 60.0);
            st.view0 = std::max(0.0, at - (m.x - g.lx0) / st.pps);
            st.follow = false;
        } else if (io.KeyShift || m.x < g.lx0) {
            st.scrollY = std::clamp(st.scrollY - io.MouseWheel * em * 3.0f, 0.0f, std::max(0.0f, g.contentH - (g.ly1 - g.ly0)));
        } else {
            st.view0 = std::max(0.0, st.view0 - io.MouseWheel * laneW * 0.12 / st.pps);
            st.follow = false;
        }
    }
    if (io.MouseWheelH != 0.0f && lanesHovered) st.view0 = std::max(0.0, st.view0 - io.MouseWheelH * laneW * 0.12 / st.pps);

    // Lines over everything: the snap, the playhead.
    dl->PushClipRect(ImVec2(g.lx0, g.a.y), ImVec2(g.lx1, g.ly1), true);
    if (st.snapShown >= 0.0) dl->AddLine(ImVec2(g.xOf(st.snapShown), g.ly0), ImVec2(g.xOf(st.snapShown), g.ly1), kSnapLine, 1.5f);
    dl->PopClipRect();

    ruler(st, g);
    headers(st, h, g);
    {
        const float x = std::round(g.xOf(st.playhead)) + 0.5f;
        if (x >= g.lx0 - 1.0f && x <= g.lx1 + 1.0f) {
            dl->AddLine(ImVec2(x, g.a.y), ImVec2(x, g.ly1), kPlayhead, 1.5f);
            dl->AddTriangleFilled(ImVec2(x - em * 0.45f, g.a.y), ImVec2(x + em * 0.45f, g.a.y), ImVec2(x, g.a.y + em * 0.6f), kPlayhead);
        }
    }
    // The horizontal scroll bar.
    {
        const ImVec2 a(g.lx0, g.b.y - g.sbH), b(g.lx1, g.b.y);
        dl->AddRectFilled(a, b, IM_COL32(28, 30, 34, 255), 3.0f);
        const double total = std::max(span * 1.1, st.view0 + laneW / st.pps);
        const float t0 = static_cast<float>(st.view0 / total), t1 = static_cast<float>(std::min(1.0, (st.view0 + laneW / st.pps) / total));
        const float x0 = a.x + (b.x - a.x) * t0, x1 = std::max(x0 + 12.0f, a.x + (b.x - a.x) * t1);
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##hscroll", ImVec2(std::max(1.0f, b.x - a.x), g.sbH));
        const bool act = ImGui::IsItemActive();
        dl->AddRectFilled(ImVec2(x0, a.y + 2.0f), ImVec2(x1, b.y - 2.0f), act ? IM_COL32(150, 160, 175, 255) : IM_COL32(90, 98, 110, 255), 3.0f);
        if (act) {
            st.view0 = std::max(0.0, st.view0 + io.MouseDelta.x / (b.x - a.x) * total);
            st.follow = false;
        }
    }
    // The tracks do not all fit: a bar on the right to go up and down.
    if (g.contentH > g.ly1 - g.ly0 + 1.0f) {
        const float w = std::round(em * 0.55f);
        const ImVec2 a(g.lx1 - w, g.ly0), b(g.lx1, g.ly1);
        dl->AddRectFilled(a, b, IM_COL32(28, 30, 34, 230), 3.0f);
        const float span = g.ly1 - g.ly0;
        const float t0 = st.scrollY / g.contentH, t1 = std::min(1.0f, (st.scrollY + span) / g.contentH);
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##vscroll", ImVec2(w, std::max(1.0f, span)));
        const bool act = ImGui::IsItemActive();
        dl->AddRectFilled(ImVec2(a.x + 2.0f, a.y + span * t0), ImVec2(b.x - 2.0f, std::max(a.y + span * t0 + 12.0f, a.y + span * t1)),
                          act ? IM_COL32(150, 160, 175, 255) : IM_COL32(90, 98, 110, 255), 3.0f);
        if (act) st.scrollY = std::clamp(st.scrollY + io.MouseDelta.y * g.contentH / span, 0.0f, g.contentH - span);
        ImGui::SetItemTooltip("More tracks below (Shift+wheel)");
    }
    // A corner button: the whole film in view.
    ImGui::SetCursorScreenPos(ImVec2(g.a.x + 4.0f, g.a.y + 2.0f));
    if (ImGui::SmallButton("Fit")) {
        st.pps = 0.0;
        st.follow = true;
    }
    ImGui::SetItemTooltip("The whole film in view (F)");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, st.snap ? ImVec4(0.55f, 0.45f, 0.15f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
    if (ImGui::SmallButton("Snap")) st.snap = !st.snap;
    ImGui::PopStyleColor();
    ImGui::SetItemTooltip(st.snap ? "Drags snap to the cuts near them (hold Ctrl not to) -- N" : "Drags land on frames only -- N");

    // The playhead stays in view while playing.
    if (st.playing && st.follow) {
        const double right = st.view0 + laneW / st.pps;
        if (st.playhead > right - laneW * 0.05 / st.pps || st.playhead < st.view0) st.view0 = std::max(0.0, st.playhead - laneW * 0.1 / st.pps);
    }
    ImGui::SetCursorScreenPos(ImVec2(g.a.x, g.b.y));
    ImGui::Dummy(ImVec2(0, 0));
}

} // namespace vsequi
