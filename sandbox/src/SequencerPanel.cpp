#include "SequencerPanel.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <future>
#include <map>
#include <memory>
#include <string>

#include <imgui.h>

#include <fitzel/audio/Waveform.hpp>

#include "Component.hpp"
#include "SceneTypes.hpp"
#include "UiStyle.hpp"

namespace sequi {

namespace {

using seq::Kind;

constexpr float kPi = 3.14159265358979323846f;

// --- Colour --------------------------------------------------------------------
//
// One hue per kind of track, and the same three hues everywhere a kind shows:
// the bar down a track header, a strip's head, its mark in the overview. Picked
// to sit beside the editor's amber accent without being it -- the accent is
// spent on what is SELECTED, and a strip that looks selected because of what it
// is would leave the real selection nothing to stand out with.
//   Camera: a cool blue, the colour of the cut.
//   Animation: the amber of the Timeline's keys, because these ARE those keys.
//   Audio: the teal of the mixer's channel strips, where the sound ends up.

ImVec4 kindRgb(Kind k) {
    switch (k) {
        case Kind::Camera: return ImVec4(0.45f, 0.56f, 0.98f, 1.0f);
        case Kind::Audio:  return ImVec4(0.27f, 0.76f, 0.70f, 1.0f);
        default:           return ImVec4(0.95f, 0.74f, 0.30f, 1.0f);
    }
}

ImVec4 mixv(ImVec4 a, ImVec4 b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}
ImU32 u32(ImVec4 c, float alpha = 1.0f) {
    c.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}

const ImVec4 kInk      (0.06f, 0.07f, 0.08f, 1.0f);   // text on a bright strip head
const ImVec4 kLaneBg   (0.105f, 0.112f, 0.128f, 1.0f);
const ImVec4 kLaneAlt  (0.118f, 0.126f, 0.144f, 1.0f);
const ImVec4 kHeaderBg (0.150f, 0.158f, 0.178f, 1.0f);
const ImVec4 kRulerBg  (0.130f, 0.137f, 0.155f, 1.0f);
const ImVec4 kPlayhead (1.00f, 0.36f, 0.30f, 1.0f);   // the Timeline's playhead red
const ImVec4 kLive     (0.98f, 0.27f, 0.24f, 1.0f);
const ImVec4 kWarn     (1.00f, 0.62f, 0.30f, 1.0f);
const ImVec4 kMuteOn   (0.73f, 0.24f, 0.22f, 1.0f);
const ImVec4 kSoloOn   (0.81f, 0.62f, 0.19f, 1.0f);
const ImVec4 kLockOn   (0.42f, 0.47f, 0.56f, 1.0f);

ImVec4 accent() { return ImGui::GetStyleColorVec4(ImGuiCol_SliderGrab); }   // #F28A1E

// --- Metrics -------------------------------------------------------------------
// In the CURRENT font's terms, for the reason the Timeline gives: the type size
// is a comfort setting, and a sequencer whose lanes stay small while the text
// grows is one that stops being readable the moment somebody needs it to be.
struct Metrics {
    float em, frameH, headerW, rulerH, overviewH, stripHead, round, magnetPx, pad;
    float laneH(Kind k) const {
        return em * (k == Kind::Audio ? 4.1f : k == Kind::Camera ? 3.5f : 2.9f);
    }
};

Metrics metrics() {
    Metrics m;
    m.em        = ImGui::GetFontSize();
    m.frameH    = ImGui::GetFrameHeight();
    m.headerW   = m.em * 15.5f;
    m.rulerH    = m.em * 1.9f;
    m.overviewH = m.em * 1.5f;
    m.stripHead = m.em * 1.25f;
    m.round     = m.em * 0.28f;
    m.magnetPx  = m.em * 0.6f;
    m.pad       = m.em * 0.35f;
    return m;
}

// --- Icons -----------------------------------------------------------------------
// Drawn, not typed: the UI font has no transport glyphs, and "|<" in a button is
// a programmer's play button. Each one is a few primitives in a `size` square.
enum class Icon {
    Camera, Anim, Audio, Play, Pause, Stop, ToStart, ToEnd, PrevCut, NextCut,
    FrameBack, FrameFwd, Plus, Split, Magnet, Follow, Eye, Copy, Trash, Loop, Settings
};

void drawIcon(ImDrawList* dl, Icon ic, ImVec2 c, float s, ImU32 col) {
    const float h = s * 0.5f;
    const float lw = std::max(1.5f, s * 0.11f);
    auto tri = [&](float dir, float cx, float w) {   // a play triangle pointing `dir`
        dl->AddTriangleFilled(ImVec2(cx - dir * w * 0.5f, c.y - w * 0.62f),
                              ImVec2(cx - dir * w * 0.5f, c.y + w * 0.62f),
                              ImVec2(cx + dir * w * 0.5f, c.y), col);
    };
    auto bar = [&](float x, float w) {
        dl->AddRectFilled(ImVec2(x - w * 0.5f, c.y - h * 0.72f), ImVec2(x + w * 0.5f, c.y + h * 0.72f), col, 1.0f);
    };
    switch (ic) {
        case Icon::Camera: {
            const ImVec2 a(c.x - h * 0.95f, c.y - h * 0.5f), b(c.x + h * 0.25f, c.y + h * 0.55f);
            dl->AddRectFilled(a, b, col, s * 0.08f);
            dl->AddTriangleFilled(ImVec2(b.x - 1.0f, c.y), ImVec2(c.x + h * 0.95f, c.y - h * 0.5f),
                                  ImVec2(c.x + h * 0.95f, c.y + h * 0.55f), col);
            dl->AddCircleFilled(ImVec2(c.x - h * 0.55f, a.y - h * 0.22f), h * 0.24f, col);
            dl->AddCircleFilled(ImVec2(c.x - h * 0.05f, a.y - h * 0.22f), h * 0.24f, col);
            break;
        }
        case Icon::Anim: {
            const float r = h * 0.78f;
            dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r),
                              ImVec2(c.x - r, c.y), col);
            break;
        }
        case Icon::Audio: {
            const float hs[5] = {0.35f, 0.8f, 0.55f, 1.0f, 0.45f};
            for (int i = 0; i < 5; ++i) {
                const float x = c.x + (static_cast<float>(i) - 2.0f) * s * 0.19f;
                const float y = h * 0.85f * hs[i];
                dl->AddRectFilled(ImVec2(x - s * 0.055f, c.y - y), ImVec2(x + s * 0.055f, c.y + y), col, 1.0f);
            }
            break;
        }
        case Icon::Play:      tri(1.0f, c.x + s * 0.04f, s * 0.62f); break;
        case Icon::Pause:     bar(c.x - s * 0.14f, s * 0.17f); bar(c.x + s * 0.14f, s * 0.17f); break;
        case Icon::Stop:
            dl->AddRectFilled(ImVec2(c.x - h * 0.6f, c.y - h * 0.6f), ImVec2(c.x + h * 0.6f, c.y + h * 0.6f), col, 1.5f);
            break;
        case Icon::ToStart:   bar(c.x - s * 0.24f, s * 0.11f); tri(-1.0f, c.x + s * 0.08f, s * 0.46f); break;
        case Icon::ToEnd:     bar(c.x + s * 0.24f, s * 0.11f); tri(1.0f, c.x - s * 0.08f, s * 0.46f); break;
        case Icon::PrevCut:   tri(-1.0f, c.x - s * 0.13f, s * 0.38f); tri(-1.0f, c.x + s * 0.17f, s * 0.38f); break;
        case Icon::NextCut:   tri(1.0f, c.x - s * 0.17f, s * 0.38f); tri(1.0f, c.x + s * 0.13f, s * 0.38f); break;
        case Icon::FrameBack: tri(-1.0f, c.x - s * 0.06f, s * 0.42f); bar(c.x + s * 0.22f, s * 0.1f); break;
        case Icon::FrameFwd:  tri(1.0f, c.x + s * 0.06f, s * 0.42f); bar(c.x - s * 0.22f, s * 0.1f); break;
        case Icon::Plus:
            dl->AddRectFilled(ImVec2(c.x - h * 0.7f, c.y - lw * 0.5f), ImVec2(c.x + h * 0.7f, c.y + lw * 0.5f), col);
            dl->AddRectFilled(ImVec2(c.x - lw * 0.5f, c.y - h * 0.7f), ImVec2(c.x + lw * 0.5f, c.y + h * 0.7f), col);
            break;
        case Icon::Split: {
            dl->AddLine(ImVec2(c.x, c.y - h * 0.85f), ImVec2(c.x, c.y + h * 0.85f), col, lw);
            dl->AddTriangleFilled(ImVec2(c.x - h * 0.75f, c.y - h * 0.35f), ImVec2(c.x - h * 0.2f, c.y),
                                  ImVec2(c.x - h * 0.75f, c.y + h * 0.35f), col);
            dl->AddTriangleFilled(ImVec2(c.x + h * 0.75f, c.y - h * 0.35f), ImVec2(c.x + h * 0.2f, c.y),
                                  ImVec2(c.x + h * 0.75f, c.y + h * 0.35f), col);
            break;
        }
        case Icon::Magnet: {
            const float r = h * 0.55f;
            dl->PathArcTo(ImVec2(c.x, c.y), r, 0.0f, kPi, 12);
            dl->PathStroke(col, 0, lw * 1.6f);
            dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x - r, c.y - h * 0.6f), col, lw * 1.6f);
            dl->AddLine(ImVec2(c.x + r, c.y), ImVec2(c.x + r, c.y - h * 0.6f), col, lw * 1.6f);
            dl->AddRectFilled(ImVec2(c.x - r - lw, c.y - h * 0.85f), ImVec2(c.x - r + lw, c.y - h * 0.55f), col);
            dl->AddRectFilled(ImVec2(c.x + r - lw, c.y - h * 0.85f), ImVec2(c.x + r + lw, c.y - h * 0.55f), col);
            break;
        }
        case Icon::Follow:
            dl->AddLine(ImVec2(c.x - h * 0.8f, c.y), ImVec2(c.x + h * 0.25f, c.y), col, lw);
            dl->AddTriangleFilled(ImVec2(c.x + h * 0.05f, c.y - h * 0.4f), ImVec2(c.x + h * 0.5f, c.y),
                                  ImVec2(c.x + h * 0.05f, c.y + h * 0.4f), col);
            dl->AddLine(ImVec2(c.x + h * 0.75f, c.y - h * 0.65f), ImVec2(c.x + h * 0.75f, c.y + h * 0.65f), col, lw);
            break;
        case Icon::Eye: {
            const float w = h * 0.95f, e = h * 0.52f;
            dl->PathArcTo(ImVec2(c.x, c.y + e * 0.9f), w * 1.18f, -kPi * 0.78f, -kPi * 0.22f, 12);
            dl->PathStroke(col, 0, lw);
            dl->PathArcTo(ImVec2(c.x, c.y - e * 0.9f), w * 1.18f, kPi * 0.22f, kPi * 0.78f, 12);
            dl->PathStroke(col, 0, lw);
            dl->AddCircleFilled(c, h * 0.3f, col);
            break;
        }
        case Icon::Copy:
            dl->AddRect(ImVec2(c.x - h * 0.7f, c.y - h * 0.7f), ImVec2(c.x + h * 0.3f, c.y + h * 0.3f), col, 1.5f, 0, lw);
            dl->AddRectFilled(ImVec2(c.x - h * 0.3f, c.y - h * 0.3f), ImVec2(c.x + h * 0.7f, c.y + h * 0.7f), col, 1.5f);
            break;
        case Icon::Trash:
            dl->AddRectFilled(ImVec2(c.x - h * 0.75f, c.y - h * 0.62f), ImVec2(c.x + h * 0.75f, c.y - h * 0.48f), col);
            dl->AddRectFilled(ImVec2(c.x - h * 0.25f, c.y - h * 0.8f), ImVec2(c.x + h * 0.25f, c.y - h * 0.62f), col);
            dl->AddRectFilled(ImVec2(c.x - h * 0.55f, c.y - h * 0.38f), ImVec2(c.x + h * 0.55f, c.y + h * 0.8f), col, 1.5f);
            break;
        case Icon::Settings: {
            // A cog: eight teeth round a ring.
            const float ro = h * 0.78f, ri = h * 0.52f, tw = 0.22f;
            for (int i = 0; i < 8; ++i) {
                const float a = static_cast<float>(i) * kPi * 0.25f;
                const ImVec2 d(std::cos(a), std::sin(a)), n(-d.y, d.x);
                const float w = ro * tw;
                dl->AddQuadFilled(ImVec2(c.x + d.x * ri + n.x * w, c.y + d.y * ri + n.y * w),
                                  ImVec2(c.x + d.x * ro + n.x * w, c.y + d.y * ro + n.y * w),
                                  ImVec2(c.x + d.x * ro - n.x * w, c.y + d.y * ro - n.y * w),
                                  ImVec2(c.x + d.x * ri - n.x * w, c.y + d.y * ri - n.y * w), col);
            }
            dl->AddCircle(c, ri * 0.82f, col, 0, lw * 1.7f);
            break;
        }
        case Icon::Loop: {
            const float r = h * 0.6f;
            dl->PathArcTo(c, r, kPi * 0.15f, kPi * 1.75f, 16);
            dl->PathStroke(col, 0, lw);
            const ImVec2 tip(c.x + r * std::cos(kPi * 0.15f), c.y + r * std::sin(kPi * 0.15f));
            dl->AddTriangleFilled(ImVec2(tip.x - h * 0.32f, tip.y - h * 0.05f),
                                  ImVec2(tip.x + h * 0.32f, tip.y - h * 0.05f),
                                  ImVec2(tip.x, tip.y + h * 0.36f), col);
            break;
        }
    }
}

// A button with an icon on it. `on` lights it in the accent (a switch that is
// on); the tooltip is what the icon cannot say.
bool iconButton(const char* id, Icon ic, const char* tip, bool on = false, float w = 0.0f) {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 sz(w > 0.0f ? w : h * 1.3f, h);
    const bool pressed = ImGui::Button(id, sz);
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // A switch that is on says so twice: the icon in the accent, and a bar of
    // it under the button -- the same mark the theme puts under a selected tab.
    const ImU32 ink = disabled ? ImGui::GetColorU32(ImGuiCol_TextDisabled)
                    : on       ? u32(mixv(accent(), ImVec4(1, 1, 1, 1), 0.2f))
                               : ImGui::GetColorU32(ImGuiCol_Text);
    drawIcon(dl, ic, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), h * 0.62f, ink);
    if (on)
        dl->AddRectFilled(ImVec2(a.x + h * 0.22f, b.y - 2.5f), ImVec2(b.x - h * 0.22f, b.y), u32(accent()), 1.0f);
    if (tip) ImGui::SetItemTooltip("%s", tip);
    return pressed;
}

// A thin upright rule between two groups of a toolbar row.
void groupRule() {
    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.55f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  h = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + h * 0.18f), ImVec2(p.x, p.y + h * 0.82f),
                                        IM_COL32(255, 255, 255, 34), 1.0f);
    ImGui::Dummy(ImVec2(1.0f, h));
    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.55f);
}

// A small square toggle with a letter on it (M, S, L, H on a track header),
// lit in its own colour when on.
bool letterToggle(const char* id, const char* letter, bool& v, ImVec4 onCol, const char* tip, float size) {
    ImGui::PushID(id);
    if (v) {
        ImGui::PushStyleColor(ImGuiCol_Button, onCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mixv(onCol, ImVec4(1, 1, 1, 1), 0.15f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, mixv(onCol, ImVec4(1, 1, 1, 1), 0.25f));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    const bool pressed = ImGui::Button(letter, ImVec2(size, size));
    ImGui::PopStyleVar();
    if (v) ImGui::PopStyleColor(3);
    if (pressed) v = !v;
    ImGui::SetItemTooltip("%s", tip);
    ImGui::PopID();
    return pressed;
}

// --- Text helpers ----------------------------------------------------------------

// "01:23:12" -- minutes, seconds, frames. Frames rather than hundredths because
// the grid is frames: a strip moved one step reads one more, not 0.03 more.
std::string timecode(float t, float fps) {
    const float f   = std::max(1.0f, fps);
    const int   tot = static_cast<int>(std::floor(std::max(0.0f, t) * f + 0.5f));
    const int   fr  = tot % static_cast<int>(std::round(f));
    const int   sec = static_cast<int>(static_cast<float>(tot) / f);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", sec / 60, sec % 60, fr);
    return buf;
}

void clippedText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col,
                 const std::string& s, const ImVec4& clip) {
    dl->AddText(font ? font : ImGui::GetFont(), size, pos, col, s.c_str(), nullptr, 0.0f, &clip);
}

float toDb(float g)   { return g <= 0.001f ? -60.0f : 20.0f * std::log10(g); }
float fromDb(float d) { return d <= -59.9f ? 0.0f : std::pow(10.0f, d / 20.0f); }

const Entity* findEntity(const std::vector<Entity>& es, int id) {
    for (const Entity& e : es) if (e.id == id) return &e;
    return nullptr;
}

std::string entityName(const std::vector<Entity>& es, int id) {
    const Entity* e = findEntity(es, id);
    if (!e) return id < 0 ? std::string("No camera") : "(deleted " + std::to_string(id) + ")";
    return e->name.empty() ? "Camera " + std::to_string(id) : e->name;
}

std::vector<int> cameraIds(const std::vector<Entity>& es) {
    std::vector<int> out;
    for (const Entity& e : es) if (e.components.get<CameraComponent>()) out.push_back(e.id);
    return out;
}

// A strip's sound name to a path, remembered: the host's lookup walks the whole
// asset database, and the strips ask every frame. Forgotten every few seconds,
// so a project switched or a sound imported is picked up without anyone asking.
std::map<std::string, std::string> g_paths;
double g_pathsAt = 0.0;
std::string pathOf(const Host& h, const std::string& name) {
    if (name.empty() || !h.resolveSound) return std::string();
    if (ImGui::GetTime() - g_pathsAt > 3.0) { g_paths.clear(); g_pathsAt = ImGui::GetTime(); }
    auto it = g_paths.find(name);
    if (it == g_paths.end()) it = g_paths.emplace(name, h.resolveSound(name)).first;
    return it->second;
}

// --- Waveforms ---------------------------------------------------------------------
// Read off the main thread -- a whole decode of a three-minute song is a
// visible stall -- and kept for the session by path. A strip whose waveform is
// still being read draws a placeholder rather than waiting for it.
struct WaveSlot {
    std::future<fitzel::Waveform> job;
    fitzel::Waveform              wave;
    bool                          ready = false;
};
std::map<std::string, std::unique_ptr<WaveSlot>> g_waves;

const fitzel::Waveform* waveform(const std::string& path) {
    if (path.empty()) return nullptr;
    auto& slot = g_waves[path];
    if (!slot) {
        slot = std::make_unique<WaveSlot>();
        slot->job = std::async(std::launch::async, [path] {
            fitzel::Waveform w;
            fitzel::readWaveform(path, w, 120.0f);
            return w;
        });
    }
    if (!slot->ready && slot->job.valid() &&
        slot->job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        slot->wave  = slot->job.get();
        slot->ready = true;
    }
    return slot->ready ? &slot->wave : nullptr;
}

// --- Panel state ---------------------------------------------------------------------
// The panel's own, like the Timeline's selection: nobody else acts on it and it
// does not belong to the scene. Indices are uids and are re-validated every
// frame, so a scene load leaves them pointing at nothing rather than at a
// stranger.
struct Drag {
    enum Mode { None, Move, TrimL, TrimR, FadeIn, FadeOut, Scrub, EndMark, Overview } mode = None;
    int   strip = -1;
    float grab = 0.0f;                       // mouse time minus the strip start
    float start0 = 0.0f, length0 = 0.0f, offset0 = 0.0f;
};

struct State {
    int   selStrip = -1;
    int   selTrack = -1;
    float pps      = 0.0f;   // pixels per second; 0 = fit on the next frame
    float view0    = 0.0f;   // seconds at the left edge of the lanes
    bool  snap     = true;
    bool  follow   = true;
    Drag  drag;
    int   renameTrack = -1;
    char  renameBuf[64] = "";
    char  soundFilter[64] = "";
    int   addTrack = -1;     // the track the add popup is for
    float addAt    = 0.0f;   // ...and where it puts what is picked
    bool  focused  = false;
    int   lastSeq  = -1;
    float park     = 0.0f;   // the playhead while the game runs another sequence
    float playFrom = 0.0f;   // where Play was pressed: Stop goes back there
    bool  openAdd  = false;
    bool  openStripMenu = false;
    bool  openTrackMenu = false;
};
State g;

// --- Editing helpers -----------------------------------------------------------------

// The start nearest `desired` at which a strip of `len` fits on `tr` without
// overlapping another (ignoring `exclude`). Strips are blocks: dragged into a
// neighbour, one stops against it instead of sliding under it, which is the
// behaviour that needs no explaining and leaves no hidden strip behind.
float placeFree(const seq::Track& tr, int exclude, float desired, float len) {
    std::vector<std::pair<float, float>> occ;
    for (const seq::Strip& s : tr.strips)
        if (s.uid != exclude) occ.emplace_back(s.start, s.end());
    std::sort(occ.begin(), occ.end());
    desired = std::max(0.0f, desired);
    float best = desired, bestD = FLT_MAX, gap0 = 0.0f;
    for (const auto& [a, b] : occ) {
        if (a - gap0 >= len - 1e-4f) {
            const float p = std::clamp(desired, gap0, a - len);
            if (std::fabs(p - desired) < bestD) { bestD = std::fabs(p - desired); best = p; }
        }
        gap0 = std::max(gap0, b);
    }
    const float p = std::max(desired, gap0);
    if (std::fabs(p - desired) < bestD) best = p;
    return best;
}

// Where the strip after/before `st` on its track begins/ends (for trimming
// against a neighbour). +inf / 0 when there is none.
float nextStart(const seq::Track& tr, const seq::Strip& st) {
    float n = FLT_MAX;
    for (const seq::Strip& o : tr.strips)
        if (o.uid != st.uid && o.start >= st.start - 1e-4f) n = std::min(n, o.start);
    return n;
}
float prevEnd(const seq::Track& tr, const seq::Strip& st) {
    float p = 0.0f;
    for (const seq::Strip& o : tr.strips)
        if (o.uid != st.uid && o.end() <= st.end() + 1e-4f && o.start < st.start) p = std::max(p, o.end());
    return p;
}

// Snap a time: onto a magnet (the playhead, 0, the sequence end, any other
// strip's edge) when one is within a few pixels, else onto the frame grid.
// Alt held lets go of everything but the grid's absence -- the escape hatch for
// the rare time a magnet is in the way.
float snapTime(const seq::Sequence& s, float t, float playhead, int exclude, float pps,
               const Metrics& m, float* dist = nullptr) {
    t = std::max(0.0f, t);
    if (dist) *dist = FLT_MAX;
    if (!g.snap || ImGui::GetIO().KeyAlt) return t;
    float best = t, bestD = m.magnetPx / std::max(pps, 1e-3f);
    bool hit = false;
    auto consider = [&](float c) {
        const float d = std::fabs(c - t);
        if (d < bestD) { bestD = d; best = c; hit = true; }
    };
    consider(playhead);
    consider(0.0f);
    consider(s.duration);
    for (const seq::Track& tr : s.tracks)
        for (const seq::Strip& o : tr.strips)
            if (o.uid != exclude) { consider(o.start); consider(o.end()); }
    if (hit) { if (dist) *dist = bestD; return best; }
    return s.snap(t);
}

std::string stripLabel(const Host& h, const seq::Track& tr, const seq::Strip& st) {
    if (tr.kind == Kind::Audio) return st.sound.empty() ? std::string("(no sound)") : st.sound;
    if (tr.kind == Kind::Camera) return entityName(h.entities, st.camera);
    return st.clip.empty() ? std::string("(no clip)") : st.clip;
}

void markDirty(const Host& h) { if (h.markDirty) h.markDirty(); }

// Put a new strip on `tr` at `at` (or the nearest free place), select it, and
// grow the sequence if it now runs past the end.
void addStrip(const Host& h, seq::Sequence& s, seq::Track& tr, seq::Strip st, float at) {
    st.uid   = s.newUid();
    st.start = placeFree(tr, -1, s.snap(at), st.length);
    tr.strips.push_back(st);
    tr.sort();
    g.selStrip = st.uid;
    g.selTrack = tr.uid;
    if (st.end() > s.duration) s.duration = std::ceil(st.end());
    markDirty(h);
}

seq::Strip clipStrip(const Host& h, const std::string& clip) {
    seq::Strip st;
    st.clip = clip;
    const int ci = anim::findClip(h.clips, clip);
    if (ci >= 0) {
        st.speed  = std::max(0.05f, h.clips[ci].speed);
        st.length = seq::clipLength(h.clips[ci]) / st.speed;
        st.camera = seq::cameraOfClip(h.clips[ci], h.entities);
    }
    return st;
}

seq::Strip soundStrip(const Host& h, const std::string& name) {
    seq::Strip st;
    st.sound = name;
    const std::string path = pathOf(h, name);
    const float len = path.empty() ? 0.0f : fitzel::soundFileSeconds(path);
    st.length = len > 0.05f ? len : 4.0f;
    return st;
}

// The audio track a dropped sound goes on when it was not dropped on one: the
// selected audio track, else the first, else a new one.
seq::Track& audioTrackFor(seq::Sequence& s) {
    if (seq::Track* t = s.trackByUid(g.selTrack); t && t->kind == Kind::Audio) return *t;
    for (seq::Track& t : s.tracks) if (t.kind == Kind::Audio) return t;
    return s.addTrack(Kind::Audio);
}

// --- The add menu ------------------------------------------------------------------------
// What a track can take, listed: a camera track the scene's camera moves and
// its cameras, an animation track the clips, an audio track the sounds. Picking
// one places it at the playhead (or where the lane was double-clicked).
void addMenu(const Host& h, seq::Sequence& s) {
    if (!ImGui::BeginPopup("##seqadd")) return;
    seq::Track* tr = s.trackByUid(g.addTrack);
    if (!tr) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    ui::hint("Place at %s", timecode(g.addAt, s.fps).c_str());

    if (tr->kind == Kind::Camera) {
        ui::sectionText("Camera animations");
        int shown = 0;
        for (const anim::Clip& c : h.clips) {
            const int cam = seq::cameraOfClip(c, h.entities);
            if (cam < 0) continue;
            ++shown;
            const std::string label = c.name + "##cc";
            if (ImGui::MenuItem(label.c_str(), entityName(h.entities, cam).c_str())) {
                addStrip(h, s, *tr, clipStrip(h, c.name), g.addAt);
                ImGui::CloseCurrentPopup();
            }
        }
        if (shown == 0) ui::hint("No clip moves a camera yet. Key a camera's position in the Timeline, "
                                 "or record a flight below.");
        if (h.hasCameraPath && h.cameraFromPath) {
            if (ImGui::MenuItem("From the recorded camera path")) {
                std::string clip;
                int cam = -1;
                if (h.cameraFromPath(clip, cam)) {
                    seq::Strip st = clipStrip(h, clip);
                    st.camera = cam;
                    addStrip(h, s, *tr, st, g.addAt);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemTooltip("Turn the Camera path window's recording into a camera\n"
                                  "and a clip that flies it, and place it as a shot.");
        }
        ui::sectionText("Held shot");
        if (h.cameraFromView) {
            if (ImGui::MenuItem("New camera from the view")) {
                const int id = h.cameraFromView();
                if (id >= 0) {
                    seq::Strip st;
                    st.camera = id;
                    st.length = 3.0f;
                    addStrip(h, s, *tr, st, g.addAt);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemTooltip("A camera standing where the viewport's view stands, looking where it\n"
                                  "looks -- placed here as a held shot");
        }
        const std::vector<int> cams = cameraIds(h.entities);
        if (cams.empty()) ui::hint("The scene has no camera yet -- make one from the view above.");
        for (int id : cams) {
            const std::string label = entityName(h.entities, id) + "##hold" + std::to_string(id);
            if (ImGui::MenuItem(label.c_str())) {
                seq::Strip st;
                st.camera = id;
                st.length = 3.0f;
                addStrip(h, s, *tr, st, g.addAt);
                ImGui::CloseCurrentPopup();
            }
        }
        if (!cams.empty()) {
            ImGui::Separator();
            ui::sectionText("Other clips");
            for (const anim::Clip& c : h.clips) {
                if (seq::cameraOfClip(c, h.entities) >= 0 || c.empty()) continue;
                const std::string label = c.name + "##oc";
                if (ImGui::MenuItem(label.c_str())) {
                    addStrip(h, s, *tr, clipStrip(h, c.name), g.addAt);
                    ImGui::CloseCurrentPopup();
                }
            }
        }
    } else if (tr->kind == Kind::Anim) {
        ui::sectionText("Animations");
        int shown = 0;
        for (const anim::Clip& c : h.clips) {
            if (c.empty()) continue;
            ++shown;
            char right[32];
            std::snprintf(right, sizeof(right), "%.1f s", seq::clipLength(c));
            const std::string label = c.name + "##ac";
            if (ImGui::MenuItem(label.c_str(), right)) {
                addStrip(h, s, *tr, clipStrip(h, c.name), g.addAt);
                ImGui::CloseCurrentPopup();
            }
        }
        if (shown == 0) ui::hint("No animation has keys yet -- make one in the Timeline.");
    } else {
        ui::sectionText("Sounds");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        ui::searchBox("##sndfilter", g.soundFilter, sizeof(g.soundFilter), "Filter sounds...");
        const std::vector<std::string> names = h.soundNames ? h.soundNames() : std::vector<std::string>{};
        if (names.empty()) ui::hint("No sounds in the project. Drop .wav/.ogg/.mp3 files on the Assets browser.");
        ImGui::BeginChild("##sndlist", ImVec2(ImGui::GetFontSize() * 16.0f,
                          std::min(14.0f, static_cast<float>(names.size()) + 0.5f) * ImGui::GetFrameHeightWithSpacing()),
                          ImGuiChildFlags_None);
        for (const std::string& n : names) {
            if (!ui::icontains(n.c_str(), g.soundFilter)) continue;
            if (ImGui::Selectable(n.c_str())) {
                addStrip(h, s, *tr, soundStrip(h, n), g.addAt);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndChild();
    }
    ImGui::EndPopup();
}

// --- Drawing a strip -------------------------------------------------------------------

struct StripView {
    ImVec2 a, b;          // the strip's rectangle
    ImVec4 clip;          // the lane it may draw into
    float  t0, pps;       // time at the lanes' left edge, and the zoom
    float  laneX0;
    bool   selected, hovered, live, muted;
};

float xOf(const StripView& v, float t) { return v.laneX0 + (t - v.t0) * v.pps; }

void hatch(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float step) {
    dl->PushClipRect(a, b, true);
    for (float x = a.x - (b.y - a.y); x < b.x; x += step)
        dl->AddLine(ImVec2(x, b.y), ImVec2(x + (b.y - a.y), a.y), col, 1.0f);
    dl->PopClipRect();
}

void drawStrip(ImDrawList* dl, const Host& h, const seq::Track& tr, const seq::Strip& st,
               const StripView& v, const Metrics& m) {
    ImVec4 base = kindRgb(tr.kind);
    if (v.muted) base = mixv(base, ImVec4(0.45f, 0.46f, 0.5f, 1.0f), 0.7f);
    const ImVec4 body  = mixv(base, kLaneBg, 0.72f);
    const ImVec4 head  = v.hovered ? mixv(base, ImVec4(1, 1, 1, 1), 0.12f) : base;
    const float  w     = v.b.x - v.a.x;
    const float  hb    = std::min(m.stripHead, (v.b.y - v.a.y) * 0.45f);
    const float  r     = std::min(m.round, w * 0.5f);

    dl->PushClipRect(ImVec2(v.clip.x, v.clip.y), ImVec2(v.clip.z, v.clip.w), true);

    // Body and head. The head is the strip's colour at full strength, so a
    // sequence reads by kind at a glance; the body is that colour sunk into the
    // lane, so what is drawn IN it (a waveform, keys, the camera) has contrast.
    dl->AddRectFilled(v.a, v.b, u32(body, v.muted ? 0.7f : 1.0f), r);
    dl->AddRectFilled(v.a, ImVec2(v.b.x, v.a.y + hb), u32(head, v.muted ? 0.75f : 1.0f), r,
                      ImDrawFlags_RoundCornersTop);

    const ImVec4 textClip(std::max(v.a.x, v.clip.x) + 3.0f, v.a.y, std::min(v.b.x, v.clip.z) - 3.0f, v.b.y);
    ImFont* bold = ui::boldFont();
    const float fs = m.em * 0.86f;
    const std::string label = stripLabel(h, tr, st);
    // The label follows the left edge of the view, so a long strip scrolled half
    // out of sight still says what it is.
    float tx = std::max(v.a.x, v.clip.x) + m.pad;
    const Icon ic = tr.kind == Kind::Camera ? Icon::Camera : tr.kind == Kind::Audio ? Icon::Audio : Icon::Anim;
    if (w > m.em * 1.6f) {
        drawIcon(dl, ic, ImVec2(tx + fs * 0.5f, v.a.y + hb * 0.5f), fs * 0.95f, u32(kInk, 0.85f));
        tx += fs * 1.25f;
    }
    clippedText(dl, bold, fs, ImVec2(tx, v.a.y + (hb - fs) * 0.5f), u32(kInk), label, textClip);

    // The right of the head: LIVE on the shot that is the view, else the length.
    {
        char len[32];
        std::snprintf(len, sizeof(len), "%.1fs", st.length);
        const char* tag = v.live ? "LIVE" : len;
        const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs * 0.9f, FLT_MAX, 0.0f, tag);
        const float  labelW = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, label.c_str()).x;
        const float  rx = std::min(v.b.x, v.clip.z) - m.pad - ts.x;
        if (rx > tx + labelW + m.em) {
            if (v.live) {
                dl->AddRectFilled(ImVec2(rx - 4.0f, v.a.y + 2.0f), ImVec2(rx + ts.x + 4.0f, v.a.y + hb - 2.0f),
                                  u32(kLive), hb * 0.4f);
                dl->AddText(ImGui::GetFont(), fs * 0.9f, ImVec2(rx, v.a.y + (hb - ts.y) * 0.5f),
                            IM_COL32_WHITE, tag);
            } else {
                dl->AddText(ImGui::GetFont(), fs * 0.9f, ImVec2(rx, v.a.y + (hb - ts.y) * 0.5f),
                            u32(kInk, 0.6f), tag);
            }
        }
    }

    const ImVec2 ba(v.a.x, v.a.y + hb), bb = v.b;        // the body
    const float  midY = (ba.y + bb.y) * 0.5f;
    const ImVec4 hi = mixv(base, ImVec4(1, 1, 1, 1), 0.25f);

    if (tr.kind == Kind::Camera) {
        // Film: sprocket holes along the foot of the body. It is decoration --
        // and it is what tells a camera strip from an animation strip before
        // the colour does.
        const float ph = std::max(3.0f, m.em * 0.22f), pw = ph * 1.4f, step = pw * 2.4f;
        const float y0 = bb.y - ph - 3.0f;
        for (float x = ba.x + step * 0.5f; x < bb.x - pw; x += step)
            dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + pw, y0 + ph), u32(kLaneBg, 0.9f), 1.5f);
        const float lineY = ba.y + (y0 - ba.y - fs) * 0.5f;
        const int   ci = st.clip.empty() ? -1 : anim::findClip(h.clips, st.clip);
        std::string sub = st.clip.empty() ? std::string("held shot")
                        : ci < 0 ? "missing clip: " + st.clip : st.clip;
        if (st.camera < 0) sub = "pick a camera";
        const ImU32 subCol = (st.camera < 0 || (!st.clip.empty() && ci < 0)) ? u32(kWarn) : u32(hi, 0.95f);
        clippedText(dl, nullptr, fs, ImVec2(std::max(v.a.x, v.clip.x) + m.pad, lineY), subCol, sub, textClip);
        if (ci >= 0) {   // the clip's keys, faintly, so a move's rhythm shows
            const anim::Clip& c = h.clips[ci];
            for (const anim::Track& t : c.tracks)
                for (const anim::Key& k : t.keys) {
                    const float sx = xOf(v, st.start + (k.t - st.offset) / std::max(st.speed, 1e-3f));
                    if (sx < ba.x || sx > bb.x) continue;
                    dl->AddLine(ImVec2(sx, y0 - 2.0f), ImVec2(sx, y0 - 2.0f - ph), u32(hi, 0.25f));
                }
        }
    } else if (tr.kind == Kind::Anim) {
        const int ci = anim::findClip(h.clips, st.clip);
        if (ci < 0) {
            hatch(dl, ba, bb, u32(kWarn, 0.25f), m.em * 0.5f);
            clippedText(dl, nullptr, fs, ImVec2(std::max(v.a.x, v.clip.x) + m.pad, midY - fs * 0.5f),
                        u32(kWarn), "missing clip", textClip);
        } else {
            const anim::Clip& c = h.clips[ci];
            const float len  = seq::clipLength(c);
            const float spd  = std::max(st.speed, 1e-3f);
            // Where the clip's own material runs out inside the strip: past it the
            // strip holds the last pose (hatched), or comes round again (a loop
            // mark per pass).
            const float runEnd = st.start + (len - st.offset) / spd;
            if (!st.loop && runEnd < st.end() - 1e-3f)
                hatch(dl, ImVec2(std::max(ba.x, xOf(v, runEnd)), ba.y), bb, u32(hi, 0.14f), m.em * 0.45f);
            std::vector<float> times;
            for (const anim::Track& t : c.tracks)
                for (const anim::Key& k : t.keys) times.push_back(k.t);
            std::sort(times.begin(), times.end());
            times.erase(std::unique(times.begin(), times.end(),
                                    [](float x, float y) { return std::fabs(x - y) < 1e-3f; }), times.end());
            const float kr = std::max(2.5f, m.em * 0.2f);
            const int passes = st.loop ? static_cast<int>(std::ceil((st.length * spd + st.offset) / len)) + 1 : 1;
            for (int p = 0; p < passes && p < 64; ++p) {
                const float base0 = static_cast<float>(p) * len;
                if (st.loop && p > 0) {
                    const float lx = xOf(v, st.start + (base0 - st.offset) / spd);
                    if (lx > ba.x && lx < bb.x)
                        dl->AddLine(ImVec2(lx, ba.y + 2.0f), ImVec2(lx, bb.y - 2.0f), u32(hi, 0.35f), 1.0f);
                }
                for (float kt : times) {
                    const float sx = xOf(v, st.start + (base0 + kt - st.offset) / spd);
                    if (sx < ba.x - kr || sx > bb.x + kr) continue;
                    if (sx < xOf(v, st.start) || sx > xOf(v, st.end())) continue;
                    dl->AddQuadFilled(ImVec2(sx, midY - kr), ImVec2(sx + kr, midY), ImVec2(sx, midY + kr),
                                      ImVec2(sx - kr, midY), u32(hi, p == 0 ? 0.95f : 0.55f));
                }
            }
            dl->AddLine(ImVec2(ba.x + 2.0f, midY), ImVec2(bb.x - 2.0f, midY), u32(hi, 0.18f));
        }
    } else {
        // Audio: the waveform, peak as the outline and RMS as the body, both
        // scaled by the strip's gain and its fades -- what is drawn is what
        // will be heard.
        const std::string path = pathOf(h, st.sound);
        const fitzel::Waveform* wf = waveform(path);
        const float amp = (bb.y - ba.y) * 0.46f;
        if (!wf) {
            const float pulse = 0.25f + 0.15f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f);
            dl->AddLine(ImVec2(ba.x + 2.0f, midY), ImVec2(bb.x - 2.0f, midY), u32(hi, pulse), 2.0f);
            clippedText(dl, nullptr, fs * 0.9f, ImVec2(std::max(v.a.x, v.clip.x) + m.pad, midY - fs),
                        u32(hi, 0.6f), path.empty() ? "no sound" : "reading...", textClip);
        } else if (wf->empty()) {
            hatch(dl, ba, bb, u32(kWarn, 0.25f), m.em * 0.5f);
            clippedText(dl, nullptr, fs, ImVec2(std::max(v.a.x, v.clip.x) + m.pad, midY - fs * 0.5f),
                        u32(kWarn), "cannot read " + st.sound, textClip);
        } else {
            const float x0 = std::max(ba.x, v.clip.x), x1 = std::min(bb.x, v.clip.z);
            const ImU32 cPeak = u32(mixv(base, body, 0.25f), 0.95f);
            const ImU32 cRms  = u32(mixv(base, ImVec4(1, 1, 1, 1), 0.3f));
            const float step = 1.0f;
            for (float x = std::floor(x0); x < x1; x += step) {
                const float t0 = v.t0 + (x - v.laneX0) / v.pps;
                const float t1 = v.t0 + (x + step - v.laneX0) / v.pps;
                const float s0 = st.sourceTime(t0), s1 = st.sourceTime(t1);
                if (s1 < 0.0f || s0 > wf->seconds) continue;
                const float env = st.gain * st.fadeAt(0.5f * (t0 + t1));
                const float pk  = std::min(1.0f, wf->peakOver(s0, s1) * env);
                const float rm  = std::min(1.0f, wf->rmsOver(s0, s1) * env);
                const float ph  = std::max(0.5f, pk * amp), rh = rm * amp;
                dl->AddRectFilled(ImVec2(x, midY - ph), ImVec2(x + step, midY + ph), cPeak);
                if (rh > 0.5f) dl->AddRectFilled(ImVec2(x, midY - rh), ImVec2(x + step, midY + rh), cRms);
            }
            // Past the end of the file there is nothing to hear: say so.
            const float fileEnd = st.start + (wf->seconds - st.offset);
            if (fileEnd < st.end() - 1e-3f)
                hatch(dl, ImVec2(std::max(ba.x, xOf(v, fileEnd)), ba.y), bb, u32(hi, 0.12f), m.em * 0.45f);
        }
        // The fades: the curve itself, and the area it silences sunk darker.
        auto fadeShape = [&](float fa, float fb, bool in) {
            if (fb - fa < 1e-4f) return;
            const int n = 18;
            ImVec2 pts[n + 1];
            for (int i = 0; i <= n; ++i) {
                const float t = fa + (fb - fa) * static_cast<float>(i) / n;
                const float e = st.fadeAt(t);
                pts[i] = ImVec2(xOf(v, t), bb.y - 1.0f - e * (bb.y - ba.y - 2.0f));
            }
            for (int i = 0; i < n; ++i) {
                const ImVec2 top = in ? ImVec2(pts[i].x, ba.y) : ImVec2(pts[i].x, ba.y);
                dl->AddQuadFilled(top, ImVec2(pts[i + 1].x, ba.y), pts[i + 1], pts[i], IM_COL32(0, 0, 0, 90));
            }
            dl->AddPolyline(pts, n + 1, u32(ImVec4(1, 1, 1, 1), 0.75f), 0, 1.5f);
        };
        fadeShape(st.start, st.start + st.fadeIn, true);
        fadeShape(st.end() - st.fadeOut, st.end(), false);
        // The fade handles, on the head's lower edge: grab and pull inward.
        if (v.hovered || v.selected) {
            const float hs = std::max(4.0f, m.em * 0.32f);
            for (float t : {st.start + st.fadeIn, st.end() - st.fadeOut}) {
                const float hx = std::clamp(xOf(v, t), v.a.x + hs, v.b.x - hs);
                dl->AddRectFilled(ImVec2(hx - hs, v.a.y + hb - hs), ImVec2(hx + hs, v.a.y + hb + hs),
                                  IM_COL32(240, 242, 248, 235), 2.0f);
                dl->AddRect(ImVec2(hx - hs, v.a.y + hb - hs), ImVec2(hx + hs, v.a.y + hb + hs),
                            u32(kInk), 2.0f, 0, 1.0f);
            }
        }
    }

    // Outline: the accent for the selection, the live red for the shot on air,
    // a lighter rim of its own colour otherwise.
    if (v.selected)
        dl->AddRect(ImVec2(v.a.x - 1.0f, v.a.y - 1.0f), ImVec2(v.b.x + 1.0f, v.b.y + 1.0f),
                    u32(accent()), r + 1.0f, 0, 2.5f);
    else if (v.live)
        dl->AddRect(v.a, v.b, u32(kLive, 0.9f), r, 0, 2.0f);
    else
        dl->AddRect(v.a, v.b, u32(mixv(base, ImVec4(1, 1, 1, 1), 0.1f), 0.55f), r, 0, 1.0f);

    // The trim edges, lit under the pointer so the target is visible before
    // it is pressed.
    if (v.hovered && !v.selected) {
        dl->AddRectFilled(ImVec2(v.a.x, v.a.y + hb), ImVec2(v.a.x + 2.0f, v.b.y), u32(hi, 0.6f));
        dl->AddRectFilled(ImVec2(v.b.x - 2.0f, v.a.y + hb), ImVec2(v.b.x, v.b.y), u32(hi, 0.6f));
    }
    dl->PopClipRect();
}

// --- The empty states -------------------------------------------------------------------

seq::Sequence freshSequence(const std::vector<seq::Sequence>& seqs) {
    seq::Sequence s;
    int n = static_cast<int>(seqs.size()) + 1;
    while (seq::find(seqs, "Sequence " + std::to_string(n)) >= 0) ++n;
    s.name = "Sequence " + std::to_string(n);
    // The three tracks almost every sequence starts with, so the first thing
    // the author sees is somewhere to put a shot, a move and a sound -- not
    // three buttons asking which they want first.
    s.addTrack(Kind::Camera);
    s.addTrack(Kind::Anim);
    s.addTrack(Kind::Audio);
    return s;
}

void centeredText(const char* t, bool bold = false) {
    const float w = ImGui::CalcTextSize(t).x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowWidth() - w) * 0.5f));
    if (bold) ui::title("%s", t);
    else      ImGui::TextDisabled("%s", t);
}

} // namespace

bool ownsKeys() { return g.focused; }

void selectStrip(int stripUid) { g.selStrip = stripUid; }

void drawPanel(const Host& h) {
    if (!h.show) { g.focused = false; return; }
    ImGui::SetNextWindowSize(ImVec2(1180.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Sequencer", &h.show)) { g.focused = false; ImGui::End(); return; }
    g.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const Metrics m  = metrics();
    seq::Runner&  r  = h.runner;
    const float   em = m.em;

    // --- No sequence yet ----------------------------------------------------
    if (h.seqs.empty()) {
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetContentRegionAvail().y * 0.28f));
        centeredText("No sequence in this scene yet", true);
        centeredText("A sequence lays recorded animations, camera shots and sounds out on one clock.");
        centeredText("Shots side by side on a camera track cut from one camera to the next by themselves.");
        ImGui::Dummy(ImVec2(0.0f, em * 0.6f));
        const float bw = em * 13.0f;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - bw) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Button, mixv(ImGui::GetStyleColorVec4(ImGuiCol_Button), accent(), 0.6f));
        if (ImGui::Button("Create a sequence", ImVec2(bw, m.frameH * 1.5f))) {
            h.seqs.push_back(freshSequence(h.seqs));
            h.editSeq = 0;
            g = State{};
            markDirty(h);
        }
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }

    h.editSeq = std::clamp(h.editSeq, 0, static_cast<int>(h.seqs.size()) - 1);
    // Another sequence picked (here or by a scene load): the preview of the old
    // one ends before this one is allowed to pose anything.
    if (g.lastSeq != h.editSeq) {
        if (r.preview) r.endPreview(h.clips, h.entities, h.activeCam);
        if (!h.gameRunning) { r.sequence = h.editSeq; r.time = 0.0f; }
        g.selStrip = g.selTrack = -1;
        g.pps = 0.0f;
        g.view0 = 0.0f;
        g.lastSeq = h.editSeq;
    }
    if (!h.gameRunning && !r.preview && r.sequence != h.editSeq) r.sequence = h.editSeq;

    seq::Sequence& s    = h.seqs[h.editSeq];
    const bool     mine = (r.sequence == h.editSeq);
    const float    step = 1.0f / std::max(1.0f, s.fps);
    const float    now  = mine ? r.time : g.park;

    // Keep the selection honest before anything reads it.
    if (!s.stripByUid(g.selStrip)) g.selStrip = -1;
    if (!s.trackByUid(g.selTrack)) g.selTrack = -1;

    auto beginPreview = [&] {
        if (h.beforePreview) h.beforePreview();
        r.beginPreview(h.editSeq, h.seqs, h.clips, h.entities, h.activeCam);
    };
    // MOVING THE PLAYHEAD IS THE REQUEST TO SEE IT, as in the Timeline: every
    // control that sets a time poses the scene and takes the cut.
    auto goTo = [&](float t) {
        t = std::clamp(t, 0.0f, s.duration);
        if (h.gameRunning && !mine) { g.park = t; return; }
        r.seek(t);
        if (!h.gameRunning && !r.preview)
            beginPreview();
    };
    auto play = [&] {
        if (h.gameRunning) {
            if (!mine) { r.start(h.editSeq, h.activeCam); r.seek(g.park); }
            else       r.playing = true;
        } else {
            if (!r.preview) beginPreview();
            r.playing = true;
        }
        if (r.time >= s.duration - 1e-3f) r.seek(0.0f);
        g.playFrom = r.time;
    };
    auto stop = [&] {
        if (h.gameRunning) { if (mine) r.stop(h.seqs, h.activeCam); return; }
        r.playing = false;
        r.endPreview(h.clips, h.entities, h.activeCam);
        r.time = g.playFrom;
    };
    // Every cut on every camera track, for the jump-to-cut buttons.
    auto cutTime = [&](bool forward) {
        float best = forward ? FLT_MAX : -FLT_MAX;
        bool any = false;
        for (const seq::Track& tr : s.tracks) {
            if (tr.kind != Kind::Camera) continue;
            for (const seq::Strip& st : tr.strips)
                for (float t : {st.start, st.end()}) {
                    if (forward ? (t > now + 1e-3f && t < best) : (t < now - 1e-3f && t > best)) {
                        best = t; any = true;
                    }
                }
        }
        if (!any) for (const seq::Track& tr : s.tracks)          // no cuts: any strip edge
            for (const seq::Strip& st : tr.strips)
                for (float t : {st.start, st.end()})
                    if (forward ? (t > now + 1e-3f && t < best) : (t < now - 1e-3f && t > best)) {
                        best = t; any = true;
                    }
        return any ? best : (forward ? s.duration : 0.0f);
    };

    // Flow the toolbar's groups onto as many lines as the panel is wide.
    auto flow = [](float w) {
        ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();
    };

    // --- The toolbar: one row ---------------------------------------------------
    // Which sequence, the transport, the clock, what is on air, and the three
    // switches -- the things reached for while WORKING. The sequence's settings
    // (length, frame rate, how it starts) are reached for once, and live in the
    // bar under the lanes when nothing is selected, so they do not cost the
    // lanes a row of height for the whole session.
    const float pickW = em * 9.0f;
    ImGui::SetNextItemWidth(pickW);
    if (ImGui::BeginCombo("##seqpick", s.name.c_str())) {
        for (int i = 0; i < static_cast<int>(h.seqs.size()); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(h.seqs[i].name.c_str(), i == h.editSeq) && i != h.editSeq)
                h.editSeq = i;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("The sequence on the panel");
    ImGui::SameLine(0.0f, 2.0f);
    {
        // The sequence's own settings -- Play on start, Loop, length, frame
        // rate -- live in the bar under the lanes whenever nothing is selected.
        // This is the way back to them from any selection (as is Esc).
        const bool showing = g.selStrip < 0 && g.selTrack < 0;
        if (iconButton("##seqset", Icon::Settings,
                       "Sequence settings: Play on start, Loop, length, frame rate  (Esc)", showing)) {
            g.selStrip = -1;
            g.selTrack = -1;
        }
    }
    ImGui::SameLine(0.0f, 2.0f);
    if (iconButton("##seqnew", Icon::Plus, "New sequence")) {
        if (r.preview) r.endPreview(h.clips, h.entities, h.activeCam);
        h.seqs.push_back(freshSequence(h.seqs));
        h.editSeq = static_cast<int>(h.seqs.size()) - 1;
        markDirty(h);
    }
    ImGui::SameLine(0.0f, 2.0f);
    if (iconButton("##seqdup", Icon::Copy, "Duplicate this sequence")) {
        if (r.preview) r.endPreview(h.clips, h.entities, h.activeCam);
        seq::Sequence copy = s;
        copy.name += " copy";
        copy.playOnStart = false;
        h.seqs.push_back(std::move(copy));
        h.editSeq = static_cast<int>(h.seqs.size()) - 1;
        markDirty(h);
    }
    ImGui::SameLine(0.0f, 2.0f);
    if (iconButton("##seqdel", Icon::Trash, "Delete this sequence")) {
        if (r.preview) r.endPreview(h.clips, h.entities, h.activeCam);
        if (h.gameRunning && mine) r.stop(h.seqs, h.activeCam);
        h.seqs.erase(h.seqs.begin() + h.editSeq);
        h.editSeq = std::max(0, h.editSeq - 1);
        r.sequence = -1;
        g.lastSeq = -1;
        markDirty(h);
        ImGui::End();
        return;   // `s` is gone; the next frame draws the survivor
    }

    // Transport.
    groupRule();
    if (iconButton("##tstart", Icon::ToStart, "Go to the start  (Home)")) goTo(0.0f);
    ImGui::SameLine(0.0f, 2.0f);
    if (iconButton("##tprev", Icon::PrevCut, "Previous cut")) goTo(cutTime(false));
    ImGui::SameLine(0.0f, 2.0f);
    if (iconButton("##tback", Icon::FrameBack, "One frame back  (Left)")) goTo(s.snap(now - step));
    ImGui::SameLine(0.0f, 2.0f);
    const bool rolling = mine && r.playing;
    {
        // The play button is the one coloured control in the row: it is what
        // the eye looks for, and while it rolls it is the accent itself.
        const ImVec4 btn = ImGui::GetStyleColorVec4(ImGuiCol_Button);
        ImGui::PushStyleColor(ImGuiCol_Button, rolling ? accent() : btn);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rolling ? mixv(accent(), ImVec4(1, 1, 1, 1), 0.15f)
                                                              : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent());
        // Idle, the triangle is drawn in the accent on a plain button; rolling,
        // the button is the accent and the bars on it are dark.
        ImGui::PushStyleColor(ImGuiCol_Text, rolling ? kInk : mixv(accent(), ImVec4(1, 1, 1, 1), 0.1f));
        if (iconButton("##tplay", rolling ? Icon::Pause : Icon::Play,
                       rolling ? "Pause  (Space)" : "Play  (Space)", false, m.frameH * 2.2f)) {
            if (rolling) r.playing = false;
            else         play();
        }
        if (h.probe) h.probe("play", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::PopStyleColor(4);
    }
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::BeginDisabled(!(mine && (r.playing || r.preview)));
    if (iconButton("##tstop", Icon::Stop, "Stop: the scene and the camera go back to how they were")) stop();
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 2.0f);
    if (iconButton("##tfwd", Icon::FrameFwd, "One frame on  (Right)")) goTo(s.snap(now + step));
    ImGui::SameLine(0.0f, 2.0f);
    if (iconButton("##tnext", Icon::NextCut, "Next cut")) goTo(cutTime(true));
    ImGui::SameLine(0.0f, 2.0f);
    if (iconButton("##tend", Icon::ToEnd, "Go to the end  (End)")) goTo(s.duration);

    // The clock, big: the one number everybody looks for. A click on it turns
    // it into a field to type a time into -- the exact way to get somewhere.
    groupRule();
    {
        static bool typing = false;
        ImFont* f = ui::boldFont() ? ui::boldFont() : ImGui::GetFont();
        const float fs = em * 1.25f;
        const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, "00:00:00");
        const ImVec2 box(ts.x + em * 1.3f, m.frameH);
        if (typing) {
            float t = now;
            ImGui::SetNextItemWidth(box.x);
            ImGui::SetKeyboardFocusHere();
            if (ImGui::InputFloat("##tsec", &t, 0.0f, 0.0f, "%.2f",
                                  ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                goTo(s.snap(t));
                typing = false;
            }
            if (ImGui::IsItemDeactivated()) typing = false;
            ImGui::SetItemTooltip("A time in seconds; Enter goes there");
        } else {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##tc", box)) typing = true;
            const bool hot = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + box.x, p.y + box.y), IM_COL32(10, 11, 14, 255), m.round);
            dl->AddRect(p, ImVec2(p.x + box.x, p.y + box.y),
                        u32(rolling ? accent() : ImVec4(1, 1, 1, hot ? 0.3f : 0.1f)), m.round);
            dl->AddText(f, fs, ImVec2(p.x + (box.x - ts.x) * 0.5f, p.y + (box.y - ts.y) * 0.5f),
                        u32(rolling ? mixv(accent(), ImVec4(1, 1, 1, 1), 0.35f) : ImVec4(0.93f, 0.95f, 0.99f, 1.0f)),
                        timecode(now, s.fps).c_str());
            ImGui::SetItemTooltip("%.3f s of %.1f s -- minutes:seconds:frames at %g fps.\nClick to type a time.",
                                  now, s.duration, s.fps);
        }
    }

    // On air: the camera the cut is on, the way a vision mixer shows it.
    {
        const bool onAir = mine && (r.playing || r.preview) && r.frame.camera >= 0;
        const std::string cam = onAir ? entityName(h.entities, r.frame.camera) : std::string("off air");
        const ImVec2 ts = ImGui::CalcTextSize(cam.c_str());
        const ImVec2 box(ts.x + em * 3.2f, m.frameH);
        ImGui::SameLine(0.0f, em * 0.5f);
        if (ImGui::GetContentRegionAvail().x < box.x) ImGui::NewLine();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + box.x, p.y + box.y),
                          u32(onAir ? mixv(kLive, kLaneBg, 0.5f) : kLaneBg), box.y * 0.5f);
        dl->AddCircleFilled(ImVec2(p.x + box.y * 0.5f, p.y + box.y * 0.5f), em * 0.24f,
                            u32(onAir ? kLive : ImVec4(0.38f, 0.39f, 0.43f, 1.0f)));
        drawIcon(dl, Icon::Camera, ImVec2(p.x + box.y * 0.5f + em * 1.0f, p.y + box.y * 0.5f), em * 0.85f,
                 ImGui::GetColorU32(onAir ? ImGuiCol_Text : ImGuiCol_TextDisabled));
        dl->AddText(ImVec2(p.x + box.y * 0.5f + em * 1.75f, p.y + (box.y - ts.y) * 0.5f),
                    ImGui::GetColorU32(onAir ? ImGuiCol_Text : ImGuiCol_TextDisabled), cam.c_str());
        ImGui::InvisibleButton("##onair", box);
        ImGui::SetItemTooltip(onAir ? "On air: the camera the cut is on right now"
                                    : "No shot is on air -- press Play or move the playhead");
    }

    // Preview, at the right end of the row when there is room.
    {
        const float sw = m.frameH * 1.3f;
        ImGui::SameLine();
        const float avail = ImGui::GetContentRegionAvail().x;
        if (avail < sw) ImGui::NewLine();
        else            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - sw);
        bool pv = mine && r.preview;
        ImGui::BeginDisabled(h.gameRunning);
        if (iconButton("##pv", Icon::Eye,
                       "Preview: show the scene as the sequence poses it and look\n"
                       "through its cameras. Off puts the scene and the view back.", pv)) {
            if (pv) r.endPreview(h.clips, h.entities, h.activeCam);
            else    beginPreview();
        }
        ImGui::EndDisabled();
    }
    ImGui::Dummy(ImVec2(0.0f, em * 0.1f));

    // --- Geometry of the edit area -------------------------------------------
    const float detailsH = (m.frameH + em * 0.35f) * 3.0f + ImGui::GetStyle().ItemSpacing.y * 5.0f + em * 0.6f;
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const float  W   = ImGui::GetContentRegionAvail().x;
    const float  availH = ImGui::GetContentRegionAvail().y;
    const float  lanesH = std::max(em * 6.0f, availH - detailsH - m.rulerH - m.overviewH - em * 0.5f);

    // The lanes' total height decides whether the lane area shows a scrollbar,
    // and with it where the right edge of the time axis is -- worked out before
    // anything is drawn so the ruler above and the lanes below agree.
    float contentH = 0.0f;
    for (const seq::Track& tr : s.tracks) contentH += m.laneH(tr.kind) + 1.0f;
    contentH += m.frameH * 1.6f + em;   // the add-track row
    const bool  vScroll = contentH > lanesH;
    const float sbW     = vScroll ? ImGui::GetStyle().ScrollbarSize : 0.0f;
    const float laneX0  = top.x + m.headerW;
    const float laneX1  = top.x + W - sbW;
    const float laneW   = std::max(em * 4.0f, laneX1 - laneX0);

    const float extent = std::max({s.duration, s.contentEnd(), 1.0f});
    if (g.pps <= 0.0f) g.pps = laneW / (extent * 1.04f);
    g.pps = std::clamp(g.pps, 2.0f, 3000.0f);
    const float visible = laneW / g.pps;
    g.view0 = std::clamp(g.view0, 0.0f, std::max(0.0f, extent * 1.1f - visible * 0.5f));

    // Follow the playhead: page along when it runs off the right of the view.
    if (g.follow && rolling && (now > g.view0 + visible * 0.97f || now < g.view0))
        g.view0 = std::max(0.0f, now - visible * 0.08f);

    auto xAt = [&](float t) { return laneX0 + (t - g.view0) * g.pps; };
    auto tAt = [&](float x) { return g.view0 + (x - laneX0) / g.pps; };

    ImDrawList* wdl = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();

    // Zoom about a time under the pointer, keeping that time where it is.
    auto zoomAt = [&](float factor, float x) {
        const float t = tAt(x);
        g.pps = std::clamp(g.pps * factor, 2.0f, 3000.0f);
        g.view0 = std::max(0.0f, t - (x - laneX0) / g.pps);
    };

    // --- Ruler ----------------------------------------------------------------
    {
        // The corner over the track headers holds the add-track buttons: they
        // are about the column under them.
        const ImVec2 c0 = top, c1(laneX0, top.y + m.rulerH);
        wdl->AddRectFilled(c0, c1, u32(kRulerBg));
        ImGui::SetCursorScreenPos(ImVec2(top.x + m.pad, top.y + (m.rulerH - m.frameH) * 0.5f));
        ui::title("Tracks");
        ImGui::SameLine();
        const float bx = laneX0 - m.pad - (m.frameH * 1.3f + 2.0f) * 3.0f;
        ImGui::SetCursorScreenPos(ImVec2(bx, top.y + (m.rulerH - m.frameH) * 0.5f));
        if (iconButton("##addcam", Icon::Camera, "Add a camera track")) { g.selTrack = s.addTrack(Kind::Camera).uid; markDirty(h); }
        ImGui::SameLine(0.0f, 2.0f);
        if (iconButton("##addanim", Icon::Anim, "Add an animation track")) { g.selTrack = s.addTrack(Kind::Anim).uid; markDirty(h); }
        ImGui::SameLine(0.0f, 2.0f);
        if (iconButton("##addaud", Icon::Audio, "Add an audio track")) { g.selTrack = s.addTrack(Kind::Audio).uid; markDirty(h); }

        const ImVec2 r0(laneX0, top.y), r1(laneX1, top.y + m.rulerH);
        wdl->AddRectFilled(r0, r1, u32(kRulerBg));
        ImGui::SetCursorScreenPos(r0);
        ImGui::InvisibleButton("##ruler", ImVec2(laneW, m.rulerH));
        const bool rHover = ImGui::IsItemHovered();
        if (ImGui::IsItemActivated()) {
            const float ex = xAt(s.duration);
            g.drag = Drag{};
            g.drag.mode = std::fabs(io.MousePos.x - ex) < em * 0.5f ? Drag::EndMark : Drag::Scrub;
        }
        if (ImGui::IsItemActive()) {
            const float t = tAt(io.MousePos.x);
            if (g.drag.mode == Drag::EndMark) {
                s.duration = std::max(0.5f, g.snap ? s.snap(t) : t);
                markDirty(h);
            } else {
                goTo(g.snap ? s.snap(t) : t);
            }
        }
        if (ImGui::IsItemDeactivated()) g.drag = Drag{};
        if (rHover) {
            // Over the ruler the wheel always zooms: there is nothing to scroll.
            if (io.MouseWheel != 0.0f) zoomAt(io.MouseWheel > 0 ? 1.2f : 1.0f / 1.2f, io.MousePos.x);
            if (std::fabs(io.MousePos.x - xAt(s.duration)) < em * 0.5f)
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            ImGui::SetItemTooltip("Click or drag to move the playhead. Wheel zooms.\n"
                                  "Drag the end mark to change the length.");
        }

        wdl->PushClipRect(r0, r1, true);
        // Ticks: the label spacing picks the step, never the other way round, so
        // the labels are readable at every zoom.
        static const float kSteps[] = {0.1f, 0.2f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 15.0f, 30.0f, 60.0f, 120.0f, 300.0f};
        float major = 300.0f;
        for (float st : kSteps) if (st * g.pps >= em * 4.5f) { major = st; break; }
        const float frameW = step * g.pps;
        const float minor = (frameW >= em * 0.45f && major <= 1.0f) ? step : major / (major >= 1.0f && major != 15.0f ? 5.0f : 3.0f);
        const ImU32 cTick = IM_COL32(255, 255, 255, 60), cMaj = IM_COL32(255, 255, 255, 140);
        for (float t = std::floor(g.view0 / minor) * minor; t <= g.view0 + visible + minor; t += minor) {
            if (t < -1e-4f) continue;
            const float x = xAt(t);
            const bool isMajor = std::fabs(std::remainder(t, major)) < minor * 0.25f;
            wdl->AddLine(ImVec2(x, r1.y - (isMajor ? m.rulerH * 0.45f : m.rulerH * 0.2f)), ImVec2(x, r1.y),
                         isMajor ? cMaj : cTick);
            // No label the end mark would sit on.
            if (isMajor && std::fabs(x - xAt(s.duration)) > em * 0.9f) {
                char lbl[24];
                const int sec = static_cast<int>(std::round(t * 1000.0f)) / 1000;
                if (major >= 1.0f) std::snprintf(lbl, sizeof(lbl), "%d:%02d", sec / 60, sec % 60);
                else               std::snprintf(lbl, sizeof(lbl), "%.1f", t);
                wdl->AddText(ImGui::GetFont(), em * 0.82f, ImVec2(x + 3.0f, r0.y + em * 0.15f),
                             IM_COL32(220, 224, 232, 200), lbl);
            }
        }
        // Past the end: shaded, with the end mark as a handle.
        const float ex = xAt(s.duration);
        if (ex < r1.x) wdl->AddRectFilled(ImVec2(std::max(ex, r0.x), r0.y), r1, IM_COL32(0, 0, 0, 70));
        wdl->AddLine(ImVec2(ex, r0.y), ImVec2(ex, r1.y), u32(accent(), 0.8f), 2.0f);
        wdl->AddTriangleFilled(ImVec2(ex - em * 0.45f, r0.y), ImVec2(ex, r0.y), ImVec2(ex, r0.y + em * 0.55f),
                               u32(accent(), 0.9f));
        // The cuts, as small marks in the cut colour along the ruler's foot.
        for (const seq::Track& tr : s.tracks) {
            if (tr.kind != Kind::Camera || tr.mute) continue;
            for (const seq::Strip& st : tr.strips) {
                const float x = xAt(st.start);
                wdl->AddTriangleFilled(ImVec2(x - em * 0.28f, r1.y), ImVec2(x + em * 0.28f, r1.y),
                                       ImVec2(x, r1.y - em * 0.35f), u32(kindRgb(Kind::Camera), 0.9f));
            }
        }
        // The playhead's flag.
        const float px = xAt(now);
        const float fw = em * 0.5f;
        wdl->AddQuadFilled(ImVec2(px - fw, r0.y + em * 0.1f), ImVec2(px + fw, r0.y + em * 0.1f),
                           ImVec2(px + fw, r1.y - fw), ImVec2(px - fw, r1.y - fw), u32(kPlayhead));
        wdl->AddTriangleFilled(ImVec2(px - fw, r1.y - fw), ImVec2(px + fw, r1.y - fw), ImVec2(px, r1.y),
                               u32(kPlayhead));
        wdl->PopClipRect();
        wdl->AddLine(ImVec2(top.x, r1.y - 0.5f), ImVec2(laneX1, r1.y - 0.5f), IM_COL32(0, 0, 0, 120));
    }

    // --- Lanes ------------------------------------------------------------------
    ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + m.rulerH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kLaneBg);
    ImGui::BeginChild("##seqlanes", ImVec2(W, lanesH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetCursorScreenPos();   // includes the scroll
        const ImVec2 childMin = ImGui::GetWindowPos();
        const ImVec2 childMax(childMin.x + ImGui::GetWindowWidth(), childMin.y + ImGui::GetWindowHeight());
        const bool childHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

        // Pass 1: where every lane is.
        struct Lane { int track; float y0, y1; };
        std::vector<Lane> lanes;
        {
            float y = o.y;
            for (int i = 0; i < static_cast<int>(s.tracks.size()); ++i) {
                const float lh = m.laneH(s.tracks[i].kind);
                lanes.push_back({i, y, y + lh});
                y += lh + 1.0f;
            }
        }
        const float lanesEnd = lanes.empty() ? o.y : lanes.back().y1 + 1.0f;

        // What the pointer is over, worked out once for every lane: a strip and
        // which part of it, before any widget claims the click.
        enum Zone { ZNone, ZBody, ZLeft, ZRight, ZFadeIn, ZFadeOut };
        struct Hit { int lane = -1; int strip = -1; Zone zone = ZNone; };
        auto hitTest = [&](ImVec2 p) {
            Hit hit;
            for (int li = 0; li < static_cast<int>(lanes.size()); ++li) {
                const Lane& L = lanes[li];
                if (p.y < L.y0 || p.y >= L.y1 || p.x < laneX0 || p.x >= laneX1) continue;
                hit.lane = li;
                const seq::Track& tr = s.tracks[L.track];
                const float hb = std::min(m.stripHead, (L.y1 - L.y0 - 4.0f) * 0.45f);
                for (const seq::Strip& st : tr.strips) {
                    const float a = xAt(st.start), b = xAt(st.end());
                    if (p.x < a - 2.0f || p.x > b + 2.0f) continue;
                    hit.strip = st.uid;
                    const float edge = std::clamp((b - a) * 0.22f, 3.0f, em * 0.55f);
                    if (tr.kind == Kind::Audio && p.y < L.y0 + 2.0f + hb + em * 0.4f && p.y > L.y0 + 2.0f + hb - em * 0.6f) {
                        const float fi = xAt(st.start + st.fadeIn), fo = xAt(st.end() - st.fadeOut);
                        if (std::fabs(p.x - fi) < em * 0.5f) { hit.zone = ZFadeIn; return hit; }
                        if (std::fabs(p.x - fo) < em * 0.5f) { hit.zone = ZFadeOut; return hit; }
                    }
                    hit.zone = p.x < a + edge ? ZLeft : p.x > b - edge ? ZRight : ZBody;
                    return hit;
                }
                return hit;
            }
            return hit;
        };

        // Pass 2: the widgets -- each lane one wide button, each header its
        // controls -- with what happened to them noted for pass 3.
        int  pressedLane = -1, activeLane = -1, releasedLane = -1, rightLane = -1, dblLane = -1;
        int  dropLane = -1;
        int  pendingTrack = -1;
        std::string dropSound;
        for (int li = 0; li < static_cast<int>(lanes.size()); ++li) {
            const Lane& L = lanes[li];
            seq::Track& tr = s.tracks[L.track];
            const float lh = L.y1 - L.y0;
            ImGui::PushID(tr.uid);

            // Header background, the kind bar down its left, the selection.
            const bool trSel = (g.selTrack == tr.uid);
            dl->AddRectFilled(ImVec2(o.x, L.y0), ImVec2(laneX0, L.y1),
                              u32(trSel ? mixv(kHeaderBg, accent(), 0.10f) : kHeaderBg));
            dl->AddRectFilled(ImVec2(o.x, L.y0), ImVec2(o.x + em * (trSel ? 0.32f : 0.22f), L.y1),
                              u32(kindRgb(tr.kind), tr.mute ? 0.4f : 1.0f));
            dl->AddLine(ImVec2(laneX0 - 1.0f, L.y0), ImVec2(laneX0 - 1.0f, L.y1), IM_COL32(0, 0, 0, 140));

            // The whole header selects the track; the buttons on it sit on top.
            ImGui::SetCursorScreenPos(ImVec2(o.x, L.y0));
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::InvisibleButton("##hdr", ImVec2(m.headerW, lh))) { g.selTrack = tr.uid; g.selStrip = -1; }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { g.selTrack = tr.uid; g.openTrackMenu = true; }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                g.renameTrack = tr.uid;
                std::snprintf(g.renameBuf, sizeof(g.renameBuf), "%s", tr.name.c_str());
            }

            // Row 1: icon, name, and the track's switches at the right end --
            // M(ute), S(olo) or H(old), L(ock), and + to place something.
            const float bs   = std::round(m.frameH * 0.82f);
            const float x0   = o.x + em * 0.6f;
            const float row1 = L.y0 + m.pad;
            const float midY = row1 + bs * 0.5f;
            const Icon kic = tr.kind == Kind::Camera ? Icon::Camera : tr.kind == Kind::Audio ? Icon::Audio : Icon::Anim;
            drawIcon(dl, kic, ImVec2(x0 + em * 0.5f, midY), em * 0.95f,
                     u32(kindRgb(tr.kind), tr.mute ? 0.5f : 1.0f));
            const float nameX = x0 + em * 1.4f;
            const float btnX  = laneX0 - m.pad - (bs + 3.0f) * 4.0f;
            if (g.renameTrack == tr.uid) {
                ImGui::SetCursorScreenPos(ImVec2(nameX, midY - m.frameH * 0.5f));
                ImGui::SetNextItemWidth(btnX - nameX - m.pad);
                ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##rename", g.renameBuf, sizeof(g.renameBuf),
                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                    tr.name = g.renameBuf;
                    g.renameTrack = -1;
                    markDirty(h);
                }
                if (ImGui::IsItemDeactivated()) g.renameTrack = -1;
            } else {
                ImFont* bold = ui::boldFont() ? ui::boldFont() : ImGui::GetFont();
                const ImVec4 clip(nameX, L.y0, btnX - 4.0f, L.y1);
                clippedText(dl, bold, em, ImVec2(nameX, midY - em * 0.55f),
                            ImGui::GetColorU32(tr.mute ? ImGuiCol_TextDisabled : ImGuiCol_Text), tr.name, clip);
            }
            ImGui::SetCursorScreenPos(ImVec2(btnX, row1));
            if (letterToggle("m", "M", tr.mute, kMuteOn, "Mute: the track does nothing", bs)) markDirty(h);
            ImGui::SameLine(0.0f, 3.0f);
            if (tr.kind == Kind::Audio) {
                if (letterToggle("s", "S", tr.solo, kSoloOn, "Solo: while any audio track is soloed, only those are heard", bs))
                    markDirty(h);
            } else {
                if (letterToggle("h", "H", tr.hold, mixv(kindRgb(tr.kind), kLaneBg, 0.45f),
                                 "Hold: after a strip ends, keep its last pose until the next one", bs))
                    markDirty(h);
            }
            ImGui::SameLine(0.0f, 3.0f);
            if (letterToggle("l", "L", tr.lock, kLockOn, "Lock: strips on this track cannot be moved", bs)) markDirty(h);
            ImGui::SameLine(0.0f, 3.0f);
            if (iconButton("##add", Icon::Plus,
                           tr.kind == Kind::Camera ? "Add a shot at the playhead"
                           : tr.kind == Kind::Audio ? "Add a sound at the playhead"
                                                    : "Add an animation at the playhead",
                           false, bs)) {
                g.addTrack = tr.uid;
                g.addAt    = now;
                g.openAdd  = true;
            }
            if (h.probe) h.probe("add:" + std::to_string(tr.uid), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

            // Row 2 says what the track does -- for an audio track, as controls:
            // its level as two buttons and a number (the editor's rule for an
            // amount: one click, one step) and the mixer channel it feeds.
            const float row2 = row1 + bs + m.pad;
            if (row2 + bs <= L.y1 - 2.0f) {
                ImFont* f = ImGui::GetFont();
                const float fs = em * 0.86f;
                if (tr.kind == Kind::Audio) {
                    const float d = toDb(tr.gain);
                    char db[24];
                    if (tr.gain <= 0.0f) std::snprintf(db, sizeof(db), "-inf dB");
                    else                 std::snprintf(db, sizeof(db), "%+.1f dB", d);
                    const float dbW = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, "-60.0 dB").x + em * 0.4f;
                    ImGui::SetCursorScreenPos(ImVec2(x0, row2));
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
                    if (ImGui::Button("-##g", ImVec2(bs, bs))) { tr.gain = fromDb(std::max(-60.0f, d - 1.0f)); markDirty(h); }
                    ImGui::SetItemTooltip("Track level 1 dB down");
                    ImGui::SameLine(0.0f, 2.0f);
                    const ImVec2 tp = ImGui::GetCursorScreenPos();
                    dl->AddRectFilled(tp, ImVec2(tp.x + dbW, tp.y + bs), IM_COL32(10, 11, 14, 200), 3.0f);
                    // A level bar under the number: how far up the fader is.
                    const float frac = std::clamp((d + 60.0f) / 66.0f, 0.0f, 1.0f);
                    dl->AddRectFilled(ImVec2(tp.x + 2.0f, tp.y + bs - 3.0f),
                                      ImVec2(tp.x + 2.0f + (dbW - 4.0f) * frac, tp.y + bs - 1.5f),
                                      u32(kindRgb(Kind::Audio), tr.mute ? 0.35f : 0.9f), 1.0f);
                    const float tw = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, db).x;
                    dl->AddText(f, fs, ImVec2(tp.x + (dbW - tw) * 0.5f, tp.y + (bs - fs) * 0.5f - 1.0f),
                                ImGui::GetColorU32(ImGuiCol_Text), db);
                    ImGui::Dummy(ImVec2(dbW, bs));
                    ImGui::SameLine(0.0f, 2.0f);
                    if (ImGui::Button("+##g", ImVec2(bs, bs))) { tr.gain = fromDb(std::min(6.0f, d + 1.0f)); markDirty(h); }
                    ImGui::SetItemTooltip("Track level 1 dB up");
                    ImGui::SameLine(0.0f, em * 0.5f);
                    // The output, as a small button that opens the channel list.
                    const std::string out = "> " + tr.channel;
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    const float ow = std::max(bs, laneX0 - m.pad - ImGui::GetCursorScreenPos().x);
                    if (ImGui::Button((out + "##out").c_str(), ImVec2(ow, bs))) ImGui::OpenPopup("##outmenu");
                    ImGui::PopStyleColor();
                    ImGui::SetItemTooltip("Plays through the mixer channel '%s' -- click to change", tr.channel.c_str());
                    ImGui::PopStyleVar();
                    if (ImGui::BeginPopup("##outmenu")) {
                        ui::hint("Output channel");
                        const std::vector<std::string> chans = h.channels ? h.channels() : std::vector<std::string>{};
                        for (const std::string& c : chans)
                            if (ImGui::MenuItem(c.c_str(), nullptr, c == tr.channel)) { tr.channel = c; markDirty(h); }
                        ImGui::EndPopup();
                    }
                } else {
                    int n = static_cast<int>(tr.strips.size());
                    char info[64];
                    if (tr.kind == Kind::Camera) std::snprintf(info, sizeof(info), "%d shot%s", n, n == 1 ? "" : "s");
                    else                         std::snprintf(info, sizeof(info), "%d strip%s", n, n == 1 ? "" : "s");
                    dl->AddText(f, fs, ImVec2(nameX, row2 + (bs - fs) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), info);
                }
            }

            // The lane: one button the whole width, so a press anywhere on it is
            // the panel's to interpret.
            ImGui::SetCursorScreenPos(ImVec2(laneX0, L.y0));
            ImGui::InvisibleButton("##lane", ImVec2(laneW, lh),
                                   ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            if (h.probe) h.probe("lane:" + std::to_string(tr.uid), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) pressedLane = li;
            if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) activeLane = li;
            if (ImGui::IsItemDeactivated()) releasedLane = li;
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) rightLane = li;
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) dblLane = li;
            // A sound dragged in from the Assets browser.
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ASSET_GUID")) {
                    const std::string guid(static_cast<const char*>(pl->Data), static_cast<std::size_t>(pl->DataSize));
                    dropSound = h.soundOfAsset ? h.soundOfAsset(guid) : std::string();
                    if (!dropSound.empty()) dropLane = li;
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::PopID();
        }

        // The add-track row under the last lane, which is also where a sound
        // dropped below every track makes a new one for itself.
        {
            const float y = lanesEnd + em * 0.3f;
            ImGui::SetCursorScreenPos(ImVec2(o.x + em * 0.5f, y));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, m.round);
            // Added after the lanes are drawn, not now: a camera track goes in
            // at the top and would shift every lane laid out above.
            if (ImGui::Button("+ Camera")) pendingTrack = static_cast<int>(Kind::Camera);
            ImGui::SetItemTooltip("Add a camera track: shots on it cut the view from camera to camera");
            ImGui::SameLine(0.0f, 2.0f);
            if (ImGui::Button("+ Animation")) pendingTrack = static_cast<int>(Kind::Anim);
            ImGui::SetItemTooltip("Add an animation track: place the scene's clips on it");
            ImGui::SameLine(0.0f, 2.0f);
            if (ImGui::Button("+ Audio")) pendingTrack = static_cast<int>(Kind::Audio);
            ImGui::SetItemTooltip("Add an audio track: sounds on it play like regions in a DAW");
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            const float dz0 = laneX0, dz1 = laneX1;
            ImGui::SetCursorScreenPos(ImVec2(dz0, y - em * 0.3f));
            ImGui::InvisibleButton("##dropzone", ImVec2(std::max(1.0f, dz1 - dz0), m.frameH * 1.6f));
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ASSET_GUID")) {
                    const std::string guid(static_cast<const char*>(pl->Data), static_cast<std::size_t>(pl->DataSize));
                    const std::string snd = h.soundOfAsset ? h.soundOfAsset(guid) : std::string();
                    if (!snd.empty()) {
                        seq::Track& nt = s.addTrack(Kind::Audio);
                        addStrip(h, s, nt, soundStrip(h, snd), tAt(io.MousePos.x));
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::GetDragDropPayload() && ImGui::GetDragDropPayload()->IsDataType("ASSET_GUID"))
                dl->AddText(ImVec2(dz0 + em, y + (m.frameH - em) * 0.5f), IM_COL32(255, 255, 255, 110),
                            "Drop a sound here for a new audio track");
        }

        // Pass 3: what the pointer did.
        const Hit hover = childHovered ? hitTest(io.MousePos) : Hit{};
        if (pressedLane >= 0) {
            const Hit hit = hitTest(io.MousePos);
            seq::Track& tr = s.tracks[lanes[pressedLane].track];
            g.selTrack = tr.uid;
            g.drag = Drag{};
            if (hit.strip >= 0) {
                g.selStrip = hit.strip;
                const seq::Strip* st = s.stripByUid(hit.strip);
                if (st && !tr.lock) {
                    g.drag.strip   = hit.strip;
                    // Where on the strip it was taken: a move keeps the strip under
                    // the pointer by this, a trim keeps the edge under it -- so a
                    // press a few pixels inside the edge does not make it jump.
                    g.drag.grab    = tAt(io.MousePos.x) -
                                     (hit.zone == ZRight ? st->end()
                                      : hit.zone == ZFadeIn ? st->start + st->fadeIn
                                      : hit.zone == ZFadeOut ? st->end() - st->fadeOut
                                      : st->start);
                    g.drag.start0  = st->start;
                    g.drag.length0 = st->length;
                    g.drag.offset0 = st->offset;
                    g.drag.mode = hit.zone == ZLeft ? Drag::TrimL : hit.zone == ZRight ? Drag::TrimR
                                : hit.zone == ZFadeIn ? Drag::FadeIn : hit.zone == ZFadeOut ? Drag::FadeOut
                                : Drag::Move;
                }
            } else {
                // Empty lane: the cursor goes there, as in every DAW.
                g.selStrip  = -1;
                g.drag.mode = Drag::Scrub;
                goTo(snapTime(s, tAt(io.MousePos.x), now, -1, g.pps, m));
            }
        }
        if (activeLane >= 0 && g.drag.mode != Drag::None) {
            const float mt = tAt(io.MousePos.x);
            seq::Track* owner = nullptr;
            seq::Strip* st = g.drag.strip >= 0 ? s.stripByUid(g.drag.strip, &owner) : nullptr;
            const float minLen = step;
            bool changed = false;
            switch (g.drag.mode) {
                case Drag::Scrub:
                    goTo(snapTime(s, mt, now, -1, g.pps, m));
                    break;
                case Drag::Move: if (st && owner) {
                    // Snap whichever end of the strip is nearer a magnet.
                    const float want = std::max(0.0f, mt - g.drag.grab);
                    float dA = FLT_MAX, dB = FLT_MAX;
                    const float a = snapTime(s, want, now, st->uid, g.pps, m, &dA);
                    const float b = snapTime(s, want + st->length, now, st->uid, g.pps, m, &dB) - st->length;
                    float ns = (dB < dA) ? b : a;
                    // Over another lane of the same kind: the strip changes track.
                    seq::Track* target = owner;
                    for (const Lane& L : lanes)
                        if (io.MousePos.y >= L.y0 && io.MousePos.y < L.y1) {
                            seq::Track& t2 = s.tracks[L.track];
                            if (t2.kind == owner->kind && !t2.lock) target = &t2;
                        }
                    ns = placeFree(*target, st->uid, ns, st->length);
                    if (target != owner) {
                        seq::Strip moved = *st;
                        moved.start = ns;
                        owner->strips.erase(std::remove_if(owner->strips.begin(), owner->strips.end(),
                                            [&](const seq::Strip& x) { return x.uid == moved.uid; }),
                                            owner->strips.end());
                        target->strips.push_back(moved);
                        target->sort();
                        g.selTrack = target->uid;
                        changed = true;
                    } else if (std::fabs(ns - st->start) > 1e-5f) {
                        st->start = ns;
                        owner->sort();
                        changed = true;
                    }
                    break;
                }
                case Drag::TrimL: if (st && owner) {
                    const float end = g.drag.start0 + g.drag.length0;
                    float ns = snapTime(s, mt - g.drag.grab, now, st->uid, g.pps, m);
                    ns = std::max(ns, prevEnd(*owner, *st));
                    // Not before the source's own start: there is nothing there. A
                    // held shot has no source and a looping clip has no start to
                    // run out of, so those two stretch freely (their In stays 0).
                    const float spd = owner->kind == Kind::Audio ? 1.0f : std::max(st->speed, 1e-3f);
                    const bool  bounded = owner->kind == Kind::Audio || (!st->clip.empty() && !st->loop);
                    if (bounded) ns = std::max(ns, g.drag.start0 - g.drag.offset0 / spd);
                    ns = std::min(ns, end - minLen);
                    if (std::fabs(ns - st->start) > 1e-5f) {
                        st->offset = std::max(0.0f, g.drag.offset0 + (ns - g.drag.start0) * spd);
                        st->start  = ns;
                        st->length = end - ns;
                        st->fadeIn = std::min(st->fadeIn, st->length);
                        changed = true;
                    }
                    break;
                }
                case Drag::TrimR: if (st && owner) {
                    float ne = snapTime(s, mt - g.drag.grab, now, st->uid, g.pps, m);
                    ne = std::min(ne, nextStart(*owner, *st));
                    if (owner->kind == Kind::Audio) {
                        const std::string path = pathOf(h, st->sound);
                        if (const fitzel::Waveform* wf = waveform(path); wf && wf->seconds > 0.0f)
                            ne = std::min(ne, st->start + (wf->seconds - st->offset));
                    }
                    ne = std::max(ne, st->start + minLen);
                    if (std::fabs(ne - st->end()) > 1e-5f) {
                        st->length  = ne - st->start;
                        st->fadeOut = std::min(st->fadeOut, st->length);
                        changed = true;
                    }
                    break;
                }
                case Drag::FadeIn: if (st) {
                    float f = snapTime(s, mt - g.drag.grab, now, st->uid, g.pps, m) - st->start;
                    f = std::clamp(f, 0.0f, st->length - st->fadeOut);
                    if (std::fabs(f - st->fadeIn) > 1e-5f) { st->fadeIn = f; changed = true; }
                    break;
                }
                case Drag::FadeOut: if (st) {
                    float f = st->end() - snapTime(s, mt - g.drag.grab, now, st->uid, g.pps, m);
                    f = std::clamp(f, 0.0f, st->length - st->fadeIn);
                    if (std::fabs(f - st->fadeOut) > 1e-5f) { st->fadeOut = f; changed = true; }
                    break;
                }
                default: break;
            }
            if (changed) {
                if (seq::Strip* cur = s.stripByUid(g.drag.strip); cur && cur->end() > s.duration)
                    s.duration = std::ceil(cur->end());
                markDirty(h);
            }
        }
        if (releasedLane >= 0) g.drag = Drag{};
        if (rightLane >= 0) {
            const Hit hit = hitTest(io.MousePos);
            g.selTrack = s.tracks[lanes[rightLane].track].uid;
            g.addTrack = g.selTrack;
            g.addAt    = snapTime(s, tAt(io.MousePos.x), now, -1, g.pps, m);
            if (hit.strip >= 0) { g.selStrip = hit.strip; g.openStripMenu = true; }
            else                { g.selStrip = -1;        g.openAdd = true; }
        }
        if (dblLane >= 0 && hover.strip < 0) {
            g.addTrack = s.tracks[lanes[dblLane].track].uid;
            g.addAt    = snapTime(s, tAt(io.MousePos.x), now, -1, g.pps, m);
            g.openAdd  = true;
        }
        if (dropLane >= 0) {
            seq::Track& tr = s.tracks[lanes[dropLane].track];
            seq::Track& dst = tr.kind == Kind::Audio ? tr : audioTrackFor(s);
            addStrip(h, s, dst, soundStrip(h, dropSound), tAt(io.MousePos.x));
        }

        // The pointer's shape says what a press will do.
        if (childHovered && g.drag.mode == Drag::None && hover.strip >= 0) {
            seq::Track* ht = hover.lane >= 0 ? &s.tracks[lanes[hover.lane].track] : nullptr;
            if (ht && !ht->lock)
                ImGui::SetMouseCursor(hover.zone == ZBody ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_ResizeEW);
        } else if (g.drag.mode == Drag::TrimL || g.drag.mode == Drag::TrimR ||
                   g.drag.mode == Drag::FadeIn || g.drag.mode == Drag::FadeOut) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        } else if (g.drag.mode == Drag::Move) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }

        // Wheel: Ctrl zooms about the pointer, Shift (or a sideways wheel)
        // scrolls the time, a plain wheel scrolls the tracks.
        if (childHovered) {
            if (io.KeyCtrl && io.MouseWheel != 0.0f) {
                zoomAt(io.MouseWheel > 0 ? 1.2f : 1.0f / 1.2f, io.MousePos.x);
            } else if (io.MouseWheelH != 0.0f || (io.KeyShift && io.MouseWheel != 0.0f)) {
                const float d = io.MouseWheelH != 0.0f ? io.MouseWheelH : io.MouseWheel;
                g.view0 = std::max(0.0f, g.view0 - d * visible * 0.08f);
            } else if (io.MouseWheel != 0.0f) {
                ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseWheel * em * 3.0f);
            }
        }

        // Pass 4: draw the lanes and their strips.
        const ImVec4 laneClip(laneX0, childMin.y, laneX1, childMax.y);
        for (int li = 0; li < static_cast<int>(lanes.size()); ++li) {
            const Lane& L = lanes[li];
            const seq::Track& tr = s.tracks[L.track];
            dl->PushClipRect(ImVec2(laneX0, L.y0), ImVec2(laneX1, L.y1), true);
            dl->AddRectFilled(ImVec2(laneX0, L.y0), ImVec2(laneX1, L.y1), u32(li % 2 ? kLaneAlt : kLaneBg));
            // Faint second lines, and the shaded region past the end.
            static const float kGrid[] = {0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 30.0f, 60.0f};
            float gs = 60.0f;
            for (float gg : kGrid) if (gg * g.pps >= em * 2.5f) { gs = gg; break; }
            for (float t = std::floor(g.view0 / gs) * gs; t <= g.view0 + visible + gs; t += gs)
                if (t >= 0.0f) dl->AddLine(ImVec2(xAt(t), L.y0), ImVec2(xAt(t), L.y1), IM_COL32(255, 255, 255, 12));
            const float ex = xAt(s.duration);
            if (ex < laneX1) dl->AddRectFilled(ImVec2(std::max(ex, laneX0), L.y0), ImVec2(laneX1, L.y1), IM_COL32(0, 0, 0, 60));
            dl->PopClipRect();
            dl->AddLine(ImVec2(o.x, L.y1), ImVec2(laneX1, L.y1), IM_COL32(0, 0, 0, 150));

            for (const seq::Strip& st : tr.strips) {
                StripView v;
                v.a = ImVec2(xAt(st.start), L.y0 + 2.0f);
                v.b = ImVec2(std::max(xAt(st.end()), v.a.x + 2.0f), L.y1 - 2.0f);
                if (v.b.x < laneX0 || v.a.x > laneX1) continue;
                v.clip     = ImVec4(laneX0, std::max(L.y0, laneClip.y), laneX1, std::min(L.y1, laneClip.w));
                v.t0       = g.view0;
                v.pps      = g.pps;
                v.laneX0   = laneX0;
                v.selected = (g.selStrip == st.uid);
                v.hovered  = (hover.strip == st.uid) || (g.drag.strip == st.uid && g.drag.mode != Drag::None);
                v.live     = tr.kind == Kind::Camera && mine && (r.playing || r.preview) && r.frame.shot == st.uid;
                v.muted    = tr.mute;
                drawStrip(dl, h, tr, st, v, m);
                if (h.probe) h.probe("strip:" + std::to_string(st.uid), v.a, v.b);
            }
        }

        // The pointer's time, faintly, while aiming at a lane.
        if (childHovered && hover.lane >= 0 && g.drag.mode == Drag::None) {
            const float ht = snapTime(s, tAt(io.MousePos.x), now, -1, g.pps, m);
            const float hx = xAt(ht);
            dl->AddLine(ImVec2(hx, childMin.y), ImVec2(hx, lanesEnd), IM_COL32(255, 255, 255, 40));
        }

        // Pass 5: the playhead, over every lane.
        {
            const float px = xAt(now);
            if (px >= laneX0 && px <= laneX1)
                dl->AddLine(ImVec2(px, childMin.y), ImVec2(px, std::max(lanesEnd, childMax.y)),
                            u32(kPlayhead, 0.95f), 2.0f);
        }

        if (s.tracks.empty()) {
            const char* msg = "No tracks. Add a camera, animation or audio track with the buttons below.";
            const ImVec2 ts = ImGui::CalcTextSize(msg);
            dl->AddText(ImVec2(laneX0 + (laneW - ts.x) * 0.5f, o.y + em * 1.5f),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), msg);
        }

        // Reserve what was drawn, so the child knows how far it scrolls.
        ImGui::SetCursorScreenPos(o);
        ImGui::Dummy(ImVec2(W - sbW, contentH));
        if (pendingTrack >= 0) {
            g.selTrack = s.addTrack(static_cast<Kind>(pendingTrack)).uid;
            g.selStrip = -1;
            markDirty(h);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // --- Overview -----------------------------------------------------------------
    // The whole sequence in one strip under the lanes, with the part on screen
    // framed: a map to click into rather than a scrollbar to aim at.
    {
        const ImVec2 o0(laneX0, ImGui::GetCursorScreenPos().y + em * 0.15f);
        const ImVec2 o1(laneX1, o0.y + m.overviewH);
        const float full = std::max({extent * 1.05f, g.view0 + visible});
        auto ox = [&](float t) { return o0.x + t / full * (o1.x - o0.x); };
        wdl->AddRectFilled(o0, o1, IM_COL32(14, 15, 18, 255), m.round);
        // The corner beside it: how the lanes are looked at and aimed into --
        // snapping, following the playhead, and the zoom as buttons.
        ImGui::SetCursorScreenPos(ImVec2(top.x + m.pad, o0.y + (m.overviewH - m.frameH) * 0.5f));
        if (iconButton("##snap", Icon::Magnet,
                       "Snap: strip edges land on the frame grid, and onto the playhead or\n"
                       "another strip's edge when they come close. Hold Alt to place freely.", g.snap))
            g.snap = !g.snap;
        ImGui::SameLine(0.0f, 2.0f);
        if (iconButton("##follow", Icon::Follow, "Follow the playhead while playing", g.follow))
            g.follow = !g.follow;
        ImGui::SameLine(0.0f, em * 0.6f);
        if (ImGui::Button("-##zo", ImVec2(m.frameH, 0.0f))) zoomAt(1.0f / 1.5f, laneX0 + laneW * 0.5f);
        ImGui::SetItemTooltip("Zoom out  (Ctrl+wheel)");
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::Button("Fit", ImVec2(em * 2.4f, 0.0f))) { g.pps = laneW / (extent * 1.04f); g.view0 = 0.0f; }
        ImGui::SetItemTooltip("Show the whole sequence");
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::Button("+##zi", ImVec2(m.frameH, 0.0f))) zoomAt(1.5f, xAt(now) > laneX0 && xAt(now) < laneX1 ? xAt(now) : laneX0 + laneW * 0.5f);
        ImGui::SetItemTooltip("Zoom in on the playhead  (Ctrl+wheel)");

        // Every strip, one thin row per track.
        const int nt = std::max(1, static_cast<int>(s.tracks.size()));
        const float rowH = std::max(1.5f, (m.overviewH - 4.0f) / static_cast<float>(nt));
        for (int i = 0; i < static_cast<int>(s.tracks.size()); ++i) {
            const seq::Track& tr = s.tracks[i];
            const float y = o0.y + 2.0f + rowH * static_cast<float>(i);
            for (const seq::Strip& st : tr.strips)
                wdl->AddRectFilled(ImVec2(ox(st.start), y), ImVec2(std::max(ox(st.end()), ox(st.start) + 1.5f), y + rowH - 0.5f),
                                   u32(kindRgb(tr.kind), tr.mute ? 0.3f : 0.75f));
        }
        const float ex = ox(s.duration);
        wdl->AddLine(ImVec2(ex, o0.y), ImVec2(ex, o1.y), u32(accent(), 0.7f));
        const float vx0 = ox(g.view0), vx1 = ox(g.view0 + visible);
        wdl->AddRectFilled(ImVec2(vx0, o0.y), ImVec2(vx1, o1.y), IM_COL32(255, 255, 255, 22), m.round);
        wdl->AddRect(ImVec2(vx0, o0.y), ImVec2(vx1, o1.y), u32(accent(), 0.85f), m.round, 0, 1.5f);
        const float px = ox(now);
        wdl->AddLine(ImVec2(px, o0.y), ImVec2(px, o1.y), u32(kPlayhead), 1.5f);

        ImGui::SetCursorScreenPos(o0);
        ImGui::InvisibleButton("##overview", ImVec2(std::max(1.0f, o1.x - o0.x), m.overviewH));
        if (ImGui::IsItemActivated()) {
            const float t = (io.MousePos.x - o0.x) / (o1.x - o0.x) * full;
            g.drag = Drag{};
            g.drag.mode = Drag::Overview;
            // Grabbed inside the frame: it moves by where it was taken. Outside:
            // the frame jumps there, centred, and moves from the middle.
            g.drag.grab = (io.MousePos.x >= vx0 && io.MousePos.x <= vx1) ? t - g.view0 : visible * 0.5f;
        }
        if (ImGui::IsItemActive() && g.drag.mode == Drag::Overview) {
            const float t = (io.MousePos.x - o0.x) / (o1.x - o0.x) * full;
            g.view0 = std::max(0.0f, t - g.drag.grab);
        }
        if (ImGui::IsItemDeactivated()) g.drag = Drag{};
        ImGui::SetItemTooltip("The whole sequence: drag the frame to look along it");
        ImGui::SetCursorScreenPos(ImVec2(top.x, o1.y + em * 0.35f));
    }

    // --- Popups ---------------------------------------------------------------------
    if (g.openAdd) { ImGui::OpenPopup("##seqadd"); g.openAdd = false; g.soundFilter[0] = 0; }
    addMenu(h, s);

    if (g.openStripMenu) { ImGui::OpenPopup("##seqstrip"); g.openStripMenu = false; }
    bool deleteStrip = false;
    if (ImGui::BeginPopup("##seqstrip")) {
        seq::Track* tr = nullptr;
        if (seq::Strip* st = s.stripByUid(g.selStrip, &tr)) {
            ui::title("%s", stripLabel(h, *tr, *st).c_str());
            if (ImGui::MenuItem("Split at playhead", "S", false, st->covers(now) && now > st->start + 1e-3f)) {
                const int nu = seq::split(s, st->uid, s.snap(now));
                if (nu > 0) { g.selStrip = nu; markDirty(h); }
            }
            if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
                seq::Strip c = *st;
                addStrip(h, s, *tr, c, st->end());
            }
            if (ImGui::MenuItem("Move to playhead")) {
                st->start = placeFree(*tr, st->uid, s.snap(now), st->length);
                tr->sort();
                markDirty(h);
            }
            if (tr->kind == Kind::Camera && st->camera >= 0 && h.selectEntity &&
                ImGui::MenuItem("Select the camera in the scene"))
                h.selectEntity(st->camera);
            ImGui::Separator();
            if (ImGui::MenuItem("Delete", "Del")) deleteStrip = true;
        } else {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (g.openTrackMenu) { ImGui::OpenPopup("##seqtrack"); g.openTrackMenu = false; }
    bool deleteTrack = false;
    if (ImGui::BeginPopup("##seqtrack")) {
        if (seq::Track* tr = s.trackByUid(g.selTrack)) {
            ui::title("%s", tr->name.c_str());
            if (ImGui::MenuItem("Rename")) {
                g.renameTrack = tr->uid;
                std::snprintf(g.renameBuf, sizeof(g.renameBuf), "%s", tr->name.c_str());
            }
            int idx = 0;
            for (int i = 0; i < static_cast<int>(s.tracks.size()); ++i) if (s.tracks[i].uid == tr->uid) idx = i;
            if (ImGui::MenuItem("Move up", nullptr, false, idx > 0)) {
                std::swap(s.tracks[idx], s.tracks[idx - 1]); markDirty(h);
            } else if (ImGui::MenuItem("Move down", nullptr, false, idx + 1 < static_cast<int>(s.tracks.size()))) {
                std::swap(s.tracks[idx], s.tracks[idx + 1]); markDirty(h);
            } else {
                if (tr->kind == Kind::Audio && ImGui::BeginMenu("Output channel")) {
                    const std::vector<std::string> chans = h.channels ? h.channels() : std::vector<std::string>{};
                    for (const std::string& c : chans)
                        if (ImGui::MenuItem(c.c_str(), nullptr, c == tr->channel)) { tr->channel = c; markDirty(h); }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Delete track")) deleteTrack = true;
            }
        } else {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // --- Keys -------------------------------------------------------------------------
    if (g.focused && !io.WantTextInput && !ImGui::IsAnyItemActive()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) { if (rolling) r.playing = false; else play(); }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { g.selStrip = -1; g.selTrack = -1; }
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) goTo(0.0f);
        if (ImGui::IsKeyPressed(ImGuiKey_End, false))  goTo(s.duration);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))   goTo(s.snap(now - (io.KeyShift ? 1.0f : step)));
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))  goTo(s.snap(now + (io.KeyShift ? 1.0f : step)));
        if (g.selStrip >= 0) {
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
                deleteStrip = true;
            if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
                const int nu = seq::split(s, g.selStrip, s.snap(now));
                if (nu > 0) { g.selStrip = nu; markDirty(h); }
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) {
                seq::Track* tr = nullptr;
                if (seq::Strip* st = s.stripByUid(g.selStrip, &tr)) {
                    seq::Strip c = *st;
                    addStrip(h, s, *tr, c, st->end());
                }
            }
        }
    }

    // --- The strip bar: the selection, as numbers ------------------------------------
    // The half of the panel that makes every drag above optional.
    ImGui::Separator();
    {
        seq::Track* tr = nullptr;
        seq::Strip* st = s.stripByUid(g.selStrip, &tr);
        const float sw = ui::stepperWidth("%.2f s");
        if (st && tr) {
            const Icon ic = tr->kind == Kind::Camera ? Icon::Camera : tr->kind == Kind::Audio ? Icon::Audio : Icon::Anim;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            drawIcon(wdl, ic, ImVec2(p.x + em * 0.5f, p.y + m.frameH * 0.5f), em, u32(kindRgb(tr->kind)));
            ImGui::Dummy(ImVec2(em * 1.1f, m.frameH));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ui::title("%s", stripLabel(h, *tr, *st).c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("on %s  ·  %s - %s", tr->name.c_str(), timecode(st->start, s.fps).c_str(),
                                timecode(st->end(), s.fps).c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(!(st->covers(now) && now > st->start + 1e-3f));
            if (iconButton("##split", Icon::Split, "Split at the playhead  (S)")) {
                const int nu = seq::split(s, st->uid, s.snap(now));
                if (nu > 0) { g.selStrip = nu; markDirty(h); }
                st = s.stripByUid(g.selStrip, &tr);
            }
            ImGui::EndDisabled();
            ImGui::SameLine(0.0f, 2.0f);
            if (iconButton("##dup", Icon::Copy, "Duplicate after itself  (Ctrl+D)")) {
                seq::Strip c = *st;
                addStrip(h, s, *tr, c, st->end());
                st = s.stripByUid(g.selStrip, &tr);
            }
            ImGui::SameLine(0.0f, 2.0f);
            if (iconButton("##del", Icon::Trash, "Delete  (Del)")) deleteStrip = true;

            if (st && !deleteStrip) {
                ImGui::BeginDisabled(tr->lock);
                // Start, length, in: a frame per click, and buttons that put an
                // edge on the playhead -- the precise way to line a cut up with
                // a beat or a door opening.
                float v = st->start;
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Start");
                ImGui::SameLine();
                if (ui::stepper("##st", v, step, 0.0f, 3600.0f, "%.2f s", sw)) {
                    st->start = placeFree(*tr, st->uid, s.snap(v), st->length);
                    tr->sort();
                    markDirty(h);
                    st = s.stripByUid(g.selStrip, &tr);
                }
                ImGui::SameLine();
                if (ImGui::Button("Start here")) {
                    st->start = placeFree(*tr, st->uid, s.snap(now), st->length);
                    tr->sort();
                    markDirty(h);
                    st = s.stripByUid(g.selStrip, &tr);
                }
                ImGui::SetItemTooltip("Move the strip so it starts at the playhead");
                flow(sw + em * 10.0f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Length");
                ImGui::SameLine();
                v = st->length;
                if (ui::stepper("##len", v, step, step, 3600.0f, "%.2f s", sw)) {
                    st->length = std::clamp(s.snap(v), step, std::max(step, nextStart(*tr, *st) - st->start));
                    st->fadeIn  = std::min(st->fadeIn, st->length);
                    st->fadeOut = std::min(st->fadeOut, st->length - st->fadeIn);
                    if (st->end() > s.duration) s.duration = std::ceil(st->end());
                    markDirty(h);
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(now <= st->start + step * 0.5f);
                if (ImGui::Button("End here")) {
                    st->length = std::clamp(s.snap(now) - st->start, step,
                                            std::max(step, nextStart(*tr, *st) - st->start));
                    markDirty(h);
                }
                ImGui::EndDisabled();
                ImGui::SetItemTooltip("Trim the strip so it ends at the playhead");
                flow(sw + em * 4.0f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("In");
                ImGui::SameLine();
                v = st->offset;
                if (ui::stepper("##off", v, step, 0.0f, 3600.0f, "%.2f s", sw)) { st->offset = std::max(0.0f, v); markDirty(h); }
                ImGui::SetItemTooltip("How far into its %s the strip begins", tr->kind == Kind::Audio ? "sound" : "clip");
                ImGui::EndDisabled();

                // The kind's own fields.
                if (tr->kind == Kind::Camera || tr->kind == Kind::Anim) {
                    if (tr->kind == Kind::Camera) {
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted("Camera");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(em * 11.0f);
                        if (ImGui::BeginCombo("##cam", entityName(h.entities, st->camera).c_str())) {
                            for (int id : cameraIds(h.entities)) {
                                ImGui::PushID(id);
                                if (ImGui::Selectable(entityName(h.entities, id).c_str(), id == st->camera)) {
                                    st->camera = id;
                                    markDirty(h);
                                }
                                ImGui::PopID();
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::SameLine();
                        ImGui::BeginDisabled(st->camera < 0 || !h.selectEntity);
                        if (ImGui::Button("Select")) h.selectEntity(st->camera);
                        ImGui::SetItemTooltip("Select this camera in the scene");
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                    }
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(tr->kind == Kind::Camera ? "Move" : "Clip");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(em * 11.0f);
                    const char* none = tr->kind == Kind::Camera ? "(held shot)" : "(none)";
                    if (ImGui::BeginCombo("##clip", st->clip.empty() ? none : st->clip.c_str())) {
                        if (tr->kind == Kind::Camera && ImGui::Selectable(none, st->clip.empty())) { st->clip.clear(); markDirty(h); }
                        for (const anim::Clip& c : h.clips) {
                            if (c.empty()) continue;
                            if (ImGui::Selectable(c.name.c_str(), c.name == st->clip)) {
                                st->clip = c.name;
                                // A camera move picked for a shot with no camera
                                // brings its camera along.
                                if (tr->kind == Kind::Camera && st->camera < 0)
                                    st->camera = seq::cameraOfClip(c, h.entities);
                                markDirty(h);
                            }
                        }
                        ImGui::EndCombo();
                    }
                    if (!st->clip.empty()) {
                        flow(ui::stepperWidth("%.2fx") + em * 12.0f);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted("Speed");
                        ImGui::SameLine();
                        float spd = st->speed;
                        if (ui::stepper("##spd", spd, 0.05f, 0.05f, 8.0f, "%.2fx")) { st->speed = spd; markDirty(h); }
                        ImGui::SameLine();
                        const int ci = anim::findClip(h.clips, st->clip);
                        ImGui::BeginDisabled(ci < 0);
                        if (ImGui::Button("Fit clip")) {
                            st->length = std::clamp((seq::clipLength(h.clips[ci]) - st->offset) / std::max(st->speed, 0.01f),
                                                    step, std::max(step, nextStart(*tr, *st) - st->start));
                            markDirty(h);
                        }
                        ImGui::SetItemTooltip("Make the strip exactly as long as its clip");
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        if (ImGui::Checkbox("Loop", &st->loop)) markDirty(h);
                        ImGui::SetItemTooltip("Play the clip round again while the strip lasts");
                    }
                } else {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted("Sound");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(em * 12.0f);
                    if (ImGui::BeginCombo("##snd", st->sound.empty() ? "(none)" : st->sound.c_str())) {
                        for (const std::string& n : h.soundNames ? h.soundNames() : std::vector<std::string>{})
                            if (ImGui::Selectable(n.c_str(), n == st->sound)) { st->sound = n; markDirty(h); }
                        ImGui::EndCombo();
                    }
                    flow(ui::stepperWidth("%+.1f dB") + em * 3.0f);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted("Gain");
                    ImGui::SameLine();
                    float db = toDb(st->gain);
                    if (ui::stepper("##sg", db, 1.0f, -60.0f, 6.0f, "%+.1f dB")) { st->gain = fromDb(db); markDirty(h); }
                    flow(sw * 2.0f + em * 9.0f);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted("Fade in");
                    ImGui::SameLine();
                    float f = st->fadeIn;
                    if (ui::stepper("##fi", f, step * 3.0f, 0.0f, st->length - st->fadeOut, "%.2f s", sw)) { st->fadeIn = f; markDirty(h); }
                    ImGui::SameLine();
                    ImGui::TextUnformatted("out");
                    ImGui::SameLine();
                    f = st->fadeOut;
                    if (ui::stepper("##fo", f, step * 3.0f, 0.0f, st->length - st->fadeIn, "%.2f s", sw)) { st->fadeOut = f; markDirty(h); }
                }
            }
        } else if (seq::Track* t = s.trackByUid(g.selTrack)) {
            const Icon ic = t->kind == Kind::Camera ? Icon::Camera : t->kind == Kind::Audio ? Icon::Audio : Icon::Anim;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            drawIcon(wdl, ic, ImVec2(p.x + em * 0.5f, p.y + m.frameH * 0.5f), em, u32(kindRgb(t->kind)));
            ImGui::Dummy(ImVec2(em * 1.1f, m.frameH));
            ImGui::SameLine();
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s", t->name.c_str());
            ImGui::SetNextItemWidth(em * 12.0f);
            if (ImGui::InputText("##trname", buf, sizeof(buf))) { t->name = buf; markDirty(h); }
            ImGui::SameLine();
            ImGui::TextDisabled("%s track  ·  %d strip%s", seq::kindName(t->kind),
                                static_cast<int>(t->strips.size()), t->strips.size() == 1 ? "" : "s");
            ImGui::SameLine();
            if (iconButton("##addat", Icon::Plus, "Add at the playhead")) {
                g.addTrack = t->uid; g.addAt = now; g.openAdd = true;
            }
            ImGui::SameLine(0.0f, 2.0f);
            if (iconButton("##deltr", Icon::Trash, "Delete this track")) deleteTrack = true;
            if (t->kind == Kind::Audio) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Level");
                ImGui::SameLine();
                float db = toDb(t->gain);
                if (ui::stepper("##tg", db, 1.0f, -60.0f, 6.0f, "%+.1f dB")) { t->gain = fromDb(db); markDirty(h); }
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Output");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(em * 9.0f);
                if (ImGui::BeginCombo("##chan", t->channel.c_str())) {
                    for (const std::string& c : h.channels ? h.channels() : std::vector<std::string>{})
                        if (ImGui::Selectable(c.c_str(), c == t->channel)) { t->channel = c; markDirty(h); }
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("The mixer channel this track plays through");
            } else {
                if (ImGui::Checkbox("Hold the last pose between strips", &t->hold)) markDirty(h);
                if (t->kind == Kind::Camera)
                    ui::hint("The topmost camera track with a shot under the playhead is the view; "
                             "between shots the last one holds.");
            }
        } else {
            // Nothing selected: the sequence itself.
            const ImVec2 p = ImGui::GetCursorScreenPos();
            drawIcon(wdl, Icon::Play, ImVec2(p.x + em * 0.5f, p.y + m.frameH * 0.5f), em, u32(accent()));
            ImGui::Dummy(ImVec2(em * 1.1f, m.frameH));
            ImGui::SameLine();
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s", s.name.c_str());
            ImGui::SetNextItemWidth(em * 11.0f);
            if (ImGui::InputText("##seqname", buf, sizeof(buf))) { s.name = buf; markDirty(h); }
            ImGui::SetItemTooltip("The sequence's name -- what game.playSequence() asks for");
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Length");
            ImGui::SameLine();
            float d = s.duration;
            if (ui::stepper("##dur", d, 1.0f, 1.0f, 3600.0f, "%.1f s")) { s.duration = std::max(d, 0.5f); markDirty(h); }
            ImGui::SameLine();
            ImGui::BeginDisabled(s.contentEnd() < 0.1f);
            if (ImGui::Button("Fit to strips")) { s.duration = std::max(0.5f, s.snap(s.contentEnd())); markDirty(h); }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("End the sequence where its last strip ends");
            flow(em * 8.0f);
            static const float kRates[] = {24.0f, 25.0f, 30.0f, 50.0f, 60.0f};
            char cur[32];
            std::snprintf(cur, sizeof(cur), "%g fps", s.fps);
            ImGui::SetNextItemWidth(em * 6.0f);
            if (ImGui::BeginCombo("##fps", cur)) {
                for (float f : kRates) {
                    char lbl[32];
                    std::snprintf(lbl, sizeof(lbl), "%g fps", f);
                    if (ImGui::Selectable(lbl, std::fabs(f - s.fps) < 0.01f)) { s.fps = f; markDirty(h); }
                }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("The frame grid: every strip edge snaps to it, and the clock counts it");

            if (ImGui::Checkbox("Play on start", &s.playOnStart)) {
                // One sequence opens the game: ticking a second hands it the job.
                if (s.playOnStart)
                    for (seq::Sequence& o : h.seqs) if (&o != &s) o.playOnStart = false;
                markDirty(h);
            }
            ImGui::SetItemTooltip("Run this sequence the moment the game starts -- an intro, a flyover");
            ImGui::SameLine();
            if (ImGui::Checkbox("Loop", &s.loop)) markDirty(h);
            ImGui::SameLine();
            if (ImGui::Checkbox("Give camera back", &s.releaseCamera)) markDirty(h);
            ImGui::SetItemTooltip("When the sequence ends in the game, the view goes back to the camera\n"
                                  "it had before the first shot. Off, the last shot stays the view.");
            ui::hint("Click a strip or a track header to edit it; the cog (or Esc) brings these settings "
                     "back. Double-click an empty lane to place something; drag a sound from the Assets "
                     "browser onto an audio track. Space plays, S splits, Del deletes, Ctrl+wheel zooms.");
        }
    }

    if (deleteStrip) {
        for (seq::Track& tr : s.tracks)
            tr.strips.erase(std::remove_if(tr.strips.begin(), tr.strips.end(),
                                           [&](const seq::Strip& x) { return x.uid == g.selStrip; }),
                            tr.strips.end());
        g.selStrip = -1;
        markDirty(h);
    }
    if (deleteTrack) {
        s.tracks.erase(std::remove_if(s.tracks.begin(), s.tracks.end(),
                                      [&](const seq::Track& x) { return x.uid == g.selTrack; }),
                       s.tracks.end());
        g.selTrack = g.selStrip = -1;
        markDirty(h);
    }

    ImGui::End();
}

} // namespace sequi
